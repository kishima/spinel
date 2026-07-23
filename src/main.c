/* spinel: the single-binary Spinel compiler *and* driver.
 *
 * One executable does the whole job, with no shell wrapper and no chain of
 * helper binaries (the legacy parse -> analyze -> codegen pipeline collapsed
 * into this process). It:
 *   1. parses the .rb (Prism walk -> text AST -> in-memory node table),
 *   2. infers types and emits C (codegen_program),
 *   3. invokes the C compiler (cc) to link the generated C against
 *      libspinel_rt.a into a native binary, and
 *   4. optionally runs that binary (-E).
 *
 * Usage:
 *   spinel app.rb               compile to ./app
 *   spinel app.rb -o myapp      compile to ./myapp
 *   spinel app.rb -c            emit C only (app.c)
 *   spinel app.rb -S            print C to stdout
 *   spinel -e 'puts 42'         compile inline source (default name: a)
 *   spinel -E app.rb a b c      compile to a temp dir, run, ARGV=[a,b,c]
 */
#include "node_table.h"
#include "spinel_rev.h"
#include "codegen.h"
#include "analyze.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#if defined(__APPLE__)
  #include <mach-o/dyld.h>
#endif
#include <sys/wait.h>
#define PATH_SEP '/'
#define EXE_SUFFIX ""

/* Defined in spinel_parse.c (compiled with -DSPINEL_PARSE_AS_LIB). */
char *sp_parse_file_to_text(const char *source_file, const char *argv0);

/* ---------- tiny growable string ---------- */
typedef struct { char *p; size_t len, cap; } Str;
static void s_reserve(Str *s, size_t extra) {
  if (s->len + extra + 1 > s->cap) {
    s->cap = (s->len + extra + 1) * 2;
    s->p = realloc(s->p, s->cap);
  }
}
static void s_add(Str *s, const char *t) {
  size_t n = strlen(t);
  s_reserve(s, n);
  memcpy(s->p + s->len, t, n);
  s->len += n;
  s->p[s->len] = '\0';
}
/* Append a shell-quoted token followed by a space (paths may contain spaces). */
static void s_add_arg(Str *s, const char *t) {
  s_add(s, "\"");
  s_add(s, t);
  s_add(s, "\" ");
}

static void set_env(const char *k, const char *v) {
  setenv(k, v, 1);
}

static int file_exists(const char *p) {
  FILE *f = fopen(p, "rb");
  if (f) { fclose(f); return 1; }
  return 0;
}

static int proc_id(void) {
  return (int)getpid();
}

static const char *temp_dir(void) {
  const char *t = getenv("TMPDIR");
  if (!t) t = "/tmp";
  return t;
}

/* Build "<tmpdir>/spinel_<tag>_<pid>_<n><ext>" into `out`. */
static void make_temp_path(char *out, size_t sz, const char *tag, int n, const char *ext) {
  snprintf(out, sz, "%s%cspinel_%s_%d_%d%s", temp_dir(), PATH_SEP, tag, proc_id(), n, ext);
}

/* Directory containing this executable (no trailing separator). */
static void exe_dir(const char *argv0, char *out, size_t outsz) {
  char buf[4096];
  buf[0] = '\0';
#if defined(__APPLE__)
  uint32_t bsz = (uint32_t)sizeof buf;
  if (_NSGetExecutablePath(buf, &bsz) != 0) buf[0] = '\0';
#else
  ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (n > 0) buf[n] = '\0'; else buf[0] = '\0';
#endif
  if (!buf[0]) {
    /* Fall back to argv0 (resolve when it carries a path). */
    if (!realpath(argv0, buf)) { strncpy(buf, argv0, sizeof buf - 1); buf[sizeof buf - 1] = '\0'; }
  }
  else {
    /* Canonicalize: _NSGetExecutablePath returns the path spinel was invoked
       through WITHOUT following symlinks, so the installed tree's
       /usr/local/bin/spinel symlink resolved lib/ against bin/ and the link
       line pointed at bin/lib/libspinel_rt.a. (/proc/self/exe is already
       fully resolved; realpath is a no-op on that branch.) */
    char rp[4096];
    if (realpath(buf, rp)) { strncpy(buf, rp, sizeof buf - 1); buf[sizeof buf - 1] = '\0'; }
  }
  char *slash = strrchr(buf, '/');
  if (slash) *slash = '\0';
  else strcpy(buf, ".");
  strncpy(out, buf, outsz - 1);
  out[outsz - 1] = '\0';
}

/* Resolve the lib/ directory: a dev tree builds the binary at build/spinel
   (lib at ../lib); an installed tree drops it next to lib/. */
static void resolve_lib_dir(const char *argv0, char *out, size_t outsz) {
  char dir[4096], cand[4096];
  exe_dir(argv0, dir, sizeof dir);
  snprintf(cand, sizeof cand, "%s%clib%clibspinel_rt.a", dir, PATH_SEP, PATH_SEP);
  if (file_exists(cand)) { snprintf(out, outsz, "%s%clib", dir, PATH_SEP); return; }
  snprintf(cand, sizeof cand, "%s%c..%clib%clibspinel_rt.a", dir, PATH_SEP, PATH_SEP, PATH_SEP);
  if (file_exists(cand)) { snprintf(out, outsz, "%s%c..%clib", dir, PATH_SEP, PATH_SEP); return; }
  /* Last resort: assume sibling lib/ (may not exist; cc will report). */
  snprintf(out, outsz, "%s%clib", dir, PATH_SEP);
}

/* Append the FFI link/cflag markers the codegen embedded in the C source.
   Each marker line is a C comment of the form  PREFIX <flags> SP/  where
   PREFIX is e.g. the SPINEL_LINK opener; `prefix` includes that opener. */
static void scrape_ffi_markers(const char *csrc, const char *prefix, Str *out) {
  size_t plen = strlen(prefix);
  const char *p = csrc;
  while (p && *p) {
    const char *eol = strchr(p, '\n');
    size_t llen = eol ? (size_t)(eol - p) : strlen(p);
    if (llen > plen && strncmp(p, prefix, plen) == 0) {
      /* strip the trailing space + comment close */
      const char *body = p + plen;
      size_t blen = llen - plen;
      while (blen > 0 && (body[blen-1] == ' ' || body[blen-1] == '/' || body[blen-1] == '*')) blen--;
      char tmp[1024];
      if (blen >= sizeof tmp) blen = sizeof tmp - 1;
      memcpy(tmp, body, blen);
      tmp[blen] = '\0';
      s_add(out, tmp);
      s_add(out, " ");
    }
    if (!eol) break;
    p = eol + 1;
  }
}

static int write_text_file(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (!f) { fprintf(stderr, "spinel: cannot write '%s'\n", path); return 0; }
  fputs(text, f);
  fclose(f);
  return 1;
}

/* Read an entire file into a malloc'd NUL-terminated buffer (caller frees),
   or NULL on error. Used by --inject to splice a raw C file after the
   generated translation unit. */
static char *read_text_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  if (n < 0) { fclose(f); return NULL; }
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t rd = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[rd] = 0;
  return buf;
}

static void usage(void) {
  fprintf(stderr,
    "Spinel AOT Compiler\n\n"
    "Usage: spinel app.rb              - compile to ./app\n"
    "       spinel app.rb -o myapp     - compile to ./myapp\n"
    "       spinel app.rb -c           - generate app.c only\n"
    "       spinel app.rb -S           - print C to stdout\n"
    "       spinel -e 'puts 42'        - compile inline source\n"
    "       spinel -E app.rb a b c     - compile + run with ARGV=[a, b, c]\n\n"
    "Options:\n"
    "  -o FILE     Output file\n"
    "  --link ARG  Extra link input (object/archive/-lLIB); repeatable\n"
    "  --version   Print the compiler build revision\n"
    "  -c          C source only (don't compile)\n"
    "  --no-main   Emit `int <entry>(void)` instead of main(); implies -c (library mode)\n"
    "  --entry NAME  Entry function name for --no-main (default: spinel_program_main)\n"
    "  --inject FILE  Append a raw C file after the generated unit\n"
    "  -I DIR      Add a feature search root for `require \"name\"` (like ruby -I)\n"
    "  --emit-rbs  Dump inferred type signatures as RBS (-> app.rbs), no binary\n"
    "  --emit-types Dump per-position inferred types + diagnostics as JSON\n"
    "  --emit-symbol-map  Dump emitted-symbol -> Ruby-name map as JSON, no binary\n"
    "  -S          Print C to stdout\n"
    "  -E          Run the compiled binary; leftover args become its ARGV\n"
    "  -O LEVEL    Optimization level (default: 2)\n"
    "  -g          Add debug info (-g) + #line, leaving -O as-is\n"
    "  --debug     Debug build: step through the .rb in gdb/lldb (#line, -g -O0)\n"
    "  --no-line-map  Suppress #line directives\n"
    "  --cc=CMD    C compiler (default: cc)\n"
    "  -e STR      Inline Ruby source (repeatable; joined with newlines)\n"
    "  --rbs DIR   Seed analyzer with RBS signatures from DIR (advisory)\n"
    "  --int-overflow=MODE  Int +/-/* overflow handling (default: raise)\n"
    "  --dump-ast  Print the text AST and exit (debug)\n");
}

int main(int argc, char **argv) {
  const char *source = NULL;
  const char *output = NULL;
  const char *link_extra[64]; int n_link_extra = 0;
  const char *cc_cmd = "cc";
  const char *opt_level = "2";
  const char *int_overflow = "raise";
  const char *rbs_dir = NULL;
  const char *inject_path = NULL;
  int c_only = 0, stdout_mode = 0, run_mode = 0, dump_ast = 0;
  int emit_rbs = 0, emit_types = 0, emit_symbol_map = 0;
  int debug = 0, line_map = 1, want_g = 0;
  /* Accumulated -e source and the program ARGV after the -E boundary. */
  Str eval_src = {0};
  int eval_used = 0;
  char **run_args = NULL;
  int n_run_args = 0;

  for (int i = 1; i < argc; ) {
    const char *a = argv[i];
    if (!strncmp(a, "--source=", 9))      { source = a + 9; i++; }
    else if (!strncmp(a, "--output=", 9)) { output = a + 9; i++; }
    else if (!strncmp(a, "--cc=", 5))     { cc_cmd = a + 5; i++; }
    else if (!strncmp(a, "--rbs=", 6))    { rbs_dir = a + 6; i++; }
    else if (sp_streq(a, "--rbs"))         { if (++i < argc) rbs_dir = argv[i]; i++; }
    else if (!strncmp(a, "--int-overflow=", 15)) { int_overflow = a + 15; i++; }
    else if (sp_streq(a, "--int-overflow")) { if (++i < argc) int_overflow = argv[i]; i++; }
    else if (sp_streq(a, "-o"))            { if (++i < argc) output = argv[i]; i++; }
    else if (sp_streq(a, "--link"))        { if (++i < argc && n_link_extra < 64) link_extra[n_link_extra++] = argv[i]; i++; }
    else if (sp_streq(a, "-O"))            { if (++i < argc) opt_level = argv[i]; i++; }
    else if (sp_streq(a, "--debug"))       { debug = 1; opt_level = "0"; want_g = 1; i++; }
    else if (sp_streq(a, "-g"))            { debug = 1; want_g = 1; i++; }
    else if (sp_streq(a, "--line-map"))    { line_map = 1; i++; }
    else if (sp_streq(a, "--no-line-map")) { line_map = 0; i++; }
    else if (sp_streq(a, "-c"))            { c_only = 1; i++; }
    /* Library mode: emit `int <entry>(void)` instead of main(); imply -c since
       the result is not a standalone runnable binary. --entry sets the name. */
    else if (sp_streq(a, "--no-main"))     { g_no_main = 1; c_only = 1; i++; }
    else if (!strncmp(a, "--entry=", 8))   { g_entry_name = a + 8; i++; }
    else if (sp_streq(a, "--entry"))       { if (++i < argc) g_entry_name = argv[i]; i++; }
    /* Splice a raw C file after the generated translation unit (escape hatch to
       call generated static functions from hand-written C). */
    else if (!strncmp(a, "--inject=", 9))  { inject_path = a + 9; i++; }
    else if (sp_streq(a, "--inject"))      { if (++i < argc) inject_path = argv[i]; i++; }
    else if (sp_streq(a, "-I"))            { if (++i < argc) sp_add_feature_root(argv[i]); i++; }
    else if (!strncmp(a, "-I", 2) && a[2]) { sp_add_feature_root(a + 2); i++; }
    else if (sp_streq(a, "-S"))            { stdout_mode = 1; i++; }
    else if (sp_streq(a, "-E"))            { run_mode = 1; i++; }
    else if (sp_streq(a, "--emit-rbs"))    { emit_rbs = 1; i++; }
    else if (sp_streq(a, "--emit-types"))  { emit_types = 1; i++; }
    else if (sp_streq(a, "--emit-symbol-map")) { emit_symbol_map = 1; i++; }
    else if (sp_streq(a, "--dump-ast"))    { dump_ast = 1; i++; }
    else if (sp_streq(a, "-h") || sp_streq(a, "--help")) { usage(); return 0; }
    else if (sp_streq(a, "--version")) { printf("spinel %s\n", SPINEL_BUILD_REV); return 0; }
    else if (sp_streq(a, "-e")) {
      if (++i < argc) {
        if (eval_used) s_add(&eval_src, "\n");
        s_add(&eval_src, argv[i]);
        eval_used = 1;
      }
      i++;
    }
    else if (sp_streq(a, "--")) {
      i++;
      if (!source && i < argc) { source = argv[i]; i++; }
      if (run_mode) { run_args = &argv[i]; n_run_args = argc - i; }
      break;
    }
    else if (a[0] != '-') {
      /* A positional: the source path. In run mode it ends flag parsing and
         the rest of the line becomes the program's ARGV. */
      if (!source) {
        source = a;
        i++;
        if (run_mode) { run_args = &argv[i]; n_run_args = argc - i; break; }
      }
      else {
        i++;  /* extra positionals are ignored in compile mode */
      }
    }
    else {
      /* Unknown flag. In run mode it is the ARGV boundary (the program may
         take its own flags); otherwise ignore it. */
      if (run_mode) { run_args = &argv[i]; n_run_args = argc - i; break; }
      i++;
    }
  }

  /* -e: materialize the inline source as a temp .rb. */
  char eval_path[4096] = {0};
  if (eval_used) {
    make_temp_path(eval_path, sizeof eval_path, "eval", 0, ".rb");
    if (!write_text_file(eval_path, eval_src.p ? eval_src.p : "")) return 1;
    source = eval_path;
  }
  free(eval_src.p);

  if (!source) { usage(); return 1; }
  if (!file_exists(source)) { fprintf(stderr, "spinel: %s: No such file\n", source); return 1; }

  /* Mode-conflict checks mirror the old driver. */
  if (run_mode && (output || c_only || stdout_mode)) {
    fprintf(stderr, "spinel: -E (run) cannot be combined with -o/-c/-S\n");
    return 2;
  }

  const char *ov_define = NULL;
  if (sp_streq(int_overflow, "raise"))   ov_define = "-DSP_INT_OVERFLOW_MODE_RAISE";
  else if (sp_streq(int_overflow, "wrap"))    ov_define = "-DSP_INT_OVERFLOW_MODE_WRAP";
  else if (sp_streq(int_overflow, "promote")) ov_define = "-DSP_INT_OVERFLOW_MODE_PROMOTE";
  else { fprintf(stderr, "spinel: --int-overflow expects raise|wrap|promote, got '%s'\n", int_overflow); return 2; }
  /* Let the analyzer widen accumulating int locals to bigint more freely in
     promote mode (block-iteration loops, not just `while`). See analyze.h. */
  g_promote_mode = sp_streq(int_overflow, "promote");

  /* Base name for default output paths. */
  char basename[1024];
  if (eval_used) {
    strcpy(basename, "a");
  }
  else {
    const char *b = source;
    const char *sl = strrchr(source, '/');
    if (sl) b = sl + 1;
    strncpy(basename, b, sizeof basename - 1);
    basename[sizeof basename - 1] = '\0';
    char *dot = strstr(basename, ".rb");
    if (dot && dot[3] == '\0') *dot = '\0';
  }

  /* --rbs: extract advisory seeds via the sibling spinel_rbs_extract binary
     and hand them to the analyzer through SPINEL_RBS_SEED. An empty result is a
     silent no-op; a missing extractor is warned about, since --rbs was asked for
     explicitly and the seeds would otherwise vanish without a trace (#1792). */
  char seed_path[4096] = {0};
  if (rbs_dir && *rbs_dir) {
    char dir[4096], extractor[4096];
    exe_dir(argv[0], dir, sizeof dir);
    snprintf(extractor, sizeof extractor, "%s%cspinel_rbs_extract%s", dir, PATH_SEP, EXE_SUFFIX);
    if (!file_exists(extractor)) {
      fprintf(stderr, "spinel: warning: --rbs %s requested but spinel_rbs_extract "
              "was not found beside the compiler; .rbs type seeds are unavailable "
              "(rebuild with `make deps`, or reinstall a toolchain that ships it)\n", rbs_dir);
    }
    if (file_exists(extractor)) {
      make_temp_path(seed_path, sizeof seed_path, "seed", 0, ".txt");
      Str cmd = {0};
      s_add_arg(&cmd, extractor);
      s_add_arg(&cmd, rbs_dir);
      s_add(&cmd, "> ");
      s_add_arg(&cmd, seed_path);
      int rc = system(cmd.p);
      free(cmd.p);
      if (rc == 0 && file_exists(seed_path)) {
        set_env("SPINEL_RBS_SEED", seed_path);
      }
      else {
        remove(seed_path);
        seed_path[0] = '\0';
      }
    }
  }

  /* Source mapping: the parser stamps node positions and codegen emits #line
     when SPINEL_DEBUG / SPINEL_LINE_MAP is set. --debug/-g use the fuller
     debug path; otherwise line-map (on by default) just adds #line. The emit
     modes that need positions force SPINEL_DEBUG below. */
  if (debug) set_env("SPINEL_DEBUG", "1");
  else if (line_map) set_env("SPINEL_LINE_MAP", "1");

  /* Analyze-only emit modes write their artifact from inside codegen_program
     and produce an empty translation unit; route the output path via env. */
  char emit_out[4096];
  if (emit_rbs) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".rbs", sizeof emit_out - strlen(emit_out) - 1); }
    set_env("SPINEL_EMIT_RBS", emit_out);
  }
  else if (emit_types) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".types.json", sizeof emit_out - strlen(emit_out) - 1); }
    set_env("SPINEL_DEBUG", "1");
    set_env("SPINEL_EMIT_TYPES", emit_out);
  }
  else if (emit_symbol_map) {
    snprintf(emit_out, sizeof emit_out, "%s", output ? output : basename);
    if (!output) { strncat(emit_out, ".symbols.json", sizeof emit_out - strlen(emit_out) - 1); }
    set_env("SPINEL_DEBUG", "1");
    set_env("SPINEL_EMIT_SYMBOL_MAP", emit_out);
  }

  /* ---------- pipeline: parse -> AST -> codegen ---------- */
  char *text = sp_parse_file_to_text(source, argv[0]);
  if (eval_path[0]) remove(eval_path);
  if (!text) { fprintf(stderr, "spinel: parse failed for '%s'\n", source); if (seed_path[0]) remove(seed_path); return 1; }

  if (dump_ast) { fputs(text, stdout); free(text); if (seed_path[0]) remove(seed_path); return 0; }

  NodeTable *nt = nt_load_text(text);
  free(text);
  if (!nt) { fprintf(stderr, "spinel: failed to load AST\n"); if (seed_path[0]) remove(seed_path); return 1; }

  char *csrc = codegen_program(nt);
  nt_free(nt);
  if (seed_path[0]) remove(seed_path);
  if (!csrc) { fprintf(stderr, "spinel: codegen failed\n"); return 1; }

  /* --inject: concatenate a raw C file after all generated (static) definitions,
     so hand-written C can call into the generated unit. */
  if (inject_path) {
    char *inj = read_text_file(inject_path);
    if (!inj) { fprintf(stderr, "spinel: cannot read --inject file '%s'\n", inject_path); free(csrc); return 1; }
    Str merged = {0};
    s_add(&merged, csrc);
    s_add(&merged, "\n/* --inject: ");
    s_add(&merged, inject_path);
    s_add(&merged, " */\n");
    s_add(&merged, inj);
    free(inj);
    free(csrc);
    csrc = merged.p;
  }

  /* Emit modes already wrote their file; codegen returned empty C. */
  if (emit_rbs || emit_types || emit_symbol_map) {
    fprintf(stderr, "Wrote %s\n", emit_out);
    free(csrc);
    return 0;
  }

  if (stdout_mode) { fputs(csrc, stdout); free(csrc); return 0; }

  /* Decide where the generated C goes. */
  char c_path[4096];
  int c_is_temp = 0;
  if (c_only) {
    snprintf(c_path, sizeof c_path, "%s", output ? output : basename);
    if (!output) strncat(c_path, ".c", sizeof c_path - strlen(c_path) - 1);
  }
  else {
    make_temp_path(c_path, sizeof c_path, "out", 0, ".c");
    c_is_temp = 1;
  }
  if (!write_text_file(c_path, csrc)) { free(csrc); return 1; }

  if (c_only) { fprintf(stderr, "Wrote %s\n", c_path); free(csrc); return 0; }

  /* ---------- link: cc <generated C> -> native binary ---------- */
  char lib_dir[4096];
  resolve_lib_dir(argv[0], lib_dir, sizeof lib_dir);

  Str ffi_links = {0}, ffi_cflags = {0}, native_objs = {0};
  scrape_ffi_markers(csrc, "/* SPINEL_LINK: ", &ffi_links);
  scrape_ffi_markers(csrc, "/* SPINEL_CFLAGS: ", &ffi_cflags);
  /* native_obj (Path B): root-relative carried-C object paths to link, one
     package's C linked only when its require appeared. Resolved against the
     compiler's base dir (beside lib/ and packages/) at the link step below. */
  scrape_ffi_markers(csrc, "/* SPINEL_LINK_OBJ: ", &native_objs);
  /* A program that uses Thread/Mutex/Queue/... links the -DSP_THREADS runtime
     variant (libspinel_rt_mt.a) plus -lpthread; everything else links the
     byte-identical single-threaded archive. See codegen's SPINEL_USES_THREADS
     marker and the two-variant build in the Makefile. */
  int uses_threads = strstr(csrc, "/* SPINEL_USES_THREADS */") != NULL;
  const char *rt_lib = uses_threads ? "libspinel_rt_mt.a" : "libspinel_rt.a";
  free(csrc);

  /* Output binary path: -E uses a temp so the cwd stays clean. */
  char bin_path[4096];
  int bin_is_temp = 0;
  if (run_mode) {
    char rdir[4096];
    snprintf(rdir, sizeof rdir, "run_%s", basename);
    make_temp_path(bin_path, sizeof bin_path, rdir, 0, EXE_SUFFIX);
    bin_is_temp = 1;
  }
  else {
    snprintf(bin_path, sizeof bin_path, "%s%s", output ? output : basename, output ? "" : EXE_SUFFIX);
  }

  Str cmd = {0};
  char tmp[8192];
  s_add(&cmd, cc_cmd);
  s_add(&cmd, " ");
  snprintf(tmp, sizeof tmp, "-O%s ", opt_level); s_add(&cmd, tmp);
  s_add(&cmd, "-Wno-all -ffunction-sections -fdata-sections ");
  snprintf(tmp, sizeof tmp, "-I\"%s\" -I\"%s%cregexp\" ", lib_dir, lib_dir, PATH_SEP); s_add(&cmd, tmp);
  /* Compile the generated TU with the same threading define as the mt runtime
     archive it links, so the per-worker SP_TLS globals (sp_gc_roots, ...) it
     references through the runtime headers get the matching thread-local
     storage class. initial-exec keeps those reads a single segment load. */
  if (uses_threads) s_add(&cmd, "-DSP_THREADS -ftls-model=initial-exec ");
  if (ffi_cflags.p) s_add(&cmd, ffi_cflags.p);
  s_add_arg(&cmd, c_path);
  /* --link objects/archives sit between the generated TU and the runtime
     archive: they reference sp_ runtime symbols, and ld resolves left to
     right. --link -l flags go after -lm below, where DSOs belong. */
  for (int li = 0; li < n_link_extra; li++)
    if (strncmp(link_extra[li], "-l", 2) != 0) s_add_arg(&cmd, link_extra[li]);
  /* native_obj carried-C objects: resolve each root-relative path against the
     base dir (lib_dir minus its trailing "/lib", where packages/ also lives)
     and add as a link input, before the runtime archive. Try the dev-tree
     "<base>/../" fallback like the require resolver. Same left-to-right rule:
     they reference sp_ runtime symbols resolved from the archive that follows. */
  if (native_objs.p && native_objs.p[0]) {
    int blen = (int)strlen(lib_dir);
    if (blen >= 4 && strcmp(lib_dir + blen - 4, "/lib") == 0) blen -= 4;
    char *toks = strdup(native_objs.p);
    for (char *t = strtok(toks, " "); t; t = strtok(NULL, " ")) {
      /* Dedup against --link inputs: spin links a package's carried C from its
         own cache while the package also declares it via native_obj -- both on
         the link line is a multiple-definition error. The same basename in an
         explicit --link object means it is already covered. */
      const char *tb = strrchr(t, '/'); tb = tb ? tb + 1 : t;
      int dup = 0;
      for (int li = 0; li < n_link_extra && !dup; li++) {
        const char *lb = strrchr(link_extra[li], '/');
        lb = lb ? lb + 1 : link_extra[li];
        if (strcmp(lb, tb) == 0) dup = 1;
      }
      if (dup) continue;
      char op[4096];
      snprintf(op, sizeof op, "%.*s/%s", blen, lib_dir, t);
      if (access(op, F_OK) != 0)
        snprintf(op, sizeof op, "%.*s/../%s", blen, lib_dir, t);
      s_add_arg(&cmd, op);
    }
    free(toks);
  }
  snprintf(tmp, sizeof tmp, "\"%s%c%s\" ", lib_dir, PATH_SEP, rt_lib); s_add(&cmd, tmp);
  /* -lm AFTER the archive: ld processes inputs left to right and (with the
     GNU default --as-needed) drops a DSO no preceding input references.
     sp_format.o pulls in sqrt/sin/cos, so libm must follow libspinel_rt.a. */
  s_add(&cmd, "-lm ");
#if !defined(__APPLE__)
  s_add(&cmd, "-lcrypt ");  /* String#crypt = libc crypt(3); --as-needed drops it when unused */
#endif
  for (int li = 0; li < n_link_extra; li++)
    if (strncmp(link_extra[li], "-l", 2) == 0) { s_add(&cmd, link_extra[li]); s_add(&cmd, " "); }
  if (uses_threads) s_add(&cmd, "-lpthread ");
  s_add(&cmd, ov_define); s_add(&cmd, " ");
  if (want_g) s_add(&cmd, "-g ");
#if !defined(__APPLE__)
  if (debug) s_add(&cmd, "-rdynamic ");  /* ELF: name user frames in backtraces */
#endif
  /* An ffi_lib name whose archive already arrived as an explicit --link
     input (spin's [[build]] artifacts) is satisfied: also emitting the -l
     flag would make ld search the system paths for a library that only
     exists in the build cache. A --link basename lib<name>.(a|so) counts as
     providing -l<name>, so one package source serves both worlds -- the
     Makefile's -L via ffi_cflags and spin's absolute --link paths. */
  if (ffi_links.p) {
    char *ltoks = strdup(ffi_links.p);
    for (char *t = strtok(ltoks, " "); t; t = strtok(NULL, " ")) {
      int covered = 0;
      if (strncmp(t, "-l", 2) == 0) {
        char want_a[512], want_so[512];
        snprintf(want_a, sizeof want_a, "lib%s.a", t + 2);
        snprintf(want_so, sizeof want_so, "lib%s.so", t + 2);
        for (int li = 0; li < n_link_extra && !covered; li++) {
          const char *lb = strrchr(link_extra[li], '/');
          lb = lb ? lb + 1 : link_extra[li];
          if (strcmp(lb, want_a) == 0 || strcmp(lb, want_so) == 0) covered = 1;
        }
      }
      if (!covered) { s_add(&cmd, t); s_add(&cmd, " "); }
    }
    free(ltoks);
  }
#if defined(__APPLE__)
  s_add(&cmd, "-Wl,-dead_strip ");
#else
  s_add(&cmd, "-Wl,--gc-sections ");
#endif
  s_add(&cmd, "-o ");
  s_add_arg(&cmd, bin_path);
  free(ffi_links.p);
  free(ffi_cflags.p);

  int cc_rc = system(cmd.p);
  free(cmd.p);
  if (c_is_temp) remove(c_path);
  if (cc_rc != 0) {
    fprintf(stderr, "spinel: C compilation failed\n");
    if (bin_is_temp) remove(bin_path);
    return 1;
  }

  if (!run_mode) {
    fprintf(stderr, "%s -> %s\n", source, bin_path);
    return 0;
  }

  /* ---------- run: hand off to the freshly compiled binary ---------- */
  Str run = {0};
  s_add_arg(&run, bin_path);
  for (int k = 0; k < n_run_args; k++) s_add_arg(&run, run_args[k]);
  int run_rc = system(run.p);
  free(run.p);
  remove(bin_path);
  if (WIFEXITED(run_rc)) return WEXITSTATUS(run_rc);
  return run_rc ? 1 : 0;
}
