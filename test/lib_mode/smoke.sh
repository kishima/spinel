#!/usr/bin/env bash
# Smoke test for library mode (--no-main / --entry) and --inject.
#
# The standard test harness compiles each .rb to a runnable binary and diffs
# against a CRuby oracle; a --no-main unit is not standalone-runnable, so it is
# verified here instead:
#   1. `--no-main --entry NAME -c` emits `int NAME(void)` and no `int main(`.
#   2. Linked against a host stub `int main(){return NAME();}`, the program's
#      output matches a normal `-E` compile byte-for-byte.
#   3. `--inject FILE` splices a raw C file (here, the host main) into the same
#      translation unit, so no separate stub object is needed.
#
# Usage: test/lib_mode/smoke.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
RT="$LIB/libspinel_rt.a"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

cat > "$TMP/prog.rb" <<'RB'
BEGIN { puts "begin" }
END { puts "end-1" }
END { puts "end-2" }
class Greeter
  def initialize(n); @n = n; end
  def hi; "hi #{@n} => #{2 * 3}"; end
end
puts Greeter.new("spinel").hi
puts "body"
RB

# reference output from a normal compile+run
"$SP" -E "$TMP/prog.rb" > "$TMP/ref.txt" 2>/dev/null

# 1. --no-main --entry emits the entry and no main
"$SP" --no-main --entry sp_prog_entry -o "$TMP/prog.c" "$TMP/prog.rb" >/dev/null 2>&1
if ! grep -q "int sp_prog_entry(void)" "$TMP/prog.c"; then echo "FAIL: entry fn not emitted"; fail=1; fi
if grep -q "int main(" "$TMP/prog.c"; then echo "FAIL: main() emitted under --no-main"; fail=1; fi

# 2. link with a host stub and compare output
printf 'int sp_prog_entry(void);\nint main(void){return sp_prog_entry();}\n' > "$TMP/host.c"
if cc -O2 -w -I"$LIB" "$TMP/prog.c" "$TMP/host.c" "$RT" -lm -lcrypt -o "$TMP/prog_bin" 2>"$TMP/link1.err"; then
  "$TMP/prog_bin" > "$TMP/nomain.txt" 2>&1
  if ! diff -q "$TMP/ref.txt" "$TMP/nomain.txt" >/dev/null; then
    echo "FAIL: --no-main output differs from -E"; diff "$TMP/ref.txt" "$TMP/nomain.txt" | head; fail=1
  fi
else
  echo "FAIL: --no-main link error"; tail -5 "$TMP/link1.err"; fail=1
fi

# 3. --inject splices the host main into the same unit
"$SP" --no-main --entry sp_prog_entry --inject "$TMP/host.c" -o "$TMP/prog_inj.c" "$TMP/prog.rb" >/dev/null 2>&1
if ! grep -q "inject:" "$TMP/prog_inj.c"; then echo "FAIL: inject marker missing"; fail=1; fi
if cc -O2 -w -I"$LIB" "$TMP/prog_inj.c" "$RT" -lm -lcrypt -o "$TMP/prog_inj_bin" 2>"$TMP/link2.err"; then
  "$TMP/prog_inj_bin" > "$TMP/inject.txt" 2>&1
  if ! diff -q "$TMP/ref.txt" "$TMP/inject.txt" >/dev/null; then
    echo "FAIL: --inject output differs from -E"; diff "$TMP/ref.txt" "$TMP/inject.txt" | head; fail=1
  fi
else
  echo "FAIL: --inject link error"; tail -5 "$TMP/link2.err"; fail=1
fi

if [ "$fail" -eq 0 ]; then echo "lib_mode smoke: PASS"; else echo "lib_mode smoke: FAIL"; fi
exit $fail
