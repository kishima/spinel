/* sp_ctx.h -- per-instance runtime context (SP_MULTI_CTX).
 *
 * Lets several Spinel-compiled programs share one process, each with its own
 * heap and GC on its own OS thread. See docs/internals/multi-instance.md.
 *
 * Design contract:
 *   - SP_MULTI_CTX UNDEFINED (default): this header is inert. The runtime's
 *     mutable globals keep their original definitions in sp_alloc.c / sp_gc.c /
 *     sp_re.c / sp_random.c and their original names resolve to those globals.
 *     Nothing here changes the generated code or the hot path -- byte-identical.
 *   - SP_MULTI_CTX DEFINED: the same globals become macros onto sp_ctx fields
 *     reached through SP_CTX() (a thread-local current-instance pointer). The
 *     runtime .c files guard their definitions with #ifndef SP_MULTI_CTX so the
 *     state lives only in the ctx, initialized by sp_instance_create().
 *
 * Only library-side state (compiled once into libspinel_rt.a and shared across
 * translation units) needs relocation. sp_runtime.h statics are per-TU already
 * (it is included only by the generated program) and stay put.
 */
#ifndef SP_CTX_H
#define SP_CTX_H

#include <stddef.h>
#include "sp_types.h"   /* mrb_int, sp_gc_hdr fwd via sp_gc.h consumers */

struct sp_gc_hdr;
struct sp_str_hdr;

/* Per-instance runtime state. Field names are the original global names with
 * their sp_/sp_gc_ prefix dropped; the compat macros below re-attach them. */
typedef struct sp_ctx {
  /* --- string allocator (was sp_alloc.c) --- */
  struct sp_str_hdr *str_heap;
  size_t str_heap_bytes;
  size_t str_threshold;
  size_t str_threshold_init;
  int    str_stress_checked;

  /* --- object GC thresholds (was sp_alloc.c) --- */
  size_t gc_threshold;
  size_t gc_threshold_init;
  int    gc_stress_checked;

  /* --- GC heap/state (was sp_gc.c) --- */
  struct sp_gc_hdr *gc_heap;
  struct sp_gc_hdr *gc_old_heap;
  size_t gc_bytes;
  size_t gc_old_bytes;
  int    gc_cycle;
  int    gc_verify;
  void **gc_mark_stack;
  int    gc_mark_top;
  struct sp_gc_hdr **gc_vsnap;
  size_t gc_vsnap_n, gc_vsnap_cap;
  size_t gc_max_bytes;
  int    gc_max_bytes_init;
  void  *gc_dbg_ctx;

  /* --- GC root stack (was sp_gc.c / sp_gc.h) --- */
  void ***gc_roots;   /* default: points at a static array; MC: heap */
  int     gc_nroots;
  int     gc_roots_cap;

  /* --- GC program hooks (per program; see multi-instance.md) --- */
  void (*gc_mark_globals_hook)(void);
  void (*gc_str_sweep_hook)(void);
  void (*gc_mark_suspended_fibers_hook)(void);

  /* --- allocation backend (T3-2 sp_mem_* hooks) --- */
  void  *mem_ud;
  void *(*mem_alloc)(void *ud, size_t);    /* MUST zero-fill */
  void *(*mem_realloc)(void *ud, void *, size_t);
  void  (*mem_dealloc)(void *ud, void *);
} sp_ctx;

/* ------------------------------------------------------------------------- */
#ifdef SP_MULTI_CTX

#ifdef SP_THREADS
#error "SP_MULTI_CTX and SP_THREADS are mutually exclusive (see multi-instance.md)"
#endif

/* Platform-provided current-instance accessor. Reference impl in sp_ctx.c uses
 * a __thread pointer; the ESP-IDF port (Phase 5) will use a FreeRTOS
 * task-local storage pointer. */
sp_ctx *sp_ctx_current(void);
void    sp_ctx_set_current(sp_ctx *ctx);
#define SP_CTX() sp_ctx_current()

/* Instance lifecycle. */
typedef struct {
  size_t gc_threshold;        /* 0 = default (256 KiB) */
  size_t str_threshold;       /* 0 = default */
  int    root_stack_entries;  /* 0 = default (SP_GC_STACK_MAX) */
  void  *mem_ud;              /* opaque, passed to the hooks below */
  void *(*alloc)(void *ud, size_t);          /* NULL = calloc default; MUST zero */
  void *(*realloc_fn)(void *ud, void *, size_t);
  void  (*dealloc)(void *ud, void *);
} sp_instance_config;

sp_ctx *sp_instance_create(const sp_instance_config *cfg);
void    sp_instance_destroy(sp_ctx *ctx);

/* --- name-compatibility macros: original global -> ctx field --- */
#define sp_str_heap            (SP_CTX()->str_heap)
#define sp_str_heap_bytes      (SP_CTX()->str_heap_bytes)
#define sp_str_threshold       (SP_CTX()->str_threshold)
#define sp_str_threshold_init  (SP_CTX()->str_threshold_init)
#define sp_str_stress_checked  (SP_CTX()->str_stress_checked)
#define sp_gc_threshold        (SP_CTX()->gc_threshold)
#define sp_gc_threshold_init   (SP_CTX()->gc_threshold_init)
#define sp_gc_stress_checked   (SP_CTX()->gc_stress_checked)
#define sp_gc_heap             (SP_CTX()->gc_heap)
#define sp_gc_old_heap         (SP_CTX()->gc_old_heap)
#define sp_gc_bytes            (SP_CTX()->gc_bytes)
#define sp_gc_old_bytes        (SP_CTX()->gc_old_bytes)
#define sp_gc_cycle            (SP_CTX()->gc_cycle)
#define sp_gc_verify           (SP_CTX()->gc_verify)
#define sp_gc_mark_stack       (SP_CTX()->gc_mark_stack)
#define sp_gc_mark_top         (SP_CTX()->gc_mark_top)
#define sp_gc_vsnap            (SP_CTX()->gc_vsnap)
#define sp_gc_vsnap_n          (SP_CTX()->gc_vsnap_n)
#define sp_gc_vsnap_cap        (SP_CTX()->gc_vsnap_cap)
#define sp_gc_max_bytes        (SP_CTX()->gc_max_bytes)
#define sp_gc_max_bytes_init   (SP_CTX()->gc_max_bytes_init)
#define sp_gc_dbg_ctx          (SP_CTX()->gc_dbg_ctx)
#define sp_gc_roots            (SP_CTX()->gc_roots)
#define sp_gc_nroots           (SP_CTX()->gc_nroots)
#define sp_gc_mark_globals_hook          (SP_CTX()->gc_mark_globals_hook)
#define sp_gc_str_sweep_hook             (SP_CTX()->gc_str_sweep_hook)
#define sp_gc_mark_suspended_fibers_hook (SP_CTX()->gc_mark_suspended_fibers_hook)

/* Root-stack capacity: dynamic per instance. */
#define SP_GC_ROOTS_CAP (SP_CTX()->gc_roots_cap)

/* --- allocation wrappers (route through the instance's backend) --- */
void *sp_mem_alloc(size_t n);    /* zero-filled */
void *sp_mem_zalloc(size_t n);   /* zero-filled (alias for clarity) */
void *sp_mem_realloc(void *p, size_t n);
void  sp_mem_free(void *p);

#else  /* !SP_MULTI_CTX -- default: inert, globals stay as-is */

#define SP_GC_ROOTS_CAP SP_GC_STACK_MAX

/* Direct libc; folds to the original call, zero cost. */
#include <stdlib.h>
static inline void *sp_mem_alloc(size_t n)            { return malloc(n); }
static inline void *sp_mem_zalloc(size_t n)           { return calloc(1, n); }
static inline void *sp_mem_realloc(void *p, size_t n) { return realloc(p, n); }
static inline void  sp_mem_free(void *p)              { free(p); }

#endif /* SP_MULTI_CTX */

#endif /* SP_CTX_H */
