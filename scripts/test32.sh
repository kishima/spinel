#!/usr/bin/env bash
#
# test32.sh -- run the feature-test suite with the runtime and every generated
# test TU compiled at -m32 (ILP32). This is the 32-bit gate: a portable proxy
# for the eventual Xtensa/ESP32 target, whose defining trait is a 32-bit
# mrb_int / pointer. It catches width-dependent codegen and runtime regressions
# (e.g. __int128 fallbacks, pointer/size_t truncation, sign extension) long
# before a cross-toolchain build is involved.
#
# It is deliberately self-contained and isolated: it builds a 32-bit runtime
# archive into build/m32/ and NEVER touches the 64-bit build/ tree or the
# host-native lib/libspinel_rt.a that `make test` uses. The compiler itself
# (bin/spinel) stays host-native -- it only emits architecture-independent C.
#
# Requires a multilib C toolchain (Debian/Ubuntu: `gcc-multilib`).
#
# Usage: scripts/test32.sh   (from the repo root; expects bin/spinel built)
#   env: CC (default: from `make` config, else cc), JOBS (default: nproc),
#        REF_RUBY (unused -- all tests ship a .expected oracle).

set -u
cd "$(dirname "$0")/.."
ROOT=$(pwd)

SPINEL=${SPINEL:-bin/spinel}
CC=${CC:-cc}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
M32=-m32
# Same warning posture as the 64-bit harness, minus -Werror: the gate is about
# runtime behaviour (the .expected diff), so a benign 32-bit warning (a format
# width mismatch in test code, say) must not mask a real miscompile as an ERR.
# A genuine compile failure still surfaces as ERR.
BASE_CFLAGS="-O2 -Wno-all -Wno-unknown-warning-option -Wno-alloc-size-larger-than -Wno-format-truncation"
SEC_FLAGS="-ffunction-sections -fdata-sections"
LDFLAGS="-lm"
# String#crypt pulls libc crypt(3) (glibc: libcrypt). A multilib host often has
# only the 64-bit libcrypt, and the target this gate stands in for (ESP32) has
# no crypt(3) at all. Probe for a linkable 32-bit libcrypt: if present, link it
# and run the crypt tests; if not, drop -lcrypt and skip the crypt-dependent
# tests (they exercise a libc feature, not 32-bit codegen).
# crypt(3) is referenced from a core runtime member (sp_str.c: sp_str_crypt), so
# every linked program needs the symbol resolved -- not only the crypt tests.
CRYPT_OK=0
CRYPT_STUB=""
_cp=/tmp/.sp_m32_crypt.$$.c; _cb=/tmp/.sp_m32_crypt.$$
printf 'int main(void){return 0;}\n' > "$_cp"
if $CC $M32 "$_cp" -lcrypt -o "$_cb" 2>/dev/null; then CRYPT_OK=1; LDFLAGS="$LDFLAGS -lcrypt"; fi
rm -f "$_cp" "$_cb"

if [ ! -x "$SPINEL" ] && [ ! -f "$SPINEL" ]; then
  echo "test32: $SPINEL not found -- run 'make' first" >&2
  exit 1
fi

# ---- 0. multilib preflight -------------------------------------------------
probe_c=/tmp/.sp_m32_probe.$$.c
probe_bin=/tmp/.sp_m32_probe.$$
printf 'int main(void){return 0;}\n' > "$probe_c"
if ! $CC $M32 "$probe_c" -o "$probe_bin" 2>/dev/null; then
  echo "test32: this toolchain cannot compile -m32 objects." >&2
  echo "        install a multilib toolchain (Debian/Ubuntu: apt install gcc-multilib)." >&2
  rm -f "$probe_c" "$probe_bin"
  exit 1
fi
rm -f "$probe_c" "$probe_bin"

BDIR=build/m32
mkdir -p "$BDIR/regexp"

RT_MEMBERS="sp_bigint sp_crypto sp_pack sp_time sp_core sp_net sp_system sp_ctx sp_gc sp_alloc sp_marshal sp_format sp_string sp_inspect sp_array sp_str sp_re sp_random sp_fiber sp_sched sp_io sp_cold"
RE_SRCS="re_compile re_exec re_utf8"

# ---- 1. 32-bit runtime archive (built once) --------------------------------
echo "test32: building 32-bit runtime archive ($BDIR)..."
compile_fail=0
for m in $RT_MEMBERS; do
  obj="$BDIR/$m.o"
  if [ ! -f "$obj" ] || [ "lib/$m.c" -nt "$obj" ]; then
    if ! $CC $M32 -c -O2 -Wno-all $SEC_FLAGS -Ilib -Ilib/regexp "lib/$m.c" -o "$obj"; then
      echo "test32: FAILED to compile lib/$m.c at -m32" >&2
      compile_fail=1
    fi
  fi
done
for r in $RE_SRCS; do
  obj="$BDIR/regexp/$r.o"
  if [ ! -f "$obj" ] || [ "lib/regexp/$r.c" -nt "$obj" ]; then
    if ! $CC $M32 -c -O2 $SEC_FLAGS -Ilib/regexp "lib/regexp/$r.c" -o "$obj"; then
      echo "test32: FAILED to compile lib/regexp/$r.c at -m32" >&2
      compile_fail=1
    fi
  fi
done
[ $compile_fail -eq 0 ] || { echo "test32: runtime did not build at -m32; aborting." >&2; exit 1; }

RT_LIB="$BDIR/libspinel_rt.a"
rm -f "$RT_LIB"
# Explicit member list, NOT a glob: build/m32/ also holds non-runtime objects
# (bundled packages, the crypt stub) which must not be archived, or they collide
# with the copies passed separately on the link line.
RT_OBJS=""
for m in $RT_MEMBERS; do RT_OBJS="$RT_OBJS $BDIR/$m.o"; done
for r in $RE_SRCS; do RT_OBJS="$RT_OBJS $BDIR/regexp/$r.o"; done
ar rcs "$RT_LIB" $RT_OBJS

# ---- 2. 32-bit bundled package objects -------------------------------------
declare -A PKG_INC=( [json]=packages/json [stringio]=packages/stringio [strscan]=packages/strscan [base64]=packages/base64 )
BUNDLED_OBJS=""
for p in json stringio strscan base64; do
  src="${PKG_INC[$p]}/sp_$p.c"
  obj="$BDIR/sp_$p.o"
  if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
    if ! $CC $M32 -c -O2 -Wno-all $SEC_FLAGS -Ilib -I"${PKG_INC[$p]}" "$src" -o "$obj"; then
      echo "test32: FAILED to compile $src at -m32" >&2
      exit 1
    fi
  fi
  BUNDLED_OBJS="$BUNDLED_OBJS $obj"
done

# ---- 3. per-test compile + run + diff --------------------------------------
# Collect the test list (raise mode: exclude the promote_* tests, which only
# have defined output under --int-overflow=promote).
TESTS=$(ls test/*.rb | grep -v '/promote_' | sort)
# Width-dependent tests whose 64-bit oracle cannot hold on ILP32 (and a handful
# of documented 32-bit fork bugs). See test/skip32.txt for the annotated list.
SKIP32=test/skip32.txt
if [ -f "$SKIP32" ]; then
  skipset=$(sed 's/#.*//' "$SKIP32" | awk 'NF{print "test/"$1".rb"}' | sort -u)
  nskip=$(printf '%s\n' "$skipset" | grep -c . || true)
  echo "test32: skipping $nskip width-dependent test(s) listed in $SKIP32."
  TESTS=$(comm -23 <(printf '%s\n' "$TESTS") <(printf '%s\n' "$skipset"))
fi
if [ "$CRYPT_OK" -eq 0 ]; then
  # No linkable 32-bit libcrypt. Two consequences:
  #  (1) crypt(3) is referenced by the core runtime (sp_str_crypt) and so must
  #      resolve for EVERY program -- provide a trap stub, linked into each test.
  #  (2) the crypt tests would actually call it, so skip them (libc feature, not
  #      32-bit codegen); with them gone the stub is never invoked.
  stub_c="$BDIR/crypt_stub.c"
  cat > "$stub_c" <<'EOF'
/* -m32 gate stub: resolves crypt(3) when no 32-bit libcrypt is installed. The
   crypt-dependent tests are skipped, so this is a link-only placeholder. */
#include <stddef.h>
char *crypt(const char *key, const char *salt) { (void)key; (void)salt; return NULL; }
EOF
  CRYPT_STUB="$BDIR/crypt_stub.o"
  $CC $M32 -c -O2 "$stub_c" -o "$CRYPT_STUB" || { echo "test32: failed to build crypt stub" >&2; exit 1; }
  CRYPT_TESTS=$(grep -l 'crypt' test/*.rb 2>/dev/null | sort)
  ncrypt=$(printf '%s\n' "$CRYPT_TESTS" | grep -c . || true)
  echo "test32: no 32-bit libcrypt; using a crypt() trap stub and skipping $ncrypt crypt(3) test(s)."
  TESTS=$(comm -23 <(printf '%s\n' "$TESTS") <(printf '%s\n' "$CRYPT_TESTS"))
fi
NTESTS=$(printf '%s\n' "$TESTS" | grep -c . || true)
echo "test32: running $NTESTS tests at -m32 with $JOBS jobs..."

RESDIR="$BDIR/results"
rm -rf "$RESDIR"; mkdir -p "$RESDIR"

export ROOT SPINEL CC M32 BASE_CFLAGS SEC_FLAGS LDFLAGS RT_LIB BUNDLED_OBJS RESDIR CRYPT_STUB
run_one() {
  local rb="$1"
  local name; name=$(basename "$rb" .rb)
  local stamp="$RESDIR/$name.ok"
  local tmp; tmp=$(mktemp -d /tmp/sp32.XXXXXX)
  local cfile="$tmp/t.c" bin="$tmp/t"
  local args="" stdinf=/dev/null
  [ -f "$rb.args" ] && args=$(cat "$rb.args")
  [ -f "$rb.stdin" ] && stdinf="$rb.stdin"

  if ! $SPINEL "$rb" -c --no-line-map -o "$cfile" 2>/dev/null; then
    echo "ERR-codegen" > "$stamp"; rm -rf "$tmp"; return
  fi
  # shellcheck disable=SC2086
  if ! $CC $M32 $BASE_CFLAGS $SEC_FLAGS -Ilib "$cfile" $BUNDLED_OBJS "$RT_LIB" $CRYPT_STUB $LDFLAGS -o "$bin" 2>"$tmp/cc.err"; then
    echo "ERR-compile" > "$stamp"; cp "$tmp/cc.err" "$stamp.log" 2>/dev/null; rm -rf "$tmp"; return
  fi

  local exp="$tmp/exp" act="$tmp/act" experr="$tmp/experr" acterr="$tmp/acterr"
  LC_ALL=C sed 's/\r$//' "$rb.expected" > "$exp"
  # 30s (vs the 64-bit harness's 10s): -m32 binaries run slower and the suite
  # runs heavily parallel, so a 10s budget spuriously killed slow-but-correct
  # tests (empty output -> false FAIL) under load.
  # shellcheck disable=SC2086
  timeout 30 "$bin" $args < "$stdinf" > "$tmp/act.raw" 2> "$tmp/acterr.raw"
  LC_ALL=C sed 's/\r$//' "$tmp/act.raw" > "$act"
  LC_ALL=C sed 's/\r$//' "$tmp/acterr.raw" > "$acterr"
  if [ -f "$rb.err.expected" ]; then
    LC_ALL=C sed 's/\r$//' "$rb.err.expected" > "$experr"
  else
    : > "$experr"
  fi

  if cmp -s "$exp" "$act" && cmp -s "$experr" "$acterr"; then
    echo PASS > "$stamp"
  else
    echo FAIL > "$stamp"
    { echo "=== stdout diff (expected vs actual) ==="; diff -u "$exp" "$act" || true;
      echo "=== stderr diff (expected vs actual) ==="; diff -u "$experr" "$acterr" || true; } > "$stamp.diff" 2>&1
  fi
  rm -rf "$tmp"
}
export -f run_one

printf '%s\n' "$TESTS" | xargs -P "$JOBS" -I{} bash -c 'run_one "$@"' _ {}

# ---- 4. report -------------------------------------------------------------
pass=$(grep -rl '^PASS' "$RESDIR"/*.ok 2>/dev/null | wc -l | tr -d ' ')
fail=$(grep -rl '^FAIL' "$RESDIR"/*.ok 2>/dev/null | wc -l | tr -d ' ')
err=$(grep -rl '^ERR' "$RESDIR"/*.ok 2>/dev/null | wc -l | tr -d ' ')
echo
for f in "$RESDIR"/*.ok; do
  st=$(cat "$f"); bn=$(basename "$f" .ok)
  case "$st" in
    FAIL) echo "FAIL: $bn"; head -30 "$f.diff" 2>/dev/null;;
    ERR*) echo "$st: $bn"; [ -f "$f.log" ] && head -6 "$f.log";;
  esac
done
echo "test32: $pass pass, $fail fail, $err error (of $NTESTS)"
if [ "$fail" -ne 0 ] || [ "$err" -ne 0 ]; then exit 1; fi
