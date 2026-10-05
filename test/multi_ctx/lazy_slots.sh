#!/usr/bin/env bash
# The generated TU's lazily allocated runtime slots across instances.
#
# Two pieces of TU state in spinel_rt.h are allocated on first use, from the
# instance that happens to be current:
#   - the break-scope stack (sp_brk_stack and its parallel arrays), on the
#     first block `break` out of a yielding method;
#   - the frozen-string dedup table (sp_fstr_tab), on the first String#-@.
# Under SP_MULTI_CTX an instance's memory goes away with the instance, so a
# later instance of the same program must not keep using those pointers: the
# slots have to start empty in every new instance, and must be kept (not
# reallocated) across entry calls of the same instance.
#
# Checks:
#   1. restart: an ext program is initialised, called, destroyed and created
#      again many times, each instance on its own pool. The pool of a destroyed
#      instance is filled with a pattern (as a FreeRTOS heap fill would), so a
#      stale pointer into it reads garbage. Every round must print the CRuby
#      answers.
#   2. the same under AddressSanitizer, with the dead pool poisoned: any read
#      or write through a stale slot is reported (use-after-poison), separately
#      for the break stack and the dedup table.
#   3. rerun: a --no-main program (whose entry runs the whole program, statics
#      cleared, on every call) is called many times in ONE instance; the slots
#      belong to that instance, so the pool must not grow by a fresh break stack
#      or table on every call.
#
# Usage: test/multi_ctx/lazy_slots.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
HERE="$ROOT/test/multi_ctx"
EST="$HERE/estalloc"
MCFLAGS="-DSP_MULTI_CTX -include $LIB/sp_mem_override.h"
ROUNDS="${ROUNDS:-20}"
TMP="$(mktemp -d)"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$TMP"' EXIT
fail=0
ulimit -c 0 2>/dev/null || true

if [ ! -f "$MC" ]; then echo "FAIL: $MC not built (make lib/libspinel_rt_mc.a)"; exit 1; fi

cat > "$TMP/lazy_k.rb" <<'RB'
module LazyK
  def self.each_upto(n)
    i = 0
    while i < n
      yield i
      i += 1
    end
    -1
  end

  # a block `break` out of a yielding method: the break-scope stack
  def self.first_over(limit, n)
    each_upto(n) { |x| break x if x * x > limit }
  end

  # String#-@ on a built string: the frozen-string dedup table
  def self.interned(i)
    a = -"tag#{i % 5}"
    b = -"tag#{i % 5}"
    a.equal?(b) ? a.length * 10 + i % 5 : -99
  end
end

if __FILE__ == $0
  p LazyK.first_over(50, 100)
  p LazyK.interned(3)
end
RB

# the --no-main shape: the whole program is the entry
cat > "$TMP/lazy_m.rb" <<'RB'
def each_upto(n)
  i = 0
  while i < n
    yield i
    i += 1
  end
  -1
end
r = each_upto(100) { |x| break x if x * x > 50 }
s = -"tag#{r}"
t = -"tag#{r}"
puts "#{r} #{s.equal?(t)}"
RB

"$SP" "$TMP/lazy_k.rb" -c --no-line-map --ext-init Init_lazy_k \
  --ext-entry LazyK.first_over,LazyK.interned -o "$TMP/lazy_k.c" >"$TMP/gen_k.log" 2>&1 \
  || { echo "FAIL: lazy_k generation"; tail -5 "$TMP/gen_k.log"; exit 1; }
"$SP" --no-main --entry lazy_m_entry -o "$TMP/lazy_m.c" "$TMP/lazy_m.rb" >"$TMP/gen_m.log" 2>&1 \
  || { echo "FAIL: lazy_m generation"; tail -5 "$TMP/gen_m.log"; exit 1; }
grep -q "sp_brk_push()" "$TMP/lazy_k.c" && grep -q "sp_str_uminus_val(" "$TMP/lazy_k.c" \
  || { echo "FAIL: lazy_k.c no longer reaches the break stack and the dedup table"; exit 1; }

cat > "$TMP/host.c" <<'C'
#include "lazy_k.h"
#include "estalloc/estalloc.h"
#include <stdio.h>
#ifdef __SANITIZE_ADDRESS__
#include <sanitizer/asan_interface.h>
#define POISON(p, n)   ASAN_POISON_MEMORY_REGION((p), (n))
#define UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define POISON(p, n)   ((void)0)
#define UNPOISON(p, n) ((void)0)
#endif

static void *est_alloc_hook(void *ud, size_t n)            { return est_calloc((ESTALLOC*)ud, 1u, (unsigned)n); }
static void *est_realloc_hook(void *ud, void *p, size_t n) { return est_realloc((ESTALLOC*)ud, p, (unsigned)n); }
static void  est_free_hook(void *ud, void *p)              { est_free((ESTALLOC*)ud, p); }

#define POOL_SZ (1u * 1024 * 1024)
static _Alignas(16) unsigned char pools[2][POOL_SZ];

typedef struct { sp_int a, b, r; } call_t;
static void c_first(void *p)  { call_t *c = p; c->r = sp_LazyK_s_first_over(c->a, c->b).v.i; }
static void c_intern(void *p) { call_t *c = p; c->r = sp_LazyK_s_interned(c->a); }

static sp_int call(void (*fn)(void *), call_t *c) {
  const char *cls = 0, *msg = 0;
  if (Init_lazy_k_try(fn, c, &cls, &msg)) { printf("raise %s: %s\n", cls, msg); return -1000; }
  return c->r;
}

/* lazy_k <brk|fstr|both> <rounds>: each round is a new instance on the other
   pool; the pool just left is filled and (under ASan) poisoned */
int main(int argc, char **argv) {
  const char *what = argc > 1 ? argv[1] : "both";
  int rounds = argc > 2 ? atoi(argv[2]) : 10;
  int brk = strcmp(what, "fstr") != 0, fstr = strcmp(what, "brk") != 0;
  for (int r = 0; r < rounds; r++) {
    unsigned char *pool = pools[r & 1];
    UNPOISON(pool, POOL_SZ);
    ESTALLOC *e = est_init(pool, POOL_SZ);
    sp_instance_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.mem_ud = e; cfg.alloc = est_alloc_hook; cfg.realloc_fn = est_realloc_hook; cfg.dealloc = est_free_hook;
    sp_ctx *x = sp_instance_create(&cfg);
    sp_ctx_set_current(x);
    Init_lazy_k();
    printf("round");
    for (int i = 0; i < 4; i++) {
      call_t c = { 30 + i * 10, 100, 0 };
      if (brk) printf(" %ld", (long)call(c_first, &c));
      c = (call_t){ i, 0, 0 };
      if (fstr) printf(" %ld", (long)call(c_intern, &c));
    }
    printf("\n");
    sp_ctx_set_current(NULL);
    sp_instance_destroy(x);
    memset(pool, 0xA5, POOL_SZ);
    POISON(pool, POOL_SZ);
  }
  return 0;
}
C

cat > "$TMP/rerun.c" <<'C'
#include "sp_gc.h"
#include "estalloc/estalloc.h"
#include <stdio.h>
#include <string.h>
int lazy_m_entry(void);
static void *est_alloc_hook(void *ud, size_t n)            { return est_calloc((ESTALLOC*)ud, 1u, (unsigned)n); }
static void *est_realloc_hook(void *ud, void *p, size_t n) { return est_realloc((ESTALLOC*)ud, p, (unsigned)n); }
static void  est_free_hook(void *ud, void *p)              { est_free((ESTALLOC*)ud, p); }
#define POOL_SZ (4u * 1024 * 1024)
static _Alignas(16) unsigned char pool[POOL_SZ];
static unsigned used(ESTALLOC *e) { est_take_statistics(e); return e->stat.used; }
/* rerun <calls>: one instance, the --no-main entry called <calls> times;
   prints the pool growth from call 2 to the last call */
int main(int argc, char **argv) {
  int calls = argc > 1 ? atoi(argv[1]) : 40;
  ESTALLOC *e = est_init(pool, POOL_SZ);
  sp_instance_config cfg; memset(&cfg, 0, sizeof cfg);
  cfg.mem_ud = e; cfg.alloc = est_alloc_hook; cfg.realloc_fn = est_realloc_hook; cfg.dealloc = est_free_hook;
  sp_ctx *x = sp_instance_create(&cfg);
  sp_ctx_set_current(x);
  unsigned base = 0;
  for (int i = 0; i < calls; i++) {
    lazy_m_entry();
    if (i == 1) base = used(e);
  }
  fprintf(stderr, "growth %u\n", used(e) - base);
  sp_instance_destroy(x);
  return 0;
}
C

cc -c -O2 -w -DESTALLOC_DEBUG -I"$EST" "$EST/estalloc.c" -o "$TMP/estalloc.o" 2>"$TMP/e.est" \
  || { echo "FAIL: estalloc.c compile"; tail -5 "$TMP/e.est"; exit 1; }
for p in lazy_k lazy_m; do
  cc -c -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/$p.c" -o "$TMP/$p.o" 2>"$TMP/e.$p" \
    || { echo "FAIL: $p.c compile"; tail -5 "$TMP/e.$p"; exit 1; }
done
cc -O2 -w $MCFLAGS -I"$TMP" -I"$LIB" -I"$LIB/regexp" -I"$HERE" "$TMP/host.c" "$TMP/lazy_k.o" \
  "$TMP/estalloc.o" "$MC" -lm -lcrypt -lpthread -o "$TMP/host" 2>"$TMP/e.host" \
  || { echo "FAIL: host link"; tail -5 "$TMP/e.host"; exit 1; }
cc -O2 -w -DSP_MULTI_CTX -DESTALLOC_DEBUG -I"$LIB" -I"$HERE" "$TMP/rerun.c" "$TMP/lazy_m.o" \
  "$TMP/estalloc.o" "$MC" -lm -lcrypt -lpthread -o "$TMP/rerun" 2>"$TMP/e.rerun" \
  || { echo "FAIL: rerun link"; tail -5 "$TMP/e.rerun"; exit 1; }

# CRuby's answers for one round (the same calls host.c makes)
expect="round"
for i in 0 1 2 3; do
  expect="$expect $(ruby -e "load '$TMP/lazy_k.rb'; print LazyK.first_over(30 + $i * 10, 100), ' ', LazyK.interned($i)")"
done

# 1. restart, dead pools filled
"$TMP/host" both "$ROUNDS" > "$TMP/out.txt" 2>"$TMP/err.txt"; rc=$?
bad="$(grep -vxF -e "$expect" "$TMP/out.txt" | head -3)"
n="$(grep -cxF -e "$expect" "$TMP/out.txt")"
if [ "$rc" -ne 0 ] || [ -n "$bad" ] || [ "$n" -ne "$ROUNDS" ]; then
  echo "FAIL: restart (rc=$rc, $n of $ROUNDS rounds right)"; echo "$bad"; tail -3 "$TMP/err.txt"; fail=1
fi

# 2. restart under ASan, dead pools poisoned (runtime built from source)
RT_MEMBERS="$(sed -n 's/^RT_MEMBERS = //p' "$ROOT/Makefile")"
ASRC=""; for m in $RT_MEMBERS; do ASRC="$ASRC $LIB/$m.c"; done
ARE="$LIB/regexp/re_compile.c $LIB/regexp/re_exec.c $LIB/regexp/re_utf8.c"
if cc -g -O1 -w -fsanitize=address $MCFLAGS -I"$TMP" -I"$LIB" -I"$LIB/regexp" -I"$HERE" \
      "$TMP/host.c" "$TMP/lazy_k.c" $ASRC $ARE -DESTALLOC_DEBUG -I"$EST" "$EST/estalloc.c" \
      -lm -lcrypt -lpthread -o "$TMP/host_asan" 2>"$TMP/e.asan"; then
  for what in brk fstr; do
    ASAN_OPTIONS=detect_leaks=0 "$TMP/host_asan" "$what" 4 > "$TMP/asan_$what.txt" 2>&1; rc=$?
    if [ "$rc" -ne 0 ] || grep -q "ERROR: AddressSanitizer" "$TMP/asan_$what.txt"; then
      echo "FAIL: restart under ASan ($what, rc=$rc)"
      grep -m1 -A6 "ERROR: AddressSanitizer" "$TMP/asan_$what.txt" | sed 's/^/    /'; fail=1
    fi
  done
else
  echo "SKIP: ASan build unavailable"; tail -3 "$TMP/e.asan"
fi

# 3. rerun in one instance: the slots stay with the instance
"$TMP/rerun" 40 > "$TMP/rerun.txt" 2>"$TMP/rerun.err"; rc=$?
growth="$(sed -n 's/^growth //p' "$TMP/rerun.err")"
lines="$(grep -cx "8 true" "$TMP/rerun.txt")"
if [ "$rc" -ne 0 ] || [ "$lines" -ne 40 ] || [ -z "$growth" ] || [ "$growth" -gt 65536 ]; then
  echo "FAIL: rerun in one instance (rc=$rc, $lines of 40 right, pool growth ${growth:-?} B)"; fail=1
fi

if [ "$fail" -eq 0 ]; then echo "multi_ctx lazy_slots: PASS ($ROUNDS restarts, ASan, rerun growth ${growth} B)"; else echo "multi_ctx lazy_slots: FAIL"; fi
exit $fail
