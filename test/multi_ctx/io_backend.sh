#!/usr/bin/env bash
# The per-instance I/O backend (sp_instance_config io_*) under SP_MULTI_CTX:
# every File/Dir op that names a path must reach the host's backend, never the
# libc filesystem behind its back, so a host that serializes its filesystem
# (fmruby's file HAL) sees all of it.
#
#   1. A program that reads, writes, appends, lists, stats, renames and
#      deletes, run in one instance whose backend forwards to the libc one and
#      counts each hook: the output must equal the default build's (-E), and
#      every hook the program's ops need must have been called.
#   2. The path ops the backend contract cannot express (links, permissions,
#      times, the process cwd, raw descriptors) raise NotImplementedError
#      instead of reaching the filesystem.
#
# Usage: test/multi_ctx/io_backend.sh   (run from the fork root; SPINEL/LIB overridable)
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SP="${SPINEL:-$ROOT/bin/spinel}"
LIB="${LIB:-$ROOT/lib}"
MC="$LIB/libspinel_rt_mc.a"
MCFLAGS="-DSP_MULTI_CTX -include $LIB/sp_mem_override.h"
TMP="$(mktemp -d)"
[ -n "${KEEP:-}" ] || trap 'rm -rf "$TMP"' EXIT
fail=0

if [ ! -f "$MC" ]; then echo "FAIL: $MC not built (make lib/libspinel_rt_mc.a)"; exit 1; fi

# --- host: one instance, a counting backend over the libc defaults ----------
cat > "$TMP/host.c" <<'C'
#include "sp_gc.h"   /* pulls in sp_ctx.h */
#include "sp_io.h"   /* sp_io_posix_* */
#include <stdio.h>
int sp_prog_entry(void);
enum { OPEN, STAT, OPENDIR, REMOVE, RENAME, MKDIR, RMDIR, NHOOK };
static const char *names[NHOOK] = { "open", "stat", "opendir", "remove", "rename", "mkdir", "rmdir" };
static int calls[NHOOK];
static void *h_open(void *ud, const char *p, const char *m) { calls[OPEN]++; return sp_io_posix_open(ud, p, m); }
static int h_stat(void *ud, const char *p, long *s, int *d, int *r) { calls[STAT]++; return sp_io_posix_stat(ud, p, s, d, r); }
static void *h_opendir(void *ud, const char *p) { calls[OPENDIR]++; return sp_io_posix_opendir(ud, p); }
static int h_remove(void *ud, const char *p) { calls[REMOVE]++; return sp_io_posix_remove(ud, p); }
static int h_rename(void *ud, const char *a, const char *b) { calls[RENAME]++; return sp_io_posix_rename(ud, a, b); }
static int h_mkdir(void *ud, const char *p) { calls[MKDIR]++; return sp_io_posix_mkdir(ud, p); }
static int h_rmdir(void *ud, const char *p) { calls[RMDIR]++; return sp_io_posix_rmdir(ud, p); }
int main(void) {
  sp_instance_config cfg = {0};
  cfg.io_open = h_open; cfg.io_stat = h_stat; cfg.io_opendir = h_opendir;
  cfg.io_remove = h_remove; cfg.io_rename = h_rename;
  cfg.io_mkdir = h_mkdir; cfg.io_rmdir = h_rmdir;
  sp_ctx *c = sp_instance_create(&cfg);
  if (!c) { fprintf(stderr, "instance_create failed\n"); return 1; }
  sp_ctx_set_current(c);
  int rc = sp_prog_entry();
  sp_instance_destroy(c);
  for (int i = 0; i < NHOOK; i++) fprintf(stderr, "%s=%d\n", names[i], calls[i]);
  return rc;
}
C

# --- 1. the ops a program uses, against the default build ------------------
cat > "$TMP/ops.rb" <<'RB'
def sp_prog_entry_ruby
  dir = "io_backend_dir"
  Dir.mkdir(dir)
  path = dir + "/a.txt"
  File.write(path, "one\ntwo\n")
  File.open(path, "a") { |f| f.write("three\n") }
  puts File.read(path)
  puts File.readlines(path).length
  puts File.size(path)
  puts File.exist?(path)
  puts File.file?(path)
  puts File.directory?(dir)
  puts File.zero?(path)
  puts File.ftype(dir)
  puts File.readable?(path)
  s = File.stat(path)
  puts s.size
  puts s.file?
  File.open(path, "r") { |f| puts f.gets }
  File.rename(path, dir + "/b.txt")
  puts File.exist?(path)
  puts Dir.children(dir).sort.join(",")
  d = Dir.open(dir)
  n = 0
  while (e = d.read)
    n += 1 if e == "b.txt"
  end
  d.close
  puts n
  File.delete(dir + "/b.txt")
  puts Dir.empty?(dir)
  Dir.rmdir(dir)
  puts File.exist?(dir)
end
sp_prog_entry_ruby
RB

( cd "$TMP" && "$SP" -E "$TMP/ops.rb" > "$TMP/ref.txt" 2>"$TMP/ref.err" )
"$SP" --no-main --entry sp_prog_entry -o "$TMP/ops.c" "$TMP/ops.rb" >/dev/null 2>&1
if cc -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/ops.c" "$TMP/host.c" "$MC" \
      -lm -lcrypt -lpthread -o "$TMP/ops" 2>"$TMP/e1"; then
  ( cd "$TMP" && ./ops > "$TMP/ops.out" 2>"$TMP/ops.calls" ); rc=$?
  if [ "$rc" -ne 0 ] || ! cmp -s "$TMP/ref.txt" "$TMP/ops.out"; then
    echo "FAIL: backend run differs from -E (rc=$rc)"; diff "$TMP/ref.txt" "$TMP/ops.out" | head; fail=1
  fi
  for h in open stat opendir remove rename mkdir rmdir; do
    n="$(sed -n "s/^$h=//p" "$TMP/ops.calls")"
    if [ -z "$n" ] || [ "$n" -eq 0 ]; then echo "FAIL: the $h hook was never called"; fail=1; fi
  done
else
  echo "FAIL: link error"; tail -5 "$TMP/e1"; fail=1
fi

# --- 2. what the backend cannot express is refused -----------------------------
refuse() {   # name, ruby statement
  printf 'def sp_prog_entry_ruby\n  begin\n    %s\n    puts "reached"\n  rescue NotImplementedError\n    puts "refused"\n  end\nend\nsp_prog_entry_ruby\n' "$2" > "$TMP/r_$1.rb"
  "$SP" --no-main --entry sp_prog_entry -o "$TMP/r_$1.c" "$TMP/r_$1.rb" >/dev/null 2>&1 &&
    cc -O2 -w $MCFLAGS -I"$LIB" -I"$LIB/regexp" "$TMP/r_$1.c" "$TMP/host.c" "$MC" \
       -lm -lcrypt -lpthread -o "$TMP/r_$1" 2>/dev/null ||
    { echo "FAIL: $1 did not build"; fail=1; return; }
  out="$( cd "$TMP" && ./"r_$1" 2>/dev/null )"
  if [ "$out" != "refused" ]; then echo "FAIL: $1 answered '$out', not NotImplementedError"; fail=1; fi
}
: > "$TMP/x.txt"
refuse symlink  'File.symlink("x.txt", "y.txt")'
refuse chmod    'File.chmod(0644, "x.txt")'
refuse mtime    'File.mtime("x.txt")'
refuse truncate 'File.truncate("x.txt", 0)'
refuse chdir    'Dir.chdir("/")'

if [ "$fail" -eq 0 ]; then echo "multi_ctx io_backend: PASS"; else exit 1; fi
