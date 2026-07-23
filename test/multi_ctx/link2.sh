#!/usr/bin/env bash
# T4-0: two distinct Spinel programs linked into ONE binary, each driven as its
# own instance. This is what a firmware image needs (e.g. kernel + desktop as
# separate Spinel programs in one ELF) and what plain generated TUs could not do
# before: each TU used to export ~24 runtime symbols (exception/proc/signal
# machinery), so two TUs collided at link. Those are now per-instance ctx
# pointers (or shared .a globals), leaving only the entry global per TU.
#
# The test compiles two programs with different entries, links them with a host
# that runs each in its own instance, and checks both outputs -- including
# exceptions, sorting and interpolation, which exercise the routed functions.
#
# Usage: test/multi_ctx/link2.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
MCFLAGS="-DSP_MULTI_CTX -include $LIB/sp_mem_override.h"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0

if [ ! -f "$MC" ]; then echo "FAIL: $MC not built"; exit 1; fi

cat > "$TMP/prog_a.rb" <<'RB'
def a_run
  x = []
  6.times { |i| x << "a#{i}" }
  begin
    raise "boom" if x.length > 100
    x.first(0)[9]          # IndexError path stays untaken; exercises raise plumbing
  rescue => e
    puts "A-rescue:#{e.class}"
  end
  puts "A:#{x.length}:#{[3, 1, 2].sort.inspect}:#{10 / 2}"
end
a_run
RB

cat > "$TMP/prog_b.rb" <<'RB'
def b_run
  h = { name: "spinel", n: 7 }
  puts "B:#{h[:name]}:#{h[:n]}:#{"%04d" % 42}"
end
b_run
RB

"$SP" --no-main --entry a_entry -o "$TMP/a.c" "$TMP/prog_a.rb" >/dev/null 2>&1
"$SP" --no-main --entry b_entry -o "$TMP/b.c" "$TMP/prog_b.rb" >/dev/null 2>&1

# reference outputs from normal standalone compiles
"$SP" -E "$TMP/prog_a.rb" > "$TMP/ref_a.txt" 2>/dev/null
"$SP" -E "$TMP/prog_b.rb" > "$TMP/ref_b.txt" 2>/dev/null

cat > "$TMP/host.c" <<'C'
#include "sp_gc.h"
#include <stdio.h>
int a_entry(void);
int b_entry(void);
int main(void) {
  /* Two programs, two instances, one process. */
  sp_instance_config ca = {0};
  sp_ctx_set_current(sp_instance_create(&ca));
  a_entry();
  sp_instance_config cb = {0};
  sp_ctx_set_current(sp_instance_create(&cb));
  b_entry();
  return 0;
}
C

cc -c -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/a.c" -o "$TMP/a.o" 2>"$TMP/ea" || { echo "FAIL: prog_a compile"; tail "$TMP/ea"; exit 1; }
cc -c -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/b.c" -o "$TMP/b.o" 2>"$TMP/eb" || { echo "FAIL: prog_b compile"; tail "$TMP/eb"; exit 1; }

if cc -O2 -w -DSP_MULTI_CTX -I"$LIB" "$TMP/a.o" "$TMP/b.o" "$TMP/host.c" "$MC" -lm -lcrypt -lpthread -o "$TMP/two" 2>"$TMP/el"; then
  "$TMP/two" > "$TMP/out.txt" 2>&1
  { cat "$TMP/ref_a.txt"; cat "$TMP/ref_b.txt"; } > "$TMP/ref.txt"
  if ! diff -q "$TMP/ref.txt" "$TMP/out.txt" >/dev/null; then
    echo "FAIL: combined output differs from the two standalone references"
    diff "$TMP/ref.txt" "$TMP/out.txt" | head; fail=1
  fi
else
  echo "FAIL: two-program link error (symbol collision?)"; grep -i "multiple definition" "$TMP/el" | head; tail -5 "$TMP/el"; fail=1
fi

if [ "$fail" -eq 0 ]; then echo "multi_ctx link2: PASS (two programs, one binary)"; else echo "multi_ctx link2: FAIL"; fi
exit $fail
