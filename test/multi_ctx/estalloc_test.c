/* estalloc_test.c -- Phase 3.5 T3.5-3: drive SP_MULTI_CTX instances on a custom
 * (non-libc) allocator, proving the per-instance memory isolation the override
 * enables. Uses estalloc (vendored, BSD-3) as the backend so every runtime
 * allocation lands in a caller-owned fixed buffer, one per instance.
 *
 * Scenarios (see test/multi_ctx/estalloc.sh):
 *   S1 isolation  -- two instances on two pools; the worked one uses more of its
 *                    pool than the idle one, and destroying it leaves the other
 *                    pool untouched (the heaps are genuinely separate).
 *   S2 concurrency-- N threads x M iterations, each on its own pool; all succeed.
 *   S3 exhaustion -- a pool too small for the program: the backend returns NULL
 *                    and the runtime stops via sp_oom_die (exit 1 + message),
 *                    never a silent NULL deref.
 *
 * Linked against the "rich" generated program (sp_prog_entry). Built WITHOUT and
 * WITH AddressSanitizer; because every runtime allocation goes to a static pool
 * buffer (not malloc), a clean ASan+leak run also shows no cross-instance
 * corruption and no libc-level leak. */
#include "sp_gc.h"                 /* sp_instance_config, sp_ctx, lifecycle */
#include "estalloc/estalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

int sp_prog_entry(void);           /* generated "rich" program */

/* estalloc-backed hooks. est_calloc zero-fills, satisfying the sp_instance_config
 * "alloc MUST zero" contract. mem_ud carries the ESTALLOC* for this instance. */
static void *est_alloc_hook(void *ud, size_t n)            { return est_calloc((ESTALLOC*)ud, 1u, (unsigned)n); }
static void *est_realloc_hook(void *ud, void *p, size_t n) { return est_realloc((ESTALLOC*)ud, p, (unsigned)n); }
static void  est_free_hook(void *ud, void *p)              { est_free((ESTALLOC*)ud, p); }

static sp_instance_config est_cfg(ESTALLOC *e) {
  sp_instance_config c; memset(&c, 0, sizeof c);
  c.mem_ud = e; c.alloc = est_alloc_hook; c.realloc_fn = est_realloc_hook; c.dealloc = est_free_hook;
  return c;
}
static unsigned est_used(ESTALLOC *e) { est_take_statistics(e); return e->stat.used; }

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); fails = 1; } } while (0)

#define POOL_SZ (2u * 1024 * 1024)
static _Alignas(16) unsigned char poolA[POOL_SZ];
static _Alignas(16) unsigned char poolB[POOL_SZ];

/* S1: pool isolation + stat independence. */
static void scenario_isolation(void) {
  ESTALLOC *ea = est_init(poolA, POOL_SZ);
  ESTALLOC *eb = est_init(poolB, POOL_SZ);
  sp_instance_config ca = est_cfg(ea), cb = est_cfg(eb);

  sp_ctx *ia = sp_instance_create(&ca);
  sp_ctx_set_current(ia);
  CHECK(sp_prog_entry() == 0, "S1: program entry returned nonzero");

  sp_ctx *ib = sp_instance_create(&cb);       /* created, never run */
  unsigned ua = est_used(ea), ub = est_used(eb);
  CHECK(ua > ub, "S1: worked instance A should use more of its pool than idle B");
  CHECK(ub > 0,  "S1: idle instance B should hold its ctx in its own pool");

  unsigned ub_before = est_used(eb);
  sp_instance_destroy(ia);
  sp_ctx_set_current(ib);
  CHECK(est_used(eb) == ub_before, "S1: destroying A perturbed B's pool");
  sp_instance_destroy(ib);
  sp_ctx_set_current(NULL);
  fprintf(stderr, "  S1: poolA.used=%u poolB.used=%u (separate heaps)\n", ua, ub);
}

/* S2: concurrent instances, each on its own pool, re-init'd per iteration so the
 * pool is reclaimed in bulk (a hard instance teardown frees the arenas, not the
 * live GC objects -- see multi-instance.md). */
#define NT 3
#define ITERS 20
static _Alignas(16) unsigned char poolT[NT][POOL_SZ];
static void *worker(void *arg) {
  int id = (int)(long)arg;
  for (int k = 0; k < ITERS; k++) {
    ESTALLOC *e = est_init(poolT[id], POOL_SZ);
    sp_instance_config cfg = est_cfg(e);
    sp_ctx *c = sp_instance_create(&cfg);
    sp_ctx_set_current(c);
    if (sp_prog_entry() != 0) return (void *)1L;
    sp_instance_destroy(c);
  }
  return (void *)0L;
}
static void scenario_concurrency(void) {
  pthread_t t[NT]; long bad = 0;
  for (int i = 0; i < NT; i++) pthread_create(&t[i], NULL, worker, (void *)(long)i);
  for (int i = 0; i < NT; i++) { void *r; pthread_join(t[i], &r); bad |= (long)r; }
  CHECK(bad == 0, "S2: a concurrent worker iteration failed");
}

/* S3: pool exhaustion -> sp_oom_die (exit 1). Run in a child: sp_oom_die calls
 * exit(1) after printing to stderr. The pool is large enough for create (ctx +
 * a small root stack) but far too small for the program's live set. */
static void scenario_exhaustion(void) {
  pid_t pid = fork();
  if (pid == 0) {
    static _Alignas(16) unsigned char tiny[96 * 1024];
    ESTALLOC *e = est_init(tiny, sizeof tiny);
    sp_instance_config cfg = est_cfg(e);
    cfg.root_stack_entries = 512;             /* keep create's root alloc small */
    sp_ctx *c = sp_instance_create(&cfg);
    if (!c) _exit(1);                         /* create-time OOM is also a clean stop */
    sp_ctx_set_current(c);
    sp_prog_entry();                          /* must overrun the pool -> sp_oom_die */
    _exit(0);                                 /* reached only if the pool sufficed */
  }
  int st; waitpid(pid, &st, 0);
  CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 1,
        "S3: pool exhaustion should stop via sp_oom_die (exit 1), not crash or succeed");
}

int main(void) {
  scenario_isolation();
  scenario_concurrency();
  scenario_exhaustion();
  if (fails) { fprintf(stderr, "estalloc_test: FAIL\n"); return 1; }
  fprintf(stderr, "estalloc_test: PASS (isolation, %d threads x %d iters, exhaustion)\n", NT, ITERS);
  return 0;
}
