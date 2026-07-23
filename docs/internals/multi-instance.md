# Runtime multi-instancing (SP_MULTI_CTX)

Status: design + Phase 1 (state inventory). Implementation lands incrementally.

## Goal

Run several Spinel-compiled programs (e.g. an OS kernel, a desktop, a shell)
inside one process, each with its own independent heap and GC, concurrently on
separate OS threads (FreeRTOS tasks / pthreads). Each program is internally
single-threaded (Spinel's Thread/Fiber concurrency is *not* used); one GC runs
per instance with no cross-instance locking.

Non-goals: running two instances of the *same* compiled program (see
Constraints), and combining `SP_MULTI_CTX` with `SP_THREADS` (a `#error`).

## Key structural fact: two kinds of mutable state

Spinel's mutable runtime state splits cleanly by *where it is compiled*:

1. **Library state — in `lib/*.c`, compiled once into `libspinel_rt.a` and
   shared by every generated translation unit (TU).** This is the state that
   must become per-instance. It lives in `sp_alloc.c` (string/GC heap
   thresholds + string heap list), `sp_gc.c` (GC heap, roots, cycle, mark
   scratch, hooks), `sp_re.c` (last-match state) and `sp_random.c` (RNG state).

2. **TU-local state — `static` in `sp_runtime.h`, which is included *only* by
   the generated TU (verified: no `lib/*.c` includes `sp_runtime.h`).** Every
   compiled program gets its own private copy: exception/catch/break stacks,
   backtrace buffer, format scratch, ARGV cache, at-exit hooks, the object
   comparison hooks, and the user-globals GC bucket table. Different programs
   are therefore already isolated here for free; the same program compiled once
   and run as two instances would *share* these — hence the same-program
   constraint below.

`SP_MULTI_CTX` only needs to relocate category (1) into a per-instance
`sp_ctx`. Category (2) stays exactly as it is. This keeps the change small and
the generated code untouched.

## Global state inventory

Baseline: `make test` = 1991 pass / 1 fail (the 1 fail is the pre-existing
cosmetic `nilclass_bool_ops_conversions` Complex-display case, unrelated).

Legend: **CTX** = move into `sp_ctx`; **TU** = stays TU-static (category 2, no
change); **SHARED** = stays a process global; **OUT** = out of scope
(fiber/scheduler, unused in the embedded target).

### `lib/sp_alloc.c` → CTX

| symbol | note |
|---|---|
| `sp_str_heap`, `sp_str_heap_bytes` | string heap list + live bytes |
| `sp_str_threshold`, `sp_str_threshold_init` | string GC trigger |
| `sp_gc_threshold`, `sp_gc_threshold_init` | object GC trigger |
| `sp_str_stress_checked`, `sp_gc_stress_checked` | stress-env latch (per instance for clean stats) |
| `sp_str_lcache[]` (`SP_TLS`) | small-string cache; program data |
| `sp_heap_lock` (pthread_mutex) | **SHARED / no-op**: one thread per instance means no intra-instance contention; keep the symbol so `SP_THREADS` builds still link, but the multi-ctx fast path takes no lock |

### `lib/sp_gc.c` + `lib/sp_gc.h` → CTX

| symbol | note |
|---|---|
| `sp_gc_heap`, `sp_gc_old_heap` | young/old generation lists |
| `sp_gc_bytes`, `sp_gc_old_bytes`, `sp_gc_cycle` | live accounting |
| `sp_gc_nroots` (`SP_TLS`), `sp_gc_roots[]` | root stack; **default: static array the ctx points at; SP_MULTI_CTX: heap-allocated per `root_stack_entries`** |
| `sp_gc_mark_stack`, `sp_gc_mark_top` | mark scratch |
| `sp_gc_vsnap`, `sp_gc_vsnap_n`, `sp_gc_vsnap_cap` | verify snapshot |
| `sp_gc_verify`, `sp_gc_max_bytes`, `sp_gc_max_bytes_init`, `sp_gc_dbg_ctx` | debug/watermark |
| `sp_gc_mark_globals_hook` | **CTX — critical.** codegen emits `sp_gc_mark_globals_hook = sp_mark_user_globals;` per program (`src/codegen.c`). With one global pointer, a second instance overwrites the first and GC would mark the wrong program's globals. As a ctx field the compat macro routes the assignment (run after `sp_ctx_set_current`) into the right instance automatically — codegen unchanged. |
| `sp_gc_str_sweep_hook` | set once by the `sp_alloc.c` constructor to `sp_str_sweep` (same fn for all); could stay SHARED, but move to CTX for uniformity so an instance's sweep uses its own string heap |
| `sp_gc_mark_suspended_fibers_hook` | OUT normally (fibers unused); leave NULL |

### `lib/sp_re.c` → CTX (all `SP_TLS`)

`sp_re_caps[]`, `sp_re_captures[]`, `sp_re_last_str`, `sp_re_match_str`,
`sp_re_match_pre`, `sp_re_match_post`, `sp_re_last_ncap`, `sp_re_last_pat` —
`$~` / last-match state; per-instance program data.

### `lib/sp_random.c` → CTX (all `SP_TLS`)

`sp_krand_state`, `sp_krand_seeded`, `sp_random_default`, `sp_random_auto_ctr`,
`sp_kernel_seed` — RNG state must be independent so instances are reproducible.

### `lib/sp_str.c` → SHARED

`sp_char_cache[256][3]`, `sp_char_cache_init` — an init-once, deterministic
single-byte-string lookup table. Idempotent fill; keep process-shared. (Under
`SP_THREADS` the existing race is benign — same values.)

### `sp_runtime.h` statics → TU (no change)

`sp_bt_buf/sp_bt_n`, `sp_gc_buckets[]`, `sp_fstr_cap/len`,
`sp_argv_array_cache`, `sp_argf_obj`, `sp_pending_exc_*`, `sp_exc_*` stack,
`sp_catch_*` stack, `sp_brk_*`, `sp_proc_ret_head`, `sp_at_exit_hooks[]`,
`sp_obj_eq/cmp/hash/eql_hook`. Per-TU already; different programs are isolated.

### `lib/sp_fiber.c`, `lib/sp_sched.c` → OUT

Green-thread/scheduler state (`SP_TLS`). Unused in the embedded target;
`SP_MULTI_CTX` + `SP_THREADS` is a `#error`.

## Design

### Mode flag

`SP_MULTI_CTX`:

- **undefined (default)** — behaviourally identical to today. `SP_CTX()`
  resolves to `&sp_ctx_default` (a single static instance) as a constant the
  compiler folds away; state fields keep their current storage so the allocator
  hot path in `sp_runtime.h` sees no indirection. Upstream tests/bench must be
  byte-identical in this mode.
- **defined** — `SP_CTX()` returns the current instance via a platform hook:

  ```c
  /* sp_ctx.h */
  sp_ctx *sp_ctx_current(void);          /* platform-provided */
  void    sp_ctx_set_current(sp_ctx *ctx);
  #define SP_CTX() sp_ctx_current()
  ```

  Reference (POSIX): `static __thread sp_ctx *g_sp_ctx;`. The ESP-IDF port
  (Phase 5) will back these with a FreeRTOS task-local storage pointer.

### Name-compatibility macros (errno style)

`sp_ctx.h` maps each relocated global name onto its ctx field:

```c
#define sp_gc_heap    (SP_CTX()->gc_heap)
#define sp_gc_nroots  (SP_CTX()->gc_nroots)
/* ...one per CTX symbol... */
```

The actual *definitions* (`sp_gc.c` etc.) are deleted and their initial values
move into `sp_ctx_default`'s initializer. lib and generated C reference the
names unchanged, so codegen needs no edit. Any `extern` of a relocated global
must be removed before the macro is in scope; build the whole tree with
`-Werror -Wshadow` once to catch collisions between a macro name and a local.

### Full allocation hooking (mandatory for the ESP32 target)

The fmruby ESP32 target forbids bare `malloc` and requires each task's
`fmrb_mem` pool. So *every* `malloc/calloc/realloc/free` in `lib/` — not just
the GC/string heaps but temporary buffers in `sp_str.c` (format, UTF-8, sub/
gsub), regexp, etc. — routes through thin wrappers:

```c
void *sp_mem_alloc (size_t);   /* default: malloc     ; MC: SP_CTX()->alloc      */
void *sp_mem_zalloc(size_t);   /* default: calloc     ; MC: SP_CTX()->alloc(zeroed)*/
void *sp_mem_realloc(void*, size_t);
void  sp_mem_free(void*);
```

- Default mode expands to the libc call directly (zero cost).
- `alloc` **must zero-fill** (GC relies on calloc semantics).
- Hooks carry `mem_ud` (fmruby passes the task's `ESTALLOC*` TLSF handle;
  `alloc=est_calloc`, `realloc=est_realloc`, `dealloc=est_free`). Same
  allocator and stats path (`mrb_get_estalloc_stats`) as the mruby VM, so
  switching engines does not change the memory layout.
- `malloc_trim` (glibc-only) becomes a no-op in hook mode (as the Darwin branch
  already is).
- CI check: `nm libspinel_rt_mc.a` must show **no** undefined
  `malloc/calloc/realloc/free` references (a `make` target guards regressions).
  Non-allocating libc (`getenv`, …) is exempt.

### Instance API

```c
typedef struct {
  size_t gc_threshold;        /* 0 = default (256 KiB) */
  size_t str_threshold;       /* 0 = default */
  int    root_stack_entries;  /* 0 = default (SP_GC_STACK_MAX) */
  void  *mem_ud;              /* opaque, handed to the hooks below */
  void *(*alloc)(void *ud, size_t);          /* NULL = calloc default; MUST zero */
  void *(*realloc_fn)(void *ud, void *, size_t);
  void  (*dealloc)(void *ud, void *);
} sp_instance_config;

sp_ctx *sp_instance_create(const sp_instance_config *cfg);
void    sp_instance_destroy(sp_ctx *ctx);   /* frees heaps + roots */
```

Library-mode contract (`--no-main`): the host calls
`sp_ctx_set_current(sp_instance_create(&cfg))` **before** invoking the
program's `<name>_entry`; the generated entry is unchanged. `destroy` is unused
by the kernel but required for leak-checking tests.

Threshold contract: `gc_threshold` / `str_threshold` must be set well below the
pool size. If the `alloc` hook returns NULL (pool exhausted) the runtime takes
the existing `sp_oom_die` path.

## Constraints

- **One instance per compiled program.** Category-(2) TU statics are shared by
  all instances of the *same* TU, so two instances of the identical program
  would corrupt each other's exception stack etc. fmruby runs kernel / desktop
  / shell as one instance each, so this is fine. Enforced only by convention;
  the multi-instance test includes a same-program case to document the failure
  mode.
- **`SP_MULTI_CTX` + `SP_THREADS` = `#error`.** The threaded build must still
  compile on its own; the two archives (`libspinel_rt.a`,
  `libspinel_rt_mc.a`) build independently.
- `__attribute__((constructor))` init runs once per process; it must not be
  confused with per-instance init, which lives entirely in
  `sp_instance_create`.
