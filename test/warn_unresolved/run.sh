#!/bin/sh
# SPINEL_WARN_UNRESOLVED reports only calls the generated program makes:
# poly_predicate.rb compiles without a report (a `?` method reached through
# the poly dispatch's trial of a builtin arm), poly_missing.rb keeps its one.
SPINEL=${SPINEL:-bin/spinel}
dir=$(dirname "$0")
tmp=$(mktemp -d /tmp/spinel-warnunres.XXXXXX)
trap 'rm -rf "$tmp"' EXIT
ok=1
SPINEL_WARN_UNRESOLVED=1 "$SPINEL" "$dir/poly_predicate.rb" -c -o "$tmp/a.c" >"$tmp/a.out" 2>&1 || { echo "warn-unresolved-test: FAIL (poly_predicate.rb did not compile)"; cat "$tmp/a.out"; ok=0; }
if grep -q "warning: unresolved call" "$tmp/a.out"; then
  echo "warn-unresolved-test: FAIL (poly_predicate.rb reported a call it does not make)"
  grep "warning: unresolved call" "$tmp/a.out"; ok=0
fi
SPINEL_WARN_UNRESOLVED=1 "$SPINEL" "$dir/poly_missing.rb" -c -o "$tmp/b.c" >"$tmp/b.out" 2>&1
if ! grep -q "warning: unresolved call 'bar?' on poly receiver" "$tmp/b.out"; then
  echo "warn-unresolved-test: FAIL (poly_missing.rb lost its report)"; cat "$tmp/b.out"; ok=0
fi
[ $ok = 1 ] && echo "warn-unresolved-test: ok"
[ $ok = 1 ]
