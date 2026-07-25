#!/usr/bin/env bash
# check_stack.sh -- no runtime function may put more than SP_STACK_LIMIT bytes
# on the stack when built for a small-stack port.
#
# Why this is a gate and not a review habit: a hosted program runs on an 8MB
# stack, so a 4KB scratch buffer in a leaf helper is invisible here and fatal on
# a port. An ESP32 FreeRTOS task gets 12-16KB in total and a single generated
# Ruby method frame already costs 2-4KB of it, so one such helper is enough to
# run off the end of the stack (sp_sprintf did exactly that).
#
# The check compiles the runtime in the port configuration -- SP_STACK_SCRATCH_MAX
# small, which is what makes the runtime clamp its scratch buffers and drop the
# helpers whose buffer size is their semantics -- and reads the per-function
# frame sizes GCC writes to .su files. Frames in generated Ruby code are NOT in
# scope: those are a codegen concern, and a port sizes its task stacks for them.
#
# Usage: scripts/check_stack.sh [limit_bytes]
set -u

cd "$(dirname "$0")/.."
LIMIT="${1:-1024}"
BUDGET="${SP_STACK_SCRATCH_MAX:-512}"
OUT="build/stackcheck"
ALLOW="scripts/stack_allow.txt"

rm -rf "$OUT"; mkdir -p "$OUT"

# ILP32 matches the ports we care about (Xtensa / riscv32). Fall back to the
# native width when gcc-multilib is absent -- the buffers this catches are
# oversized either way.
M32="-m32"
echo 'int main(void){return 0;}' > "$OUT/probe.c"
${CC:-cc} $M32 "$OUT/probe.c" -o "$OUT/probe" 2>/dev/null || M32=""
[ -z "$M32" ] && echo "note: no 32-bit toolchain, measuring at native width"

# Port flags. sp_fiber.c / sp_sched.c are excluded from a port build (see
# SP_NO_MMAN); strscan.c needs oniguruma and is not part of the runtime archive.
FLAGS="-c -w -O2 -fkeep-static-functions -fstack-usage $M32
       -DSP_MULTI_CTX -DSP_NO_MMAN -DSP_STACK_SCRATCH_MAX=$BUDGET -Ilib"
fail=0
for f in lib/*.c; do
  case "$(basename "$f")" in sp_fiber.c|sp_sched.c|strscan.c) continue;; esac
  if ! ${CC:-cc} $FLAGS "$f" -o "$OUT/$(basename "$f").o" 2>"$OUT/$(basename "$f").err"; then
    echo "FAIL: $f does not compile in the port configuration"; sed -n '1,5p' "$OUT/$(basename "$f").err"; fail=1
  fi
done

# The bulk of the runtime lives in sp_runtime.h as static functions, which only
# materialise inside a generated TU -- compile one. -fkeep-static-functions keeps
# the ones this program happens not to call, so coverage does not depend on which
# Ruby we feed in.
if [ -x bin/spinel ]; then
  echo 'puts 1' > "$OUT/probe.rb"
  if bin/spinel -c "$OUT/probe.rb" -o "$OUT/probe_gen.c" >/dev/null 2>&1; then
    ${CC:-cc} $FLAGS "$OUT/probe_gen.c" -o "$OUT/probe_gen.o" 2>"$OUT/probe_gen.err" ||
      { echo "FAIL: generated TU does not compile in the port configuration"
        sed -n '1,5p' "$OUT/probe_gen.err"; fail=1; }
  else
    echo "FAIL: could not compile $OUT/probe.rb"; fail=1
  fi
else
  echo "note: bin/spinel not built -- sp_runtime.h statics not covered (run make first)"
fi
mv ./*.su "$OUT/" 2>/dev/null

# Report every frame over the limit whose code lives in lib/ (a .su line is
# "file:line:col:function<TAB>size<TAB>qualifier"). Generated Ruby frames carry
# the .rb path and are skipped.
report=$(cat "$OUT"/*.su 2>/dev/null | awk -F'\t' -v lim="$LIMIT" '
  $2+0 > lim && ($1 ~ /\/lib\// || $1 ~ /^lib\//) {
    n = split($1, p, ":"); print $2 "\t" p[n] "\t" $1
  }' | sort -rn)

allowed=$(grep -vE '^\s*(#|$)' "$ALLOW" 2>/dev/null | awk '{print $1}')
violations=""
while IFS= read -r line; do
  [ -z "$line" ] && continue
  sym=$(printf '%s' "$line" | cut -f2)
  base=${sym%%.*}   # GCC suffixes clones: foo.isra, foo.constprop
  if printf '%s\n' $allowed | grep -qx -e "$sym" -e "$base"; then
    printf 'allowed: %s bytes  %s\n' "$(printf '%s' "$line" | cut -f1)" "$sym"
  else
    violations="$violations$line"$'\n'
  fi
done <<< "$report"

if [ -n "${violations//[$'\n']/}" ]; then
  echo
  echo "stack check FAILED: runtime frames over $LIMIT bytes at SP_STACK_SCRATCH_MAX=$BUDGET"
  printf '%s' "$violations" | while IFS= read -r line; do
    [ -z "$line" ] && continue
    printf '  %8s bytes  %s\n' "$(printf '%s' "$line" | cut -f1)" "$(printf '%s' "$line" | cut -f3)"
  done
  echo
  echo "Fix by clamping the buffer with SP_SCRATCH() when a smaller one is"
  echo "transparent, or by omitting the helper on small-stack ports when its"
  echo "buffer size is its semantics (see SP_STACK_SCRATCH_MAX in lib/sp_types.h)."
  echo "If neither applies, add the symbol to $ALLOW with the reason."
  exit 1
fi

[ "$fail" -ne 0 ] && exit 1
echo "stack check PASS: no runtime frame over $LIMIT bytes at SP_STACK_SCRATCH_MAX=$BUDGET"
exit 0
