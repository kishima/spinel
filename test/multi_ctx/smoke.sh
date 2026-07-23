#!/usr/bin/env bash
# Smoke test for the multi-instance runtime (-DSP_MULTI_CTX, libspinel_rt_mc.a).
#
# Under SP_MULTI_CTX the runtime's mutable globals live in a per-instance sp_ctx
# reached through a thread-local pointer, so several Spinel-compiled programs can
# share one process, each on its own OS thread with its own heap and GC (see
# docs/internals/multi-instance.md). A program built this way is not standalone-
# runnable: the host must call sp_ctx_set_current(sp_instance_create(...)) before
# the program entry. That is what this test exercises, in three steps:
#   1. Single instance: output matches a normal `-E` compile byte-for-byte.
#   2. N instances on N threads, concurrently, each running an allocation- and
#      GC-heavy program: every thread must produce the correct result (heaps are
#      independent -- no cross-instance corruption).
#   3. The same N-thread run under AddressSanitizer, built from source: no memory
#      error on the concurrent alloc/GC/sweep paths.
#
# Usage: test/multi_ctx/smoke.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
NTHREADS="${NTHREADS:-4}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

if [ ! -f "$MC" ]; then echo "FAIL: $MC not built (make lib/libspinel_rt_mc.a)"; exit 1; fi

# --- host that creates one instance per thread and runs the program entry ---
cat > "$TMP/host.c" <<'C'
#include "sp_gc.h"   /* defines sp_RbVal etc.; pulls in sp_ctx.h */
#include <stdio.h>
#include <pthread.h>
int sp_prog_entry(void);
static void *run(void *arg) {
  (void)arg;
  sp_instance_config cfg = {0};
  sp_ctx *c = sp_instance_create(&cfg);
  if (!c) { fprintf(stderr, "instance_create failed\n"); return (void *)1; }
  sp_ctx_set_current(c);
  int rc = sp_prog_entry();
  sp_instance_destroy(c);
  return (void *)(long)rc;
}
#ifndef NTHREADS
#define NTHREADS 4
#endif
int main(void) {
  pthread_t t[NTHREADS];
  long bad = 0;
  for (int i = 0; i < NTHREADS; i++) pthread_create(&t[i], NULL, run, NULL);
  for (int i = 0; i < NTHREADS; i++) { void *r; pthread_join(t[i], &r); bad |= (long)r; }
  return bad ? 3 : 0;
}
C

# --- allocation- and GC-heavy program with a deterministic result ---
cat > "$TMP/stress.rb" <<'RB'
def sp_prog_entry_ruby
  sum = 0
  1000.times do |i|
    a = []
    20.times { |j| a << "s#{i}_#{j}" }
    sum += a.length
    h = {}
    5.times { |k| h["key#{k}"] = i * k }
    sum += h.size
  end
  puts sum
end
sp_prog_entry_ruby
RB

"$SP" -E "$TMP/stress.rb" > "$TMP/ref.txt" 2>/dev/null
REF="$(cat "$TMP/ref.txt")"
"$SP" --no-main --entry sp_prog_entry -o "$TMP/stress.c" "$TMP/stress.rb" >/dev/null 2>&1

# 1. single instance (NTHREADS=1) matches -E
if cc -O2 -w -DSP_MULTI_CTX -DNTHREADS=1 -I"$LIB" -I"$LIB/regexp" \
      "$TMP/stress.c" "$TMP/host.c" "$MC" -lm -lcrypt -lpthread -o "$TMP/single" 2>"$TMP/e1"; then
  if [ "$("$TMP/single")" != "$REF" ]; then echo "FAIL: single-instance output differs from -E"; fail=1; fi
else
  echo "FAIL: single-instance link error"; tail -5 "$TMP/e1"; fail=1
fi

# 2. N instances on N threads: every line must equal the reference
if cc -O2 -w -DSP_MULTI_CTX -DNTHREADS=$NTHREADS -I"$LIB" -I"$LIB/regexp" \
      "$TMP/stress.c" "$TMP/host.c" "$MC" -lm -lcrypt -lpthread -o "$TMP/multi" 2>"$TMP/e2"; then
  "$TMP/multi" > "$TMP/multi.out" 2>&1; rc=$?
  n_ok="$(grep -cx "$REF" "$TMP/multi.out")"
  if [ "$rc" -ne 0 ] || [ "$n_ok" -ne "$NTHREADS" ]; then
    echo "FAIL: $NTHREADS-thread run: rc=$rc, $n_ok/$NTHREADS lines correct"; head "$TMP/multi.out"; fail=1
  fi
else
  echo "FAIL: $NTHREADS-thread link error"; tail -5 "$TMP/e2"; fail=1
fi

# 3. N-thread run under AddressSanitizer, built from source (the archive is -O2
#    without -g/instrumentation). Skipped if the compiler lacks ASan.
RT_MEMBERS="sp_bigint sp_crypto sp_pack sp_time sp_core sp_net sp_system sp_ctx sp_gc sp_alloc sp_marshal sp_format sp_string sp_inspect sp_array sp_str sp_re sp_random sp_fiber sp_sched sp_io sp_cold"
SRCS=""; for m in $RT_MEMBERS; do SRCS="$SRCS $LIB/$m.c"; done
RESRC="$LIB/regexp/re_compile.c $LIB/regexp/re_exec.c $LIB/regexp/re_utf8.c"
if cc -g -O1 -w -DSP_MULTI_CTX -DNTHREADS=$NTHREADS -fsanitize=address \
      -I"$LIB" -I"$LIB/regexp" "$TMP/stress.c" "$TMP/host.c" $SRCS $RESRC \
      -lm -lcrypt -lpthread -o "$TMP/asan" 2>"$TMP/e3"; then
  ASAN_OPTIONS=detect_leaks=0 "$TMP/asan" > "$TMP/asan.out" 2>&1; rc=$?
  n_ok="$(grep -cx "$REF" "$TMP/asan.out")"
  if [ "$rc" -ne 0 ] || [ "$n_ok" -ne "$NTHREADS" ]; then
    echo "FAIL: ASan $NTHREADS-thread run: rc=$rc, $n_ok/$NTHREADS correct"; head -40 "$TMP/asan.out"; fail=1
  fi
else
  echo "SKIP: ASan build unavailable (compiler lacks -fsanitize=address)"; tail -3 "$TMP/e3"
fi

if [ "$fail" -eq 0 ]; then echo "multi_ctx smoke: PASS ($NTHREADS threads)"; else echo "multi_ctx smoke: FAIL"; fi
exit $fail
