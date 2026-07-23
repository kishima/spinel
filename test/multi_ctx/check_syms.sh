#!/usr/bin/env bash
# nm gate for the multi-instance build: prove the allocation override actually
# took hold. Under -DSP_MULTI_CTX every libc malloc/calloc/realloc/free/strdup
# must be remapped (by lib/sp_mem_override.h, force-included) to the per-instance
# sp_mem_* wrappers, so NO object in libspinel_rt_mc.a -- nor a generated program
# TU built the same way -- may carry an undefined reference to those libc names.
# The one exception is sp_ctx.o, which DEFINES the wrappers and legitimately
# reaches libc.
#
# This catches a whole class of build-system regressions: a TU that loses the
# -include (new source, reordered rule, a package built without MC_DEF) silently
# goes back to the shared libc heap and breaks pool isolation. nm sees it here.
#
# Usage: test/multi_ctx/check_syms.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
OVERRIDE="$LIB/sp_mem_override.h"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

# Forbidden undefined references (allocation entry points). reallocarray /
# posix_memalign / aligned_alloc are not used today but are guarded so a future
# introduction cannot slip past the override unnoticed.
BAD='^(malloc|calloc|realloc|free|strdup|reallocarray|posix_memalign|aligned_alloc)$'

check_obj() { # <object> <allow-libc: yes|no> <label>
  local obj="$1" allow="$2" label="$3"
  local hits
  hits="$(nm -u "$obj" 2>/dev/null | awk '{print $NF}' | grep -E "$BAD" | sort -u | tr '\n' ' ')"
  if [ -n "$hits" ] && [ "$allow" != yes ]; then
    echo "FAIL: $label references libc allocation: $hits"
    fail=1
  fi
}

if [ ! -f "$MC" ]; then echo "FAIL: $MC not built"; exit 1; fi

# 1. Every archive member except sp_ctx.o must be clean.
( cd "$TMP" && ar x "$MC" )
for o in "$TMP"/*.o; do
  bn="$(basename "$o")"
  if [ "$bn" = sp_ctx.o ]; then
    # sp_ctx.o is the implementation site; it SHOULD reach libc. Sanity-check
    # that it does (a clean sp_ctx.o would mean the wrappers vanished).
    if ! nm -u "$o" 2>/dev/null | awk '{print $NF}' | grep -qE "$BAD"; then
      echo "FAIL: sp_ctx.o has no libc allocation reference -- wrappers missing?"; fail=1
    fi
    continue
  fi
  check_obj "$o" no "archive:$bn"
done

# 2. A generated program TU, compiled the multi-ctx way, must also be clean.
#    The program exercises many allocation paths (strings, arrays, hashes,
#    symbols, regex, JSON) so a broad slice of the sp_runtime.h inlines is
#    instantiated in the object under test.
cat > "$TMP/rich.rb" <<'RB'
def sp_prog_entry_ruby
  a = []
  50.times { |i| a << "item-#{i}-#{i * i}" }
  h = {}
  a.each_with_index { |s, i| h[s] = i }
  m = "hello world 123" =~ /(\w+)\s+(\w+)/ ? [$1, $2] : []
  puts (a.length + h.size + m.length)
end
sp_prog_entry_ruby
RB
"$SP" --no-main --entry sp_prog_entry -o "$TMP/rich.c" "$TMP/rich.rb" >/dev/null 2>&1
if cc -c -O2 -w -DSP_MULTI_CTX -include "$OVERRIDE" -I"$LIB" -I"$LIB/regexp" \
      "$TMP/rich.c" -o "$TMP/rich.o" 2>"$TMP/cc.err"; then
  check_obj "$TMP/rich.o" no "generated:rich.o"
else
  echo "FAIL: generated program did not compile with the override"; tail -5 "$TMP/cc.err"; fail=1
fi

if [ "$fail" -eq 0 ]; then echo "check-mc-syms: PASS (only sp_ctx.o reaches libc)"; else echo "check-mc-syms: FAIL"; fi
exit $fail
