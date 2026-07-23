/* sp_ctx.c -- per-instance runtime context implementation (SP_MULTI_CTX).
 * Inert in the default (single-context) build. See sp_ctx.h and
 * docs/internals/multi-instance.md. */
#include "sp_gc.h"   /* sp_RbVal etc. for the sp_ctx struct; also pulls sp_ctx.h */
#include "sp_ctx.h"

#ifdef SP_MULTI_CTX
#include <stdlib.h>
#include <string.h>

/* Defined in sp_alloc.c (also declared in sp_alloc.h). Wired into the object
 * collector per instance below, replacing the default build's constructor. */
void sp_str_sweep(void);

/* Reference current-instance accessor: a thread-local pointer. One instance
 * runs per OS thread (programs are internally single-threaded). The ESP-IDF
 * port (Phase 5) swaps this for a FreeRTOS task-local storage pointer. */
static __thread sp_ctx *g_sp_ctx = NULL;

sp_ctx *sp_ctx_current(void)          { return g_sp_ctx; }
void    sp_ctx_set_current(sp_ctx *c) { g_sp_ctx = c; }

/* --- allocation backend wrappers --- */
void *sp_mem_alloc(size_t n) {
  sp_ctx *c = g_sp_ctx;
  void *p = c->mem_alloc(c->mem_ud, n);   /* backend zero-fills */
  if (!p) sp_oom_die();
  return p;
}
void *sp_mem_zalloc(size_t n) { return sp_mem_alloc(n); }
void *sp_mem_realloc(void *p, size_t n) {
  sp_ctx *c = g_sp_ctx;
  void *r = c->mem_realloc(c->mem_ud, p, n);
  if (n && !r) sp_oom_die();
  return r;
}
void sp_mem_free(void *p) {
  sp_ctx *c = g_sp_ctx;
  c->mem_dealloc(c->mem_ud, p);
}

/* --- default libc backend (used when cfg->alloc is NULL) --- */
static void *dflt_alloc(void *ud, size_t n)            { (void)ud; return calloc(1, n); }
static void *dflt_realloc(void *ud, void *p, size_t n) { (void)ud; return realloc(p, n); }
static void  dflt_dealloc(void *ud, void *p)           { (void)ud; free(p); }

#ifndef SP_GC_STACK_MAX
#define SP_GC_STACK_MAX 65536
#endif

sp_ctx *sp_instance_create(const sp_instance_config *cfg) {
  sp_instance_config z = {0};
  if (!cfg) cfg = &z;
  void *(*a)(void *, size_t)          = cfg->alloc      ? cfg->alloc      : dflt_alloc;
  void *(*re)(void *, void *, size_t) = cfg->realloc_fn ? cfg->realloc_fn : dflt_realloc;
  void  (*de)(void *, void *)         = cfg->dealloc    ? cfg->dealloc    : dflt_dealloc;

  sp_ctx *c = (sp_ctx *)a(cfg->mem_ud, sizeof(sp_ctx));
  if (!c) return NULL;
  memset(c, 0, sizeof(*c));
  c->mem_ud = cfg->mem_ud;
  c->mem_alloc = a; c->mem_realloc = re; c->mem_dealloc = de;

  size_t gct = cfg->gc_threshold  ? cfg->gc_threshold  : (size_t)256 * 1024;
  size_t sct = cfg->str_threshold ? cfg->str_threshold : (size_t)256 * 1024;
  c->gc_threshold = c->gc_threshold_init = gct;
  c->str_threshold = c->str_threshold_init = sct;

  int rn = cfg->root_stack_entries ? cfg->root_stack_entries : SP_GC_STACK_MAX;
  c->gc_roots = (void ***)a(cfg->mem_ud, (size_t)rn * sizeof(void **));
  if (!c->gc_roots) { de(cfg->mem_ud, c); return NULL; }
  c->gc_roots_cap = rn;
  c->gc_nroots = 0;

  /* Wire the string sweep into this instance's collector (the default build
   * does this in a process constructor; under SP_MULTI_CTX it is per-ctx). */
  c->gc_str_sweep_hook = sp_str_sweep;

  /* GC verify: read the env here rather than in the process constructor, which
   * has no current instance to write into. */
  { const char *v = getenv("SPINEL_GC_VERIFY"); c->gc_verify = (v && *v && *v != '0'); }
  return c;
}

void sp_instance_destroy(sp_ctx *c) {
  if (!c) return;
  void (*de)(void *, void *) = c->mem_dealloc;
  void *ud = c->mem_ud;
  /* Free GC heaps. Objects with finalizers/recycle are torn down by the
   * collector's normal sweep; a hard teardown just frees the arenas. */
  de(ud, c->gc_roots);
  de(ud, c->gc_mark_stack);
  de(ud, c->gc_vsnap);
  de(ud, c);
}

#endif /* SP_MULTI_CTX */
