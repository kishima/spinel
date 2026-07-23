#!/usr/bin/env bash
# Phase 3.5 T3.5-3: exercise SP_MULTI_CTX instances on a custom (non-libc)
# allocator -- the case that actually proves per-instance memory isolation and
# the only one that would catch a mismatched alloc/free pair (the smoke test
# uses the libc backend). Backend = estalloc (vendored under estalloc/, BSD-3),
# one fixed pool buffer per instance.
#
#   estalloc_test       : S1 isolation + stats, S2 concurrency, S3 exhaustion
#   estalloc_root_test  : S4 GC-root-stack overflow aborts cleanly
#   estalloc_test+ASan  : the above under AddressSanitizer with leak detection ON
#
# The two program shapes (rich, deep) go in separate binaries: two generated TUs
# cannot co-link (each emits the runtime's non-static symbols).
#
# Usage: test/multi_ctx/estalloc.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
HERE="$ROOT/test/multi_ctx"
EST="$HERE/estalloc"
MCFLAGS="-DSP_MULTI_CTX -include $LIB/sp_mem_override.h"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
ulimit -c 0 2>/dev/null || true   # S4 aborts on purpose; no core files

if [ ! -f "$MC" ]; then echo "FAIL: $MC not built"; exit 1; fi

# --- programs -------------------------------------------------------------
# rich: retains a few thousand strings so its live set dwarfs a small pool.
cat > "$TMP/rich.rb" <<'RB'
def sp_prog_entry_ruby
  a = []
  3000.times { |i| a << "string number #{i} with some length" }
  sum = 0
  a.each { |s| sum += s.length }
  puts sum
end
sp_prog_entry_ruby
RB
# deep: a rooted string per recursion frame, so simultaneous GC roots grow with
# depth and blow past a tiny root-stack cap.
cat > "$TMP/deep.rb" <<'RB'
def rec(n)
  return 0 if n <= 0
  s = "depth marker #{n}"
  rec(n - 1) + s.length
end
puts rec(400)
RB

"$SP" --no-main --entry sp_prog_entry -o "$TMP/rich.c" "$TMP/rich.rb" >/dev/null 2>&1
"$SP" --no-main --entry sp_prog_entry -o "$TMP/deep.c" "$TMP/deep.rb" >/dev/null 2>&1

# estalloc.c and the generated TUs (the latter need the allocation override so
# their sp_runtime.h inline allocations route through the instance backend).
cc -c -O2 -w -DESTALLOC_DEBUG -I"$EST" "$EST/estalloc.c" -o "$TMP/estalloc.o" 2>"$TMP/e.est" \
  || { echo "FAIL: estalloc.c compile"; tail -5 "$TMP/e.est"; exit 1; }
cc -c -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/rich.c" -o "$TMP/rich.o" 2>"$TMP/e.rich" \
  || { echo "FAIL: rich.c compile"; tail -5 "$TMP/e.rich"; exit 1; }
cc -c -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/deep.c" -o "$TMP/deep.o" 2>"$TMP/e.deep" \
  || { echo "FAIL: deep.c compile"; tail -5 "$TMP/e.deep"; exit 1; }

# host objects (NOT overridden: they call est_* directly and never malloc).
HOSTF="-O2 -w -DSP_MULTI_CTX -DESTALLOC_DEBUG -I$LIB -I$LIB/regexp -I$HERE"
cc -c $HOSTF "$HERE/estalloc_test.c"      -o "$TMP/etest.o"  2>"$TMP/e.h1" || { echo "FAIL: estalloc_test.c"; tail "$TMP/e.h1"; exit 1; }
cc -c $HOSTF "$HERE/estalloc_root_test.c" -o "$TMP/ertest.o" 2>"$TMP/e.h2" || { echo "FAIL: estalloc_root_test.c"; tail "$TMP/e.h2"; exit 1; }

run() { # <label> <binary>   (stdout is program noise; stderr carries diagnostics)
  "$2" >/dev/null 2>"$TMP/err"; local rc=$?
  sed 's/^/    /' "$TMP/err"
  return $rc
}

# --- S1/S2/S3 -------------------------------------------------------------
cc -O2 -w -I"$LIB" "$TMP/etest.o" "$TMP/rich.o" "$TMP/estalloc.o" "$MC" -lm -lcrypt -lpthread -o "$TMP/etest" \
  || { echo "FAIL: link estalloc_test"; fail=1; }
[ -x "$TMP/etest" ] && { run "estalloc_test" "$TMP/etest" || fail=1; }

# --- S4 -------------------------------------------------------------------
cc -O2 -w -I"$LIB" "$TMP/ertest.o" "$TMP/deep.o" "$TMP/estalloc.o" "$MC" -lm -lcrypt -o "$TMP/ertest" \
  || { echo "FAIL: link estalloc_root_test"; fail=1; }
[ -x "$TMP/ertest" ] && { run "estalloc_root_test" "$TMP/ertest" || fail=1; }

# --- S1/S2/S3 under AddressSanitizer + leak detection ---------------------
# Built from source (the archive is not instrumented). Every runtime allocation
# lands in a static pool, so a clean leak report also confirms no libc-level
# leak and no cross-instance corruption.
RT_MEMBERS="sp_bigint sp_crypto sp_pack sp_time sp_core sp_net sp_system sp_ctx sp_gc sp_alloc sp_marshal sp_format sp_string sp_inspect sp_array sp_str sp_re sp_random sp_fiber sp_sched sp_io sp_cold"
ASRC=""; for m in $RT_MEMBERS; do ASRC="$ASRC $LIB/$m.c"; done
ARE="$LIB/regexp/re_compile.c $LIB/regexp/re_exec.c $LIB/regexp/re_utf8.c"
if cc -g -O1 -w -fsanitize=address -I"$LIB" -I"$LIB/regexp" -I"$HERE" \
      $MCFLAGS "$TMP/rich.c" $ASRC $ARE \
      -DESTALLOC_DEBUG -I"$EST" "$EST/estalloc.c" \
      "$HERE/estalloc_test.c" -lm -lcrypt -lpthread -o "$TMP/etest_asan" 2>"$TMP/e.asan"; then
  "$TMP/etest_asan" >/dev/null 2>"$TMP/err"; rc=$?
  sed 's/^/    /' "$TMP/err"
  if [ "$rc" -ne 0 ]; then echo "FAIL: estalloc_test under ASan (rc=$rc)"; fail=1; fi
else
  echo "SKIP: ASan build unavailable"; tail -3 "$TMP/e.asan"
fi

# Note: estalloc_test.c is compiled once with -include sp_mem_override.h absent
# on purpose (hosts call est_* directly); only the runtime + generated TUs are
# overridden. The nm gate (check_syms.sh) enforces that on the archive side.

if [ "$fail" -eq 0 ]; then echo "multi_ctx estalloc: PASS"; else echo "multi_ctx estalloc: FAIL"; fi
exit $fail
