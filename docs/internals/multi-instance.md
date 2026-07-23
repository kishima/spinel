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
| `sp_gc_mark_globals_hook` | **CTX — critical.** Installed per program: the runtime default `sp_re_mark_globals`, overridden by codegen's `sp_gc_mark_globals_hook = sp_mark_user_globals;` when the program has heap-typed globals. With one global pointer, a second instance overwrites the first and GC would mark the wrong program's globals. As a ctx field the compat macro routes the assignment into the right instance. **The install must run after `sp_ctx_set_current`, so it cannot stay a process constructor** — see "Per-instance TU init" below. |
| `sp_gc_str_sweep_hook` | `sp_str_sweep` (same fn for all instances). Default build: set by the `sp_alloc.c` constructor. MC: `sp_instance_create` sets `c->gc_str_sweep_hook` (the constructor writes a ctx field, which does not exist at process-constructor time). |
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
`fmrb_mem` pool. So *every* `malloc/calloc/realloc/free/strdup` in the runtime —
not just the GC/string heaps but the temporary buffers in `sp_str.c` (format,
UTF-8, sub/gsub), regexp, etc., ~490 sites in all — must route through the
instance backend, **and so must the generated program TU** (its `sp_runtime.h`
inline allocations).

Rather than rewrite ~490 call sites (error-prone, and a standing conflict
against upstream merges), the libc names are remapped with function-like macros
in `lib/sp_mem_override.h`, force-injected into every mc TU via
`-include lib/sp_mem_override.h` (the Makefile `MC_DEF`). This is the same
"errno-style" indirection the runtime globals use — codegen and the sources are
untouched, and the header is **never** included in the default build, so that
build stays byte-identical.

```c
/* lib/sp_mem_override.h (mc build only) */
#include <stdlib.h>            /* real declarations first */
#include <string.h>
void *sp_mem_malloc(size_t);
void *sp_mem_calloc(size_t, size_t);
void *sp_mem_realloc(void *, size_t);
void  sp_mem_free(void *);
char *sp_mem_strdup(const char *);
#define malloc(n)    sp_mem_malloc(n)
#define calloc(a,b)  sp_mem_calloc((a),(b))
#define realloc(p,n) sp_mem_realloc((p),(n))
#define free(p)      sp_mem_free(p)
#define strdup(s)    sp_mem_strdup(s)
```

- The wrappers are **defined once**, in `sp_ctx.c`, which `#undef`s the macros at
  the top so its bodies reach real libc. No other TU may `#undef` them.
- They route through the current instance's backend when one is set, else fall
  back to libc (only a stray pre-entry allocation could hit that, and it must not
  be freed across the boundary). The backend **must zero-fill**
  (`sp_instance_config` contract), so `malloc` and `calloc` collapse onto one
  hook; the modest zero-fill cost is tracked by `make bench`.
- Hooks carry `mem_ud` (fmruby passes the task's `ESTALLOC*` TLSF handle;
  `alloc=est_calloc`, `realloc=est_realloc`, `dealloc=est_free`). Same allocator
  and stats path as the mruby VM, so switching engines does not change the
  memory layout.
- `malloc_trim` (glibc-only) is a no-op under `SP_MULTI_CTX` (and `<malloc.h>` is
  not pulled in, since it would be re-processed after the remap).
- **Boundary — NOT hooked:** stdio internal buffers (`fopen`/`printf`/`getline`
  …) allocate inside libc and go to the system heap; on ESP32 that is newlib
  over the system heap. Only the call form `name(` is remapped, so a bare
  function-pointer reference to `free` (none exist today) stays libc — the nm
  gate would catch it. Non-allocating libc (`getenv`, …) is likewise exempt.
- **nm gate** (`make check-mc-syms`, `test/multi_ctx/check_syms.sh`): every member
  of `libspinel_rt_mc.a` and a freshly built generated program TU must carry
  **zero** undefined references to `malloc/calloc/realloc/free/strdup`
  (+ `reallocarray`/`posix_memalign`/`aligned_alloc`). Only `sp_ctx.o` — the
  definition site — may. This catches a TU that loses the `-include` and silently
  falls back to the shared heap. Wired as a `test-multi-ctx` prerequisite.

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
void    sp_instance_destroy(sp_ctx *ctx);   /* frees the arenas + root/scratch */
```

Library-mode contract (`--no-main`): the host calls
`sp_ctx_set_current(sp_instance_create(&cfg))` **before** invoking the
program's `<name>_entry`; the generated entry is unchanged. `destroy` is unused
by the kernel but required for leak-checking tests. It frees the ctx, the root
stack, the mark scratch and the verify snapshot; the live GC-heap objects are
*not* individually torn down (a hard teardown just drops the arenas — for a pool
backend the whole pool is reclaimed, so this is leak-free by construction).

Threshold contract: `gc_threshold` / `str_threshold` must be set well below the
pool size. If the `alloc` hook returns NULL (pool exhausted) the runtime takes
the existing `sp_oom_die` path (message + `exit`), never a silent NULL deref.

Root stack: `root_stack_entries` bounds live `SP_GC_ROOT` registrations. On ESP32
it should be sized to the program (the default `SP_GC_STACK_MAX` is large — an
idle instance already reserves it). Overflowing it under `SP_MULTI_CTX` calls
`sp_gc_root_overflow_die()` (message + `abort`) rather than dropping a root and
corrupting the heap later; the default build keeps the historical return-0.

### Testing

- `make test-multi-ctx` runs, in order: the nm gate (`check_syms.sh`), the smoke
  test (`smoke.sh` — single instance matches `-E`; N concurrent instances each
  compute the right result; ASan clean), and the estalloc test (`estalloc.sh` —
  pool isolation + stat independence, concurrent instances each on their own
  pool, pool-exhaustion → `sp_oom_die`, root-overflow → abort; the isolation/
  concurrency slice also runs under ASan with leak detection on).
- estalloc (`test/multi_ctx/estalloc/`, BSD-3) is vendored purely as a test
  backend; it is not part of the runtime.

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

### Per-instance TU init (the constructor trap)

A generated program TU installs per-program hooks — the GC globals-mark
(`sp_gc_install_tu_hooks`), the JSON/poly vtable (`sp_json_install_hooks`), and
the string-sweep hook (`sp_alloc.c`) — that in the default build run as
`__attribute__((constructor))` before `main`. Under `SP_MULTI_CTX` those hooks
are per-instance ctx fields reached through `SP_CTX()`, and **no instance is
current at process-constructor time** (`SP_CTX()` is NULL), so a constructor
that writes them dereferences NULL before `main`.

Fix: the hook installers become plain functions under `SP_MULTI_CTX` (the
`SP_TU_CTOR` macro is `__attribute__((constructor))` by default, empty under
MC), and the program entry calls them once the host has made an instance
current:

- `sp_runtime.h` provides `sp_tu_ctx_init()` (MC only), which calls
  `sp_gc_install_tu_hooks()` + `sp_json_install_hooks()`.
- codegen emits `#ifdef SP_MULTI_CTX sp_tu_ctx_init(); #endif` at the top of the
  entry, **before** `sp_re_init()`, so the runtime defaults are installed first
  and `sp_re_init()` then layers the symbol/regex/user-globals overrides on top.
- the string-sweep and `SPINEL_GC_VERIFY` are read in `sp_instance_create`.

All of this is stripped in the default build (guards + macro fold), so the
generated code and runtime stay byte-identical there. Validated by
`test/multi_ctx/smoke.sh` (`make test-multi-ctx`): single-instance output
matches `-E`, N concurrent instances each compute the correct result, clean
under ASan.

### Multi-program linking (T4-0)

To link *different* programs into one binary (an OS's kernel + desktop as
separate Spinel programs in one ELF), a generated TU must export nothing but its
entry. It used to export ~24 non-static symbols the runtime `.a` references, so
two TUs collided at link. These are:

- **4 data** (`sp_trap_state`, `sp_trap_proc`, `_sp_proc_poly_ret`,
  `_sp_proc_poly_args`) → moved to `sp_ctx` fields (`#ifndef SP_MULTI_CTX` drops
  the `sp_runtime.h` definitions; a name macro reaches the field).
- **~20 functions** (`sp_raise_cls`, `sp_sprintf`, `sp_proc_call`, `sp_box_proc`,
  `sp_exc_*`, `sp_signal_*`, `sp_fiber_reraise`, `sp_bigint_raise_zerodiv`) →
  per-instance function pointers in `sp_ctx`, registered by `sp_tu_ctx_init`,
  reached by the runtime through name macros. The `sp_runtime.h` definitions
  become `SP_TU_STATIC` (one private copy per TU); an `#undef` block after the
  ctx include lets the header use the direct names and take their addresses.
  Prototype re-declarations in the runtime `.c`/`.h` are `#ifndef SP_MULTI_CTX`.

Two runtime TUs cannot include `sp_ctx.h` — the regexp engine and the bigint
mruby-shim carry conflicting types (`mrb_bool` etc.) — so they cannot use the
name macros. The three functions they call are therefore provided as real `.a`
globals in `sp_ctx.c`: `sp_raise_cls` / `sp_bigint_raise_zerodiv` forward to the
current instance's registered copy; `sp_sprintf` is program-independent
(`vsnprintf` + the ctx-routed string heap), so one shared definition is correct.

The default build keeps external linkage (`SP_TU_STATIC` empty), so it is
unchanged. `test/multi_ctx/link2.sh` links two programs into one binary and runs
each as its own instance; `check_syms.sh` asserts a generated TU exports no
global but its entry (catches a codegen regression that re-introduces one).

The **objcopy `--prefix-symbols` fallback** (duplicate the runtime per program)
was not needed; it remains the escape hatch if a future symbol proves
un-routable, at the cost of runtime duplication in flash (a Phase-5 concern).
