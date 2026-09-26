#!/usr/bin/env bash
# Several --ext-init programs in ONE binary under SP_MULTI_CTX, each driven as
# its own instance, concurrently.
#
# An ext program (docs/internals/ext-design.md) is a library: the host calls
# its init once, then its typed entries as often as it likes. Upstream links
# one such program per image -- its runtime hooks (sp_sym_to_s, sp_class_to_s,
# sp_sym_intern(_n), and the exception/proc machinery) are exported for the
# host, so a second program collides at link. Under SP_MULTI_CTX those stay
# private to each program and the runtime reaches the current instance's copy
# through sp_ctx, the same path link2.sh checks for --no-main programs.
#
# Three programs with deliberately different symbol and class tables (a
# symbol id means a different name in each, so a hook reached through the
# wrong program prints the wrong word) are compiled, linked into one host and
# checked four ways:
#   1. nm: each program TU exports only its init, its try helper and entries.
#   2. seq: one thread runs the three programs one after another, each in its
#      own instance; the output must equal CRuby running the same call
#      sequence (ext3_driver.rb).
#   3. par: three threads run the three programs at the same time; the output
#      of every program must equal the sequential run. Repeated with the
#      collector forced on every allocation burst (SPINEL_GC_STRESS=1).
#   4. restart: each program's instance is destroyed and a new one created
#      in its place; init must start the program from its initial state (a
#      module ivar that counts calls reads 1 again, a cached object is nil
#      again) instead of reaching into the destroyed heap.
#
# Usage: test/multi_ctx/ext3.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
MCFLAGS="-DSP_MULTI_CTX -include $LIB/sp_mem_override.h"
ROUNDS="${ROUNDS:-300}"
TMP="$(mktemp -d)"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$TMP"' EXIT
fail=0

if [ ! -f "$MC" ]; then echo "FAIL: $MC not built (make lib/libspinel_rt_mc.a)"; exit 1; fi
if ! command -v ruby >/dev/null 2>&1; then echo "SKIP: ext3 needs ruby for the reference"; exit 0; fi

# --- program 1: strings, a string-keyed hash, a raise ---------------------
cat > "$TMP/k_str.rb" <<'RB'
module StrK
  def self.words(text, i)
    @calls = (@calls || 0) + 1
    raise ArgumentError, "empty text at #{i}" if text.empty?
    h = {}
    text.split(" ").each { |w| h[w] = (h[w] || 0) + 1 }
    out = h.keys.sort.map { |k| "#{k}=#{h[k]}" }.join(",")
    "#{i}:#{@calls}:#{out}:#{text.bytesize}"
  end

  def self.calls
    @calls || 0
  end
end

if __FILE__ == $0
  puts StrK.words("a b a", 0)
  puts StrK.calls
end
RB

# --- program 2: integer arrays, a Float result, a cached object ------------
cat > "$TMP/k_num.rb" <<'RB'
class Sieve
  attr_reader :n, :primes
  def initialize(n)
    @n = n
    flags = Array.new(n + 1, true)
    flags[0] = false
    flags[1] = false
    i = 2
    while i * i <= n
      if flags[i]
        j = i * i
        while j <= n
          flags[j] = false
          j += i
        end
      end
      i += 1
    end
    @primes = []
    k = 0
    while k <= n
      @primes << k if flags[k]
      k += 1
    end
  end
end

module NumK
  def self.primes(n)
    s = @sieve
    if s.nil? || s.n != n
      s = Sieve.new(n)
      @sieve = s
    end
    sum = 0
    s.primes.each { |p| sum += p }
    s.primes.length * 100000 + sum % 100000
  end

  def self.mean_sqrt(n)
    t = 0.0
    k = 1
    while k <= n
      t += Math.sqrt(k.to_f)
      k += 1
    end
    t / n
  end

  def self.cached
    @sieve.nil? ? -1 : @sieve.n
  end
end

if __FILE__ == $0
  p NumK.primes(100)
  p NumK.mean_sqrt(10)
  p NumK.cached
end
RB

# --- program 3: user classes, symbols, inspect ------------------------------
cat > "$TMP/k_obj.rb" <<'RB'
class Pt
  attr_reader :x, :y, :tag
  def initialize(x, y, tag)
    @x = x
    @y = y
    @tag = tag
  end

  def to_s
    "#{@tag}(#{@x},#{@y})"
  end
end

module ObjK
  TAGS = [:north, :east, :south, :west, :center]

  def self.shapes(n)
    @calls = (@calls || 0) + 1
    pts = []
    i = 0
    while i < n
      pts << Pt.new((i * 7) % 11, (i * 3) % 5, TAGS[i % 5])
      i += 1
    end
    pts = pts.sort { |a, b| a.x == b.x ? a.y <=> b.y : a.x <=> b.x }
    h = {}
    pts.each { |p| h[p.tag] = (h[p.tag] || 0) + 1 }
    # the hash is rendered by hand: Hash#inspect changed its format in
    # Ruby 3.4, and the reference may come from an older CRuby
    counts = h.keys.map { |k| "#{k}=#{h[k]}" }.join(",")
    "#{@calls} #{pts.first} #{pts.last} #{counts} #{pts.first.class} #{TAGS.last.inspect}"
  end

  def self.calls
    @calls || 0
  end
end

if __FILE__ == $0
  puts ObjK.shapes(7)
  p ObjK.calls
end
RB

gen() {  # gen <name> <init> <entries>
  "$SP" "$TMP/$1.rb" -c --no-line-map --ext-init "$2" --ext-entry "$3" -o "$TMP/$1.c" >"$TMP/gen_$1.log" 2>&1 \
    || { echo "FAIL: $1 generation"; tail -5 "$TMP/gen_$1.log"; return 1; }
  cc -c -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/$1.c" -o "$TMP/$1.o" 2>"$TMP/cc_$1.log" \
    || { echo "FAIL: $1 compile"; tail -5 "$TMP/cc_$1.log"; return 1; }
}
gen k_str Init_k_str StrK.words,StrK.calls || exit 1
gen k_num Init_k_num NumK.primes,NumK.mean_sqrt,NumK.cached || exit 1
gen k_obj Init_k_obj ObjK.shapes,ObjK.calls || exit 1

# 1. nm: nothing but the program's own names leaves a program TU
for k in k_str k_num k_obj; do
  extra="$(nm "$TMP/$k.o" | awk 'NF==3 && $2 ~ /^[TDBRCVW]$/ {print $3}' \
           | grep -v -e "^Init_$k\$" -e "^Init_${k}_try\$" -e '^sp_[A-Za-z0-9]*K_s_')"
  if [ -n "$extra" ]; then echo "FAIL: $k.o exports more than its init and entries:"; echo "$extra" | head; fail=1; fi
done

# --- host ---------------------------------------------------------------------
# All three headers in one TU (their include guards are per program). Each
# program runs on its own instance; its lines are collected in a buffer and
# printed after the threads join, so the order of the output never depends on
# scheduling.
cat > "$TMP/host.c" <<'C'
#include "k_str.h"
#include "k_num.h"
#include "k_obj.h"
#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>

typedef struct { char *p; size_t n, cap; } obuf;
static void outf(obuf *o, const char *fmt, ...) {
  char tmp[512]; va_list ap; va_start(ap, fmt);
  int k = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
  if (k < 0) return;
  if ((size_t)k >= sizeof tmp) k = (int)sizeof tmp - 1;
  if (o->n + (size_t)k + 1 > o->cap) {
    o->cap = (o->n + (size_t)k + 1) * 2;
    o->p = (char *)realloc(o->p, o->cap);
  }
  memcpy(o->p + o->n, tmp, (size_t)k); o->n += (size_t)k; o->p[o->n] = 0;
}

typedef struct { const char *s; sp_int i, n; const char *rs; sp_int ri; sp_float rf; } call_t;
static void c_words(void *p) { call_t *c = p; c->rs = sp_StrK_s_words(c->s, c->i); }
static void c_scalls(void *p) { call_t *c = p; c->ri = sp_StrK_s_calls(); }
static void c_primes(void *p) { call_t *c = p; c->ri = sp_NumK_s_primes(c->n); }
static void c_mean(void *p) { call_t *c = p; c->rf = sp_NumK_s_mean_sqrt(c->n); }
static void c_cached(void *p) { call_t *c = p; c->ri = sp_NumK_s_cached(); }
static void c_shapes(void *p) { call_t *c = p; c->rs = sp_ObjK_s_shapes(c->n); }
static void c_ocalls(void *p) { call_t *c = p; c->ri = sp_ObjK_s_calls(); }

typedef int (*try_fn)(void (*)(void *), void *, const char **, const char **);
static void call(obuf *o, try_fn t, void (*fn)(void *), call_t *c) {
  const char *cls = 0, *msg = 0;
  if (t(fn, c, &cls, &msg)) outf(o, "raise %s: %s\n", cls, msg);
}

static int rounds;

static sp_ctx *fresh(void) {
  sp_instance_config cfg = {0};
  sp_ctx *c = sp_instance_create(&cfg);
  sp_ctx_set_current(c);
  return c;
}

static void run_str(obuf *o) {
  sp_ctx *x = fresh(); Init_k_str();
  for (int i = 0; i < rounds; i++) {
    char text[96];
    if (i % 50 == 49) text[0] = 0;
    else snprintf(text, sizeof text, "w%d w%d w%d w%d v%d", i % 7, i % 3, i % 5, i % 3, i % 2);
    call_t c = { sp_str_from_bytes(text, strlen(text)), i, 0, 0, 0, 0 };
    call(o, Init_k_str_try, c_words, &c);
    if (c.rs) outf(o, "%s\n", c.rs);
  }
  call_t c = {0}; call(o, Init_k_str_try, c_scalls, &c); outf(o, "calls %ld\n", (long)c.ri);
  sp_instance_destroy(x);
  x = fresh(); Init_k_str();
  c = (call_t){0}; call(o, Init_k_str_try, c_scalls, &c); outf(o, "restart calls %ld\n", (long)c.ri);
  sp_instance_destroy(x);
}

static void run_num(obuf *o) {
  sp_ctx *x = fresh(); Init_k_num();
  for (int i = 0; i < rounds; i++) {
    call_t c = {0};
    c.n = 50 + (i % 4) * 25;
    call(o, Init_k_num_try, c_primes, &c); outf(o, "%ld", (long)c.ri);
    c.n = 1 + i % 9;
    call(o, Init_k_num_try, c_mean, &c); outf(o, " %.9f\n", c.rf);
  }
  call_t c = {0}; call(o, Init_k_num_try, c_cached, &c); outf(o, "cached %ld\n", (long)c.ri);
  sp_instance_destroy(x);
  x = fresh(); Init_k_num();
  c = (call_t){0}; call(o, Init_k_num_try, c_cached, &c); outf(o, "restart cached %ld\n", (long)c.ri);
  sp_instance_destroy(x);
}

static void run_obj(obuf *o) {
  sp_ctx *x = fresh(); Init_k_obj();
  for (int i = 0; i < rounds; i++) {
    call_t c = {0}; c.n = 3 + i % 13;
    call(o, Init_k_obj_try, c_shapes, &c); outf(o, "%s\n", c.rs);
  }
  sp_instance_destroy(x);
  x = fresh(); Init_k_obj();
  call_t c = {0}; call(o, Init_k_obj_try, c_ocalls, &c); outf(o, "restart calls %ld\n", (long)c.ri);
  sp_instance_destroy(x);
}

static obuf outs[3];
static void *t_str(void *a) { (void)a; run_str(&outs[0]); return NULL; }
static void *t_num(void *a) { (void)a; run_num(&outs[1]); return NULL; }
static void *t_obj(void *a) { (void)a; run_obj(&outs[2]); return NULL; }

int main(int argc, char **argv) {
  rounds = argc > 2 ? atoi(argv[2]) : 100;
  if (argc > 1 && strcmp(argv[1], "par") == 0) {
    pthread_t t[3];
    pthread_create(&t[0], NULL, t_str, NULL);
    pthread_create(&t[1], NULL, t_num, NULL);
    pthread_create(&t[2], NULL, t_obj, NULL);
    for (int i = 0; i < 3; i++) pthread_join(t[i], NULL);
  } else {
    run_str(&outs[0]); run_num(&outs[1]); run_obj(&outs[2]);
  }
  for (int i = 0; i < 3; i++) { printf("== program %d\n", i); fputs(outs[i].p ? outs[i].p : "", stdout); }
  return 0;
}
C

# The same call sequence under CRuby (the programs' `if __FILE__ == $0`
# blocks do not run: they are required, not executed).
cat > "$TMP/driver.rb" <<'RB'
rounds = ARGV[0].to_i
dir = ARGV[1]
load File.join(dir, "k_str.rb")
load File.join(dir, "k_num.rb")
load File.join(dir, "k_obj.rb")
out = [+"", +"", +""]
rounds.times do |i|
  text = i % 50 == 49 ? "" : "w#{i % 7} w#{i % 3} w#{i % 5} w#{i % 3} v#{i % 2}"
  begin
    out[0] << StrK.words(text, i) << "\n"
  rescue => e
    out[0] << "raise #{e.class}: #{e.message}\n"
  end
end
out[0] << "calls #{StrK.calls}\n"
StrK.instance_variable_set(:@calls, nil)
out[0] << "restart calls #{StrK.calls}\n"
rounds.times do |i|
  out[1] << NumK.primes(50 + (i % 4) * 25).to_s
  out[1] << format(" %.9f\n", NumK.mean_sqrt(1 + i % 9))
end
out[1] << "cached #{NumK.cached}\n"
NumK.instance_variable_set(:@sieve, nil)
out[1] << "restart cached #{NumK.cached}\n"
rounds.times { |i| out[2] << ObjK.shapes(3 + i % 13) << "\n" }
ObjK.instance_variable_set(:@calls, nil)
out[2] << "restart calls #{ObjK.calls}\n"
3.times { |i| puts "== program #{i}"; print out[i] }
RB

if ! cc -O2 -w $MCFLAGS -I"$TMP" -I"$LIB" -I"$LIB/regexp" "$TMP/host.c" \
      "$TMP/k_str.o" "$TMP/k_num.o" "$TMP/k_obj.o" "$MC" -lm -lcrypt -lpthread -o "$TMP/host" 2>"$TMP/el"; then
  echo "FAIL: three ext programs do not link into one binary"; grep -i "multiple definition" "$TMP/el" | head; tail -5 "$TMP/el"
  exit 1
fi

ruby "$TMP/driver.rb" "$ROUNDS" "$TMP" > "$TMP/ref.txt" 2>&1 || { echo "FAIL: CRuby reference"; tail -5 "$TMP/ref.txt"; exit 1; }

# 2. sequential, against CRuby
"$TMP/host" seq "$ROUNDS" > "$TMP/seq.txt" 2>&1; rc=$?
if [ "$rc" -ne 0 ] || ! cmp -s "$TMP/ref.txt" "$TMP/seq.txt"; then
  echo "FAIL: sequential run differs from CRuby (rc=$rc)"; diff "$TMP/ref.txt" "$TMP/seq.txt" | head; fail=1
fi

# 3. concurrent, against the sequential run; then with the collector stressed
for env in "" "SPINEL_GC_STRESS=1"; do
  env $env "$TMP/host" par "$ROUNDS" > "$TMP/par.txt" 2>&1; rc=$?
  if [ "$rc" -ne 0 ] || ! cmp -s "$TMP/ref.txt" "$TMP/par.txt"; then
    echo "FAIL: concurrent run ${env:+($env) }differs (rc=$rc)"; diff "$TMP/ref.txt" "$TMP/par.txt" | head; fail=1
  fi
done

# 4. restart is part of every run above ("restart ..." lines); make sure the
#    reference really exercises it
grep -q "^restart calls 0$" "$TMP/ref.txt" && grep -q "^restart cached -1$" "$TMP/ref.txt" \
  || { echo "FAIL: the restart lines are missing from the reference"; fail=1; }

if [ "$fail" -eq 0 ]; then echo "multi_ctx ext3: PASS (three ext programs, one binary, $ROUNDS rounds)"; else echo "multi_ctx ext3: FAIL"; fi
exit $fail
