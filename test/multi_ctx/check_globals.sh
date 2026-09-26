#!/usr/bin/env bash
# Leak gate for the multi-instance build: every data/bss symbol left in
# libspinel_rt_mc.a must be listed, with its reason, in globals_allow.txt.
#
# SP_MULTI_CTX moves runtime state into the per-instance sp_ctx by name. State
# upstream adds later is not a merge conflict -- it compiles, links and stays
# process-wide, shared by every instance. This lists what is still global and
# fails on anything nobody has classified.
#
# Usage: test/multi_ctx/check_globals.sh   (from the fork root; LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
ALLOW="$ROOT/test/multi_ctx/globals_allow.txt"
[ -f "$MC" ] || { echo "FAIL: $MC not built"; exit 1; }

# object symbol (function-local statics lose their .N suffix)
syms="$(nm "$MC" 2>/dev/null | awk '
  /^$/ { next }
  /:$/ { obj = substr($1, 1, length($1) - 1); next }
  NF == 3 && $2 ~ /^[BbDdVSs]$/ { s = $3; sub(/\.[0-9]+$/, "", s); print obj, s }' | sort -u)"

allowed() { # <obj> <sym>
  while read -r o p _; do
    [ "$o" = "$1" ] || continue
    case "$2" in $p) return 0;; esac
  done < <(grep -vE '^[[:space:]]*(#|$)' "$ALLOW")
  return 1
}

leaks=""; n=0
while read -r obj sym; do
  [ -z "$obj" ] && continue
  n=$((n + 1))
  allowed "$obj" "$sym" || leaks="$leaks  $obj $sym"$'\n'
done <<< "$syms"

if [ -n "$leaks" ]; then
  echo "check-mc-globals: FAIL -- process-wide state not in sp_ctx and not classified:"
  printf '%s' "$leaks"
  echo "Move it into sp_ctx (lib/sp_ctx.h), or list it in test/multi_ctx/globals_allow.txt with why it may be shared."
  exit 1
fi
echo "check-mc-globals: PASS ($n process-wide symbols, all classified)"
