#include "codegen_internal.h"

Buf expr_buf(Compiler *c, int node) {
  Buf b; memset(&b, 0, sizeof b);
  emit_expr(c, node, &b);
  return b;
}

Buf *g_pre = NULL;

/* SP_COLLECT_ERRORS recovery: in collect mode a codegen gap longjmps back to a
   per-unit recovery point armed by the output driver (codegen.c), so one run
   surfaces every unsupported construct instead of aborting on the first. */
jmp_buf g_unsup_recover;
int g_unsup_armed = 0;
int g_unsup_probe = 0;   /* silent emittability probe: longjmp without printing/exiting */
int collect_mode(void) {
  static int collect = -1;
  if (collect < 0) collect = getenv("SP_COLLECT_ERRORS") ? 1 : 0;
  return collect;
}

void buf_putn(Buf *b, const char *s, size_t n) {
  if (b->len + n + 1 > b->cap) {
    size_t nc = b->cap ? b->cap * 2 : 256;
    while (nc < b->len + n + 1) nc *= 2;
    b->p = realloc(b->p, nc);
    b->cap = nc;
  }
  memcpy(b->p + b->len, s, n);
  b->len += n;
  b->p[b->len] = '\0';
}
void buf_puts(Buf *b, const char *s) { buf_putn(b, s, strlen(s)); }
void buf_printf(Buf *b, const char *fmt, ...) {
  char tmp[512];
  va_list ap; va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0) return;
  if ((size_t)n < sizeof(tmp)) { buf_putn(b, tmp, (size_t)n); return; }
  char *big = malloc((size_t)n + 1);
  va_start(ap, fmt); vsnprintf(big, (size_t)n + 1, fmt, ap); va_end(ap);
  buf_putn(b, big, (size_t)n); free(big);
}
int  g_indent = 0;
/* Argument-hoist overrides: emit_args_filled pre-evaluates GC-hazardous
   call arguments into rooted temps; emit_expr then substitutes the temp
   name when it reaches the overridden node. */
int  g_argov_node[MAX_ARG_OVERRIDE];
char g_argov_text[MAX_ARG_OVERRIDE][16];
int  g_n_argov = 0;
/* Node id whose safe-nav (&.) guard is already emitted; the re-entrant
   emit_call skips the guard block for exactly this node. */
int  g_sn_skip = -1;
/* True if evaluating the subtree at `id` may allocate (and so may trigger
   a GC): any call, container literal, or string interpolation qualifies. */
int subtree_may_allocate(const NodeTable *nt, int id) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);
  if (!ty) return 0;
  if (sp_streq(ty, "CallNode") || sp_streq(ty, "ArrayNode") ||
      sp_streq(ty, "HashNode") || sp_streq(ty, "KeywordHashNode") ||
      sp_streq(ty, "InterpolatedStringNode") || sp_streq(ty, "SuperNode") ||
      sp_streq(ty, "ForwardingSuperNode") || sp_streq(ty, "YieldNode"))
    return 1;
  /* A NUL-containing (binary) string literal does not lower to an immortal
     rodata pointer: it allocates a heap string via sp_str_from_bytes (every
     evaluation when unfrozen, or once to fill a call-site cache when frozen).
     Either path can trigger a GC, so it must count as an allocating sibling
     so the operand-rooting logic protects a fresh operand next to it. A plain
     (NUL-free) literal is rodata and never allocates. */
  if (sp_streq(ty, "StringNode")) {
    const char *sc = nt_str(nt, id, "content");
    if (sc && nt_str_len(nt, id, "content") > strlen(sc)) return 1;
    return 0;
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_may_allocate(nt, nt_ref_at(nt, id, i))) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_may_allocate(nt, ids[j])) return 1;
  }
  return 0;
}
int  g_tmp = 0;
char g_ren_from[MAX_RENAME][96];
char g_ren_to[MAX_RENAME][112];
int  g_nren = 0;
int  g_block_id = -1;
int  g_yield_block_fallback = -1;
/* The (g_self, g_self_deref) that were active when the current g_block_id
   was captured -- i.e. the caller context of the innermost yield-method
   inline. A block spliced at a `yield` is caller code: emit_block_invoke
   emits its body under these instead of the inlined method's rebound self
   (`@map.vertices[...]` inside a block passed to an inlined method must
   read the CALLER's @map). Maintained by the inliners exactly like
   g_yield_block_fallback. */
const char *g_yield_self_fallback = NULL;
const char *g_yield_self_deref_fallback = NULL;
/* Companion to g_yield_self_fallback: the CALLER's emitting-class, so a
   block body spliced into an inlined callee resolves its implicit-self
   calls against the caller's class (the block is caller code). */
int g_yield_emitting_class_fallback = -1;
const char *g_block_param_name = NULL;
/* Inside an Enumerator.new { |y| ... } generator body, the name of the yielder
   block param. A `y << v` / `y.yield(v)` on it lowers to a Fiber.yield. */
const char *g_yielder_name = NULL;
const char *g_self = "self";
/* Member-access operator for `self`: "->" when self is a pointer (the usual
   heap object), "." when emitting a value-type method body (self is a value). */
const char *g_self_deref = "->";
/* When set, emit_inline_call_x binds self from this pre-hoisted expression
   instead of re-emitting the receiver node -- used by the poly-receiver block
   dispatch to bind self to a per-arm cast of the boxed receiver (#2448). */
const char *g_inline_recv_expr = NULL;
int g_inline_recv_class = -1;
int g_class_body_id = -1;
int g_emitting_class_id = -1;
const char *g_dm_subst_name = NULL;
int g_dm_subst_node = -1;
int g_ie_class_id = -1;
int g_ie_discard_value = 0;
const char *g_rescue_cls = NULL, *g_rescue_msg = NULL;
const char *g_retry_label = NULL;
int g_redo_stack[64];
int g_redo_depth = 0;
const char *g_loop_break_var = NULL;
/* When a direct instance_exec/eval splice is wrapped in a do{}while(0), this
   holds the C result temp so a top-level `next <v>` captures its value before
   continuing out of the splice (mirrors g_loop_break_var for `break`). */
const char *g_ie_next_var = NULL;
/* C-loop nesting depth within the current function body: `next` emits a plain
   `continue` only when inside a C loop; at depth 0 inside a proc function it
   is the proc's own return (Ruby block semantics), not a loop control. */
int g_c_loop_depth = 0;
int g_in_proc_body = 0;
/* Set while emitting an instance_exec/eval splice whose result temp is poly:
   a `break <v>` / `next <v>` carrying a scalar value must box it to match. */
int g_ie_res_poly = 0;
/* Set while emitting a block body wrapped in a valued-break setjmp scope:
   the C lvalue holding the enclosing scope's SERIAL, so a top-level
   `break <v>` lowers to sp_brk_throw(<serial>, v) rather than a C `break`.
   NULLed when entering a nested C loop (while/for/until/loop) or another
   emission context (method/proc/fiber body, instance_exec splice) whose own
   `break` must not target this scope. Non-lambda procs capture its value at
   creation as their break home. */
const char *g_brk_ser_var = NULL;
/* g_ensure_depth at the enclosing wrapper: a break at the SAME depth has no
   intervening ensure bodies and delivers by same-function `goto` (register-
   safe: a longjmp would roll back register-allocated locals mutated since
   the setjmp -- the known hazard of the catch/throw machinery); a deeper
   break longjmps via sp_brk_throw so the ensures run. */
int g_brk_ensure_base = 0;
/* The break-scope serial var bound to the CURRENT block (g_block_id): saved
   at the call site when a yielding method is inlined, re-installed while the
   block body is spliced at a yield site -- so a `break` in the block targets
   the call that received it even when the yield sits inside the method's own
   loops or nested iterators. Paired with g_yield_block_fallback the same way
   g_block_id is. */
const char *g_block_brk_var = NULL;
const char *g_yield_blk_brk_fallback = NULL;
int g_block_brk_ebase = 0;
int g_yield_blk_brk_efallback = 0;
/* Proc-literal body context: 1 = lambda (break returns from the lambda),
   2 = non-lambda proc with a break (targets its captured home scope). */
int g_proc_body_kind = 0;
/* C expression for the non-lambda proc's captured break-home serial. */
const char *g_proc_brk_home = NULL;
/* The CallNode id currently being emitted as the inner (unwrapped) call of its
   own break wrapper, so the wrapper is not re-entered recursively. */
int g_brk_skip_id = -1;
const char *g_result_var = NULL;
int g_result_poly = 0;
/* Non-lambda proc `return` support. While emitting a method that owns a
   proc-return frame, g_method_pr_label / g_method_pr_var name the single-exit
   goto label and the value var, so an explicit `return` funnels there (popping
   the frame once) instead of returning directly. While emitting a returning
   proc's body, g_proc_return_home is the C expression for the home frame index
   (read from the proc capture), so `return` longjmps to the home method. */
const char *g_method_pr_label = NULL;
const char *g_method_pr_var = NULL;
const char *g_proc_return_home = NULL;
/* Number of live setjmp exception frames (begin/rescue) enclosing the
   current emission point. A `return` from inside a try body must pop them
   (sp_exc_top -= N) before leaving -- a stale frame's jmp_buf points into
   a dead C stack frame, and the next raise longjmps into it (doom's
   SoundManager#[] early returns corrupted the stack this way).
   g_method_pr_exc_depth snapshots the depth at the return-funnel target so
   funnel gotos pop only the frames they actually exit. */
int g_exc_frame_depth = 0;
int g_loop_exc_base = 0;        /* frame depth at the innermost C-loop entry */
int g_loop_ensure_base = 0;     /* ensure depth at the innermost C-loop entry */
int g_brk_exc_base = 0;         /* frame depth at the valued-break wrapper */
int g_block_brk_exc_base = 0;   /* ... for yield-block re-entry (mirrors g_block_brk_ebase) */
int g_method_pr_exc_depth = 0;
/* Loop-invariant string-length hoisting: while a loop whose receiver string is
   not mutated in its body is being emitted, g_hoist_len_recv holds that
   receiver's AST local name and g_hoist_len_var the C temp caching its length;
   a matching `s.length`/`s.size` then emits the temp instead of strlen. */
const char *g_hoist_len_var = NULL;
const char *g_hoist_len_recv = NULL;
TyKind g_ret_type = TY_UNKNOWN;
/* Mirror of the REAL enclosing function's return funnel: yield-method inlining
   overrides g_method_pr_label/-_var/g_ret_type with a per-inline funnel, and a
   spliced block body (which lexically belongs to the real function, so its
   `return` exits that method) restores from these. Set wherever a fresh
   function context installs (or clears) its funnel. */
const char *g_fn_pr_label = NULL;
const char *g_fn_pr_var = NULL;
TyKind g_fn_ret_type = TY_UNKNOWN;
int g_current_scope_is_lowered = 0;
/* When a yielding method is inlined and its block is a forwarded REAL proc
   (the caller nil-checks its &block, so the block can't be an inlined literal),
   this holds the C expression for that proc; the inlined `yield` calls it via
   sp_proc_call instead of splicing a block body. NULL otherwise. */
const char *g_yield_proc_ref = NULL;
/* The inlined call's return-slot type while g_yield_proc_ref is set: sp_proc_call
   yields poly, but the slot may be concrete (the analyzer typed this forwarding
   context), so a value-position yield unboxes its result to this. */
TyKind g_yield_slot_ty = TY_UNKNOWN;
EnsureCtx g_ensure_stack[MAX_ENSURE_DEPTH];
int       g_ensure_depth = 0;
RescueSave g_rescue_save_stack[MAX_ENSURE_DEPTH];
int        g_rescue_save_depth = 0;

/* rescue bodies crossed by an exit to frame-depth pop_base: those entered at or
   deeper than pop_base (their exc_base >= pop_base). */
int rescues_crossed(int pop_base) {
  int k = 0;
  for (int i = 0; i < g_rescue_save_depth; i++)
    if (g_rescue_save_stack[i].exc_base >= pop_base) k++;
  return k;
}
/* Pop the k crossed rescue-body handlers (no frame pop). Used at sites whose
   frame-pop text is special (the begin..ensure deferred-return). */
void emit_cur_exc_restore(Buf *b, int pop_base) {
  int k = rescues_crossed(pop_base);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
}
int emit_frame_unwind(Buf *b, int pop_base, const char *guard) {
  int pops = g_exc_frame_depth - pop_base;
  int k = rescues_crossed(pop_base);
  if (pops <= 0 && k == 0) return 0;
  if (guard) buf_printf(b, "if (%s) { ", guard);
  if (pops > 0) buf_printf(b, "sp_exc_top -= %d; ", pops);
  if (k > 0) buf_printf(b, "sp_rescue_sp -= %d; ", k);
  if (guard) buf_puts(b, "}");
  return 1;
}
Buf g_procs;
Buf g_proc_protos;
int g_proc_counter = 0;
int g_needs_proc_poly_argslot = 0; /* any proc takes a TY_POLY arg via _sp_proc_poly_args */
/* Fiber body functions accumulate here (similar to g_procs but void(*)(sp_Fiber*)). */
int g_fiber_counter = 0;
char **g_re_src; int *g_re_flg; int g_re_count, g_re_cap;
int re_engine_flags(int pf) {
  int f = 0;
  if (pf & 4) f |= 1;
  if (pf & 8) f |= 8;
  if (pf & 16) f |= 6;
  return f;
}
int re_has_captures(const char *src) {
  if (!src) return 0;
  for (const char *p = src; *p; p++) {
    if (*p == '\\') { if (p[1]) p++; continue; }
    if (*p == '(') {
      if (p[1] != '?') return 1;
      /* (?<name>...) / (?'name'...) are named CAPTURE groups (lookbehinds
         (?<= (?<! are not) */
      if (p[1] == '?' && p[2] == '<' && p[3] != '=' && p[3] != '!') return 1;
      if (p[1] == '?' && p[2] == 0x27) return 1;
    }
  }
  return 0;
}
int re_lit_index(Compiler *c, int nid) {
  if (nid < 0) return -1;
  const char *ty = nt_type(c->nt, nid);
  if (!ty) return -1;
  /* a constant bound to a regex literal (PATTERN = /re/[.freeze], possibly
     namespaced) resolves to that literal's precompiled pattern */
  if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
    const char *nm = nt_str(c->nt, nid, "name");
    if (!nm) return -1;
    for (int k = 0; k < c->nt->count; k++) {
      const char *kt = nt_type(c->nt, k);
      if (!kt || (!sp_streq(kt, "ConstantWriteNode") && !sp_streq(kt, "ConstantPathWriteNode"))) continue;
      const char *kn = nt_str(c->nt, k, "name");
      if (!kn || !sp_streq(kn, nm)) continue;
      int v = nt_ref(c->nt, k, "value");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "CallNode") &&
          nt_str(c->nt, v, "name") && sp_streq(nt_str(c->nt, v, "name"), "freeze"))
        v = nt_ref(c->nt, v, "receiver");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "RegularExpressionNode"))
        return re_lit_index(c, v);
    }
    return -1;
  }
  /* a local variable of type TY_REGEX: look up its write node */
  if (sp_streq(ty, "LocalVariableReadNode") && comp_ntype(c, nid) == TY_REGEX) {
    const char *nm = nt_str(c->nt, nid, "name");
    if (!nm) return -1;
    for (int k = 0; k < c->nt->count; k++) {
      const char *kt = nt_type(c->nt, k);
      if (!kt || !sp_streq(kt, "LocalVariableWriteNode")) continue;
      const char *kn = nt_str(c->nt, k, "name");
      if (!kn || !sp_streq(kn, nm)) continue;
      int v = nt_ref(c->nt, k, "value");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "RegularExpressionNode"))
        return re_lit_index(c, v);
    }
    return -1;
  }
  if (!sp_streq(ty, "RegularExpressionNode")) return -1;
  const char *src = nt_str(c->nt, nid, "unescaped");
  if (!src) return -1;
  int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
  for (int i = 0; i < g_re_count; i++)
    if (g_re_flg[i] == flg && sp_streq(g_re_src[i], src)) return i;
  if (g_re_count >= g_re_cap) {
    g_re_cap = g_re_cap ? g_re_cap * 2 : 8;
    g_re_src = realloc(g_re_src, sizeof(char *) * (size_t)g_re_cap);
    g_re_flg = realloc(g_re_flg, sizeof(int) * (size_t)g_re_cap);
  }
  g_re_src[g_re_count] = (char *)src;
  g_re_flg[g_re_count] = flg;
  return g_re_count++;
}
const char *re_lit_src(Compiler *c, int nid) {
  if (nid < 0) return NULL;
  const char *ty = nt_type(c->nt, nid);
  if (!ty) return NULL;
  if (sp_streq(ty, "RegularExpressionNode")) return nt_str(c->nt, nid, "unescaped");
  if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
    const char *nm = nt_str(c->nt, nid, "name");
    if (!nm) return NULL;
    for (int k = 0; k < c->nt->count; k++) {
      const char *kt = nt_type(c->nt, k);
      if (!kt || (!sp_streq(kt, "ConstantWriteNode") && !sp_streq(kt, "ConstantPathWriteNode"))) continue;
      const char *kn = nt_str(c->nt, k, "name");
      if (!kn || !sp_streq(kn, nm)) continue;
      int v = nt_ref(c->nt, k, "value");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "CallNode") &&
          nt_str(c->nt, v, "name") && sp_streq(nt_str(c->nt, v, "name"), "freeze"))
        v = nt_ref(c->nt, v, "receiver");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "RegularExpressionNode"))
        return nt_str(c->nt, v, "unescaped");
    }
  }
  return NULL;
}
/* Prism flags of a literal/const-bound regexp (-1 if `nid` is not one). Mirrors
   re_lit_src's resolution so callers can check a regex operand for flags. */
int re_lit_flags(Compiler *c, int nid) {
  if (nid < 0) return -1;
  const char *ty = nt_type(c->nt, nid);
  if (!ty) return -1;
  if (sp_streq(ty, "RegularExpressionNode")) return (int)nt_int(c->nt, nid, "flags", 0);
  if (sp_streq(ty, "ConstantReadNode") || sp_streq(ty, "ConstantPathNode")) {
    const char *nm = nt_str(c->nt, nid, "name");
    if (!nm) return -1;
    for (int k = 0; k < c->nt->count; k++) {
      const char *kt = nt_type(c->nt, k);
      if (!kt || (!sp_streq(kt, "ConstantWriteNode") && !sp_streq(kt, "ConstantPathWriteNode"))) continue;
      const char *kn = nt_str(c->nt, k, "name");
      if (!kn || !sp_streq(kn, nm)) continue;
      int v = nt_ref(c->nt, k, "value");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "CallNode") &&
          nt_str(c->nt, v, "name") && sp_streq(nt_str(c->nt, v, "name"), "freeze"))
        v = nt_ref(c->nt, v, "receiver");
      if (v >= 0 && nt_type(c->nt, v) && sp_streq(nt_type(c->nt, v), "RegularExpressionNode"))
        return (int)nt_int(c->nt, v, "flags", 0);
    }
  }
  return -1;
}
void emit_interp(Compiler *c, int id, Buf *b);  /* forward */

/* Emit a regex pattern expression to `b`, handling both static literals and
   interpolated patterns. For interpolated patterns, setup is emitted to
   g_pre and a temp mrb_regexp_pattern* variable name is written to `b`.
   Returns 1 if handled, 0 if nid is not a recognizable regex. */
int emit_regex_pat_to_buf(Compiler *c, int nid, Buf *b) {
  int ri = re_lit_index(c, nid);
  if (ri >= 0) { buf_printf(b, "sp_re_pat_%d", ri); return 1; }
  const char *ty = nt_type(c->nt, nid);
  if (ty && sp_streq(ty, "InterpolatedRegularExpressionNode")) {
    int flg = re_engine_flags((int)nt_int(c->nt, nid, "flags", 0));
    int ts = ++g_tmp, tp = ++g_tmp;
    /* Emit the interpolated pattern into a local buffer: an embedded call that
       roots its own args pushes those decls to g_pre, which must land as whole
       statements before this temp's decl, not inside its initializer (#1498). */
    Buf pv; memset(&pv, 0, sizeof pv);
    emit_interp(c, nid, &pv);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "const char *_t%d = %s;\n", ts, pv.p ? pv.p : "\"\"");
    free(pv.p);
    emit_indent(g_pre, g_indent);
    buf_printf(g_pre, "mrb_regexp_pattern *_t%d = re_compile(_t%d, (int64_t)strlen(_t%d), %d);\n", tp, ts, ts, flg);
    buf_printf(b, "_t%d", tp);
    return 1;
  }
  return 0;
}
int nameset_has(NameSet *s, const char *nm) {
  if (!nm) return 0;
  for (int i = 0; i < s->n; i++) if (sp_streq(s->v[i], nm)) return 1;
  return 0;
}
void nameset_add(NameSet *s, const char *nm) {
  if (!nm || nameset_has(s, nm)) return;
  if (s->n >= s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->v = realloc(s->v, sizeof(char *) * (size_t)s->cap); }
  s->v[s->n++] = nm;
}
const char *g_cap_struct = NULL;
NameSet *g_cap_names = NULL;
int g_needs_at_exit = 0;
int g_needs_class_machinery = 0;
int g_has_user_global_marks = 0;
/* Per-instance reset of this TU's file-scope statics (SP_MULTI_CTX). Built
   alongside the GC mark list so the two cannot drift; emitted after the class
   bodies, once the pooled classes are known. */
char *g_tu_reset_globals = NULL;
NameSet *g_pool_classes = NULL;
int g_tu_has_reset = 0;
int g_uses_symbols = 0;
int g_uses_marshal = 0;
int g_emit_sym_rt = 0;
int g_emit_class_names = 0;
int g_emit_obj_dispatch = 0;
int g_uses_program_name = 0;
int g_gen_obj_hash = 0;
int g_uses_regex = 0;
int g_uses_argv = 0;
int g_uses_threads = 0;
int g_has_user_cmp = 0;
int g_gen_obj_hashkey = 0;
int g_gen_obj_valeq = 0;
int g_re_init_needed = 0;
void emit_local_ref(Compiler *c, int scope_node, const char *name, Buf *b) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, name)) {
    /* A TY_PROC capture is stored as (mrb_int)(uintptr_t)sp_Proc* in the cell.
       Cast it back to sp_Proc* so call sites work. A heap-object cell is a real
       typed pointer, so its deref is already the right lvalue (no cast). */
    LocalVar *clv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
    if (clv && clv->type == TY_PROC)
      buf_printf(b, "(sp_Proc *)(uintptr_t)(*((%s *)_cap)->%s)", g_cap_struct, name);
    else
      buf_printf(b, "(*((%s *)_cap)->%s)", g_cap_struct, name);
    return;
  }
  LocalVar *lv = scope_node >= 0 ? scope_local(comp_scope_of(c, scope_node), name) : NULL;
  if (lv && lv->is_cell) {
    if (lv->type == TY_PROC) buf_printf(b, "(sp_Proc *)(uintptr_t)(*_cell_%s)", name);
    else buf_printf(b, "(*_cell_%s)", name);
    return;
  }
  buf_printf(b, "lv_%s", rename_local(name));
}
void emit_yblk_ref(Buf *b) {
  if (g_cap_struct && g_cap_names && nameset_has(g_cap_names, "__yblk__")) {
    buf_printf(b, "(sp_Proc *)(uintptr_t)(*(((%s *)_cap)->__yblk__))", g_cap_struct);
  }
  else {
    buf_puts(b, "lv___yblk__");
  }
}
void emit_tail_lead(Buf *b) {
  if (g_result_var) buf_printf(b, "%s = ", g_result_var);
  else buf_puts(b, "return ");
}
/* The C representation of Ruby `nil` for a concretely-typed slot (vs
   default_value's zero-value): a fresh block-local starts nil, and several
   types carry an in-band nil sentinel (NULL string, SP_INT_NIL, NaN float,
   (sp_sym)-1). Types with no sentinel fall back to the zero value. */
static const char *nil_value(TyKind t) {
  switch (t) {
    case TY_STRING: return "NULL";
    case TY_INT:    return "SP_INT_NIL";
    case TY_FLOAT:  return "sp_float_nil()";
    case TY_POLY:   return "sp_box_nil()";
    default:        return NULL;
  }
}

static int subtree_has_param_named(const NodeTable *nt, int id, const char *nm) {
  if (id < 0) return 0;
  const char *ty = nt_type(nt, id);

  /* numbered params (_1.._9): the NumberedParametersNode carries no child
     parameter nodes, yet the block's locals list contains the names. */
  if (ty && sp_streq(ty, "NumberedParametersNode") &&
      nm[0] == '_' && nm[1] >= '1' && nm[1] <= '9' && !nm[2]) return 1;
  if (ty && sp_streq(ty, "ItParametersNode") && sp_streq(nm, "it")) return 1;
  if (ty && (strstr(ty, "ParameterNode") || sp_streq(ty, "LocalVariableTargetNode"))) {
    const char *pn = nt_str(nt, id, "name");
    /* shadow-renaming rewrites a param's node-table name to NAME__bpNN;
       the parser's locals list keeps the raw NAME -- match both. */
    if (pn) {
      size_t nl = strlen(nm);
      if (sp_streq(pn, nm)) return 1;
      if (!strncmp(pn, nm, nl) && !strncmp(pn + nl, "__bp", 4)) return 1;
    }
  }
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++)
    if (subtree_has_param_named(nt, nt_ref_at(nt, id, i), nm)) return 1;
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0;
    const int *ids = nt_arr_at(nt, id, i, &n);
    for (int j = 0; j < n; j++)
      if (subtree_has_param_named(nt, ids[j], nm)) return 1;
  }
  return 0;
}

void emit_block_locals_reset(Compiler *c, int blk, Buf *b, int indent) {
  if (blk < 0) return;
  const char *locs = nt_str(c->nt, blk, "locals");
  if (!locs || !*locs) return;
  char tmpn_buf[128];
  const char *p = locs;
  while (*p) {
    const char *e = strchr(p, ',');
    size_t l = e ? (size_t)(e - p) : strlen(p);
    if (l) {
      /* names longer than the stack buffer are legal Ruby; heap-fall back
         rather than silently skipping the reset */
      char *tmpn = l < sizeof tmpn_buf ? tmpn_buf : malloc(l + 1);
      if (!tmpn) break;
      memcpy(tmpn, p, l); tmpn[l] = 0;
      /* Skip every name bound by the block's parameter list, including
         destructured, optional, rest, and shadow (`; x`) declarations --
         collected straight from the parameters subtree, since
         block_param_name only covers plain leading params. */
      int is_param = subtree_has_param_named(c->nt, nt_ref(c->nt, blk, "parameters"), tmpn);

      if (!is_param) {
        Scope *sc = comp_scope_of(c, blk);
        LocalVar *lv = sc ? scope_local(sc, tmpn) : NULL;
        if (lv && lv->type != TY_UNKNOWN && !lv->is_cell) {
          /* A value-type object local is a C struct (`sp_X lv_x`), so the
             pointer nil "NULL" that default_value returns for object types
             does not compile (`lv_x = NULL`). Reset it with the zero compound
             literal instead, matching its declaration initializer. */
          char vtbuf[128]; const char *nv = NULL;
          if (ty_is_object(lv->type)) {
            int cid = ty_object_class(lv->type);
            if (cid >= 0 && cid < c->nclasses && c->classes[cid].is_value_type) {
              snprintf(vtbuf, sizeof vtbuf, "(sp_%s){0}", c->classes[cid].c_name);
              nv = vtbuf;
            }
          }
          if (!nv) nv = nil_value(lv->type);
          if (!nv) nv = lv->type == TY_RANGE ? "(sp_Range){0}" : default_value(lv->type);
          emit_indent(b, indent);
          buf_printf(b, "lv_%s = %s;\n", rename_local(tmpn), nv);
        }
      }
      if (tmpn != tmpn_buf) free(tmpn);
    }
    if (!e) break;
    p = e + 1;
  }
}

const char *rename_local(const char *nm) {
  for (int i = 0; i < g_nren; i++)
    if (sp_streq(g_ren_from[i], nm)) return g_ren_to[i];
  return nm;
}
/* Report a feature spinel deliberately does not support (docs/limitations.md).
   Unlike `unsupported`, which describes a codegen gap and dumps the node so the
   compiler can be debugged, this names the feature and stops: the internals are
   noise when the answer is "this is a documented limit". #2652 / #2667 / #2668 */
__attribute__((noreturn)) void unsupported_feature(Compiler *c, int id, const char *msg) {
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  char pos[1200]; pos[0] = 0;
  if (ln > 0) {
    int fid = (int)nt_int(c->nt, id, "node_file", 0);
    const char *file = nt_file_path(c->nt, fid);
    if (!file || !*file) file = c->nt->source_file;
    if (!file || !*file) file = "source.rb";
    snprintf(pos, sizeof pos, "%s:%d: ", file, ln);
  }
  fprintf(stderr, "spinel: %s%s\n", pos, msg);
  if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  exit(1);
}

__attribute__((noreturn)) void unsupported(Compiler *c, int id, const char *what) {
  /* Silent emittability probe (dynamic-send arm selection): unwind without a
     diagnostic, the caller just drops this arm. */
  if (g_unsup_probe) longjmp(g_unsup_recover, 1);
  const char *ty = nt_type(c->nt, id);
  /* Ruby-map the diagnostic (#1338): a codegen gap reports against the source
     line the parser stamped (the same position the #line machinery uses), so
     the message is anchored to the .rb file instead of an opaque node id.
     Falls back to the bare form when the position wasn't stamped. */
  int ln = (int)nt_int(c->nt, id, "node_line", 0);
  char pos[1200]; pos[0] = 0;
  if (ln > 0) {
    int fid = (int)nt_int(c->nt, id, "node_file", 0);
    const char *file = nt_file_path(c->nt, fid);
    if (!file || !*file) file = c->nt->source_file;
    if (!file || !*file) file = "source.rb";
    snprintf(pos, sizeof pos, "%s:%d: ", file, ln);
  }
  const char *mname = ty && sp_streq(ty, "CallNode") ? nt_str(c->nt, id, "name") : NULL;
  if (mname) {
    int recv = nt_ref(c->nt, id, "receiver");
    int args = nt_ref(c->nt, id, "arguments");
    int ac = 0; const int *av = args >= 0 ? nt_arr(c->nt, args, "arguments", &ac) : NULL;
    /* A bare unresolved identifier (Prism variable-call: no receiver, no
       parens, no args) is CRuby's NameError -- an undefined local that fell
       through to method lookup. Name the enclosing class the way CRuby does. */
    if (recv < 0 && ac == 0 && nt_ref(c->nt, id, "block") < 0 &&
        nt_int(c->nt, id, "vcall", 0)) {
      const char *cn = g_emitting_class_id >= 0 ? class_ruby_name(c, g_emitting_class_id) : NULL;
      fprintf(stderr, "spinel: %sundefined local variable or method '%s' for %s%s (NameError)\n",
              pos, mname, cn ? "an instance of " : "main", cn ? cn : "");
      if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
      exit(1);
    }
    /* A call on a typed user object whose class chain has no such method is
       not a compiler gap: it is the program's NoMethodError, caught ahead of
       time. Report it in CRuby's words instead of the internal node dump. */
    if (recv >= 0) {
      TyKind rvt = comp_ntype(c, recv);
      if (ty_is_object(rvt)) {
        int cid = ty_object_class(rvt);
        if (cid >= 0 && cid < c->nclasses && !c->classes[cid].is_native_class &&
            comp_method_in_chain(c, cid, mname, NULL) < 0) {
          const char *cn = class_ruby_name(c, cid);
          fprintf(stderr, "spinel: %sundefined method '%s' for an instance of %s (NoMethodError)\n",
                  pos, mname, cn ? cn : "Object");
          if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
          exit(1);
        }
      }
    }
    fprintf(stderr, "spinel: %sunsupported %s: node %d (%s `%s`) recv=%s/ty%d argc=%d",
            pos, what, id, ty, mname,
            recv >= 0 ? nt_type(c->nt, recv) : "-",
            recv >= 0 ? (int)comp_ntype(c, recv) : -1, ac);
    if (ac > 0 && av) fprintf(stderr, " arg0ty%d", (int)comp_ntype(c, av[0]));
    fprintf(stderr, "\n");
  }
  else
    fprintf(stderr, "spinel: %sunsupported %s: node %d (%s)\n",
            pos, what, id, ty ? ty : "?");
  /* SP_COLLECT_ERRORS: don't abort on the first gap -- longjmp back to the
     driver's per-unit recovery point (when armed) so one run surfaces every
     unsupported construct, abandoning just this unit's (discarded) output.
     `unsupported` thus never returns: it exits, or longjmps. */
  if (collect_mode() && g_unsup_armed) longjmp(g_unsup_recover, 1);
  exit(1);
}
int builtin_class_id(const char *name) {
  if (!name) return 0;
  if (sp_streq(name, "Integer"))     return -100;
  if (sp_streq(name, "Float"))       return -101;
  if (sp_streq(name, "String"))      return -102;
  if (sp_streq(name, "Symbol"))      return -103;
  if (sp_streq(name, "Array"))       return -104;
  if (sp_streq(name, "Hash"))        return -105;
  if (sp_streq(name, "Range"))       return -106;
  if (sp_streq(name, "Time"))        return -107;
  if (sp_streq(name, "Module"))      return -108;
  if (sp_streq(name, "Class"))       return -109;
  if (sp_streq(name, "NilClass"))    return -110;
  if (sp_streq(name, "TrueClass"))   return -111;
  if (sp_streq(name, "FalseClass"))  return -112;
  if (sp_streq(name, "Numeric"))     return -113;
  if (sp_streq(name, "Comparable"))  return -114;
  if (sp_streq(name, "Enumerable"))  return -115;
  if (sp_streq(name, "Object"))      return -116;
  if (sp_streq(name, "BasicObject")) return -117;
  if (sp_streq(name, "Proc"))        return -118;
  if (sp_streq(name, "Kernel"))      return -119;
  if (sp_streq(name, "IO"))          return -120;
  if (sp_streq(name, "File"))        return -121;
  if (sp_streq(name, "Exception"))   return -122;
  if (sp_streq(name, "StandardError")) return -123;
  if (sp_streq(name, "RuntimeError")) return -124;
  if (sp_streq(name, "TypeError"))   return -125;
  if (sp_streq(name, "ArgumentError")) return -126;
  if (sp_streq(name, "NameError"))   return -127;
  if (sp_streq(name, "NoMethodError")) return -128;
  if (sp_streq(name, "StopIteration")) return -129;
  if (sp_streq(name, "Math"))        return -130;
  if (sp_streq(name, "Complex"))     return -131;
  if (sp_streq(name, "IndexError"))  return -132;
  if (sp_streq(name, "KeyError"))    return -133;
  if (sp_streq(name, "RangeError"))  return -134;
  if (sp_streq(name, "FloatDomainError")) return -135;
  if (sp_streq(name, "ZeroDivisionError")) return -136;
  if (sp_streq(name, "FrozenError")) return -137;
  if (sp_streq(name, "IOError"))     return -138;
  if (sp_streq(name, "LocalJumpError")) return -139;
  if (sp_streq(name, "NotImplementedError")) return -140;
  if (sp_streq(name, "ScriptError")) return -141;
  if (sp_streq(name, "Rational"))    return -142;
  if (sp_streq(name, "Regexp"))      return -143;
  if (sp_streq(name, "Enumerator"))  return -144;
  if (sp_streq(name, "Struct"))      return -145;
  if (sp_streq(name, "Data"))        return -146;
  if (sp_streq(name, "SyntaxError")) return -147;
  if (sp_streq(name, "SecurityError")) return -148;
  if (sp_streq(name, "RegexpError")) return -149;
  if (sp_streq(name, "EncodingError")) return -150;
  if (sp_streq(name, "SignalException")) return -151;
  if (sp_streq(name, "Interrupt"))   return -152;
  if (sp_streq(name, "ThreadError")) return -153;
  if (sp_streq(name, "FiberError"))  return -154;
  if (sp_streq(name, "ClosedQueueError")) return -155;
  if (sp_streq(name, "UncaughtThrowError")) return -156;
  if (sp_streq(name, "NoMatchingPatternError")) return -157;
  if (sp_streq(name, "NoMatchingPatternKeyError")) return -158;
  if (sp_streq(name, "EOFError"))    return -159;
  if (sp_streq(name, "Math::DomainError")) return -160;
  if (sp_streq(name, "SystemExit"))  return -161;
  if (sp_streq(name, "Signal"))      return -162;
  if (sp_streq(name, "Process::Status")) return -163;
  return 0;
}
const char *c_type_name(TyKind t) {
  if (ty_is_obj_array(t)) return "sp_PtrArray *";
  switch (t) {
    case TY_INT:         return "mrb_int";
    case TY_BIGINT:      return "sp_Bigint *";
    case TY_FLOAT:       return "mrb_float";
    case TY_BOOL:        return "mrb_bool";
    case TY_STRING:      return "const char *";
    case TY_SYMBOL:      return "sp_sym";
    case TY_RANGE:       return "sp_Range";
    case TY_FLOAT_RANGE: return "sp_FloatRange";
    case TY_TIME:        return "sp_Time";
    case TY_COMPLEX:     return "sp_Complex";
    case TY_RATIONAL:    return "sp_Rational";
    case TY_MATCHDATA:   return "sp_MatchData *";
    case TY_REGEX:       return "mrb_regexp_pattern *";
    case TY_EXCEPTION:   return "sp_Exception *";
    case TY_STRBUF:      return "sp_String *";
    case TY_INT_ARRAY:   return "sp_IntArray *";
    case TY_FLOAT_ARRAY: return "sp_FloatArray *";
    case TY_STR_ARRAY:   return "sp_StrArray *";
    case TY_STR_INT_HASH: return "sp_StrIntHash *";
    case TY_STR_STR_HASH: return "sp_StrStrHash *";
    case TY_INT_INT_HASH: return "sp_IntIntHash *";
    case TY_INT_STR_HASH: return "sp_IntStrHash *";
    case TY_SYM_POLY_HASH:  return "sp_SymPolyHash *";
    case TY_STR_POLY_HASH:  return "sp_StrPolyHash *";
    case TY_POLY_POLY_HASH: return "sp_PolyPolyHash *";
    case TY_POLY:         return "sp_RbVal";
    case TY_POLY_ARRAY:   return "sp_PolyArray *";
    case TY_PROC:         return "sp_Proc *";
    case TY_CURRY:        return "sp_Curry *";
    case TY_FIBER:        return "sp_Fiber *";
    case TY_THREAD:       return "sp_thread *";
    case TY_QUEUE:        return "sp_queue *";
    case TY_MUTEX:        return "sp_mutex *";
    case TY_CONDVAR:      return "sp_condvar *";
    case TY_RANDOM:       return "sp_Random *";
    case TY_DIR:          return "sp_Dir *";
    case TY_METHOD:       return "sp_BoundMethod *";
    case TY_IO:           return "sp_File *";
    case TY_ARGF:         return "sp_Argf *";
    case TY_ENUMERATOR:   return "sp_Enumerator *";
    case TY_CLASS:        return "sp_Class";
    default:             return NULL;
  }
}
int is_scalar_ret(TyKind t) {
  return t == TY_INT || t == TY_BIGINT || t == TY_FLOAT || t == TY_BOOL || t == TY_STRING ||
         t == TY_SYMBOL || t == TY_RANGE || t == TY_FLOAT_RANGE || t == TY_TIME || t == TY_COMPLEX || t == TY_RATIONAL || t == TY_MATCHDATA || t == TY_REGEX || t == TY_EXCEPTION ||
         t == TY_INT_ARRAY || t == TY_FLOAT_ARRAY || t == TY_STR_ARRAY ||
         t == TY_STRBUF ||
         t == TY_POLY || t == TY_POLY_ARRAY || t == TY_PROC || t == TY_CURRY || t == TY_FIBER || t == TY_THREAD || t == TY_QUEUE || t == TY_MUTEX || t == TY_CONDVAR || t == TY_RANDOM || t == TY_DIR || t == TY_METHOD || t == TY_IO || t == TY_ARGF || t == TY_ENUMERATOR || t == TY_CLASS ||
         ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t);
}
/* native binding (Path B): map a spinel type spec to the C type at the ABI
   boundary. any -> the boxed value; string -> the runtime string; scalars
   pass by value. */
const char *native_c_type(const char *spec) {
  if (!spec) return "void";
  if (sp_streq(spec, "any"))    return "sp_RbVal";
  if (sp_streq(spec, "string")) return "const char *";
  if (sp_streq(spec, "string?")) return "const char *";  /* nullable; call site wraps */
  if (sp_streq(spec, "nstring")) return "const char *";   /* NULL-able string, unboxed */
  if (sp_streq(spec, "cstring")) return "const char *";   /* borrowed C string; call site dups */
  if (sp_streq(spec, "regexp")) return "mrb_regexp_pattern *";
  if (sp_streq(spec, "int"))    return "mrb_int";
  if (sp_streq(spec, "float"))  return "double";
  if (sp_streq(spec, "bool"))   return "int";
  if (sp_streq(spec, "nil") || sp_streq(spec, "void")) return "void";
  return "sp_RbVal";
}

const char *ffi_c_type(const char *spec) {
  const FfiSpecInfo *info = ffi_spec_lookup(spec);
  return info ? info->c_type : "void";
}

/* The C type of one ffi_callback argument, used to build the trampoline's own
   pointer type. A :ptr callback arg is `const void*` -- the near-universal shape
   of C comparator/visitor callbacks (qsort, bsearch, ...) -- so the generated
   trampoline's type matches the header's declaration exactly (no
   incompatible-function-pointer error). */
const char *ffi_cb_arg_ctype(const char *spec) {
  if (sp_streq(spec, "ptr")) return "const void *";
  return ffi_c_type(spec);
}
const char *default_value(TyKind t) {
  switch (t) {
    case TY_INT:    return "0";
    case TY_FLOAT:  return "0.0";
    case TY_BOOL:   return "0";
    case TY_STRING: return "(&(\"\\xff\")[1])";
    case TY_SYMBOL: return "((sp_sym)-1)";
    case TY_RANGE:  return "(sp_Range){0}";
    case TY_FLOAT_RANGE: return "(sp_FloatRange){0}";
    case TY_TIME:   return "(sp_Time){0}";
    case TY_COMPLEX: return "(sp_Complex){0}";
    case TY_RATIONAL: return "(sp_Rational){0}";
    case TY_MATCHDATA:  return "NULL";
    case TY_REGEX:      return "NULL";
    case TY_EXCEPTION: return "NULL";
    case TY_STRBUF:    return "NULL";
    case TY_INT_ARRAY:
    case TY_FLOAT_ARRAY:
    case TY_STR_ARRAY:
    case TY_POLY_ARRAY: return "NULL";
    case TY_PROC:    return "NULL";
    case TY_CURRY:   return "NULL";
    case TY_FIBER:   return "NULL";
    case TY_THREAD:  return "NULL";
    case TY_QUEUE:   return "NULL";
    case TY_MUTEX:   return "NULL";
    case TY_CONDVAR: return "NULL";
    case TY_RANDOM:  return "NULL";
    case TY_DIR:     return "NULL";
    case TY_METHOD:  return "NULL";
    case TY_IO:      return "NULL";
    case TY_ARGF:    return "NULL";
    case TY_ENUMERATOR: return "NULL";
    case TY_POLY:    return "sp_box_nil()";
    case TY_CLASS:   return "((sp_Class){-1})";
    default:        return (ty_is_hash(t) || ty_is_object(t) || ty_is_obj_array(t)) ? "NULL" : "0";
  }
}
void emit_ctype(Compiler *c, TyKind t, Buf *b) {
  if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* value-type classes are stored inline (sp_X); others are heap pointers */
    buf_printf(b, "sp_%s %s", c->classes[cid].c_name, c->classes[cid].is_value_type ? "" : "*");
  }
  else {
    const char *n = c_type_name(t);
    buf_puts(b, n ? n : "void");
  }
}
void emit_box_open(Compiler *c, TyKind t, Buf *b) {
  if (t == TY_INT)          buf_puts(b, "sp_box_int(");
  else if (t == TY_STRING)  buf_puts(b, "sp_box_str(");
  else if (t == TY_FLOAT)   buf_puts(b, "sp_box_float(");
  else if (t == TY_BOOL)    buf_puts(b, "sp_box_bool(");
  else if (t == TY_NIL)     buf_puts(b, "sp_box_nil(); (void)(");
  else if (t == TY_SYMBOL)  buf_puts(b, "sp_box_sym(");
  else if (t == TY_INT_ARRAY)   buf_puts(b, "sp_box_int_array(");
  else if (t == TY_FLOAT_ARRAY) buf_puts(b, "sp_box_float_array(");
  else if (t == TY_STR_ARRAY)   buf_puts(b, "sp_box_str_array(");
  else if (t == TY_POLY_ARRAY)  buf_puts(b, "sp_box_poly_array(");
  else if (t == TY_CLASS) buf_puts(b, "sp_box_class(");
  else if (t == TY_COMPLEX)  buf_puts(b, "sp_box_complex(");
  else if (t == TY_RATIONAL) buf_puts(b, "sp_box_rational(");
  /* Reference-backed builtins are nilable C pointers: box NULL as nil. */
  else if (ty_nullable_builtin_id(t)) buf_puts(b, "sp_box_nullable_obj((void *)(");
  else if (ty_is_object(t)) {
    int cid = ty_object_class(t);
    /* the struct typedef is sp_<c_name>; a bare `(<Name> *)` would never
       have compiled, so this arm was effectively unreachable as written */
    buf_printf(b, "sp_box_obj((sp_%s *)( ", c->classes[cid].c_name);
  }
  /* TY_POLY: already sp_RbVal, no prefix */
}
void emit_box_close(Compiler *c, TyKind t, Buf *b) {
  (void)c;
  if (t == TY_POLY || t == TY_UNKNOWN) return; /* no-op: already sp_RbVal */
  { const char *nbid = ty_nullable_builtin_id(t);
    if (nbid) { buf_printf(b, "), %s)", nbid); return; } }
  if (ty_is_object(t))        { buf_printf(b, "), %d)", ty_object_class(t)); return; }
  buf_puts(b, ")");
}
const char *array_kind(TyKind t) {
  switch (t) {
    case TY_INT_ARRAY:   return "Int";
    case TY_FLOAT_ARRAY: return "Float";
    case TY_STR_ARRAY:   return "Str";
    default:             return NULL;
  }
}
void emit_c_escaped_n(Buf *b, const char *s, size_t len) {
  for (size_t i = 0; i < len; i++) {
    unsigned char ch = (unsigned char)s[i];
    if (ch == '\\' || ch == '"') buf_printf(b, "\\%c", ch);
    else if (ch == '\n') buf_puts(b, "\\n");
    else if (ch == '\t') buf_puts(b, "\\t");
    else if (ch == '\r') buf_puts(b, "\\r");
    else if (ch >= 0x20 && ch < 0x7f) buf_printf(b, "%c", ch);
    else buf_printf(b, "\\%03o", ch);
  }
}
void emit_c_escaped(Buf *b, const char *s) {
  if (s) emit_c_escaped_n(b, s, strlen(s));
}
/* Marker byte before string-literal data: 0xff is an immutable rodata literal
   (frozen? false, value semantics); `frozen` -- set from the node's `fzl` flag
   when its file has `# frozen_string_literal: true` -- switches to 0xf1 so
   `frozen?` is true and mutation raises FrozenError. Synthesized strings
   (symbol names, ivar names, ...) go through emit_str_literal, which is never
   frozen: the pragma only affects literals written in the source. */
void emit_str_literal_n(Buf *b, const char *content, size_t len, int frozen) {
  const char *mk = frozen ? "\\xf1" : "\\xff";
  /* A frozen literal must carry a REAL sp_str_hdr: the 0xf1 marker promises
     one immediately in front of the data (sp_str_hash caches the FNV hash
     through it, mutation guards and frozen? key off the marker). Baking the
     marker onto a bare rodata literal made that header read/write land in
     whatever rodata precedes the literal -- a garbage cached hash, so a
     Hash#[] with the literal key missed entries whose equal-content keys
     were built at runtime (#1749; ASAN: global-buffer-overflow). Emit a
     static header+marker+data object instead: the layout matches a heap
     string exactly (hdr | marker | bytes), the hash cache write hits our
     own static storage, and next=NULL keeps it off the sweep list. */
  if (frozen) {
    static int g_fzl_ctr = 0;
    int id = g_fzl_ctr++;
    size_t dl = (content ? len : 0) + 1;
    buf_printf(b, "({ static struct { sp_str_hdr h; unsigned char m; char d[%zu]; } _fzl_%d = "
                  "{ { NULL, %zu, %zu, 0 }, 0xf1, \"", dl, id, dl, (content ? len : 0));
    if (content && len) emit_c_escaped_n(b, content, len);
    buf_printf(b, "\" }; _fzl_%d.d; })", id);
    return;
  }
  if (!content || len == 0) { buf_printf(b, "(&(\"%s\")[1])", mk); return; }
  /* NUL-containing strings: use sp_str_from_bytes with explicit byte count.
     The heap string it builds is writable (0xfe), so a frozen literal is
     sealed with sp_str_freeze_val (flips the heap marker to 0xf1 in place). */
  if (len > strlen(content)) {
    /* A FROZEN NUL-containing literal is immortal (sp_str_sweep never frees a
       0xf1 string), so build it once into a call-site-local static and reuse it.
       This avoids re-allocating it on every evaluation -- which, besides the
       churn, made the literal a GC-triggering sibling that could sweep an
       unrooted operand mid-expression (e.g. the receiver in
       `data[8, 8].delete("\0")`, a use-after-free in doom's WAD name parse). */
    if (frozen) {
      static int g_binlit_ctr = 0;
      int id = g_binlit_ctr++;
      buf_printf(b, "({ static const char *_binlit_%d; _binlit_%d ? _binlit_%d : "
                    "(_binlit_%d = sp_str_freeze_val(sp_str_from_bytes(\"", id, id, id, id);
      emit_c_escaped_n(b, content, len);
      buf_printf(b, "\", %zu))); })", len);
      return;
    }
    buf_puts(b, "sp_str_from_bytes(\"");
    emit_c_escaped_n(b, content, len);
    buf_printf(b, "\", %zu)", len);
    return;
  }
  buf_printf(b, "(&(\"%s\" \"", mk);
  emit_c_escaped_n(b, content, len);
  buf_puts(b, "\")[1])");
}
void emit_str_literal(Buf *b, const char *content) {
  if (!content) { buf_puts(b, "(&(\"\\xff\")[1])"); return; }
  emit_str_literal_n(b, content, strlen(content), 0);
}
/* Ruby-source string literal (a StringNode): unlike the internal-constant
   emitter above, each OCCURRENCE gets its own static array, so two textually
   equal literals are two distinct objects and `equal?` (pointer identity)
   answers like CRuby. The plain `("\xff" "abc")` form let the C compiler merge
   equal literals into one address, making `"abc".equal?("abc")` true. A
   re-evaluated occurrence (a literal in a loop) still yields one address --
   the frozen-string-literal semantics spinel's immutable strings already have.
   Frozen and NUL-containing literals keep their existing per-site forms
   (_fzl_N / sp_str_from_bytes), which are already identity-correct. */
void emit_str_literal_src(Buf *b, const char *content, size_t len, int frozen) {
  static int g_slit_ctr = 0;
  if (frozen || (content && len > strlen(content))) {
    emit_str_literal_n(b, content, len, frozen);
    return;
  }
  int lid = g_slit_ctr++;
  buf_printf(b, "({ static const char _slit_%d[] = \"\\xff\" \"", lid);
  if (content && len) emit_c_escaped_n(b, content, len);
  buf_printf(b, "\"; &_slit_%d[1]; })", lid);
}
/* Emit a catch/throw tag expression; returns the tag KIND (0 = name tag
   matched by content, 1 = object tag matched by pointer identity). */
int emit_catch_tag(Compiler *c, int id, Buf *b) {
  const char *ty = nt_type(c->nt, id);
  if (ty && sp_streq(ty, "SymbolNode")) { emit_str_literal(b, nt_str(c->nt, id, "value")); return 0; }
  if (ty && sp_streq(ty, "StringNode")) { emit_str_literal(b, nt_str(c->nt, id, "unescaped")); return 0; }
  TyKind t = comp_ntype(c, id);
  if (t == TY_SYMBOL) {
    buf_puts(b, "sp_sym_to_s("); emit_expr(c, id, b); buf_puts(b, ")");
    return 0;
  }
  if (ty_is_object(t)) {
    /* a non-symbol object tag matches by identity: carry its pointer */
    buf_puts(b, "(const char *)(void *)("); emit_expr(c, id, b); buf_puts(b, ")");
    return 1;
  }
  if (t == TY_POLY) {
    /* boxed object tag: identity via the boxed pointer */
    buf_puts(b, "(const char *)("); emit_expr(c, id, b); buf_puts(b, ").v.p");
    return 1;
  }
  if (t == TY_STRING) {
    /* a dynamic string tag: a valid pointer, matched by content (like the
       StringNode-literal arm above) */
    emit_expr(c, id, b);
    return 0;
  }
  if (t == TY_INT) {
    /* an Integer tag: CRuby matches by identity, which for a Fixnum is value
       equality -- carry the value in the pointer slot and match by identity. */
    buf_puts(b, "(const char *)(intptr_t)("); emit_expr(c, id, b); buf_puts(b, ")");
    return 1;
  }
  /* A Float/boolean/nil/Bignum tag would emit a non-pointer (or a struct)
     into the const char* tag slot -- invalid C, and (0/1 truncation) a wrong
     match. These are vanishingly rare as catch/throw tags; reject loudly
     rather than miscompile. */
  unsupported(c, id, "catch/throw with a Float, boolean, nil, or Bignum tag (use a Symbol, String, Integer, or object tag)");
  return 0;
}
void emit_hash_key(Compiler *c, int key, TyKind kt, Buf *b) {
  TyKind actual = comp_ntype(c, key);
  /* A symbol key on a string-keyed hash (Hash.new{}'s StrPolyHash models
     symbol keys by their name) coerces to the symbol's string. A literal
     :sym becomes the name string directly; a symbol value uses sp_sym_to_s. */
  if (kt == TY_STRING && actual == TY_SYMBOL) {
    const char *kty = nt_type(c->nt, key);
    if (kty && sp_streq(kty, "SymbolNode")) {
      emit_str_literal(b, nt_str(c->nt, key, "value"));
    }
else {
      buf_puts(b, "sp_sym_to_s("); emit_expr(c, key, b); buf_puts(b, ")");
    }
    return;
  }
  if (actual == TY_POLY && kt != TY_POLY) {
    buf_puts(b, "(");
    emit_expr(c, key, b);
    buf_puts(b, kt == TY_STRING ? ").v.s" : ").v.i");  /* int/sym share v.i */
    return;
  }
  if (kt == TY_POLY && actual != TY_POLY) {
    /* PolyPolyHash key: box the typed value into sp_RbVal */
    emit_boxed(c, key, b);
    return;
  }
  emit_expr(c, key, b);
}
int unwrap_parens(Compiler *c, int id) {
  while (id >= 0) {
    const char *ty = nt_type(c->nt, id);
    if (!ty || !sp_streq(ty, "ParenthesesNode")) break;
    int body = nt_ref(c->nt, id, "body");
    int n = 0;
    const int *bd = body >= 0 ? nt_arr(c->nt, body, "body", &n) : NULL;
    if (n != 1) break;
    id = bd[0];
  }
  return id;
}

/* 1 when the receiver is a range whose begin endpoint is statically a Float
   -- directly a (possibly parenthesized) RangeNode or through a
   sole-assignment local. CRuby raises TypeError "can't iterate from Float"
   when enumerating such a range (an int begin with a float end iterates
   fine); the int-backed sp_Range would otherwise silently truncate. */
int range_float_begin(Compiler *c, int recv) {
  const NodeTable *nt = c->nt;
  int rn = unwrap_parens(c, recv);
  if (rn < 0) return 0;
  const char *rty = nt_type(nt, rn);
  if (!rty || !sp_streq(rty, "RangeNode")) {
    rn = local_sole_range_node(c, rn);
    if (rn < 0) return 0;
  }
  int lo = nt_ref(nt, rn, "left");
  return lo >= 0 && comp_ntype(c, lo) == TY_FLOAT;
}
const char *int_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_int_add";
  if (sp_streq(op, "-"))  return "sp_int_sub";
  if (sp_streq(op, "*"))  return "sp_int_mul";
  if (sp_streq(op, "/"))  return "sp_idiv";
  if (sp_streq(op, "%"))  return "sp_imod";
  if (sp_streq(op, "**")) return "sp_int_pow";
  return NULL;
}
const char *bigint_arith_fn(const char *op) {
  if (sp_streq(op, "+"))  return "sp_bigint_add";
  if (sp_streq(op, "-"))  return "sp_bigint_sub";
  if (sp_streq(op, "*"))  return "sp_bigint_mul";
  if (sp_streq(op, "/"))  return "sp_bigint_div";
  if (sp_streq(op, "%"))  return "sp_bigint_mod";
  return NULL;
}
/* True if any user exception subclass overrides #message or #to_s, so the
   default exception message/to_s path must dispatch to the user method rather
   than reporting the stored message (which defaults to the class name). */
int exc_has_user_msg_override(Compiler *c) {
  for (int i = 0; i < c->nclasses; i++) {
    if (!class_is_exc_subclass(c, i)) continue;
    /* Only a string-returning override is dispatched (see codegen_program),
       so gate on TY_STRING to match -- otherwise a non-string override would
       route every exception query through an empty dispatcher for nothing. */
    int mi_msg = comp_method_in_chain(c, i, "message", NULL);
    if (mi_msg >= 0 && (TyKind)c->scopes[mi_msg].ret == TY_STRING)
      return 1;
    int mi_tos = comp_method_in_chain(c, i, "to_s", NULL);
    if (mi_tos >= 0 && (TyKind)c->scopes[mi_tos].ret == TY_STRING)
      return 1;
  }
  return 0;
}

const char *mc(const char *name) {
  static char buf[256];
  int j = 0;
  for (const char *p = name; *p && j < (int)sizeof buf - 8; p++) {
    char ch = *p;
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
        (ch >= '0' && ch <= '9') || ch == '_') { buf[j++] = ch; continue; }
    /* operator characters map to distinct tokens so that, e.g., `&` and `|`
       (or `<<` and `>>`) don't mangle to the same C identifier */
    const char *tok;
    switch (ch) {
      case '?': tok = "_p";     break;
      case '!': tok = "_bang";  break;
      case '=': tok = "_set";   break;
      case '+': tok = "_plus";  break;
      case '-': tok = "_minus"; break;
      case '*': tok = "_star";  break;
      case '/': tok = "_slash"; break;
      case '%': tok = "_pct";   break;
      case '<': tok = "_lt";    break;
      case '>': tok = "_gt";    break;
      case '&': tok = "_amp";   break;
      case '|': tok = "_bar";   break;
      case '^': tok = "_caret"; break;
      case '~': tok = "_tilde"; break;
      case '@': tok = "_at";    break;
      case '[': tok = "_lb";    break;
      case ']': tok = "_rb";    break;
      default:  tok = "_";      break;
    }
    size_t tl = strlen(tok);
    memcpy(buf + j, tok, tl); j += (int)tl;
  }
  buf[j] = '\0';
  return buf;
}
int scope_is_shadowed(Compiler *c, int s) {
  Scope *sc = &c->scopes[s];
  if (sc->class_id < 0 || !sc->name) return 0;
  for (int k = s + 1; k < c->nscopes; k++) {
    Scope *o = &c->scopes[k];
    if (o->class_id == sc->class_id && o->is_cmethod == sc->is_cmethod &&
        o->name && sp_streq(o->name, sc->name)) return 1;
  }
  return 0;
}
/* True when scope `s` is emitted as a standalone `sp_Class_method` function, so
   a poly-dispatch `case` arm may call it without dangling at link. Mirrors the
   emission gate in codegen.c exactly: a yielding method is inlined at each call
   site (no symbol exists), and a pruned/shadowed/transplanted method is never
   defined. A dispatch arm that targets a scope failing this test references an
   undefined symbol (issues #1583 yields, #1576 pruned). */
int scope_has_callable_symbol(Compiler *c, int s) {
  if (s < 0 || s >= c->nscopes) return 0;
  Scope *sc = &c->scopes[s];
  return sc->reachable && !sc->yields && !sc->is_transplanted_source &&
         !scope_is_shadowed(c, s);
}
int struct_kwarg_value(Compiler *c, int kwh, const char *name) {
  const NodeTable *nt = c->nt;
  int n = 0;
  const int *els = nt_arr(nt, kwh, "elements", &n);
  for (int i = 0; i < n; i++) {
    if (!nt_type(nt, els[i]) || !sp_streq(nt_type(nt, els[i]), "AssocNode")) continue;
    int key = nt_ref(nt, els[i], "key");
    if (key >= 0 && nt_type(nt, key) && sp_streq(nt_type(nt, key), "SymbolNode")) {
      const char *kn = nt_str(nt, key, "value");
      if (kn && sp_streq(kn, name)) return nt_ref(nt, els[i], "value");
    }
  }
  return -1;
}
int eq_family(TyKind t) {
  if (ty_is_numeric(t)) return 1;
  if (t == TY_STRING) return 2;
  if (t == TY_BOOL) return 3;
  if (t == TY_SYMBOL) return 4;
  if (t == TY_RANGE) return 5;
  if (t == TY_FLOAT_RANGE) return 6;
  return 0;
}
int ty_matches_class(TyKind t, const char *cn, int exact) {
  const char *self_cls = NULL;
  if (t == TY_STRING || t == TY_STRBUF) self_cls = "String";
  else if (t == TY_INT || t == TY_BIGINT) self_cls = "Integer";
  else if (t == TY_FLOAT) self_cls = "Float";
  else if (t == TY_SYMBOL) self_cls = "Symbol";
  else if (t == TY_RANGE || t == TY_FLOAT_RANGE) self_cls = "Range";
  else if (ty_is_array(t)) self_cls = "Array";
  else if (ty_is_hash(t)) self_cls = "Hash";
  else if (t == TY_NIL) self_cls = "NilClass";
  else if (t == TY_BOOL) self_cls = "Boolean"; /* true/false split handled at call site */
  else if (t == TY_FIBER) self_cls = "Fiber";
  else if (t == TY_THREAD) self_cls = "Thread";
  else if (t == TY_QUEUE) self_cls = "Queue";
  else if (t == TY_MUTEX) self_cls = "Mutex";
  else if (t == TY_CONDVAR) self_cls = "ConditionVariable";
  else if (t == TY_ENUMERATOR) self_cls = "Enumerator";
  else if (t == TY_TIME) self_cls = "Time";
  else if (t == TY_COMPLEX) self_cls = "Complex";
  else if (t == TY_RATIONAL) self_cls = "Rational";
  else if (t == TY_REGEX) self_cls = "Regexp";
  else if (t == TY_MATCHDATA) self_cls = "MatchData";
  else if (t == TY_PROC) self_cls = "Proc";
  else if (t == TY_RANDOM) self_cls = "Random";
  else if (t == TY_IO) self_cls = "IO";
  if (!self_cls) return -1;
  if (sp_streq(cn, self_cls)) return 1;
  if (exact) return 0;
  if (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject") || sp_streq(cn, "Kernel")) return 1;
  if (sp_streq(cn, "Comparable") && (t == TY_STRING || t == TY_STRBUF || t == TY_INT || t == TY_BIGINT ||
                                     t == TY_FLOAT || t == TY_SYMBOL || t == TY_TIME ||
                                     t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Numeric") && (t == TY_INT || t == TY_BIGINT || t == TY_FLOAT ||
                                  t == TY_COMPLEX || t == TY_RATIONAL)) return 1;
  if (sp_streq(cn, "Enumerable") && (ty_is_array(t) || ty_is_hash(t) || t == TY_RANGE ||
                                     t == TY_ENUMERATOR)) return 1;
  return 0;
}

/* `if (frozen) raise FrozenError` guard preceding an ivar store on a
   freeze-observed class ("can't modify frozen <Name>: <inspect>"); emits
   nothing when instances of the class are never frozen. `selfexpr` is a C
   expression for the instance pointer, evaluated twice (bind a temp first
   when it has effects). */
void emit_frozen_obj_guard(Compiler *c, int cid, const char *selfexpr, Buf *b) {
  if (cid < 0 || cid >= c->nclasses) return;
  if (!c->classes[cid].freeze_observed || c->classes[cid].is_value_type) return;
  const char *rn = class_ruby_name(c, cid) ? class_ruby_name(c, cid) : c->classes[cid].name;
  buf_printf(b,
      "if (sp_gc_is_frozen((void *)%s)) "
      "sp_raise_frozen_obj(sp_box_obj((void *)%s, %d), (&(\"\\xff\" \"can't modify frozen %s\")[1])); ",
      selfexpr, selfexpr, cid, rn);
}
