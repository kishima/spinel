/* estalloc_root_test.c -- Phase 3.5 T3.5-3 (S4): a per-instance GC root stack
 * sized deliberately small must fail loudly, not corrupt the heap.
 *
 * sp_instance_config.root_stack_entries bounds SP_GC_ROOT registrations. Under
 * SP_MULTI_CTX, overflowing it calls sp_gc_root_overflow_die() -> abort(), so a
 * program that needs more simultaneous roots than the cap dies with SIGABRT and
 * a message rather than silently dropping a root and later hitting a
 * non-deterministic use-after-free.
 *
 * Linked against the "deep" generated program (sp_prog_entry): a recursion that
 * holds a rooted string per frame, so simultaneous roots grow with depth and
 * exceed the cap. Run in a child; the parent asserts death by SIGABRT. */
#include "sp_gc.h"
#include "estalloc/estalloc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <signal.h>

int sp_prog_entry(void);           /* generated "deep" recursive program */

static void *est_alloc_hook(void *ud, size_t n)            { return est_calloc((ESTALLOC*)ud, 1u, (unsigned)n); }
static void *est_realloc_hook(void *ud, void *p, size_t n) { return est_realloc((ESTALLOC*)ud, p, (unsigned)n); }
static void  est_free_hook(void *ud, void *p)              { est_free((ESTALLOC*)ud, p); }

static _Alignas(16) unsigned char pool[1024 * 1024];

int main(void) {
  pid_t pid = fork();
  if (pid == 0) {
    ESTALLOC *e = est_init(pool, sizeof pool);
    sp_instance_config cfg; memset(&cfg, 0, sizeof cfg);
    cfg.mem_ud = e; cfg.alloc = est_alloc_hook; cfg.realloc_fn = est_realloc_hook; cfg.dealloc = est_free_hook;
    cfg.root_stack_entries = 64;              /* deliberately tiny */
    sp_ctx *c = sp_instance_create(&cfg);
    if (!c) _exit(2);
    sp_ctx_set_current(c);
    sp_prog_entry();                          /* recurses past 64 live roots -> abort */
    _exit(0);                                 /* reached only if it never overflowed */
  }
  int st; waitpid(pid, &st, 0);
  if (WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT) {
    fprintf(stderr, "estalloc_root_test: PASS (root-stack overflow aborts cleanly)\n");
    return 0;
  }
  if (WIFEXITED(st))
    fprintf(stderr, "FAIL: root overflow did not abort (child exited %d -- cap not hit?)\n", WEXITSTATUS(st));
  else
    fprintf(stderr, "FAIL: root overflow: child died by signal %d, expected SIGABRT(%d)\n",
            WIFSIGNALED(st) ? WTERMSIG(st) : -1, SIGABRT);
  return 1;
}
