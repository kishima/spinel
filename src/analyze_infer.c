#include "analyze_internal.h"

/* Per-iteration memo for the (cid, ivname) full-table-scan narrow helpers
   below. Each is O(nodes); they are queried once per `@ivar[i]` expression,
   so on a large input (the self-compile) the naive form is O(nodes^2). The
   result is stable within one fixpoint iteration, so a generation-stamped
   direct-mapped cache collapses the repeats to O(nodes) per (cid, ivar).
   The generation is bumped once per fixpoint iteration (sp_narrow_memo_bump),
   so a stale entry can only survive within an iteration the fixpoint will
   re-run anyway. */
#define SP_NMEMO_SZ 16384
static unsigned g_narrow_gen = 1;
static struct { unsigned gen; long key; signed char val; } g_nmemo[SP_NMEMO_SZ];
void sp_narrow_memo_bump(void) { g_narrow_gen++; }
static long narrow_key(int which, int cid, const char *ivname) {
  unsigned long h = 1469598103934665603UL ^ (unsigned)which;
  h = (h * 1099511628211UL) ^ (unsigned)cid;
  for (const char *p = ivname; p && *p; p++) h = (h * 1099511628211UL) ^ (unsigned char)*p;
  return (long)(h & 0x7fffffffffffffffUL);
}
static int narrow_memo_get(long key, int *hit) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  if (g_nmemo[slot].gen == g_narrow_gen && g_nmemo[slot].key == key) { *hit = 1; return g_nmemo[slot].val; }
  *hit = 0; return 0;
}
static void narrow_memo_put(long key, int val) {
  unsigned slot = (unsigned)((unsigned long)key % SP_NMEMO_SZ);
  g_nmemo[slot].gen = g_narrow_gen; g_nmemo[slot].key = key; g_nmemo[slot].val = (signed char)val;
}

/* Unify the value type of every splice-bound break/next in `node` (break/next
   inside a nested loop or block bind there, not to the splice). TY_UNKNOWN if
   none carry a value. Mirrors codegen's ie_splice_value_ty so the inferred
   instance_exec result type matches the slot codegen sizes. */
TyKind ie_block_break_next_ty(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return TY_UNKNOWN;
  const char *ty = nt_type(nt, node);
  if (!ty) return TY_UNKNOWN;
  if (sp_streq(ty, "BreakNode") || sp_streq(ty, "NextNode")) {
    int a = nt_ref(nt, node, "arguments"); int an = 0;
    const int *av = a >= 0 ? nt_arr(nt, a, "arguments", &an) : NULL;
    return an > 0 ? infer_type(c, av[0]) : TY_UNKNOWN;
  }
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "BlockNode") || sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return TY_UNKNOWN;
  TyKind r = TY_UNKNOWN;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) {
    TyKind s = ie_block_break_next_ty(c, nt_ref_at(nt, node, i));
    if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
  }
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, node, i, &n);
    for (int k = 0; k < n; k++) {
      TyKind s = ie_block_break_next_ty(c, ids[k]);
      if (s != TY_UNKNOWN) r = (r == TY_UNKNOWN) ? s : ty_unify(r, s);
    }
  }
  return r;
}

int g_infer_ignore_brk = 0;

/* Post-backstop return re-derivation must not newly widen a return to poly
   (see infer_return_types); set only around analyze_program's post-pass. */
int g_ret_no_new_poly = 0;

/* Top-level `break` detector: a BreakNode binding to the enclosing block,
   stopping at nested loops and nested block-bearing calls (which capture their
   own break). Mirrors codegen_stmt's subtree_has_loop_break. */
int block_has_top_break(Compiler *c, int node) {
  const NodeTable *nt = c->nt;
  if (node < 0) return 0;
  const char *ty = nt_type(nt, node);
  if (!ty) return 0;
  if (sp_streq(ty, "BreakNode")) return 1;
  if (sp_streq(ty, "WhileNode") || sp_streq(ty, "UntilNode") || sp_streq(ty, "ForNode") ||
      sp_streq(ty, "LambdaNode") || sp_streq(ty, "DefNode") ||
      sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return 0;
  if (sp_streq(ty, "CallNode") && nt_ref(nt, node, "block") >= 0) return 0;
  int nr = nt_num_refs(nt, node);
  for (int i = 0; i < nr; i++) if (block_has_top_break(c, nt_ref_at(nt, node, i))) return 1;
  int na = nt_num_arrs(nt, node);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *el = nt_arr_at(nt, node, i, &n);
    for (int j = 0; j < n; j++) if (block_has_top_break(c, el[j])) return 1;
  }
  return 0;
}

/* A call is a break-wrapped iterator when it takes a literal block whose body
   has a top-level break and is either a receiver-bearing builtin iterator or
   a call resolving to an inline-able yielding user method (whose body -- and
   so the block, at its yield sites -- is spliced at this call site, putting
   the wrapper's setjmp in exactly the right C scope). Receiverless
   NON-methods (loop / catch / proc / lambda literals) run their own scopes
   and stay excluded, as does instance_exec/eval (handled inline). */
int call_breaks(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "CallNode")) return 0;
  int block = nt_ref(nt, id, "block");
  if (block < 0) return 0;
  const char *bty = nt_type(nt, block);
  if (!bty || !sp_streq(bty, "BlockNode")) return 0;   /* not &proc / &:sym */
  const char *name = nt_str(nt, id, "name");
  if (name && (sp_streq(name, "instance_exec") || sp_streq(name, "instance_eval"))) return 0;
  if (nt_ref(nt, id, "receiver") < 0 && call_user_yield_mi(c, id) < 0) return 0;
  return block_has_top_break(c, nt_ref(nt, block, "body"));
}

/* str.unpack1(fmt) with a literal single-directive numeric format: the
   directive fixes the extracted value's type, so the result does not need
   to stay poly (`data[4, 4].unpack1('V')` reading a WAD header count in
   doom). Count/endian suffixes ("V2", "l<", "q>*") keep the first value's
   type. Only the numeric directives sp_str_unpack decodes qualify --
   integers to TY_INT, the float/double directives to TY_FLOAT;
   multi-directive, interpolated, and other formats stay TY_POLY. */
static TyKind an_unpack1_lit_type(const NodeTable *nt, int arg) {
  const char *aty = nt_type(nt, arg);
  if (!aty || !sp_streq(aty, "StringNode")) return TY_POLY;
  const char *f = nt_str(nt, arg, "content");
  if (!f || !f[0]) return TY_POLY;
  char d = f[0];
  const char *p = f + 1;
  while (*p == '<' || *p == '>' || *p == '!' || *p == '_') p++;
  while (*p >= '0' && *p <= '9') p++;
  if (*p == '*') p++;
  if (*p) return TY_POLY;  /* further directives: not this one's type */
  if (strchr("cCsSlLqQnNvV", d)) return TY_INT;
  if (strchr("dDfFeEgG", d)) return TY_FLOAT;
  return TY_POLY;
}

/* Whether every element written into the poly-array ivar `@<ivname>` of class
   `cid` is an int-returning kind: a bound method (a dispatch-table entry, called
   with an int arg returns int), an int array, an int, or nil filler. Mirrors
   legacy's cls_ivar_observed_types check in poly_index_narrow_int. */
/* Classify a value type stored as an element of a dispatch/data table:
   1 = int-returning when indexed/called (int bit, int array get, bound
   method/proc call, or a poly assumed to be such a callable); 2 = neutral
   filler (nil/unknown); 0 = a clearly non-int kind. */
static int table_elem_int_returning(TyKind vt) {
  if (vt == TY_INT || vt == TY_INT_ARRAY || vt == TY_METHOD || vt == TY_PROC ||
      vt == TY_POLY)
    return 1;
  if (vt == TY_NIL || vt == TY_UNKNOWN) return 2;
  return 0;
}

static int ivar_array_elems_int_returning_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    /* element write `@ivar[i] = v` */
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0) continue;
      const char *rty = nt_type(nt, recv);
      if (!rty || !sp_streq(rty, "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      int k = table_elem_int_returning(comp_ntype(c, av[1]));
      if (k == 0) return 0;
      if (k == 1) saw = 1;
      continue;
    }
    /* whole-ivar write `@ivar = [..]` / `@ivar = [x] * n` */
    if (sp_streq(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || !sp_streq(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;   /* the ArrayNode whose elements to inspect */
      if (vty && sp_streq(vty, "ArrayNode")) arr = v;
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               sp_streq(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");   /* `[..] * n` */
        if (ar >= 0 && nt_type(nt, ar) && sp_streq(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      if (arr < 0) return 0;   /* non-literal whole write: can't verify */
      int en = 0;
      const int *els = nt_arr(nt, arr, "elements", &en);
      for (int e = 0; e < en; e++) {
        int k = table_elem_int_returning(comp_ntype(c, els[e]));
        if (k == 0) return 0;
        if (k == 1) saw = 1;
      }
      continue;
    }
  }
  return saw;
}

/* `@table[i][j]` where @table is a poly array of int-returning callables/arrays
   (a method dispatch table) yields an int. Returns 1 if `id` matches that shape
   for the enclosing class. The index types don't matter (they may themselves be
   poly mid-fixpoint); the result type follows from the table's element kinds. */
static int ivar_array_elems_int_returning(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(0, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_int_returning_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

static int poly_double_index_int(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;   /* slice, not an element */
  int inner = nt_ref(nt, id, "receiver");
  if (inner < 0) return 0;
  const char *ity = nt_type(nt, inner);
  if (!ity || !sp_streq(ity, "CallNode")) return 0;
  const char *inm = nt_str(nt, inner, "name");
  if (!inm || !sp_streq(inm, "[]")) return 0;
  int iargs = nt_ref(nt, inner, "arguments");
  int ian = 0;
  const int *iav = iargs >= 0 ? nt_arr(nt, iargs, "arguments", &ian) : NULL;
  if (ian != 1) return 0;
  (void)iav;
  int ivnode = nt_ref(nt, inner, "receiver");
  if (ivnode < 0) return 0;
  const char *vty = nt_type(nt, ivnode);
  if (!vty || !sp_streq(vty, "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, ivnode, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_int_returning(c, cid, ivname);
}

/* Whether every element stored into poly-array ivar `@<ivname>` is an int
   array (a nested array of int arrays, e.g. @chr_banks / @nmt_mem). Element
   reads then yield an int array rather than a boxed poly. */
static int ivar_array_elems_all_int_array_impl(Compiler *c, int cid, const char *ivname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind vt = comp_ntype(c, av[1]);
      if (vt == TY_INT_ARRAY) { saw = 1; continue; }
      if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
      return 0;
    }
    if (sp_streq(ty, "InstanceVariableWriteNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || !sp_streq(nm, ivname)) continue;
      Scope *s = comp_scope_of(c, id);
      if (!s || s->class_id != cid) continue;
      int v = nt_ref(nt, id, "value");
      if (v < 0) continue;
      const char *vty = nt_type(nt, v);
      int arr = -1;
      if (vty && sp_streq(vty, "ArrayNode")) arr = v;
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               sp_streq(nt_str(nt, v, "name"), "*")) {
        int ar = nt_ref(nt, v, "receiver");
        if (ar >= 0 && nt_type(nt, ar) && sp_streq(nt_type(nt, ar), "ArrayNode")) arr = ar;
      }
      else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
               (sp_streq(nt_str(nt, v, "name"), "map") || sp_streq(nt_str(nt, v, "name"), "collect"))) {
        /* `@x = src.map { ... }`: elements are the block's result type. */
        int blk = nt_ref(nt, v, "block");
        int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn <= 0) return 0;
        TyKind et = comp_ntype(c, bb[bn - 1]);
        if (et == TY_INT_ARRAY) { saw = 1; continue; }
        if (et == TY_NIL || et == TY_UNKNOWN) continue;
        return 0;
      }
      if (arr < 0) return 0;
      int en = 0;
      const int *els = nt_arr(nt, arr, "elements", &en);
      for (int e = 0; e < en; e++) {
        TyKind et = comp_ntype(c, els[e]);
        if (et == TY_INT_ARRAY) { saw = 1; continue; }
        if (et == TY_NIL || et == TY_UNKNOWN) continue;
        return 0;
      }
      continue;
    }
  }
  return saw;
}

/* `@nested[i]` (single index) where @nested is a poly array whose every
   element is an int array yields an int array (not a boxed poly). Returns the
   ivar name via *out_iv for codegen, or NULL. */
static int ivar_array_elems_all_int_array(Compiler *c, int cid, const char *ivname) {
  long k = narrow_key(1, cid, ivname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = ivar_array_elems_all_int_array_impl(c, cid, ivname);
  narrow_memo_put(k, v);
  return v;
}

/* Whether every element of poly-array constant `CNAME` is an int array
   (e.g. `WAVE_FORM = [..].map { (0..7).map { .. } }`). Element reads then
   yield sp_IntArray* instead of a boxed poly. All writes to the constant and
   any `CNAME[i] = v` mutation must agree. */
static int const_array_elems_all_int_array_impl(Compiler *c, const char *cname) {
  const NodeTable *nt = c->nt;
  int saw = 0;
  for (int id = 0; id < nt->count; id++) {
    const char *ty = nt_type(nt, id);
    if (!ty) continue;
    if (sp_streq(ty, "CallNode")) {
      const char *nm = nt_str(nt, id, "name");
      if (!nm || (!sp_streq(nm, "[]=") && !sp_streq(nm, "store"))) continue;
      int recv = nt_ref(nt, id, "receiver");
      if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "ConstantReadNode")) continue;
      const char *rn = nt_str(nt, recv, "name");
      if (!rn || !sp_streq(rn, cname)) continue;
      int args = nt_ref(nt, id, "arguments");
      int an = 0;
      const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
      if (an < 2) continue;
      TyKind vt = comp_ntype(c, av[1]);
      if (vt == TY_INT_ARRAY) { saw = 1; continue; }
      if (vt == TY_NIL || vt == TY_UNKNOWN) continue;
      return 0;
    }
    if (!sp_streq(ty, "ConstantWriteNode")) continue;
    const char *nm = nt_str(nt, id, "name");
    if (!nm || !sp_streq(nm, cname)) continue;
    int v = nt_ref(nt, id, "value");
    if (v < 0) return 0;
    /* `CNAME = [...].freeze` binds the same literal */
    if (nt_type(nt, v) && sp_streq(nt_type(nt, v), "CallNode") &&
        nt_str(nt, v, "name") && sp_streq(nt_str(nt, v, "name"), "freeze") &&
        nt_ref(nt, v, "receiver") >= 0)
      v = nt_ref(nt, v, "receiver");
    const char *vty = nt_type(nt, v);
    int arr = -1;
    if (vty && sp_streq(vty, "ArrayNode")) arr = v;
    else if (vty && sp_streq(vty, "CallNode") && nt_str(nt, v, "name") &&
             (sp_streq(nt_str(nt, v, "name"), "map") || sp_streq(nt_str(nt, v, "name"), "collect"))) {
      int blk = nt_ref(nt, v, "block");
      int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn <= 0) return 0;
      TyKind et = comp_ntype(c, bb[bn - 1]);
      if (et == TY_INT_ARRAY) { saw = 1; continue; }
      return 0;
    }
    if (arr < 0) return 0;
    int en = 0;
    const int *els = nt_arr(nt, arr, "elements", &en);
    for (int e = 0; e < en; e++) {
      TyKind et = comp_ntype(c, els[e]);
      if (et == TY_INT_ARRAY) { saw = 1; continue; }
      if (et == TY_NIL || et == TY_UNKNOWN) continue;
      return 0;
    }
  }
  return saw;
}

int const_array_elems_all_int_array(Compiler *c, const char *cname) {
  long k = narrow_key(2, 0, cname);
  int hit; int v = narrow_memo_get(k, &hit);
  if (hit) return v;
  v = const_array_elems_all_int_array_impl(c, cname);
  narrow_memo_put(k, v);
  return v;
}

/* `CONST[i]` on a poly-array constant of int arrays yields an int array. */
static int const_poly_index_int_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "ConstantReadNode")) return 0;
  const char *cname = nt_str(nt, recv, "name");
  LocalVar *cv = cname ? comp_const(c, cname) : NULL;
  if (!cv || cv->type != TY_POLY_ARRAY) return 0;
  return const_array_elems_all_int_array(c, cname);
}

static int poly_index_int_array(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *nm = nt_str(nt, id, "name");
  if (!nm || !sp_streq(nm, "[]")) return 0;
  int args = nt_ref(nt, id, "arguments");
  int an = 0;
  const int *av = args >= 0 ? nt_arr(nt, args, "arguments", &an) : NULL;
  if (an != 1) return 0;
  const char *aty = nt_type(nt, av[0]);
  if (aty && sp_streq(aty, "RangeNode")) return 0;
  int recv = nt_ref(nt, id, "receiver");
  if (recv < 0 || !sp_streq(nt_type(nt, recv) ? nt_type(nt, recv) : "", "InstanceVariableReadNode")) return 0;
  const char *ivname = nt_str(nt, recv, "name");
  Scope *s = comp_scope_of(c, id);
  int cid = s ? s->class_id : -1;
  if (cid < 0 || cid >= c->nclasses || !ivname) return 0;
  int iv = comp_ivar_index(&c->classes[cid], ivname);
  if (iv < 0 || c->classes[cid].ivar_types[iv] != TY_POLY_ARRAY) return 0;
  return ivar_array_elems_all_int_array(c, cid, ivname);
}

/* A plain int literal value (not an out-of-int64 bigint literal). */
static int infer_const_int_node(const NodeTable *nt, int id, long long *out) {
  const char *ty = nt_type(nt, id);
  if (!ty || !sp_streq(ty, "IntegerNode")) return 0;
  if (nt_str(nt, id, "bigval")) return 0;
  *out = (long long)nt_int(nt, id, "value", 0);
  return 1;
}

/* Whether base**exp does not fit in signed 64 bits (so it must be a Bignum). */
static int infer_int_pow_overflows(long long base, long long exp) {
  if (exp <= 0) return 0;
  if (base >= -1 && base <= 1) return 0;
  long long r = 1;
  for (long long i = 0; i < exp; i++)
    if (__builtin_mul_overflow(r, base, &r)) return 1;
  return 0;
}

/* Whether base << amount does not fit in signed 64 bits (a Bignum in CRuby --
   `1 << 64` is 2**64, not a wrapped/UB C shift). */
static int infer_int_shl_overflows(long long base, long long amount) {
  if (base == 0 || amount <= 0) return 0;
  long long r = base;
  for (long long i = 0; i < amount; i++)
    if (__builtin_mul_overflow(r, 2, &r)) return 1;
  return 0;
}

/* A blockless `range.each` is an external Enumerator only when used standalone
   or consumed by an enumerator method (#next/#peek/#rewind/#size). When it is
   the receiver of a collection method (.to_a/.map/.select/...), it materializes
   to a typed array instead, so those chains keep the fast unboxed path. */
/* Literal Float::INFINITY as a range endpoint (the infer-side twin of
   codegen's lazy_endpoint_is_infinite; nil/missing ends are checked by the
   callers directly). */
static int infer_end_is_float_inf(Compiler *c, int right) {
  const NodeTable *nt = c->nt;
  if (right < 0) return 0;
  const char *rty = nt_type(nt, right);
  if (!rty || !sp_streq(rty, "ConstantPathNode")) return 0;
  const char *cpnm = nt_str(nt, right, "name");
  if (!cpnm || !sp_streq(cpnm, "INFINITY")) return 0;
  int par = nt_ref(nt, right, "parent");
  const char *parnm = (par >= 0 && nt_type(nt, par) &&
                       sp_streq(nt_type(nt, par), "ConstantReadNode"))
                      ? nt_str(nt, par, "name") : NULL;
  return parnm && sp_streq(parnm, "Float");
}

/* True when `id` is the receiver of an enclosing call that carries a block:
   the chain emitters own that shape (arr.map.with_index { }), so the inner
   blockless call must keep its legacy typing. */
static int call_is_chain_receiver_with_block(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    if (nt_ref(nt, n, "receiver") != id) continue;
    return nt_ref(nt, n, "block") >= 0;
  }
  return 0;
}

static int range_each_is_external(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  NT_FOREACH_KIND(nt, NK_CallNode, n) {
    if (nt_ref(nt, n, "receiver") != id) continue;
    const char *m = nt_str(nt, n, "name");
    if (m && (sp_streq(m, "next") || sp_streq(m, "peek") ||
              sp_streq(m, "rewind") || sp_streq(m, "size") ||
              sp_streq(m, "class") || sp_streq(m, "inspect"))) return 1;
    return 0;   /* receiver of a collection method -> materialize to an array */
  }
  return 1;     /* standalone -> enumerator */
}

int range_enum_redispatch(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int block = nt_ref(nt, id, "block");
  int args = nt_ref(nt, id, "arguments"); int argc = 0;
  if (args >= 0) nt_arr(nt, args, "arguments", &argc);
  /* Only an integer range materializes faithfully: sp_Range holds mrb_int
     bounds, so redispatch only when a literal range receiver has both bounds
     present and typed TY_INT. A float/string bound would truncate or fail to
     compile, and a beginless/endless range (`(1..)`, `(..5)`) has no int array
     to build -- all fall through to the loud `unsupported` reject rather than
     silently miscompiling. (A non-literal receiver, e.g. `r = (1.0..5.0);
     r.find`, is the pre-existing int-only-sp_Range limitation, not detectable
     here.) */
  int rn = nt_ref(nt, id, "receiver");
  while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
    int body = nt_ref(nt, rn, "body"); int bn = 0;
    const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
    rn = bn == 1 ? bd[0] : -1;
  }
  if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
    int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
    if (lo < 0 || hi < 0) return 0;
    if (infer_type(c, lo) != TY_INT || infer_type(c, hi) != TY_INT) return 0;
  }
  /* Non-collecting Enumerable methods: their result does not depend on the
     block-produced element type, so materializing the range to an int array is
     transparent. flat_map/collect_concat also redispatch because the block-param
     typing pass types their range block parameter as an int; other array-building
     collectors (filter_map/partition/chunk_while) are not typed there yet, so
     they stay a clean reject rather than miscompile. */
  if (sp_streq(name, "group_by") || sp_streq(name, "find") ||
      sp_streq(name, "detect") || sp_streq(name, "zip") ||
      sp_streq(name, "tally")) return 1;
  /* each_slice/each_cons: the block form and the blockless Enumerator form
     (.to_a / .map chains) both materialize transparently -- the slices carry
     the range's own ints. */
  if ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) && argc >= 1)
    return 1;
  /* block-taking Enumerable forms the array emitters serve: partition,
     each_with_index, sort_by, chunk_while, sum { }, and the block forms of
     inject/reduce with an initial value. */
  if (block >= 0 &&
      (sp_streq(name, "partition") || sp_streq(name, "each_with_index") ||
       sp_streq(name, "sort_by") || sp_streq(name, "chunk_while") ||
       sp_streq(name, "sum") || sp_streq(name, "each_with_object") ||
       sp_streq(name, "take_while") || sp_streq(name, "drop_while")))
    return 1;
  if ((sp_streq(name, "inject") || sp_streq(name, "reduce")) && block >= 0)
    return 1;
  /* cycle { }: the array emitter serves both the counted and endless forms;
     the yielded elements are the range's own ints */
  if (sp_streq(name, "cycle") && block >= 0) return 1;
  if ((sp_streq(name, "flat_map") || sp_streq(name, "collect_concat")) && block >= 0) return 1;
  /* reduce/inject: the explicit symbol / initial-value forms (no block). */
  if ((sp_streq(name, "reduce") || sp_streq(name, "inject")) && argc >= 1 && block < 0) return 1;
  /* count: the block / argument forms (bare count is size, handled natively). */
  if (sp_streq(name, "count")) return block >= 0 || argc >= 1;
  /* take/drop and reverse_each materialize transparently (the results carry
     the range's own ints); filter_map has array block-param typing. */
  if ((sp_streq(name, "take") || sp_streq(name, "drop")) && argc == 1) return 1;
  if (sp_streq(name, "reverse_each")) return 1;
  if (sp_streq(name, "filter_map") && block >= 0) return 1;
  /* min(n)/max(n)/minmax with a count return arrays of the range's ints */
  if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc >= 1) return 1;
  return 0;
}

/* A Hash Enumerable method served by materializing the hash into its
   [key, value] pair array (sp_enum_items_from) and re-dispatching as a poly
   array: reduce/inject and each_with_index block forms. The hash-native
   folds (sum/count/find/map/...) keep their own emitters. */
int hash_enum_redispatch(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *name = nt_str(nt, id, "name");
  if (!name) return 0;
  int block = nt_ref(nt, id, "block");
  /* blockless pair-order Enumerables: min/max compare the [k, v] pairs */
  if (block < 0 && (sp_streq(name, "min") || sp_streq(name, "max") ||
                    sp_streq(name, "minmax")))
    return 1;
  /* blockless each_with_index: an external Enumerator of [[k, v], i] */
  if (block < 0 && sp_streq(name, "each_with_index")) return 1;
  /* pair-array Enumerables with no dedicated hash emitter: find_index, uniq,
     zip, tally, reverse_each ride the materialized redispatch, block or not
     (#2372) */
  if (sp_streq(name, "find_index") || sp_streq(name, "uniq") ||
      sp_streq(name, "zip") || sp_streq(name, "tally") ||
      sp_streq(name, "reverse_each")) return 1;
  if (block < 0 || !nt_type(nt, block) || !sp_streq(nt_type(nt, block), "BlockNode")) return 0;
  /* comparator-block min/max/minmax compare the [k, v] pairs like the
     blockless forms (min_by/max_by keep their dedicated hash emitters) */
  if (sp_streq(name, "min") || sp_streq(name, "max") || sp_streq(name, "minmax")) return 1;
  if (sp_streq(name, "none?") || sp_streq(name, "one?") || sp_streq(name, "find_all")) return 1;
  if (sp_streq(name, "each_with_index")) return 1;
  if (sp_streq(name, "reduce") || sp_streq(name, "inject")) return 1;
  /* comparator-block sort over the [k, v] pairs -> a poly array of pairs */
  if (sp_streq(name, "sort")) return 1;
  /* each_with_object / flat_map keep their dedicated hash emitters */
  /* min_by/max_by keep their dedicated hash emitters; only minmax_by rides
     the pair redispatch */
  if (sp_streq(name, "minmax_by")) return 1;
  return 0;
}

TyKind infer_call(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  /* a dynamic send lowered to a name-dispatch (desugar_dynamic_send) yields one
     of several boxed method results -> poly. */
  { int dn = 0; nt_arr(nt, id, "dyn_send_arms", &dn); if (dn > 0) return TY_POLY; }
  const char *name = nt_str(nt, id, "name");
  int recv = nt_ref(nt, id, "receiver");
  int args = nt_ref(nt, id, "arguments");
  int argc = 0;
  const int *argv = NULL;
  if (args >= 0) argv = nt_arr(nt, args, "arguments", &argc);
  if (!name) return TY_UNKNOWN;

  /* $~'s MatchData face over the match registers (codegen reads the same
     backing the back-references use): nullable strings. */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "GlobalVariableReadNode") ||
       sp_streq(nt_type(nt, recv), "BackReferenceReadNode")) &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "$~") &&
      (sp_streq(name, "pre_match") || sp_streq(name, "post_match") || sp_streq(name, "to_s")))
    return TY_STRING;

  /* A block with a top-level `break <v>` makes this call return <v>, so its
     result is the union of the normal result and the break value -- poly. The
     break wrapper suppresses this (g_infer_ignore_brk) to recover the normal
     result type for the inner emission. */
  if (!g_infer_ignore_brk && call_breaks(c, id)) return TY_POLY;

  TyKind rt = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
  /* A Float range (1.0..3.0) is a distinct type with float endpoints; it is
     not iterable, so its whole method face reduces to endpoint queries,
     membership tests, and the sole materializing method, step. */
  if (rt == TY_FLOAT_RANGE) {
    if (sp_streq(name, "begin") || sp_streq(name, "end") ||
        sp_streq(name, "first") || sp_streq(name, "last") ||
        sp_streq(name, "min") || sp_streq(name, "max"))
      return argc == 0 ? TY_FLOAT : TY_POLY;   /* first(n)/last(n) raise anyway */
    if (sp_streq(name, "cover?") || sp_streq(name, "include?") ||
        sp_streq(name, "member?") || sp_streq(name, "===") ||
        sp_streq(name, "==") || sp_streq(name, "eql?") ||
        sp_streq(name, "exclude_end?") || sp_streq(name, "frozen?") ||
        sp_streq(name, "respond_to?") || sp_streq(name, "nil?") ||
        sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "instance_of?") || sp_streq(name, "equal?"))
      return TY_BOOL;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return TY_STRING;
    if (sp_streq(name, "step")) return TY_FLOAT_ARRAY;
    if (sp_streq(name, "bsearch") && nt_ref(nt, id, "block") >= 0) return TY_FLOAT;
    if (sp_streq(name, "class")) return TY_CLASS;
    if (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
        sp_streq(name, "dup") || sp_streq(name, "clone"))
      return TY_FLOAT_RANGE;
    /* each/map/sum/to_a/... raise "can't iterate from Float" at run time; a
       poly result keeps the boxed-nil slot the raise leaves behind valid (and
       lets respond_to? report these Enumerable methods as present, like CRuby).
       A name outside this set is genuinely undefined, so leave it UNKNOWN: the
       respond_to? probe reads that as "not dispatchable" (false), matching an
       ordinary int range, and a real call errors like any unknown method. */
    {
      static const char *const iter[] = {
        "each", "map", "collect", "select", "filter", "reject", "to_a", "to_h",
        "entries", "find", "detect", "find_index", "count", "sum", "sort",
        "sort_by", "min_by", "max_by", "reduce", "inject", "each_with_index",
        "flat_map", "collect_concat", "any?", "all?", "none?", "one?", "take",
        "drop", "take_while", "drop_while", "filter_map", "partition",
        "group_by", "each_with_object", "tally", "find_all", "zip", "grep",
        "grep_v", "uniq", "reverse", "minmax", "join", "index", "size", "lazy",
        "each_cons", "each_slice", "chunk", "chunk_while", "cycle", NULL };
      for (int k = 0; iter[k]; k++) if (sp_streq(name, iter[k])) return TY_POLY;
    }
    return TY_UNKNOWN;
  }
  /* endless literal range: size is the Float infinity; take/first with a
     count materialize just the counted prefix (nothing else can) */
  if (rt == TY_RANGE && recv >= 0) {
    int rnA = recv;
    while (rnA >= 0 && nt_type(nt, rnA) && sp_streq(nt_type(nt, rnA), "ParenthesesNode")) {
      int pbA = nt_ref(nt, rnA, "body"); int pnA = 0;
      const int *ppA = pbA >= 0 ? nt_arr(nt, pbA, "body", &pnA) : NULL;
      rnA = pnA == 1 ? ppA[0] : -1;
    }
    /* a local holding only such a literal counts too (sole-assignment) */
    if (rnA >= 0 && nt_type(nt, rnA) && !sp_streq(nt_type(nt, rnA), "RangeNode")) {
      int slA = local_sole_range_node(c, rnA);
      if (slA >= 0) rnA = slA;
    }
    if (rnA >= 0 && nt_type(nt, rnA) && sp_streq(nt_type(nt, rnA), "RangeNode") &&
        (nt_ref(nt, rnA, "right") < 0 ||
         infer_end_is_float_inf(c, nt_ref(nt, rnA, "right"))) &&
        nt_ref(nt, rnA, "left") >= 0) {
      if (sp_streq(name, "size") && argc == 0) return TY_FLOAT;
      if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1)
        return TY_INT_ARRAY;
    }
  }
  if (rt == TY_RANGE && sp_streq(name, "sum") && argc == 1 &&
      nt_ref(nt, id, "block") < 0)
    return infer_type(c, argv[0]) == TY_FLOAT ? TY_FLOAT : TY_INT;
  /* A Range Enumerable method spinel serves by materializing to an int array:
     infer it as the array version (the array arms below key on `rt`). */
  if (rt == TY_RANGE && range_enum_redispatch(c, id)) rt = TY_INT_ARRAY;
  /* the block form of each_with_index returns the receiver hash (#2417) */
  if (ty_is_hash(rt) && sp_streq(name, "each_with_index") &&
      nt_ref(nt, id, "block") >= 0) return rt;
  if (ty_is_hash(rt) && hash_enum_redispatch(c, id)) rt = TY_POLY_ARRAY;
  /* A block each-family call returns its receiver (each, each_value/each_key/
     each_pair, each_with_index, reverse_each), so the value form composes:
     r = arr.each { }; arr.each { }.map { }. Gated to receivers that define
     the method -- the respond_to? machinery probes a synthesized
     `recv.m { }`, and an unconditional arm would make every type "respond"
     to each. */
  if (recv >= 0 && argc == 0 && nt_ref(nt, id, "block") >= 0 &&
      nt_type(nt, nt_ref(nt, id, "block")) &&
      sp_streq(nt_type(nt, nt_ref(nt, id, "block")), "BlockNode")) {
    /* `obj.__enum_to_a.<m> { }` (m in each / each_with_index / reverse_each) is
       the desugared each-family call on a user Enumerable / Struct receiver:
       Ruby returns the enumerable itself, so type it as the original receiver
       obj, not the intermediate member array (#2546/#2547). The codegen value
       form (emit_iter_value_expr) yields obj to match. */
    if ((sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each") || sp_streq(name, "each_entry")) &&
        nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
      const char *rnm = nt_str(nt, recv, "name");
      int orecv = rnm && sp_streq(rnm, "__enum_to_a") ? nt_ref(nt, recv, "receiver") : -1;
      if (orecv >= 0) return infer_type(c, orecv);
    }
    int enumerable_recv = ty_is_array(rt) || ty_is_hash(rt) ||
                          rt == TY_RANGE || rt == TY_ENUMERATOR;
    if (enumerable_recv &&
        (sp_streq(name, "each") || sp_streq(name, "each_with_index") ||
         sp_streq(name, "reverse_each")))
      return rt;
    if (ty_is_hash(rt) &&
        (sp_streq(name, "each_value") || sp_streq(name, "each_key") ||
         sp_streq(name, "each_pair")))
      return rt;
  }
  TyKind a0 = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
  /* Object#itself is the receiver, whatever its type -- the scattered per-type
     arms below predate this and remain harmless. */
  if (recv >= 0 && argc == 0 && sp_streq(name, "itself") &&
      nt_ref(nt, id, "block") < 0 && !an_user_defines_method(c, "itself"))
    return infer_type(c, recv);

  /* A literal integer power whose result exceeds int64 (`10 ** 30`, `2 ** 70`)
     is a Bignum in every overflow mode (CRuby). Type it bigint so codegen emits
     sp_bigint_pow rather than the saturating float sp_int_pow. */
  if (sp_streq(name, "**") && recv >= 0 && argc == 1) {
    long long base, exp;
    if (infer_const_int_node(nt, recv, &base) && infer_const_int_node(nt, argv[0], &exp) &&
        infer_int_pow_overflows(base, exp))
      return TY_BIGINT;
  }
  /* Integer ** <negative literal>: a Rational in CRuby, typed statically. A
     runtime exponent keeps the static Integer result (typing it poly would
     cascade through every int-arithmetic consumer -- see limitations.md); the
     negative case raises loudly, and the poly-dispatched path (promote-mode
     params) resolves the class at runtime in sp_poly_pow. */
  /* The cheap operand-type and name gates come BEFORE infer_type(c, recv): that
     receiver re-inference recurses through infer_call, so calling it for every
     1-arg call node makes a wide call graph explode (#2707). */
  if (a0 == TY_INT && sp_streq(name, "**") && recv >= 0 && argc == 1 &&
      infer_type(c, recv) == TY_INT) {
    long long exp;
    if (infer_const_int_node(nt, argv[0], &exp) && exp < 0) return TY_RATIONAL;
  }
  /* Integer with a Rational/Complex operand: ** Complex is Complex; ** Rational
     is a Float (by design, see codegen); fdiv is Float, div is the Integer floor. */
  if (recv >= 0 && argc == 1 && (a0 == TY_COMPLEX || a0 == TY_RATIONAL) &&
      (sp_streq(name, "**") || sp_streq(name, "fdiv") || sp_streq(name, "div")) &&
      infer_type(c, recv) == TY_INT) {
    if (sp_streq(name, "**") && a0 == TY_COMPLEX) return TY_COMPLEX;
    if (sp_streq(name, "**") && a0 == TY_RATIONAL) return TY_FLOAT;
    if (sp_streq(name, "fdiv") && (a0 == TY_RATIONAL || a0 == TY_COMPLEX)) return TY_FLOAT;
    if (sp_streq(name, "div") && (a0 == TY_RATIONAL || a0 == TY_COMPLEX)) return TY_INT;
  }
  /* A literal left shift whose result exceeds int64 (`1 << 64`, the 2**64 mask)
     is a Bignum -- type it bigint so codegen emits a bigint shift, not a UB C
     `1LL << 64LL`. */
  if (sp_streq(name, "<<") && recv >= 0 && argc == 1) {
    long long base, amount;
    if (infer_const_int_node(nt, recv, &base) && infer_const_int_node(nt, argv[0], &amount) &&
        infer_int_shl_overflows(base, amount))
      return TY_BIGINT;
  }

  /* `@table[i][j]` on a poly-array dispatch table of int-returning entries
     yields an int (a method/peek table). Resolves the optcarrot CPU's
     fetch/peek/store, which would otherwise run boxed-poly. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && poly_double_index_int(c, id))
    return TY_INT;
  /* `@nested[i]` on a poly array of int arrays yields an int array (nested
     array of int arrays -- @chr_banks / @nmt_mem). Without this the element
     read is a boxed poly and cascades poly through the PPU. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && poly_index_int_array(c, id))
    return TY_INT_ARRAY;
  /* `CONST[i]` on a poly-array constant of int arrays (WAVE_FORM / TILE_LUT
     shapes) yields an int array; codegen unboxes the poly element. */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1 && const_poly_index_int_array(c, id))
    return TY_INT_ARRAY;

  /* Complex / Rational value types. */
  /* A class-tagged poly value answers #name/#to_s/#inspect with its class name
     (Base.subclasses / #ancestors hand back boxed classes, #2656). Only when no
     user class defines the method -- then it dispatches to that instead. */
  if (recv >= 0 && rt == TY_POLY && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      !an_user_defines_method(c, name))
    return TY_STRING;
  /* bool/nil <=> : 0 for an equal immediate pair, nil otherwise (#2733) */
  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>") &&
      (rt == TY_BOOL || rt == TY_NIL)) return TY_POLY;
  /* __enum_chain(arr): the desugared Enumerable#chain / Enumerator#+ (#2545) */
  if (recv < 0 && sp_streq(name, "__enum_chain") && argc == 1) return TY_ENUMERATOR;
  /* Dir surface (#2823, #2828, #2830) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
    if (sp_streq(name, "empty?") && argc == 1) return TY_BOOL;
    if (sp_streq(name, "home") && argc == 1) return TY_STRING;
    if (sp_streq(name, "glob") && (argc == 1 || argc == 2)) return TY_STR_ARRAY;
  }
  /* the desugared ENV snapshot (#2742) */
  if (recv < 0 && sp_streq(name, "__env_to_h") && argc == 0) return TY_STR_STR_HASH;
  if (recv < 0 && sp_streq(name, "Complex")) return TY_COMPLEX;
  if (recv < 0 && sp_streq(name, "Rational") && (argc == 1 || argc == 2)) return TY_RATIONAL;
  if (recv >= 0) {
    const char *rrty = nt_type(nt, recv);
    if (rrty && sp_streq(rrty, "ConstantReadNode") && nt_str(nt, recv, "name") &&
        sp_streq(nt_str(nt, recv, "name"), "Complex") &&
        (sp_streq(name, "polar") || sp_streq(name, "rect") || sp_streq(name, "rectangular")))
      return TY_COMPLEX;
  }
  if (rt == TY_INT && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
    if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) return TY_COMPLEX;
    if (sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
  }
  if (rt == TY_FLOAT && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX) {
    if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) return TY_COMPLEX;
    if (sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
  }
  if (rt == TY_RATIONAL && argc == 1 && comp_ntype(c, argv[0]) == TY_COMPLEX &&
      (sp_streq(name, "+") || sp_streq(name, "-") ||
       sp_streq(name, "*") || sp_streq(name, "/"))) return TY_COMPLEX;
  if (rt == TY_COMPLEX) {
    if (sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) return TY_FLOAT;
    /* real/imaginary/abs/abs2 box to poly: each component keeps its CRuby
       class (Integer or Float) -- the class is a runtime property. */
    if (sp_streq(name, "real") || sp_streq(name, "imaginary") || sp_streq(name, "imag") ||
        sp_streq(name, "abs") || sp_streq(name, "magnitude") || sp_streq(name, "abs2")) return TY_POLY;
    if (sp_streq(name, "polar") || sp_streq(name, "rect") || sp_streq(name, "rectangular"))
      return TY_POLY_ARRAY;
    if (sp_streq(name, "conjugate") || sp_streq(name, "conj") || sp_streq(name, "to_c") ||
        sp_streq(name, "-@") || sp_streq(name, "+@") ||
        sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
        sp_streq(name, "/") || sp_streq(name, "quo")) return TY_COMPLEX;
    if (sp_streq(name, "**")) return TY_COMPLEX;
    /* Complex is not Comparable and has no modulo: these raise NoMethodError
       (typed Complex only so the raise expression has a consistent slot) (#2618) */
    if (sp_streq(name, "%") || sp_streq(name, "modulo")) return TY_COMPLEX;
    if (sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return TY_STRING;
    if (sp_streq(name, "to_i") || sp_streq(name, "to_int") ||
        sp_streq(name, "denominator")) return TY_INT;
    if (sp_streq(name, "to_f")) return TY_FLOAT;
    if (sp_streq(name, "to_r")) return TY_RATIONAL;
    if (sp_streq(name, "numerator")) return TY_COMPLEX;
    if (sp_streq(name, "zero?") || sp_streq(name, "real?") ||
        sp_streq(name, "integer?") || sp_streq(name, "finite?") ||
        sp_streq(name, "eql?")) return TY_BOOL;
    if (sp_streq(name, "nonzero?")) return TY_POLY;   /* self (Complex) or nil */
    if (sp_streq(name, "infinite?")) return TY_INT;      /* 1 or nil (sentinel) */
    if (sp_streq(name, "<=>") && argc == 1) return TY_INT;  /* -1/0/1 or nil (sentinel) */
    if (sp_streq(name, "rationalize") && (argc == 0 || argc == 1)) return TY_RATIONAL;
    if (sp_streq(name, "fdiv") && argc == 1) return TY_COMPLEX;
    if (sp_streq(name, "coerce") && argc == 1) return TY_POLY_ARRAY;
  }
  /* Proc#curry and curry application via []. A curried call stays TY_CURRY until
     it reaches the proc's arity, when it realizes to the proc's return type (the
     runtime accumulates int args, so completion typing covers int-returning
     procs; partial applications and other returns remain TY_CURRY). */
  if (rt == TY_PROC && sp_streq(name, "curry")) return TY_CURRY;
  if (rt == TY_CURRY && (sp_streq(name, "[]") || sp_streq(name, "call") || sp_streq(name, "()"))) {
    int complete = 0; TyKind cret = TY_UNKNOWN;
    if (curry_apply_info(c, id, &complete, &cret) && complete)
      return cret == TY_INT ? TY_INT : TY_POLY;   /* boxed realization otherwise */
    return TY_CURRY;
  }
  /* A curried proc reports as a lambda Proc (#2651). */
  if (rt == TY_CURRY && argc == 0 && sp_streq(name, "arity")) return TY_INT;
  if (rt == TY_CURRY && argc == 0 && sp_streq(name, "lambda?")) return TY_BOOL;

  /* clamp(lo, hi) with a nil (open) bound returns the receiver or the applied
     bound unchanged, boxed to preserve its class (#2588). */
  if ((rt == TY_INT || rt == TY_FLOAT) && sp_streq(name, "clamp") && argc == 2 &&
      (comp_ntype(c, argv[0]) == TY_NIL || comp_ntype(c, argv[1]) == TY_NIL))
    return TY_POLY;
  if (rt == TY_INT && sp_streq(name, "clamp") && argc == 1 &&
      nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode") &&
      ((nt_ref(nt, argv[0], "left") >= 0 && infer_type(c, nt_ref(nt, argv[0], "left")) == TY_FLOAT) ||
       (nt_ref(nt, argv[0], "right") >= 0 && infer_type(c, nt_ref(nt, argv[0], "right")) == TY_FLOAT)))
    return TY_POLY;
  /* a non-float bound can be returned as-is: a float receiver's mixed
     2-arg clamp is boxed (0.5.clamp(1, 3) is the Integer 1) */
  if (rt == TY_FLOAT && sp_streq(name, "clamp") && argc == 2 &&
      !(infer_type(c, argv[0]) == TY_FLOAT && infer_type(c, argv[1]) == TY_FLOAT) &&
      (infer_type(c, argv[0]) == TY_INT || infer_type(c, argv[0]) == TY_FLOAT) &&
      (infer_type(c, argv[1]) == TY_INT || infer_type(c, argv[1]) == TY_FLOAT)) return TY_POLY;
  if (rt == TY_INT && sp_streq(name, "divmod") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT) return TY_POLY_ARRAY;
  if (rt == TY_INT && sp_streq(name, "modulo") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT) return TY_FLOAT;
  if (rt == TY_INT && sp_streq(name, "remainder") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT) return TY_FLOAT;
  if (rt == TY_FLOAT && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL &&
      (sp_streq(name, "%") || sp_streq(name, "modulo"))) return TY_FLOAT;
  if (rt == TY_FLOAT && sp_streq(name, "clamp") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_RANGE) return TY_POLY;
  /* clamp(Float range): the clamped-to bound keeps its Float class; the result
     is boxed (Float, or Int for an in-range Int receiver) -> poly. */
  if ((rt == TY_FLOAT || rt == TY_INT) && sp_streq(name, "clamp") && argc == 1 &&
      comp_ntype(c, argv[0]) == TY_FLOAT_RANGE) return TY_POLY;
  if (rt == TY_INT && sp_streq(name, "round") && argc >= 1 &&
      nt_type(nt, argv[argc - 1]) &&
      sp_streq(nt_type(nt, argv[argc - 1]), "KeywordHashNode")) return TY_INT;
  if (rt == TY_INT && sp_streq(name, "quo") && argc == 1 && comp_ntype(c, argv[0]) == TY_FLOAT) return TY_FLOAT;
  if (rt == TY_INT && sp_streq(name, "quo")) return TY_RATIONAL;
  /* Float#quo is float division (Numeric#quo via /; no Rational) */
  if (rt == TY_FLOAT && sp_streq(name, "quo")) return TY_FLOAT;
  /* Float <op> Rational (either side) coerces to Float; comparisons bool */
  if (argc == 1 &&
      ((rt == TY_FLOAT && comp_ntype(c, argv[0]) == TY_RATIONAL) ||
       (rt == TY_RATIONAL && comp_ntype(c, argv[0]) == TY_FLOAT))) {
    if (is_arith_op(name) || sp_streq(name, "quo") || sp_streq(name, "fdiv"))
      return TY_FLOAT;
    if (is_cmp_op(name) || sp_streq(name, "==")) return TY_BOOL;
  }
  /* Integer <op> Rational coerces the Integer to Rational (result Rational for
     arithmetic, Bool/Int for comparisons). */
  if (rt == TY_INT && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) {
    if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")) return TY_RATIONAL;
    if (sp_streq(name, "%") || sp_streq(name, "modulo") || sp_streq(name, "remainder")) return TY_RATIONAL;
    if (sp_streq(name, "divmod")) return TY_POLY_ARRAY;
    if (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") || sp_streq(name, ">=") ||
        sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
    if (sp_streq(name, "<=>")) return TY_INT;
  }
  if (rt == TY_RATIONAL) {
    /* step walks the sequence yielding Rational/Integer values: with a block it
       returns the receiver (self), without one it materializes a poly array of
       the boxed values (#2566). */
    if (sp_streq(name, "step")) {
      if (nt_ref(nt, id, "block") >= 0) return rt;
      return TY_POLY_ARRAY;
    }
    if (sp_streq(name, "numerator") || sp_streq(name, "denominator")) return TY_INT;
    if (sp_streq(name, "to_f") || sp_streq(name, "fdiv")) return TY_FLOAT;
    if (sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "div")) return TY_INT;
    /* round/truncate: no digits (or a literal <= 0) is an Integer, a literal
       positive precision keeps the Rational, and a non-literal precision boxes
       to poly so the class is chosen from the runtime value. */
    if (sp_streq(name, "round") || sp_streq(name, "truncate") ||
        sp_streq(name, "floor") || sp_streq(name, "ceil")) {
      if (argc == 1) {
        if (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "IntegerNode"))
          return nt_int(nt, argv[0], "value", 0) > 0 ? TY_RATIONAL : TY_INT;
        return TY_POLY;
      }
      return TY_INT;
    }
    if (sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
        sp_streq(name, "negative?") || sp_streq(name, "finite?") ||
        sp_streq(name, "integer?") || sp_streq(name, "real?")) return TY_BOOL;
    if (sp_streq(name, "infinite?") || sp_streq(name, "imaginary") ||
        sp_streq(name, "imag")) return TY_INT;
    if (sp_streq(name, "nonzero?")) return TY_POLY;
    if (sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) return TY_POLY;
    if (sp_streq(name, "to_c")) return TY_COMPLEX;
    /* Rational#i -> Complex(0, self). spinel's Complex holds two floats, so the
       imaginary part renders as a float where CRuby keeps the exact Rational
       (see docs/limitations.md). #2706 */
    if (sp_streq(name, "i") && argc == 0) return TY_COMPLEX;
    if (sp_streq(name, "rectangular") || sp_streq(name, "rect") || sp_streq(name, "polar")) return TY_POLY_ARRAY;
    if (sp_streq(name, "coerce") && argc == 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return TY_STRING;
    if (sp_streq(name, "to_r") || sp_streq(name, "rationalize") ||
        sp_streq(name, "-@") || sp_streq(name, "+@") || sp_streq(name, "abs") ||
        sp_streq(name, "real") || sp_streq(name, "conjugate") || sp_streq(name, "conj") ||
        sp_streq(name, "abs2") || sp_streq(name, "magnitude")) return TY_RATIONAL;
    TyKind a0r = argc == 1 ? comp_ntype(c, argv[0]) : TY_UNKNOWN;
    if (argc == 1 && (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
                      sp_streq(name, "/") || sp_streq(name, "quo")))
      return a0r == TY_FLOAT ? TY_FLOAT : TY_RATIONAL;
    if (argc == 1 && sp_streq(name, "**")) return a0r == TY_INT ? TY_RATIONAL : TY_FLOAT;
    if (argc == 1 && (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
                      sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=") ||
                      sp_streq(name, "==="))) return TY_BOOL;
    if (argc == 1 && sp_streq(name, "<=>")) return TY_INT;
    if (argc == 2 && sp_streq(name, "between?")) return TY_BOOL;
    if (argc == 2 && sp_streq(name, "clamp") &&
        infer_type(c, argv[0]) == TY_RATIONAL && infer_type(c, argv[1]) == TY_RATIONAL) return TY_RATIONAL;
    if (argc == 1 && (sp_streq(name, "%") || sp_streq(name, "modulo") ||
                      sp_streq(name, "remainder")))
      return infer_type(c, argv[0]) == TY_FLOAT ? TY_FLOAT : TY_RATIONAL;
    if (argc == 1 && sp_streq(name, "divmod")) return TY_POLY_ARRAY;
  }

  /* Safe navigation &. : nil receiver always short-circuits to nil */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && sp_streq(call_op, "&.") && rt == TY_NIL) return TY_NIL;
  }

  /* nil receiver: type inference for NilClass methods */
  if (recv >= 0 && sp_streq(name, "display") && argc == 0) return TY_NIL;
  if (recv >= 0 && sp_streq(name, "instance_variable_defined?") && argc == 1 &&
      ty_is_object(rt)) return TY_BOOL;
  if (recv >= 0 && rt == TY_SYMBOL && argc == 0 && sp_streq(name, "encoding"))
    return TY_POLY;  /* a boxed Encoding value */
  if (recv >= 0 && (rt == TY_BOOL || rt == TY_SYMBOL || rt == TY_FLOAT) && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?"))) return TY_BOOL;
  if (recv >= 0 && rt == TY_FLOAT && argc == 1 && sp_streq(name, "===")) return TY_BOOL;  /* (#2400) */
  if (recv >= 0 && rt == TY_NIL) {
    if (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
        sp_streq(name, "===") || sp_streq(name, "equal?") || sp_streq(name, "eql?") ||
        sp_streq(name, "!~")) return TY_BOOL;
    if (sp_streq(name, "=~")) return TY_NIL;
    if (sp_streq(name, "rationalize")) return TY_RATIONAL;
    /* nil <=> nil is 0 (int); any other operand answers nil */
    if (sp_streq(name, "<=>") && argc == 1)
      return infer_type(c, argv[0]) == TY_NIL ? TY_INT : TY_NIL;
    if (sp_streq(name, "tap")) return TY_POLY;  /* the (boxed) nil receiver */
    if ((sp_streq(name, "then") || sp_streq(name, "yield_self")) &&
        nt_ref(nt, id, "block") >= 0) {
      int blk9 = nt_ref(nt, id, "block");
      int bd9 = nt_ref(nt, blk9, "body");
      int bn9 = 0; const int *bb9 = bd9 >= 0 ? nt_arr(nt, bd9, "body", &bn9) : NULL;
      if (bn9 >= 1) {
        TyKind bt9 = infer_type(c, bb9[bn9 - 1]);
        return bt9 == TY_NIL ? TY_POLY : bt9;
      }
    }
    if (sp_streq(name, "to_c")) return TY_COMPLEX;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return TY_STRING;
    if (sp_streq(name, "nil?") || sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "instance_of?")) return TY_BOOL;
    if (sp_streq(name, "to_i") || sp_streq(name, "to_int")) return TY_INT;
    if (sp_streq(name, "to_f")) return TY_FLOAT;
    if (sp_streq(name, "to_r")) return TY_RATIONAL;
    if (sp_streq(name, "to_a")) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_h")) return TY_SYM_POLY_HASH;
    if (sp_streq(name, "respond_to?")) return TY_BOOL;
  }

  /* <array>.cycle.first(n) / .take(n) -> same array kind (bounded consumer of the
     infinite cycle; the unbounded forms stay a loud reject). */
  if (recv >= 0 && (sp_streq(name, "first") || sp_streq(name, "take")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "cycle") &&
      nt_ref(nt, recv, "block") < 0) {
    int cargs = nt_ref(nt, recv, "arguments");
    int cac = 0; if (cargs >= 0) nt_arr(nt, cargs, "arguments", &cac);
    int pr = nt_ref(nt, recv, "receiver");
    if (cac == 0 && pr >= 0) { TyKind rt2 = infer_type(c, pr); if (ty_is_array(rt2)) return rt2; }
  }

  /* int_array.chunk_while/slice_when/chunk { |...| } .to_a -> a poly array (runs / pairs) */
  if (recv >= 0 && sp_streq(name, "to_a") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "chunk_while") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_when") ||
       sp_streq(nt_str(nt, recv, "name"), "chunk") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_before") ||
       sp_streq(nt_str(nt, recv, "name"), "slice_after")) &&
      nt_ref(nt, recv, "block") >= 0 &&
      (nt_ref(nt, recv, "arguments") < 0 ||
       (!sp_streq(nt_str(nt, recv, "name"), "slice_before") &&
        !sp_streq(nt_str(nt, recv, "name"), "slice_after")))) {
    int pr = nt_ref(nt, recv, "receiver");
    if (pr >= 0) {
      TyKind prt = infer_type(c, pr);
      if (prt == TY_INT_ARRAY || prt == TY_POLY_ARRAY || prt == TY_STR_ARRAY ||
          prt == TY_FLOAT_ARRAY ||
          (prt == TY_UNKNOWN && nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "ArrayNode")) ||
          (prt == TY_RANGE && range_enum_redispatch(c, recv)))
        return TY_POLY_ARRAY;
      /* hash.chunk { |k, v| key }.to_a materializes the same way (the chunk
         first-class emitter iterates the hash directly); gated to the named
         two-param block shape the emitter serves. */
      if (ty_is_hash(prt) && sp_streq(nt_str(nt, recv, "name"), "chunk") &&
          block_param_name(c, nt_ref(nt, recv, "block"), 0) &&
          block_param_name(c, nt_ref(nt, recv, "block"), 1))
        return TY_POLY_ARRAY;
    }
  }

  /* chunk_while/slice_when/chunk { } standing on its own (no .to_a terminal,
     which the arm above claims first): a first-class Enumerator over the
     eagerly materialized runs */
  if (recv >= 0 && nt_ref(nt, id, "block") >= 0 &&
      (sp_streq(name, "chunk_while") || sp_streq(name, "slice_when") ||
       sp_streq(name, "chunk") ||
       ((sp_streq(name, "slice_before") || sp_streq(name, "slice_after")) &&
        nt_ref(nt, id, "arguments") < 0))) {
    TyKind crt = infer_type(c, recv);
    if (crt == TY_POLY_ARRAY || crt == TY_INT_ARRAY || crt == TY_STR_ARRAY) {
      int wrapped = 0;
      NT_FOREACH_KIND(nt, NK_CallNode, w) {
        if (nt_ref(nt, w, "receiver") != id) continue;
        const char *wn = nt_str(nt, w, "name");
        if (wn && sp_streq(wn, "to_a")) wrapped = 1;
      }
      if (!wrapped) return TY_ENUMERATOR;
    }
  }

  /* an empty array literal used directly as a receiver (`[].flatten`) has no
     usage to fold an element type from; treat it as an empty poly array so
     array methods dispatch instead of falling through to unresolved. */
  if (rt == TY_UNKNOWN && recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ArrayNode")) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* first/last of an empty array is nil, boxed to poly (codegen emits
           sp_box_nil). min/max/pop/shift/sample keep the historical int-0
           shortcut pending their own nil arms. */
        if ((sp_streq(name, "first") || sp_streq(name, "last") ||
             sp_streq(name, "sample")) && argc == 0) return TY_POLY;  /* nil, boxed (#2322) */
        if ((sp_streq(name, "nil?") || sp_streq(name, "empty?")) && argc == 0) return TY_BOOL;
        /* [] + X / [] - X / set-ops keep the OTHER operand's array kind (the
           codegen emits sp_<kind>Array_<op>(NULL, X)); the result type must
           agree with that kind, not the default poly array. */
        if ((sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "&") ||
             sp_streq(name, "|") || sp_streq(name, "union") || sp_streq(name, "difference") ||
             sp_streq(name, "intersection")) && argc == 1) {
          TyKind at = infer_type(c, argv[0]);
          if (ty_is_array(at) || at == TY_POLY_ARRAY) return at;
        }
        if ((sp_streq(name, "min") || sp_streq(name, "max") ||
             sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 0) return TY_INT;
        rt = TY_POLY_ARRAY;
      }
    }
    /* mirror for an empty HASH literal receiver (`{}.freeze`): without this
       the identity `freeze` stays unresolved, so `CONST = {}.freeze` never
       gets a type and the constant is dropped from codegen entirely -- reads
       then raise "uninitialized constant" or break the C build when the
       constant is a typed ivar's || fallback (#1758). Deliberately
       TY_STR_POLY_HASH, the same C type codegen emits for a bare {}, so the
       declaration, initializer, and readers agree (POLY_POLY would disagree
       with sp_StrPolyHash_new and produce garbage). */
    else if (rty && (sp_streq(rty, "HashNode") || sp_streq(rty, "KeywordHashNode"))) {
      int en = 0; nt_arr(nt, recv, "elements", &en);
      if (en == 0) {
        /* object query methods on a bare {} fold to a bool (see codegen) */
        if (argc == 0 && (sp_streq(name, "nil?") || sp_streq(name, "frozen?") ||
                          sp_streq(name, "empty?") || sp_streq(name, "any?"))) return TY_BOOL;
        if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
                          sp_streq(name, "instance_of?"))) return TY_BOOL;
        if (argc == 0 && sp_streq(name, "to_h")) return TY_STR_POLY_HASH;   /* (#2410) */
        if (argc <= 1 && sp_streq(name, "sum"))
          return argc == 1 ? infer_type(c, argv[0]) : TY_INT;               /* (#2416) */
        if (argc == 0 && (sp_streq(name, "min") || sp_streq(name, "max"))) return TY_POLY;
        if (argc == 0 && sp_streq(name, "minmax")) return TY_POLY_ARRAY;    /* (#2406) */
        if (argc == 1 && (sp_streq(name, "<") || sp_streq(name, "<=") ||
                          sp_streq(name, ">") || sp_streq(name, ">="))) return TY_BOOL;  /* (#2399) */
        rt = TY_STR_POLY_HASH;
      }
    }
  }

  /* `<&block-param>.call(...)` inside a yielding method: the explicit-call form
     of yield. Its value is the call-site block's value (resolved like yield). */
  {
    int emi = (int)(comp_scope_of(c, id) - c->scopes);
    if (emi > 0 && is_blk_param_call(c, id, emi)) return yield_value_type(c, emi);
  }

  /* __dir__ -> the source directory (a string) */
  if (recv < 0 && sp_streq(name, "__dir__") && argc == 0) return TY_STRING;

  /* Kernel#caller -> the call stack as strings (empty in release builds) */
  if (recv < 0 && sp_streq(name, "caller") && argc <= 2) return TY_STR_ARRAY;
  /* caller_locations -> an array of Backtrace::Location objects. AOT builds keep
     no runtime frame stack (like `caller`, which is empty in release), so this is
     an empty array. Returning a poly array (not nil) keeps `&.first&.label`,
     `.each`, `.map`, and `.is_a?(Array)` well-typed and nil-safe. */
  if (recv < 0 && sp_streq(name, "caller_locations") && argc <= 2) return TY_POLY_ARRAY;

  /* bare `name` inside a class method body -> the class name string */
  if (recv < 0 && sp_streq(name, "name") && argc == 0) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }
  /* `self.name` / `self.to_s` inside a class method -> the class name string */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->is_cmethod && self->class_id >= 0) return TY_STRING;
  }

  /* loop { break val } -> the type of the break value */
  if (recv < 0 && sp_streq(name, "loop")) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        TyKind bt = scan_break_type(c, body, 0);
        if (bt != TY_UNKNOWN) return bt;
      }
    }
    return TY_NIL;
  }

  /* catch(:tag) { ... } -> unify the block's last value with every throw
     value that can target the tag, across method boundaries (the throw need
     not be syntactically inside the body). */
  if (recv < 0 && sp_streq(name, "catch")) {
    int blk = nt_ref(nt, id, "block");
    TyKind result = TY_UNKNOWN;
    if (blk >= 0) {
      int body = nt_ref(nt, blk, "body");
      if (body >= 0) {
        int bn = 0; const int *bb = nt_arr(nt, body, "body", &bn);
        if (bn > 0) result = infer_type(c, bb[bn - 1]);
      }
      const char *tag = NULL;
      int targ = nt_ref(nt, id, "arguments");
      int tac = 0; const int *tav = targ >= 0 ? nt_arr(nt, targ, "arguments", &tac) : NULL;
      if (tac >= 1 && nt_type(nt, tav[0])) {
        if (sp_streq(nt_type(nt, tav[0]), "SymbolNode")) tag = nt_str(nt, tav[0], "value");
        else if (sp_streq(nt_type(nt, tav[0]), "StringNode")) tag = nt_str(nt, tav[0], "unescaped");
      }
      TyKind tt = scan_throw_type(c, tag);
      if (tt != TY_UNKNOWN) result = ty_unify(result, tt);
    }
    return result == TY_UNKNOWN ? TY_NIL : result;
  }

  /* recv.instance_eval/exec { ... } -> the block's last-expression type
     (bare calls inside resolve via the ie node->class map). A trampoline
     method `recv.M { ... }` resolves the same way. */
  int ie_kind = (recv >= 0 && (sp_streq(name, "instance_eval") || sp_streq(name, "instance_exec")) &&
                 ty_is_object(rt) && comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  if (!ie_kind && recv >= 0 && ty_is_object(rt) && nt_ref(nt, id, "block") >= 0)
    ie_kind = comp_trampoline_kind(c, ty_object_class(rt), name, NULL) != 0;
  /* receiverless instance_eval/exec inside an instance method resolves to self */
  if (!ie_kind && recv < 0 && ie_implicit_self_class(c, id) >= 0) ie_kind = 1;
  if (ie_kind) {
    int blk = nt_ref(nt, id, "block");
    if (blk >= 0) {
      const char *bty = nt_type(nt, blk);
      if (bty && sp_streq(bty, "BlockArgumentNode")) {
        /* `instance_exec(args, &b)` forwards the enclosing method's block; the
           value it produces is that method's own forwarded-block value across
           call sites (the method inlines per site, splicing the literal). */
        Scope *encl = comp_scope_of(c, id);
        int emi = encl ? (int)(encl - c->scopes) : -1;
        if (emi >= 0) {
          TyKind ft = yield_value_type(c, emi);
          if (ft != TY_UNKNOWN && ft != TY_VOID) return ft;
        }
        /* The forwarded block's value isn't statically pinned here (its call
           sites may not be typed yet during the fixpoint). It is a real boxed
           value, not nil -- poly keeps the result a scalar carrier so the
           enclosing method stays inlinable and the splice yields it per site. */
        return TY_POLY;
      }
      int body = nt_ref(nt, blk, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      if (bn > 0) {
        TyKind bt = infer_type(c, bb[bn - 1]);
        if (bt == TY_VOID) bt = TY_NIL;
        /* A value-carrying break/next can widen the result past the last
           expression (e.g. `next val + 1` poly vs trailing `999` int). */
        TyKind bnt = ie_block_break_next_ty(c, body);
        if (bnt != TY_UNKNOWN)
          bt = (bt == TY_NIL || bt == TY_UNKNOWN) ? bnt : ty_unify(bt, bnt);
        return bt;
      }
      return TY_NIL;
    }
  }

  /* <StructClass>.members at the class level: symbol array */
  if (recv >= 0 && sp_streq(name, "members") && argc == 0) {
    const char *mrty = nt_type(nt, recv);
    int mci = -1;
    if (mrty && (sp_streq(mrty, "ConstantReadNode") || sp_streq(mrty, "ConstantPathNode")))
      mci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (mrty && (sp_streq(mrty, "LocalVariableReadNode") ||
                      (sp_streq(mrty, "CallNode") && is_struct_call(c, recv))))
      mci = class_var_static_ci(c, recv);
    if (mci >= 0 && c->classes[mci].is_struct &&
        comp_cmethod_in_chain(c, mci, "members", NULL) < 0) return TY_POLY_ARRAY;
  }
  if (recv >= 0 && sp_streq(name, "keyword_init?") && argc == 0) {
    const char *krty = nt_type(nt, recv);
    int kci = -1;
    if (krty && (sp_streq(krty, "ConstantReadNode") || sp_streq(krty, "ConstantPathNode")))
      kci = comp_class_index(c, nt_str(nt, recv, "name"));
    else if (krty && sp_streq(krty, "LocalVariableReadNode"))
      kci = class_var_static_ci(c, recv);
    if (kci >= 0 && c->classes[kci].is_struct &&
        comp_cmethod_in_chain(c, kci, "keyword_init?", NULL) < 0) return TY_POLY;  /* nil/true/false */
  }
  /* Integer.sqrt(Bignum) -> Bignum (#2420) */
  if (recv >= 0 && sp_streq(name, "sqrt") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
      infer_type(c, argv[0]) == TY_BIGINT) return TY_BIGINT;
  /* Hash[k: v] desugared to a bare hash literal: transparent passthrough */
  if (recv >= 0 && sp_streq(name, "__hash_brackets_kw")) return infer_type(c, recv);
  /* Hash[] with no arguments: an empty hash (same C type as a bare {}) */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash"))
    return TY_STR_POLY_HASH;
  /* Array/Integer.try_convert(x) -> the value or nil (poly) (#2325, #2585) */
  if (recv >= 0 && name && sp_streq(name, "try_convert") && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") &&
      (sp_streq(nt_str(nt, recv, "name"), "Array") || sp_streq(nt_str(nt, recv, "name"), "Integer")))
    return TY_POLY;
  if (recv >= 0 && name && argc == 1 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Hash")) {
    if (sp_streq(name, "try_convert")) return TY_POLY;
  }
  /* method(:sym) / <recv>.method(:sym) -> a bound Method object */
  if (name && sp_streq(name, "method") && method_sym_arg(c, id) != NULL) return TY_METHOD;

  /* <method>.call(args) / [] -> the target method's return type. */
  if (recv >= 0 && rt == TY_METHOD &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]"))) {
    int mn = method_recv_node(c, recv);
    int mi = mn >= 0 ? method_obj_target_mi(c, mn) : -1;
    if (mi >= 0) return c->scopes[mi].ret == TY_UNKNOWN ? TY_INT : c->scopes[mi].ret;
    /* Unresolved target: the bound-method ABI returns mrb_int -- except under
       promote, where every method is poly-signatured and bound methods are
       invoked through the poly ABI (sp_RbVal), so the call yields poly. */
    return g_promote_mode ? TY_POLY : TY_INT;
  }
  if (recv >= 0 && rt == TY_METHOD && argc == 0 && sp_streq(name, "to_proc")) return TY_PROC;
  /* Klass.instance_method(:m) -> an (unbound) method object; #bind re-binds (#2676) */
  if (recv >= 0 && sp_streq(name, "instance_method") && method_sym_arg(c, id) != NULL &&
      method_obj_target_mi(c, id) >= 0) return TY_METHOD;
  if (recv >= 0 && rt == TY_METHOD && argc == 1 && sp_streq(name, "bind")) return TY_METHOD;
  /* Method#owner is a class value; #receiver is the bound receiver (#2701) */
  if (recv >= 0 && rt == TY_METHOD && argc == 0 && sp_streq(name, "owner")) return TY_CLASS;
  if (recv >= 0 && rt == TY_METHOD && argc == 0 && sp_streq(name, "receiver")) {
    int mn = method_recv_node(c, recv);
    int mrecv = mn >= 0 ? nt_ref(nt, mn, "receiver") : -1;
    if (mrecv >= 0) return infer_type(c, mrecv);
  }
  /* <method>.name -> the method name as a Symbol; .arity -> int */
  if (recv >= 0 && rt == TY_METHOD && argc == 0) {
    if (sp_streq(name, "name")) return TY_SYMBOL;
    if (sp_streq(name, "arity")) return TY_INT;
    if (sp_streq(name, "to_proc")) return TY_PROC;
  }
  /* <poly>.call(args): a boxed Proc publishes its result through the boxed
     return slot, so the value is genuinely dynamic -- type it poly and let
     the call site read the slot intact (unboxing to int truncated an array
     or string result to garbage). (Skip when a user class defines `call`:
     that goes through normal dispatch and returns the method's own type.) */
  if (recv >= 0 && rt == TY_POLY &&
      (sp_streq(name, "call") || sp_streq(name, "()"))) {
    int has_user_call = 0;
    for (int k = 0; k < c->nclasses && !has_user_call; k++)
      if (comp_method_in_class(c, k, "call") >= 0) has_user_call = 1;
    if (!has_user_call) return TY_POLY;
  }

  /* strftime on a poly value that is really a Time formats to a String. A
     nilable Time (`created_at : Time?`) is held as a poly sp_RbVal, so the
     call would otherwise infer poly/unknown and the formatted string get
     discarded; the codegen poly dispatch gives it a SP_BUILTIN_TIME arm that
     formats a real Time and raises NoMethodError otherwise, so its non-raising
     result is always a String. Only when no user class supplies strftime, to
     match the codegen guard. Issue #2457 (family2 nilable value-method). */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "strftime") && argc == 1 &&
      infer_type(c, argv[0]) == TY_STRING) {
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, name, NULL);
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    if (ncand == 0) return TY_STRING;
  }

  /* ljust/rjust/center on a poly value that is really a String pads to a
     String. A string held in a poly slot (e.g. an element of a nested array)
     would otherwise infer unknown and the call fall through undispatched; the
     codegen poly dispatch gives it a SP_TAG_STR arm that pads a real String
     and raises NoMethodError otherwise, so its non-raising result is always a
     String. Only when no user class supplies the method, to match the codegen
     guard (same shape as the strftime arm above). */
  if (recv >= 0 && rt == TY_POLY && (argc == 1 || argc == 2) &&
      (sp_streq(name, "ljust") || sp_streq(name, "rjust") || sp_streq(name, "center")) &&
      (infer_type(c, argv[0]) == TY_INT || infer_type(c, argv[0]) == TY_POLY) &&
      (argc == 1 || infer_type(c, argv[1]) == TY_STRING)) {
    int ncand = 0;
    for (int k = 0; k < c->nclasses; k++) {
      int mi = comp_method_in_chain(c, k, name, NULL);
      if (mi >= 0 && argc >= c->scopes[mi].nrequired) ncand++;
    }
    if (ncand == 0) return TY_STRING;
  }

  /* proc {} / lambda {} / Proc.new {} -> a first-class Proc value */
  if (is_proc_literal(c, id)) return TY_PROC;

  /* <proc>.call(args) / .() / [] -> the proc's recorded body return type;
     Proc#=== (case/when dispatch) IS a call in CRuby */
  if (recv >= 0 && rt == TY_PROC &&
      (sp_streq(name, "call") || sp_streq(name, "()") || sp_streq(name, "[]") ||
       (sp_streq(name, "===") && argc == 1)))
    return proc_call_ret(c, recv);

  /* Proc composition: proc << proc / proc >> proc -> a new Proc. */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      (sp_streq(name, "<<") || sp_streq(name, ">>")) &&
      infer_type(c, argv[0]) == TY_PROC)
    return TY_PROC;

  /* Proc introspection */
  /* parameters(lambda: <bool/nil>) forces the view; same shape as parameters() */
  if (recv >= 0 && rt == TY_PROC && argc == 1 && sp_streq(name, "parameters"))
    return TY_POLY_ARRAY;
  if (recv >= 0 && rt == TY_PROC && argc == 0) {
    if (sp_streq(name, "arity")) return TY_INT;
    if (sp_streq(name, "lambda?")) return TY_BOOL;
    if (sp_streq(name, "parameters")) return TY_POLY_ARRAY;
    if (sp_streq(name, "source_location")) return TY_POLY_ARRAY;  /* [file, line] */
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "frozen?")) return TY_BOOL;
    if (sp_streq(name, "freeze") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "itself")) return TY_PROC;
  }
  /* Proc identity: equal?/eql?/== against another Proc -> bool */
  if (recv >= 0 && rt == TY_PROC && argc == 1 &&
      (sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==")) &&
      infer_type(c, argv[0]) == TY_PROC)
    return TY_BOOL;
  /* Hash#default_proc: the stored Hash.new{} block as a first-class Proc
     (NULL-encoded nil when the hash has none) */
  if (recv >= 0 && ty_is_hash(rt) && argc == 0 && sp_streq(name, "default_proc"))
    return TY_PROC;
  /* default_proc= installs the lambda; the assignment's value is the proc (#2371) */
  if (recv >= 0 && ty_is_hash(rt) && argc == 1 && sp_streq(name, "default_proc="))
    return TY_PROC;

  /* k = Struct.new(:a, :b): the value IS the synthesized anonymous struct
     class, as a first-class class object */
  if (anon_struct_ci_for_value(c, id) >= 0) return TY_CLASS;

  /* TY_CLASS method dispatch -- .new on a dynamic class variable returns TY_POLY.
     Exception: self.class.new(...) resolves to the enclosing class statically. */
  if (recv >= 0 && rt == TY_CLASS && sp_streq(name, "new") &&
      nt_type(nt, recv) && !sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      !sp_streq(nt_type(nt, recv), "ConstantPathNode")) {
    int _is_self_class = (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class") &&
      nt_ref(nt, recv, "receiver") >= 0 &&
      nt_type(nt, nt_ref(nt, recv, "receiver")) &&
      sp_streq(nt_type(nt, nt_ref(nt, recv, "receiver")), "SelfNode"));
    /* a local statically holding one STRUCT class (k = Struct.new(..) /
       k = StructKlass) falls through to the static-class .new arms below --
       its typed member accessors then dispatch statically. A plain class
       stays on the Tier-5 dynamic boxed path (test/dynamic_class_new.rb). */
    int _cvi = _is_self_class ? -1 : class_var_static_ci(c, recv);
    if (!_is_self_class &&
        (_cvi < 0 || !(c->classes[_cvi].is_struct || c->classes[_cvi].is_data)))
      return TY_POLY;
  }

  if (recv >= 0 && rt == TY_CLASS && !sp_streq(name, "new")) {
    if (argc == 0 && (sp_streq(name, "to_s") || sp_streq(name, "name") || sp_streq(name, "inspect")))
      return TY_STRING;
    if (argc == 0 && sp_streq(name, "nil?")) return TY_BOOL;
    if (argc == 0 && sp_streq(name, "singleton_class?")) return TY_BOOL;
    /* Module#constants -> sym array, recovered from the AST (#2674) */
    /* Class.const_set(:K, v) stores into the existing constant and yields the
       value; only a literal name whose type matches is emittable (#2675). */
    if (sp_streq(name, "const_set") && argc == 2) {
      const char *cs_aty = nt_type(nt, argv[0]);
      const char *cs_qm = NULL;
      if (cs_aty && sp_streq(cs_aty, "SymbolNode")) cs_qm = nt_str(nt, argv[0], "value");
      else if (cs_aty && sp_streq(cs_aty, "StringNode")) cs_qm = nt_str(nt, argv[0], "content");
      LocalVar *cv = cs_qm ? comp_const(c, cs_qm) : NULL;
      if (cv && cv->type != TY_UNKNOWN) return cv->type;
    }
    if (sp_streq(name, "constants") && argc <= 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "class_variables") && argc == 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "included_modules") && argc == 0) return TY_POLY_ARRAY;
    if (argc == 0 && sp_streq(name, "class")) return TY_CLASS;
    /* #superclass is a (nullable) class value: BasicObject's is the nil-class
       sentinel, carried within TY_CLASS (#2654). */
    if (argc == 0 && sp_streq(name, "superclass")) return TY_CLASS;
    if (argc == 1 && (sp_streq(name, "==") || sp_streq(name, "eql?") || sp_streq(name, "!=") ||
                      sp_streq(name, "==="))) return TY_BOOL;
    /* Class ordering is tri-state: true/false when related, nil when the two
       classes have no subclass relationship (CRuby). <=> is -1/0/1 or nil. */
    if (argc == 1 && (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
                      sp_streq(name, ">=") || sp_streq(name, "<=>"))) return TY_POLY;
    if (argc == 0 && sp_streq(name, "ancestors")) return TY_POLY_ARRAY;
    if (argc == 0 && sp_streq(name, "subclasses")) return TY_POLY_ARRAY;
    if (argc == 1 && (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?"))) return TY_BOOL;
    if (argc == 1 && sp_streq(name, "include?")) return TY_BOOL;
    if (argc == 1 && sp_streq(name, "class_variable_defined?")) return TY_BOOL;
    if (sp_streq(name, "class_variable_get") || sp_streq(name, "class_variable_set")) return TY_POLY;
    if (argc <= 1 && (sp_streq(name, "instance_methods") ||
                      sp_streq(name, "public_instance_methods") ||
                      sp_streq(name, "private_instance_methods") ||
                      sp_streq(name, "protected_instance_methods"))) return TY_POLY;
    /* a user class method on a Class-typed value carried in a plain variable
       (`model.table_name`): unify the return types of every user class
       defining it (poly on disagreement). A constant/accessor receiver keeps
       its existing dispatch, so only fire for a variable receiver (#2445). */
    {
      const char *rvty = nt_type(nt, recv);
      int recv_is_var = rvty && (sp_streq(rvty, "LocalVariableReadNode") ||
                                 sp_streq(rvty, "InstanceVariableReadNode"));
      TyKind uret = TY_UNKNOWN; int nc = recv_is_var ? 0 : -1000, set = 0;
      for (int k = 0; recv_is_var && k < c->nclasses; k++) {
        if (is_builtin_reopen(c->classes[k].name)) continue;
        int kmi = comp_cmethod_in_chain(c, k, name, NULL);
        if (kmi < 0) continue;
        if (c->scopes[kmi].nrequired != argc || c->scopes[kmi].rest_idx >= 0 ||
            c->scopes[kmi].yields ||
            (c->scopes[kmi].blk_param && c->scopes[kmi].blk_param[0])) { nc = 0; break; }
        nc++;
        TyKind kr = (TyKind)c->scopes[kmi].ret;
        if (!set) { uret = kr; set = 1; }
        else if (kr != uret) uret = TY_POLY;
      }
      if (nc > 0 && nt_ref(nt, id, "block") < 0)
        return (uret == TY_UNKNOWN || uret == TY_VOID) ? TY_POLY : uret;
    }
  }

  /* __method__ / __callee__ -> the enclosing method's name (a symbol), or
     nil at the top level, where the enclosing scope has no name (matching
     the codegen, which emits sp_box_nil() there) */
  if (recv < 0 && argc == 0 &&
      (sp_streq(name, "__method__") || sp_streq(name, "__callee__"))) {
    Scope *s = comp_scope_of(c, id);
    return (s && s->name && s->name[0]) ? TY_SYMBOL : TY_NIL;
  }

  /* identity methods: return the receiver unchanged (clone also with its
     freeze: keyword argument) */
  if (recv >= 0 &&
      (argc == 0 ||
       (argc == 1 && sp_streq(name, "clone") && argv && nt_type(nt, argv[0]) &&
        sp_streq(nt_type(nt, argv[0]), "KeywordHashNode"))) &&
      (sp_streq(name, "freeze") || sp_streq(name, "itself") ||
       sp_streq(name, "dup") || sp_streq(name, "clone")))
    return rt;

  /* bareword freeze (implicit self) returns self, so `def seal = freeze` and
     other freeze-as-value uses stay typed as the instance. (Matches the
     codegen arm that lowers bareword freeze to self.) */
  if (recv < 0 && argc == 0 && nt_ref(c->nt, id, "block") < 0 && sp_streq(name, "freeze")) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->class_id >= 0 && comp_method_in_chain(c, s->class_id, name, NULL) < 0)
      return ty_object(s->class_id);
  }
  /* bareword frozen? reads the instance's GC-header bit (see the codegen arm) */
  if (recv < 0 && argc == 0 && nt_ref(c->nt, id, "block") < 0 && sp_streq(name, "frozen?")) {
    Scope *s = comp_scope_of(c, id);
    if (s && s->class_id >= 0 && !s->is_cmethod &&
        comp_method_in_chain(c, s->class_id, name, NULL) < 0)
      return TY_BOOL;
  }

  /* x.class -> a first-class Class value for every known receiver kind
     (name-backed for builtins, id-backed for user objects) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "class")) {
    /* empty container literal receivers coerce like everywhere else */
    if (rt == TY_UNKNOWN && nt_type(nt, recv)) {
      const char *rty0 = nt_type(nt, recv);
      int en0 = 0;
      if (sp_streq(rty0, "ArrayNode")) { nt_arr(nt, recv, "elements", &en0); if (!en0) return TY_CLASS; }
      if (sp_streq(rty0, "HashNode") || sp_streq(rty0, "KeywordHashNode")) { nt_arr(nt, recv, "elements", &en0); if (!en0) return TY_CLASS; }
    }
    if (ty_is_object(rt)) return TY_CLASS;
    if (ty_is_numeric(rt) || rt == TY_STRING || rt == TY_SYMBOL || rt == TY_BOOL ||
        rt == TY_RANGE || rt == TY_TIME || rt == TY_NIL || rt == TY_POLY ||
        rt == TY_METHOD || rt == TY_PROC || rt == TY_IO || rt == TY_ARGF ||
        rt == TY_MATCHDATA || rt == TY_REGEX ||
        rt == TY_COMPLEX || rt == TY_RATIONAL || rt == TY_CURRY ||
        rt == TY_FIBER || rt == TY_ENUMERATOR || ty_is_array(rt) || ty_is_hash(rt))
      return TY_CLASS;
  }

  /* X.class.name / .to_s -> the class-name string (X.class is already that) */
  if (recv >= 0 && argc == 0 && (sp_streq(name, "name") || sp_streq(name, "to_s")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class"))
    return TY_STRING;

  /* __ENCODING__.name / .to_s / .inspect -> the encoding name string */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SourceEncodingNode"))
    return TY_STRING;
  /* <enc>.encoding.name -> the encoding name string */
  if (recv >= 0 && argc == 0 && sp_streq(name, "name") && rt == TY_POLY &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* Module.singleton_writer= / Module.singleton_reader */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") ||
       sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *cn = nt_str(nt, recv, "name");
    int ci = cn ? comp_class_index(c, cn) : -1;
    if (ci >= 0) {
      ClassInfo *cls = &c->classes[ci];
      int nlen = (int)strlen(name);
      /* setter: name ends with '=' */
      if (nlen > 1 && name[nlen - 1] == '=') {
        char base[256]; int blen = nlen - 1;
        if (blen > 0 && blen < (int)sizeof(base)) {
          memcpy(base, name, (size_t)blen); base[blen] = '\0';
          if (comp_is_sg_writer(cls, base)) return TY_VOID;
        }
      }
else {
        if (comp_is_sg_reader(cls, name)) return TY_POLY;
      }
    }
  }
  /* self.singleton_writer= / self.singleton_reader: inside a class method
     or directly in a class/module body (g_cbody_class_id). */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "SelfNode")) {
    Scope *_self = comp_scope_of(c, id);
    int _sg_cid = (_self && _self->is_cmethod && _self->class_id >= 0)
                  ? _self->class_id : g_cbody_class_id;
    if (_sg_cid >= 0) {
      ClassInfo *_cls = &c->classes[_sg_cid];
      int _nlen = (int)strlen(name);
      if (_nlen > 1 && name[_nlen - 1] == '=') {
        char _base[256]; int _blen = _nlen - 1;
        if (_blen > 0 && _blen < (int)sizeof(_base)) {
          memcpy(_base, name, (size_t)_blen); _base[_blen] = '\0';
          if (comp_is_sg_writer(_cls, _base)) return TY_VOID;
        }
      }
      else if (comp_is_sg_reader(_cls, name)) return TY_POLY;
    }
  }

  /* FFI: call on a module that registered ffi_func/ffi_buffer/ffi_read_* */
  if (recv >= 0 && nt_type(nt, recv)) {
    const char *rty_ffi = nt_type(nt, recv);
    const char *rcmod = NULL;
    if (sp_streq(rty_ffi, "ConstantReadNode"))
      rcmod = nt_str(nt, recv, "name");
    else if (sp_streq(rty_ffi, "ConstantPathNode"))
      rcmod = nt_str(nt, recv, "name");
    if (rcmod) {
      /* native binding (Path B): a native_func returns its declared type,
         gated by the module's require-gate feature. */
      int nvi = comp_native_find(c, rcmod, name);
      if (nvi >= 0) {
        const char *feat = c->native_funcs[nvi].feat;
        if (!feat || !feat[0] || sp_feature_enabled(feat))
          return native_spec_to_ty(c->native_funcs[nvi].ret);
      }
      int fi = ffi_find_func(c, rcmod, name);
      if (fi >= 0) return ffi_spec_to_ty(c->ffi_funcs[fi].ret);
      /* ffi_buffer: Module.buf_name returns the static char* (ptr type -> TY_POLY) */
      if (ffi_find_buf(c, rcmod, name) >= 0) return TY_POLY;
      /* ffi_read_*: Module.reader_name(buf) returns int or ptr */
      int ri = ffi_find_reader(c, rcmod, name);
      if (ri >= 0) {
        const char *kind = c->ffi_readers[ri].kind;
        if (kind && sp_streq(kind, "ptr")) return TY_POLY;
        return TY_INT;
      }
      /* ffi_struct: Name_new -> ptr, Name_get_<f> -> the field's type,
         Name_set_<f> -> nil. */
      int fsi, ffi;
      int fsm = ffi_struct_method(c, rcmod, name, &fsi, &ffi);
      if (fsm == FFI_SM_NEW) return TY_POLY;
      if (fsm == FFI_SM_GET) return ffi_spec_to_ty(c->ffi_structs[fsi].fields[ffi].spec);
      if (fsm == FFI_SM_SET) return TY_NIL;
      /* ffi_write_*: Module.writer_name(buf, val) returns the written value */
      int wi = ffi_find_writer(c, rcmod, name);
      if (wi >= 0) {
        const char *kind = c->ffi_writers[wi].kind;
        if (kind && sp_streq(kind, "ptr")) return TY_POLY;
        return TY_INT;
      }
    }
  }

  /* SomeClass.name / .to_s / .inspect -> class name string */
  if (recv >= 0 && argc == 0 &&
      (sp_streq(name, "name") || sp_streq(name, "to_s") || sp_streq(name, "inspect")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_STRING;
  /* SomeClass.superclass -> sp_Class value for the parent class */
  if (recv >= 0 && argc == 0 && sp_streq(name, "superclass") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_CLASS;

  /* SomeClass.ancestors -> PolyArray of class objects */
  if (recv >= 0 && argc == 0 && sp_streq(name, "ancestors") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && comp_class_index(c, nt_str(nt, recv, "name")) >= 0)
    return TY_POLY_ARRAY;

  /* SomeClass.instance_methods / .public_instance_methods -> PolyArray of symbols */
  if (recv >= 0 && argc <= 1 &&
      (sp_streq(name, "instance_methods") || sp_streq(name, "public_instance_methods") ||
       sp_streq(name, "private_instance_methods") || sp_streq(name, "protected_instance_methods")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode"))
    return TY_POLY_ARRAY;

  /* self.class.new(...) -> an instance of the enclosing class */
  if (recv >= 0 && sp_streq(name, "new") && nt_type(nt, recv) &&
      sp_streq(nt_type(nt, recv), "CallNode") && nt_str(nt, recv, "name") &&
      sp_streq(nt_str(nt, recv, "name"), "class")) {
    Scope *self = comp_scope_of(c, id);
    if (self && self->class_id >= 0) return ty_object(self->class_id);
  }

  /* Class#allocate -> a bare instance of that class (no initialize run). */
  if (recv >= 0 && sp_streq(name, "allocate") && argc == 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      /* Use the same exception-subclass predicate as codegen (class_is_exc_subclass)
         so inference and emission agree on which classes take the allocate path. */
      if (ci >= 0 && !class_is_exc_subclass(c, ci)) return ty_object(ci);
      const char *bcn = nt_str(nt, recv, "name");
      if (bcn && sp_streq(bcn, "String")) return TY_STRING;
      if (bcn && sp_streq(bcn, "Array"))  return TY_POLY_ARRAY;
      if (bcn && sp_streq(bcn, "Hash"))   return TY_POLY_POLY_HASH;
      if (bcn && sp_streq(bcn, "Object")) return TY_POLY;
    }
  }

  /* Class.new(...) -> an instance of that class; built-in .new constructors */
  if (recv >= 0 && (sp_streq(name, "new") || sp_streq(name, "__hash_new_default"))) {
    const char *rty = nt_type(nt, recv);
    /* a namespaced class (M::Sub) or root-qualified builtin (::Array etc) */
    if (rty && sp_streq(rty, "ConstantPathNode")) {
      const char *cn = nt_str(nt, recv, "name");
      int ci = cn ? comp_class_index(c, cn) : -1;
      if (ci >= 0) {
        /* an exception-subclass instance keeps its concrete class (its custom
           methods must dispatch); the exception-shaped queries route through
           the base helpers like a specialized rescue var does */
        if (class_inherits_builtin_exception(c, ci))
          return (comp_method_in_chain(c, ci, "initialize", NULL), ty_object(ci));
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      /* ::Array.new / ::String.new / ::StringIO.new etc. */
      if (cn && sp_streq(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && sp_streq(cn, "Array")) return TY_POLY_ARRAY;
      if (cn && (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) return TY_POLY;
      if (cn && sp_streq(cn, "String")) return TY_STRING;
      if (cn && sp_streq(cn, "Hash"))
        return sp_streq(name, "__hash_new_default") ? TY_POLY_POLY_HASH : TY_UNKNOWN;
      if (cn && sp_streq(cn, "Regexp")) return TY_REGEX;
      if (cn && sp_streq(cn, "Fiber")) return TY_FIBER;
      if (cn && sp_streq(cn, "File")) return TY_IO;   /* File.new is File.open (#2779) */
      if (cn && sp_streq(cn, "Dir")) return TY_DIR;   /* Dir.new is an open handle (#2821) */
      if (cn && (sp_streq(cn, "Thread") || sp_streq(cn, "Mutex") || (sp_streq(cn, "Monitor") && sp_feature_enabled("monitor")) ||
                 sp_streq(cn, "Random") || sp_streq(cn, "IO") ||
                 sp_streq(cn, "GzipReader") || sp_streq(cn, "GzipWriter"))) return TY_POLY;
    }
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "LocalVariableReadNode") ||
                (sp_streq(rty, "CallNode") && is_struct_call(c, recv)))) {  /* inline Data.define(...).new (#2682) */
      const char *cn = sp_streq(rty, "ConstantReadNode") ? nt_str(nt, recv, "name") : NULL;
      int ci = cn ? comp_class_index(c, cn) : class_var_static_ci(c, recv);
      if (ci >= 0) {
        /* an exception-subclass instance keeps its concrete class (its custom
           methods must dispatch); the exception-shaped queries route through
           the base helpers like a specialized rescue var does */
        if (class_inherits_builtin_exception(c, ci))
          return (comp_method_in_chain(c, ci, "initialize", NULL), ty_object(ci));
        int ucnew = comp_cmethod_in_chain(c, ci, "new", NULL);
        if (ucnew >= 0) return (TyKind)c->scopes[ucnew].ret;
        return ty_object(ci);
      }
      if (cn && is_builtin_exception_name(cn)) return TY_EXCEPTION;
      if (cn && sp_streq(cn, "Array") && argc == 2) return ty_array_of(infer_type(c, argv[1]));
      if (cn && sp_streq(cn, "Array")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          /* Array.new(n) { body }: element type from last expression of block body */
          int bbody = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
          if (bn > 0 && bb) { TyKind et = infer_type(c, bb[bn - 1]); if (et != TY_UNKNOWN) return ty_array_of(et); }
        }
        /* a bare `Array.new` carries no element type; leave it UNKNOWN (like an
           empty `[]`) so the push-promotion pass can narrow it from `<<`/push. */
        if (argc == 0 && blk < 0) return TY_UNKNOWN;
        return TY_POLY_ARRAY;
      }
      if (cn && sp_streq(cn, "Array")) return TY_POLY_ARRAY; /* Array.new / Array.new(n) */
      if (cn && (sp_streq(cn, "Object") || sp_streq(cn, "BasicObject"))) return TY_POLY;  /* identity sentinel */
      if (cn && sp_streq(cn, "String")) return TY_STRING;
      /* Hash.new { |hash, key| default } : a poly-keyed poly hash with a
         default-proc (the block computes the missing-key value). A default-block
         hash is polymorphic by nature (keys are populated dynamically), so the
         faithful PolyPoly variant boxes each key by value -- inspect then renders
         symbol keys as `a:`, string keys as `"a"=>`, etc., all correctly. */
      if (cn && sp_streq(cn, "Hash") && nt_ref(nt, id, "block") >= 0) return TY_POLY_POLY_HASH;
      if (cn && sp_streq(cn, "Hash"))
        /* argument-position Hash.new was renamed: PolyPoly; else key usage decides */
        return sp_streq(name, "__hash_new_default") ? TY_POLY_POLY_HASH : TY_UNKNOWN;
      if (cn && sp_streq(cn, "Regexp")) return TY_REGEX;
      /* Builtin object types */
      if (cn && sp_streq(cn, "Fiber")) return TY_FIBER;
      /* Thread.new { block }: an eager green thread (sp_thread) on the scheduler. */
      if (cn && sp_streq(cn, "Thread") && nt_ref(nt, id, "block") >= 0) return TY_THREAD;
      if (cn && (sp_streq(cn, "Queue") || sp_streq(cn, "SizedQueue"))) return TY_QUEUE;
      if (cn && (sp_streq(cn, "Mutex") || sp_streq(cn, "Monitor"))) return TY_MUTEX;
      if (cn && sp_streq(cn, "ConditionVariable")) return TY_CONDVAR;
      if (cn && sp_streq(cn, "Random")) return TY_RANDOM;
      if (cn && sp_streq(cn, "File")) return TY_IO;   /* File.new is File.open (#2779) */
      if (cn && sp_streq(cn, "Dir")) return TY_DIR;   /* Dir.new is an open handle (#2821) */
      if (cn && (sp_streq(cn, "Thread") ||
                 sp_streq(cn, "IO") ||
                 sp_streq(cn, "GzipReader") || sp_streq(cn, "GzipWriter"))) return TY_POLY;
    }
  }

  /* Regexp.compile is an alias for Regexp.new */
  if (recv >= 0 && sp_streq(name, "compile")) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *cn = nt_str(nt, recv, "name");
      if (cn && sp_streq(cn, "Regexp")) return TY_REGEX;
    }
  }

  /* StringScanner instance methods */
  /* StringScanner: a native-bound class (packages/strscan); no arms here. */

  /* Regexp class methods */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Regexp")) {
    if ((sp_streq(name, "escape") || sp_streq(name, "quote")) && argc >= 1) return TY_STRING;
    if (sp_streq(name, "union")) return TY_REGEX;  /* argc 0 = the never-matching /(?!)/ */
    if (sp_streq(name, "last_match") && argc == 0) return TY_MATCHDATA;
    if (sp_streq(name, "last_match") && argc == 1) return TY_STRING;
    if (sp_streq(name, "linear_time?") && argc == 1) return TY_BOOL;
    if (sp_streq(name, "try_convert") && argc == 1) return TY_POLY;
    if (sp_streq(name, "timeout") && argc == 0) return TY_POLY;   /* nil */
    if (sp_streq(name, "timeout") && argc == 1) return TY_POLY;   /* timeout= returns its arg */
    if (sp_streq(name, "timeout=") && argc == 1) return TY_POLY;
  }

  /* Regexp instance methods */
  if (recv >= 0 && rt == TY_REGEX) {
    if (sp_streq(name, "match?") || sp_streq(name, "===")) return TY_BOOL;
    if (sp_streq(name, "match")) return TY_MATCHDATA;
    if (sp_streq(name, "=~")) return TY_POLY;
    if (sp_streq(name, "~") && argc == 0) return TY_POLY;   /* ~ /re/ == /re/ =~ $_ */
    if (sp_streq(name, "source") || sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "names")) return TY_STR_ARRAY;
    if (sp_streq(name, "named_captures")) return TY_STR_POLY_HASH;  /* {name => [group indices]} */
    if (sp_streq(name, "freeze") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "itself")) return TY_REGEX;
    if (sp_streq(name, "frozen?")) return TY_BOOL;
    if ((sp_streq(name, "==") || sp_streq(name, "!=") ||
         sp_streq(name, "equal?") || sp_streq(name, "eql?")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "encoding")) return TY_POLY;  /* a boxed Encoding value */
    if (sp_streq(name, "fixed_encoding?")) return TY_BOOL;
    if (sp_streq(name, "options")) return TY_INT;
    if (sp_streq(name, "casefold?")) return TY_BOOL;
    if (sp_streq(name, "timeout")) return TY_POLY;   /* nil: no per-instance timeout */
  }

  /* MatchData instance methods */
  if (recv >= 0 && rt == TY_MATCHDATA) {
    if (sp_streq(name, "[]") && argc == 1 &&
        (comp_ntype(c, argv[0]) == TY_RANGE ||
         (nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode"))))
      return TY_POLY_ARRAY;   /* md[range] (#2532) */
    if (sp_streq(name, "[]") && argc == 1) return TY_STRING;
    if (sp_streq(name, "[]") && argc == 2) return TY_POLY_ARRAY;   /* md[start, length] (#2507) */
    if ((sp_streq(name, "==") || sp_streq(name, "eql?")) && argc == 1) return TY_BOOL;   /* (#2529) */
    if (sp_streq(name, "inspect") && argc == 0) return TY_STRING;   /* (#2500) */
    if (sp_streq(name, "match") && argc == 1) return TY_STRING;   /* group substring (#2501) */
    if (sp_streq(name, "match_length") && argc == 1) return TY_POLY;   /* int or nil (#2501) */
    if (sp_streq(name, "deconstruct") && argc == 0) return TY_POLY_ARRAY;   /* (#2503) */
    if (sp_streq(name, "deconstruct_keys") && argc == 1) return TY_SYM_POLY_HASH;   /* (#2503) */
    if (sp_streq(name, "named_captures") && argc == 1) return TY_SYM_POLY_HASH;   /* symbolize (#2530) */
    if (sp_streq(name, "regexp") && argc == 0) return TY_REGEX;   /* (#2499) */
    if (sp_streq(name, "pre_match") || sp_streq(name, "post_match") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "begin") || sp_streq(name, "end") || sp_streq(name, "length") || sp_streq(name, "size")) return TY_INT;
    if (sp_streq(name, "bytebegin") || sp_streq(name, "byteend")) return TY_INT;
    if (sp_streq(name, "offset") || sp_streq(name, "byteoffset")) return TY_INT_ARRAY;
    if (sp_streq(name, "values_at")) return TY_POLY_ARRAY;
    if (sp_streq(name, "captures") || sp_streq(name, "to_a")) return TY_POLY_ARRAY;
    if (sp_streq(name, "named_captures")) return TY_STR_POLY_HASH;  /* {String => String|nil} */
    if (sp_streq(name, "names")) return TY_STR_ARRAY;
    if (sp_streq(name, "string")) return TY_STRING;  /* the match subject */
    if (sp_streq(name, "nil?")) return TY_BOOL;
  }

  /* StringIO: a native-bound class (packages/stringio); no arms here. .new
     resolves through the class table, .open is Ruby in the package, and
     instance methods use the native_method declarations. */

  /* Time.now / at / local / mktime / utc / gm -> a Time value */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Time") &&
        (sp_streq(name, "now") || sp_streq(name, "at") || sp_streq(name, "local") ||
         sp_streq(name, "mktime") || sp_streq(name, "utc") || sp_streq(name, "gm") ||
         sp_streq(name, "new")))
      return TY_TIME;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC") &&
        (sp_streq(name, "start") || sp_streq(name, "compact")))
      return TY_NIL;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "GC") &&
        sp_streq(name, "stat"))
      return TY_STR_INT_HASH;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Process")) {
      if (sp_streq(name, "pid") || sp_streq(name, "ppid")) return TY_INT;
      if (sp_streq(name, "clock_gettime")) {
        /* an integer unit (:nanosecond/:microsecond/:millisecond/:second) makes
           the result an Integer; the default and float units keep it Float. */
        if (argc >= 2 && nt_type(nt, argv[1]) && sp_streq(nt_type(nt, argv[1]), "SymbolNode")) {
          const char *u = nt_str(nt, argv[1], "value");
          if (u && (sp_streq(u, "nanosecond") || sp_streq(u, "microsecond") ||
                    sp_streq(u, "millisecond") || sp_streq(u, "second"))) return TY_INT;
        }
        return TY_FLOAT;
      }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Integer") &&
        sp_streq(name, "sqrt"))
      return TY_INT;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Marshal")) {
      if (sp_streq(name, "dump") && argc == 1) return TY_STRING;
      if (sp_streq(name, "load") && argc == 1) return TY_POLY;
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math") &&
        (sp_streq(name, "sin") || sp_streq(name, "cos") || sp_streq(name, "tan") ||
         sp_streq(name, "asin") || sp_streq(name, "acos") || sp_streq(name, "atan") ||
         sp_streq(name, "atan2") || sp_streq(name, "sinh") || sp_streq(name, "cosh") ||
         sp_streq(name, "tanh") || sp_streq(name, "asinh") || sp_streq(name, "acosh") ||
         sp_streq(name, "atanh") || sp_streq(name, "exp") || sp_streq(name, "log") ||
         sp_streq(name, "log2") || sp_streq(name, "log10") || sp_streq(name, "sqrt") ||
         sp_streq(name, "cbrt") || sp_streq(name, "hypot") ||
         sp_streq(name, "ldexp") || sp_streq(name, "erf") || sp_streq(name, "erfc") ||
         sp_streq(name, "gamma")))
      return TY_FLOAT;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Math") &&
        (sp_streq(name, "lgamma") || sp_streq(name, "frexp")) && argc == 1)
      return TY_POLY_ARRAY;  /* [log(|gamma|), sign] / [fraction, exponent] */
    /* JSON.generate/dump return type comes from the native binding
       (packages/json, inferred in the FFI/native block above), not a hardcoded
       arm. */
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
        (sp_streq(name, "exist?") || sp_streq(name, "exists?")))
      return TY_BOOL;
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir")) {
      if (sp_streq(name, "pwd") || sp_streq(name, "home")) return TY_STRING;
      if (sp_streq(name, "glob") || sp_streq(name, "entries") || sp_streq(name, "children")) return TY_STR_ARRAY;
      if (sp_streq(name, "mkdir") || sp_streq(name, "rmdir") || sp_streq(name, "chdir"))
        return TY_INT;
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") &&
        (sp_streq(nt_str(nt, recv, "name"), "File") ||
         sp_streq(nt_str(nt, recv, "name"), "FileTest"))) {
      if (sp_streq(name, "basename") || sp_streq(name, "dirname") || sp_streq(name, "extname") ||
          sp_streq(name, "read") || sp_streq(name, "binread") || sp_streq(name, "expand_path") ||
          sp_streq(name, "join") || sp_streq(name, "realpath") || sp_streq(name, "ftype") ||
          sp_streq(name, "path") || sp_streq(name, "absolute_path"))
        return TY_STRING;
      if (sp_streq(name, "exist?") || sp_streq(name, "exists?"))
        return TY_BOOL;
      if (sp_streq(name, "write") || sp_streq(name, "binwrite") || sp_streq(name, "delete") ||
          sp_streq(name, "unlink") || sp_streq(name, "rename") || sp_streq(name, "size") ||
          sp_streq(name, "size?") || sp_streq(name, "chmod") || sp_streq(name, "truncate"))
        return TY_INT;
      if (sp_streq(name, "readable?") || sp_streq(name, "directory?") || sp_streq(name, "file?") ||
          sp_streq(name, "zero?") || sp_streq(name, "empty?") || sp_streq(name, "symlink?") ||
          sp_streq(name, "writable?") || sp_streq(name, "executable?") || sp_streq(name, "pipe?") ||
          sp_streq(name, "identical?") || sp_streq(name, "fnmatch") || sp_streq(name, "fnmatch?"))
        return TY_BOOL;
      if (sp_streq(name, "mtime") || sp_streq(name, "atime") || sp_streq(name, "ctime"))
        return TY_TIME;
      if (sp_streq(name, "readlines") || sp_streq(name, "split")) return TY_STR_ARRAY;
      if (sp_streq(name, "stat")) return TY_IO;   /* the path-carrying stat handle */
      /* File.open / File.new without a block -> a typed IO handle */
      if (sp_streq(name, "open") || sp_streq(name, "new")) {
        int blk = nt_ref(nt, id, "block");
        if (blk < 0) return TY_IO;
        /* Pin block param to TY_IO so body dispatch works (f.write, f.puts, etc.) */
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_IO;
        return TY_POLY;
      }
    }
    if (rty && sp_streq(rty, "ConstantReadNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "IO")) {
      /* IO.pipe -> [reader, writer], boxed IO handles (#2815) */
      if (sp_streq(name, "pipe")) return TY_POLY_ARRAY;
      if (sp_streq(name, "copy_stream") || sp_streq(name, "sysopen")) return TY_INT;
    }

    /* <local>.yield(v) or bare <local>.yield: a generator yielder / fiber yield
       returns the value the next resume (or Enumerator#feed) supplies -- poly.
       Gated on a local receiver so Fiber.yield (const receiver) and the yield
       keyword are untouched; without this the return is typed nil and
       `x = y.yield` drops the fed value. Zero-arg `y.yield` is valid too. */
    if (recv >= 0 && sp_streq(name, "yield") &&
        rty && sp_streq(rty, "LocalVariableReadNode"))
      return TY_POLY;
    /* Fiber.new {} / Thread.new {} / Fiber.current etc.
       Handles both bare Const and ::Const path forms. */
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      const char *cn2 = nt_str(nt, recv, "name");
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Enumerator") &&
          nt_ref(nt, id, "block") >= 0) return TY_ENUMERATOR;
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Fiber")) return TY_FIBER;
      /* Thread.new { block }: an eager green thread on the scheduler. */
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Thread") &&
          nt_ref(nt, id, "block") >= 0)
        return TY_THREAD;
      if (cn2 && sp_streq(name, "new") && (sp_streq(cn2, "Queue") || sp_streq(cn2, "SizedQueue"))) return TY_QUEUE;
      if (cn2 && sp_streq(name, "new") && (sp_streq(cn2, "Mutex") || sp_streq(cn2, "Monitor"))) return TY_MUTEX;
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "ConditionVariable")) return TY_CONDVAR;
      if (cn2 && sp_streq(name, "new") && sp_streq(cn2, "Random")) return TY_RANDOM;
      if (cn2 && sp_streq(cn2, "Enumerator") && sp_streq(name, "product") && (argc == 2 || argc == 3))
        return TY_ENUMERATOR;   /* #2484 */
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "current")) return TY_THREAD;
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "main")) return TY_THREAD;
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "list")) return TY_POLY_ARRAY;
      if (cn2 && sp_streq(cn2, "Thread") && sp_streq(name, "pass")) return TY_NIL;
      if (cn2 && sp_streq(cn2, "Thread") &&
          (sp_streq(name, "report_on_exception") || sp_streq(name, "report_on_exception="))) return TY_BOOL;
      if (cn2 && sp_streq(cn2, "Fiber") && sp_streq(name, "current")) return TY_FIBER;
      if (cn2 && sp_streq(cn2, "Fiber") && sp_streq(name, "yield")) return TY_POLY;
      /* Random class methods: Random.rand(float)->float / Random.rand(int)->int
         / Random.rand->float */
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "rand"))
        return argc >= 1 ? (infer_type(c, argv[0]) == TY_FLOAT ? TY_FLOAT : TY_INT) : TY_FLOAT;
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "bytes")) return TY_STRING;
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "new_seed")) return TY_INT;
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "urandom")) return TY_STRING;
      if (cn2 && sp_streq(cn2, "Random") && sp_streq(name, "srand")) return TY_INT;
    }
  }

  /* TY_FIBER instance methods */
  if (recv >= 0 && rt == TY_FIBER) {
    if (sp_streq(name, "resume") || sp_streq(name, "transfer") || sp_streq(name, "raise")) return TY_POLY;
    if (sp_streq(name, "alive?")) return TY_BOOL;
    if (sp_streq(name, "value")) return TY_POLY;
    if (sp_streq(name, "kill")) return TY_FIBER;   /* returns the receiver */
  }

  /* TY_THREAD instance methods */
  if (recv >= 0 && rt == TY_THREAD) {
    if (sp_streq(name, "value")) return TY_POLY;
    if (sp_streq(name, "join") || sp_streq(name, "kill") || sp_streq(name, "exit") ||
        sp_streq(name, "terminate") || sp_streq(name, "raise")) return TY_THREAD;   /* return self */
    if (sp_streq(name, "alive?")) return TY_BOOL;
    if (sp_streq(name, "report_on_exception") || sp_streq(name, "report_on_exception=")) return TY_BOOL;
    if (sp_streq(name, "status") || sp_streq(name, "[]") || sp_streq(name, "[]=") ||
        sp_streq(name, "name") || sp_streq(name, "name=")) return TY_POLY;
    if (sp_streq(name, "key?") || sp_streq(name, "equal?")) return TY_BOOL;
  }

  /* TY_QUEUE instance methods */
  if (recv >= 0 && rt == TY_QUEUE) {
    if (sp_streq(name, "pop") || sp_streq(name, "shift") || sp_streq(name, "deq")) return TY_POLY;
    if (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "enq") ||
        sp_streq(name, "close") || sp_streq(name, "clear")) return TY_QUEUE;   /* return self */
    if (sp_streq(name, "size") || sp_streq(name, "length") || sp_streq(name, "max")) return TY_INT;
    if (sp_streq(name, "empty?") || sp_streq(name, "closed?")) return TY_BOOL;
  }

  /* TY_MUTEX instance methods */
  if (recv >= 0 && rt == TY_MUTEX) {
    if (sp_streq(name, "lock") || sp_streq(name, "unlock")) return TY_MUTEX;   /* return self */
    if (sp_streq(name, "try_lock") || sp_streq(name, "locked?") || sp_streq(name, "owned?")) return TY_BOOL;
    if (sp_streq(name, "synchronize")) return TY_POLY;   /* the block's result */
  }

  /* TY_CONDVAR instance methods */
  if (recv >= 0 && rt == TY_CONDVAR) {
    if (sp_streq(name, "wait") || sp_streq(name, "signal") || sp_streq(name, "broadcast")) return TY_CONDVAR;
  }

  /* TY_ENUMERATOR instance methods */
  if (recv >= 0 && rt == TY_ENUMERATOR) {
    if (sp_streq(name, "next") || sp_streq(name, "peek")) return TY_POLY;
    if (sp_streq(name, "next_values") || sp_streq(name, "peek_values")) return TY_POLY_ARRAY;   /* #2482 */
    if (sp_streq(name, "+") && argc == 1 && infer_type(c, argv[0]) == TY_ENUMERATOR) return TY_ENUMERATOR;  /* #2481 */
    if (sp_streq(name, "rewind")) return TY_ENUMERATOR;
    if (sp_streq(name, "frozen?")) return TY_BOOL;
    if ((sp_streq(name, "equal?") || sp_streq(name, "eql?") || sp_streq(name, "==")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "freeze") || sp_streq(name, "itself")) return TY_ENUMERATOR;
    if (sp_streq(name, "feed") && argc == 1) return TY_NIL;   /* #feed returns nil */
    /* blockless enum.with_index(off) is another materialized Enumerator (over
       [element, index] pairs); the block/terminal-chain forms are typed below */
    if (sp_streq(name, "with_index") && argc <= 1 && nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    /* blockless enum.each_with_index / each_index -> a chained Enumerator (#2487) */
    if ((sp_streq(name, "each_with_index") || sp_streq(name, "each_index")) &&
        argc == 0 && nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    /* Stored-enumerator block form returns the underlying each return (the
       boxed source). Immediate chains (arr.each.with_index { } and the
       map/select shapes) keep their own typed arms below -- skip a blockless
       iter-shaped CallNode receiver over an array. */
    if (sp_streq(name, "with_index") && argc <= 1 && nt_ref(nt, id, "block") >= 0) {
      int wchain = 0;
      if (nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
          nt_ref(nt, recv, "block") < 0) {
        const char *wnm = nt_str(nt, recv, "name");
        int wrcv = nt_ref(nt, recv, "receiver");
        if (wnm && wrcv >= 0 &&
            (sp_streq(wnm, "each") || ty_iter_shape(wnm) != TY_ITER_NONE) &&
            ty_is_array(infer_type(c, wrcv)))
          wchain = 1;
      }
      if (!wchain) return TY_POLY;
    }
    /* #size is nil for a generator with no size, an Integer for a materialized
       snapshot, or whatever a stored size value/callable yields -- hence poly. */
    if (sp_streq(name, "size")) return TY_POLY;
    if ((sp_streq(name, "take") || sp_streq(name, "first")) && argc == 1) return TY_POLY_ARRAY;
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries")) && argc == 0) return TY_POLY_ARRAY;
    if ((sp_streq(name, "inspect") || sp_streq(name, "to_s")) && argc == 0) return TY_STRING;
  }

  /* Kernel#p returns its argument (one arg; several return the array), so it
     composes as an expression: x = p(y), f(p(y)). Statement-position p keeps
     its own emitter; this types the value form. */
  if (recv < 0 && (sp_streq(name, "p") || sp_streq(name, "pp")) && nt_ref(nt, id, "block") < 0 && argc >= 2)
    return TY_POLY_ARRAY;   /* p(a, b, ...) returns the array of its arguments */
  if (recv < 0 && (sp_streq(name, "p") || sp_streq(name, "pp")) && nt_ref(nt, id, "block") < 0 && argc == 1)
    return infer_type(c, argv[0]);
  /* Object#instance_variables: a static symbol list for a typed object */
  if (recv >= 0 && ty_is_object(rt) && argc == 0 &&
      sp_streq(name, "instance_variables"))
    return TY_POLY_ARRAY;

  /* Kernel#puts / #print return nil; typed so the value form composes. */
  if (recv < 0 && (sp_streq(name, "puts") || sp_streq(name, "print")) &&
      nt_ref(nt, id, "block") < 0)
    return TY_NIL;
  /* Kernel#warn / #printf / p() (no args) return nil in value position. */
  if (recv < 0 && (sp_streq(name, "warn") || sp_streq(name, "printf") ||
                   ((sp_streq(name, "p") || sp_streq(name, "pp")) && argc == 0)) &&
      nt_ref(nt, id, "block") < 0)
    return TY_NIL;
  /* Kernel#putc returns its argument. */
  if (recv < 0 && sp_streq(name, "putc") && argc == 1 && nt_ref(nt, id, "block") < 0)
    return infer_type(c, argv[0]);

  /* TY_RANDOM instance methods */
  if (recv >= 0 && rt == TY_RANDOM) {
    if (sp_streq(name, "rand")) {
      if (argc < 1) return TY_FLOAT;
      TyKind a0 = infer_type(c, argv[0]);
      if (a0 == TY_FLOAT || a0 == TY_FLOAT_RANGE) return TY_FLOAT;   /* rand(Float range) -> Float (#2521) */
      /* rand(Float range) -> Float (#2521) */
      const char *atype = nt_type(nt, argv[0]);
      if (atype && sp_streq(atype, "RangeNode")) {
        int lo = nt_ref(nt, argv[0], "left");
        if (lo >= 0 && infer_type(c, lo) == TY_FLOAT) return TY_FLOAT;
      }
      return TY_INT;
    }
    if (sp_streq(name, "bytes")) return TY_STRING;
    if (sp_streq(name, "seed")) return TY_INT;
    if (sp_streq(name, "class")) return TY_CLASS;
    if ((sp_streq(name, "==") || sp_streq(name, "equal?") || sp_streq(name, "eql?")) && argc == 1)
      return TY_BOOL;
  }

  /* ARGF pseudo-IO methods */
  if (recv >= 0 && rt == TY_ARGF) {
    if (sp_streq(name, "read") || sp_streq(name, "gets") || sp_streq(name, "readline") ||
        sp_streq(name, "filename") || sp_streq(name, "path") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "readlines") || sp_streq(name, "to_a")) return TY_STR_ARRAY;
    if (sp_streq(name, "eof?") || sp_streq(name, "eof")) return TY_BOOL;
    if (sp_streq(name, "each_line") || sp_streq(name, "each_string") || sp_streq(name, "each")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = TY_STRING;
      }
      return TY_ARGF;
    }
  }

  /* Dir.new / Dir.open and the Dir handle instance surface (#2821) */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "Dir") &&
      (sp_streq(name, "new") || sp_streq(name, "open")) && argc >= 1) {
    int dblk = nt_ref(nt, id, "block");
    if (dblk < 0) return TY_DIR;
    const char *dp0 = block_param_name(c, dblk, 0);
    Scope *dbs = dp0 ? comp_scope_of(c, dblk) : NULL;
    LocalVar *dlv = (dbs && dp0) ? scope_local(dbs, dp0) : NULL;
    if (dlv) dlv->type = TY_DIR;
    return TY_POLY;   /* block form: the block's boxed value (File.open's rule) */
  }
  if (recv >= 0 && rt == TY_DIR) {
    if (sp_streq(name, "class")) return TY_CLASS;
    if (sp_streq(name, "read") || sp_streq(name, "path") || sp_streq(name, "to_path"))
      return TY_STRING;
    if (sp_streq(name, "children") || sp_streq(name, "entries")) return TY_STR_ARRAY;
    if (sp_streq(name, "tell") || sp_streq(name, "pos")) return TY_INT;
    if (sp_streq(name, "close")) return TY_POLY;   /* nil */
    if (sp_streq(name, "rewind")) return TY_DIR;
    if (sp_streq(name, "each") || sp_streq(name, "each_child")) {
      int dblk3 = nt_ref(nt, id, "block");
      if (dblk3 >= 0) {
        const char *dbp3 = block_param_name(c, dblk3, 0);
        Scope *dbs3 = dbp3 ? comp_scope_of(c, dblk3) : NULL;
        LocalVar *dlv3 = (dbs3 && dbp3) ? scope_local(dbs3, dbp3) : NULL;
        if (dlv3) dlv3->type = TY_STRING;
      }
      return TY_DIR;
    }
  }

  /* TY_IO (File/IO handle) instance methods */
  if (recv >= 0 && rt == TY_IO) {
    if (sp_streq(name, "read") || sp_streq(name, "gets") || sp_streq(name, "readline") ||
        sp_streq(name, "path") || sp_streq(name, "to_path")) return TY_STRING;
    if (sp_streq(name, "read") && nt_ref(nt, id, "arguments") >= 0) return TY_STRING;
    if (sp_streq(name, "readlines")) return TY_STR_ARRAY;
    if (sp_streq(name, "write") || sp_streq(name, "syswrite") || sp_streq(name, "pos") ||
        sp_streq(name, "tell") || sp_streq(name, "seek") || sp_streq(name, "rewind"))
      return TY_INT;
    if (sp_streq(name, "close")) return TY_POLY;      /* nil (#2801) */
    if (sp_streq(name, "print") || sp_streq(name, "puts")) return TY_NIL;
    if (sp_streq(name, "flush") || sp_streq(name, "binmode")) return TY_IO;  /* self (#2799) */
    if (sp_streq(name, "closed?") || sp_streq(name, "eof?") || sp_streq(name, "eof") ||
        sp_streq(name, "tty?") || sp_streq(name, "isatty") ||
        sp_streq(name, "sync") || sp_streq(name, "sync=") ||
        sp_streq(name, "autoclose?") ||
        sp_streq(name, "==") || sp_streq(name, "equal?") || sp_streq(name, "eql?"))
      return TY_BOOL;
    if (sp_streq(name, "fileno") || sp_streq(name, "to_i") || sp_streq(name, "lineno") ||
        sp_streq(name, "lineno=") || sp_streq(name, "pos=") || sp_streq(name, "flock") ||
        sp_streq(name, "fsync") || sp_streq(name, "fdatasync") || sp_streq(name, "getbyte") ||
        sp_streq(name, "sysseek") || sp_streq(name, "size") || sp_streq(name, "chmod") ||
        sp_streq(name, "mode"))
      return TY_INT;
    if (sp_streq(name, "getc") || sp_streq(name, "readchar") || sp_streq(name, "readpartial") ||
        sp_streq(name, "sysread") || sp_streq(name, "ftype")) return TY_STRING;
    if (sp_streq(name, "mtime") || sp_streq(name, "atime") || sp_streq(name, "ctime")) return TY_TIME;
    if (sp_streq(name, "stat")) return TY_IO;
    if (sp_streq(name, "putc") || sp_streq(name, "printf") || sp_streq(name, "ungetc") ||
        sp_streq(name, "pid")) return TY_POLY;
    if (sp_streq(name, "winsize") && sp_feature_enabled("io/console")) return TY_INT_ARRAY;
    if (sp_streq(name, "<<")) return TY_IO;   /* writes, returns self (chainable) */
    if (sp_streq(name, "each_line") || sp_streq(name, "each") ||
        sp_streq(name, "each_char") || sp_streq(name, "each_byte")) {
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        const char *bp0 = block_param_name(c, blk, 0);
        Scope *bs = bp0 ? comp_scope_of(c, blk) : NULL;
        LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
        if (blv) blv->type = sp_streq(name, "each_byte") ? TY_INT : TY_STRING;
      }
      return TY_IO;
    }
    return TY_POLY;
  }

  /* Time instance methods */
  if (recv >= 0 && rt == TY_TIME) {
    if (sp_streq(name, "-") && argc > 0) {
      TyKind at = infer_type(c, argv[0]);
      /* Time - Time is a Float duration; Time - poly likewise (the poly holds
         a Time at run time, the common mixed-collection shape, #2456). An
         int/float offset keeps the Time type via the general `-` arm below. */
      if (at == TY_TIME || at == TY_POLY) return TY_FLOAT;
    }
    if (sp_streq(name, "utc") || sp_streq(name, "gmtime") || sp_streq(name, "getutc") ||
        sp_streq(name, "getgm") ||
        sp_streq(name, "localtime") || sp_streq(name, "getlocal") || sp_streq(name, "+") ||
        sp_streq(name, "-")) return TY_TIME;
    if (sp_streq(name, "clamp") && argc == 2) return TY_TIME;  /* self or a bound */
    if (sp_streq(name, "to_a") && argc == 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_r") && argc == 0) return TY_RATIONAL;
    if ((sp_streq(name, "floor") || sp_streq(name, "ceil") || sp_streq(name, "round")) && argc == 0) return TY_TIME;
    if (sp_streq(name, "xmlschema") && argc == 0) return TY_STRING;
    if (sp_streq(name, "deconstruct_keys") && argc == 1) return TY_POLY;  /* boxed Sym=>Int hash */
    if (sp_streq(name, "iso8601") && sp_feature_enabled("time")) return TY_STRING;
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect") || sp_streq(name, "strftime") ||
        sp_streq(name, "zone") || sp_streq(name, "asctime") ||
        sp_streq(name, "ctime")) return TY_STRING;
    if (sp_streq(name, "to_f")) return TY_FLOAT;
    /* Integer 0 for a whole second, else a Rational -- boxed at the arm */
    if (sp_streq(name, "subsec")) return TY_POLY;
    if (sp_streq(name, "utc?") || sp_streq(name, "gmt?") || sp_streq(name, "dst?") ||
        sp_streq(name, "isdst") ||
        sp_streq(name, "sunday?") || sp_streq(name, "monday?") ||
        sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
        sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
    /* Time <=> Time is an Integer; against a non-Time operand it is nil, so
       the result is poly (#2677). */
    if (sp_streq(name, "<=>") && argc == 1)
      return infer_type(c, argv[0]) == TY_TIME ? TY_INT : TY_POLY;
    if (sp_streq(name, "class")) return TY_STRING;
    /* predicates (is_a?/kind_of?/instance_of?/between?/...) before the int
       catch-all below swallows them */
    { size_t tnl = strlen(name); if (tnl > 0 && name[tnl - 1] == '?') return TY_BOOL; }
    /* year/mon/day/hour/min/sec/wday/yday/to_i/tv_sec/tv_usec/usec/tv_nsec/nsec/... */
    return TY_INT;
  }

  /* `Module.accessor.cmethod(...)` where the singleton accessor statically
     folds to a constant (Stage-1): dispatch as that constant's class method. */
  if (recv >= 0) {
    int fold_ci = comp_sg_reader_const(c, recv);
    if (fold_ci >= 0) {
      int mi = comp_cmethod_in_chain(c, fold_ci, name, NULL);
      if (mi >= 0) return method_call_ret(c, mi, id);
    }
    /* Stage-2: accessor holds one of several constants; unify their cmethod returns. */
    int cand[32];
    int ncand = comp_sg_reader_candidates(c, recv, cand, 32);
    if (ncand >= 2) {
      TyKind r = TY_UNKNOWN;
      for (int k = 0; k < ncand; k++) {
        int mi = comp_cmethod_in_chain(c, cand[k], name, NULL);
        if (mi >= 0) r = ty_unify(r, method_call_ret(c, mi, id));
      }
      if (r != TY_UNKNOWN) return r;
    }
  }

  /* Class.cmethod(...) / M::Sub.cmethod(...) -> the class method's return type.
     method_call_ret, not the raw scope ret: a tail-yield class method carries
     THIS call site's block value (the instance/implicit-self arms already
     route through it; the raw ret is the first site's type and a diverging
     second site miscompiled through it). */
  if (recv >= 0) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      int ci = comp_class_index(c, nt_str(nt, recv, "name"));
      if (ci >= 0) {
        int mi = comp_cmethod_in_chain(c, ci, name, NULL);
        if (mi >= 0) return method_call_ret(c, mi, id);
      }
    }
    /* obj.class.cmeth(...) -> unify class method return types across hierarchy */
    if (rty && sp_streq(rty, "CallNode") &&
        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "class")) {
      int robj = nt_ref(nt, recv, "receiver");
      TyKind rrt = robj >= 0 ? infer_type(c, robj) : TY_UNKNOWN;
      if (ty_is_object(rrt)) {
        int cid = ty_object_class(rrt);
        int mi = comp_cmethod_in_chain(c, cid, name, NULL);
        if (mi >= 0) {
          TyKind r = (TyKind)c->scopes[mi].ret;
          for (int k = 0; k < c->nclasses; k++) {
            int _desc = 0;
            for (int _p = c->classes[k].parent; _p >= 0; _p = c->classes[_p].parent)
              if (_p == cid) { _desc = 1; break; }
            if (!_desc) continue;
            int kmi = comp_cmethod_in_class(c, k, name);
            if (kmi >= 0) r = ty_unify(r, (TyKind)c->scopes[kmi].ret);
          }
          return r;
        }
      }
    }
  }

  /* Struct instance methods */
  if (recv >= 0 && ty_is_object(rt) && c->classes[ty_object_class(rt)].is_struct) {
    ClassInfo *sc = &c->classes[ty_object_class(rt)];
    if (sp_streq(name, "with") && sc->is_data) return rt;  /* copy-update returns the same type */
    if (sp_streq(name, "to_a") || sp_streq(name, "values") ||
        sp_streq(name, "deconstruct") || sp_streq(name, "members")) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_h")) {
      int block = nt_ref(nt, id, "block");
      if (block >= 0) {
        /* to_h { |k,v| [nk, nv] }: hash type from the block's pair */
        int bbody = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        int last = bn > 0 ? bb[bn - 1] : -1;
        if (last >= 0 && nt_type(nt, last) && sp_streq(nt_type(nt, last), "ArrayNode")) {
          int en = 0; const int *els = nt_arr(nt, last, "elements", &en);
          if (en == 2) {
            TyKind kt = infer_type(c, els[0]), vt = infer_type(c, els[1]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING && vt == TY_STRING) return TY_STR_STR_HASH;
            if (kt == TY_STRING) return TY_STR_POLY_HASH;
            TyKind h = ty_hash_of(kt, vt);
            return h != TY_UNKNOWN ? h : TY_STR_POLY_HASH;
          }
        }
      }
      return TY_SYM_POLY_HASH;
    }
    if ((sp_streq(name, "size") || sp_streq(name, "length")) && argc == 0) {
      /* a member of that name wins: its generated reader is the method */
      char szn[272]; snprintf(szn, sizeof szn, "@%s", name);
      if (comp_ivar_index(sc, szn) < 0) return TY_INT;
    }
    if (sp_streq(name, "values_at") && argc >= 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "hash") && argc == 0) return TY_INT;
    if (sp_streq(name, "deconstruct_keys") && argc == 1) return TY_SYM_POLY_HASH;
    if (sp_streq(name, "dig") && argc >= 1) {
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) {
        TyKind mt = sc->ivar_types[mi];
        if (argc == 1) return mt;
        /* nested struct members: walk the remaining literal keys */
        {
          ClassInfo *cur = sc; int cmi = mi; int di = 1;
          while (di < argc) {
            TyKind mt2 = cur->ivar_types[cmi];
            if (!ty_is_object(mt2) || !c->classes[ty_object_class(mt2)].is_struct) break;
            ClassInfo *nx = &c->classes[ty_object_class(mt2)];
            int nmi = struct_member_idx(c, nx, argv[di]);
            if (nmi < 0) break;
            cur = nx; cmi = nmi; di++;
          }
          if (di == argc && di > 1) return cur->ivar_types[cmi];
        }
        /* dig(member, key, ...): index into the member's container */
        if (ty_is_hash(mt) && argc == 2) return ty_hash_val(mt);
        if (ty_is_array(mt) && argc == 2) return ty_array_elem(mt);
        return TY_POLY;
      }
    }
    if (sp_streq(name, "[]") && argc == 1) {
      /* struct[:sym] or struct[int]: return specific member type if known */
      int mi = struct_member_idx(c, sc, argv[0]);
      if (mi >= 0) return sc->ivar_types[mi];
      /* integer index: try to resolve literal */
      const char *kty = nt_type(nt, argv[0]);
      if (kty && sp_streq(kty, "IntegerNode")) {
        long long idx = (long long)nt_int(nt, argv[0], "value", 0);
        if (idx < 0) idx += (long long)sc->nivars;
        if (idx >= 0 && idx < sc->nivars) return sc->ivar_types[(int)idx];
      }
      return TY_POLY;
    }
    if (sp_streq(name, "[]=") && argc == 2) return sc->nivars > 0 ? sc->ivar_types[0] : TY_POLY;
  }

  /* built-in class reopening: look up user-defined methods on scalar built-in types */
  if (recv >= 0) {
    const char *oc_cn = NULL;
    if (rt == TY_STRING)       oc_cn = "String";
    else if (rt == TY_INT)     oc_cn = "Integer";
    else if (rt == TY_FLOAT)   oc_cn = "Float";
    else if (rt == TY_SYMBOL)  oc_cn = "Symbol";
    else if (rt == TY_BOOL)    oc_cn = "TrueClass";
    if (oc_cn) {
      int oc_ci = comp_class_index(c, oc_cn);
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return method_call_ret(c, oc_mi, id);
      }
    }
  }

  /* instance_variable_get(:@x) on a POLY receiver: unify @x's declared type
     across every instantiated class that has the slot (all the same concrete
     type -> that type; mixed or none -> poly). Without this the call fell
     through to an unrelated rule and inferred a bogus type, so the whole
     chain was silently dropped. Codegen dispatches on cls_id per class. */
  if (recv >= 0 && rt == TY_POLY && sp_streq(name, "instance_variable_get") && argc >= 1) {
    const char *a0ty = nt_type(nt, argv[0]);
    if (a0ty && (sp_streq(a0ty, "SymbolNode") || sp_streq(a0ty, "StringNode"))) {
      const char *sym = sp_streq(a0ty, "SymbolNode")
                          ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
      if (sym && sym[0] == '@') {
        TyKind uni = TY_UNKNOWN;
        for (int ci = 0; ci < c->nclasses; ci++) {
          if (!c->classes[ci].instantiated) continue;
          int iv = comp_ivar_index(&c->classes[ci], sym);
          if (iv < 0) continue;
          TyKind t = c->classes[ci].ivar_types[iv];
          if (uni == TY_UNKNOWN) uni = t;
          else if (uni != t) { uni = TY_POLY; break; }
        }
        return uni == TY_UNKNOWN ? TY_POLY : uni;
      }
      return TY_POLY;
    }
  }

  /* nil? on a pointer-backed Enumerator: bool (NULL-as-nil test) */
  if (recv >= 0 && argc == 0 && sp_streq(name, "nil?") && rt == TY_ENUMERATOR)
    return TY_BOOL;

  /* frozen? on an immutable value type: constantly-true bool */
  if (recv >= 0 && argc == 0 && sp_streq(name, "frozen?") &&
      (rt == TY_INT || rt == TY_FLOAT || rt == TY_SYMBOL || rt == TY_BOOL ||
       rt == TY_NIL || rt == TY_RANGE || rt == TY_COMPLEX || rt == TY_RATIONAL ||
       rt == TY_BIGINT))
    return TY_BOOL;

  /* obj.method(...) -> the method's return type (walks the superclass chain) */
  if (recv >= 0 && ty_is_object(rt)) {
    int cid = ty_object_class(rt);
    ClassInfo *cls = &c->classes[cid];
    if (sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") || sp_streq(name, "instance_of?") ||
        sp_streq(name, "respond_to?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "nil?") || sp_streq(name, "equal?") || sp_streq(name, "frozen?")) return TY_BOOL;
    /* Object#hash default (no user hash in the chain): value/pointer int.
       Structs keep their dedicated value-based hash arm. */
    if (sp_streq(name, "hash") && argc == 0 && !cls->is_struct &&
        comp_method_in_chain(c, cid, "hash", NULL) < 0) return TY_INT;
    /* native class (C-backed): a declared instance method returns its spec type */
    if (cls->is_native_class) {
      TyKind natys[8];
      int nta = argc < 8 ? argc : 8;
      for (int a = 0; a < nta; a++) natys[a] = infer_type(c, argv[a]);
      int nm = comp_native_method_find_typed(c, cid, name, argc, 0, nta == argc ? natys : NULL);
      if (nm >= 0) {
        if (sp_streq(c->native_methods[nm].ret, "self")) return rt;  /* returns the receiver's class */
        return native_spec_to_ty(c->native_methods[nm].ret);
      }
    }
    /* Comparable#clamp returns self or the APPLIED BOUND: the receiver's
       class only when each bound is statically that class or nil (a nil
       bound clamps one-sided and is never returned); a mixed-class or
       Integer-endpoint (range form) bound can be returned as-is, so the
       result is boxed. */
    if (sp_streq(name, "clamp") && argc == 2 &&
        comp_method_in_chain(c, cid, "<=>", NULL) >= 0) {
      TyKind lo = infer_type(c, argv[0]), hi = infer_type(c, argv[1]);
      return ((lo == rt || lo == TY_NIL) && (hi == rt || hi == TY_NIL)) ? rt : TY_POLY;
    }
    if (sp_streq(name, "clamp") && argc == 1 && infer_type(c, argv[0]) == TY_RANGE &&
        comp_method_in_chain(c, cid, "<=>", NULL) >= 0) {
      /* a literal range with same-class endpoints unfolds to the object
         clamp, whose result is one of the three same-class values */
      int rn2 = argv[0];
      while (rn2 >= 0 && nt_type(nt, rn2) && sp_streq(nt_type(nt, rn2), "ParenthesesNode")) {
        int pb2 = nt_ref(nt, rn2, "body"); int pbn2 = 0;
        const int *pbb2 = pb2 >= 0 ? nt_arr(nt, pb2, "body", &pbn2) : NULL;
        rn2 = pbn2 == 1 ? pbb2[0] : -1;
      }
      if (rn2 >= 0 && nt_type(nt, rn2) && sp_streq(nt_type(nt, rn2), "RangeNode")) {
        int rlo2 = nt_ref(nt, rn2, "left"), rhi2 = nt_ref(nt, rn2, "right");
        /* two-sided, beginless (`..hi`), and endless (`lo..`) object ranges all
           unfold to sp_obj_clamp, whose result is one of the same-class values;
           the missing side becomes a nil bound. Mirror the codegen guard. */
        int has_lo2 = rlo2 >= 0 && !(nt_type(nt, rlo2) && sp_streq(nt_type(nt, rlo2), "NilNode"));
        int has_hi2 = rhi2 >= 0 && !(nt_type(nt, rhi2) && sp_streq(nt_type(nt, rhi2), "NilNode"));
        int lo_obj2 = has_lo2 && infer_type(c, rlo2) == rt;
        int hi_obj2 = has_hi2 && infer_type(c, rhi2) == rt;
        if ((lo_obj2 || hi_obj2) && (!has_lo2 || lo_obj2) && (!has_hi2 || hi_obj2)) return rt;
      }
      return TY_POLY;
    }
    /* Comparable#between?(lo, hi) on an object with `<=>` is a boolean. */
    if (sp_streq(name, "between?") && argc == 2 &&
        comp_method_in_chain(c, cid, "<=>", NULL) >= 0) return TY_BOOL;
    /* default Object#<=> (no user method): 0 or nil, so poly (#2686). */
    if (sp_streq(name, "<=>") && argc == 1 &&
        comp_method_in_chain(c, cid, "<=>", NULL) < 0) return TY_POLY;
    /* instance_variable_get(:@x) yields @x's declared type; instance_variable_set
       yields the field type too (C `lvalue = v` evaluates to the lvalue). The
       codegen lowers both to a direct iv_ field access on the known layout. */
    if ((sp_streq(name, "instance_variable_get") || sp_streq(name, "instance_variable_set")) && argc >= 1) {
      const char *a0ty = nt_type(nt, argv[0]);
      if (a0ty && (sp_streq(a0ty, "SymbolNode") || sp_streq(a0ty, "StringNode"))) {
        const char *sym = sp_streq(a0ty, "SymbolNode")
                            ? nt_str(nt, argv[0], "value") : nt_str(nt, argv[0], "content");
        /* A name in the layout yields its declared type; an undefined-but-valid
           `@`-name reads as nil and a bad name (no `@`) raises NameError -- both poly. */
        /* Data/Struct members are not @-ivars in CRuby: read as nil (#2849) */
        int iv = (sym && sym[0] == '@' && !cls->is_struct) ? comp_ivar_index(cls, sym) : -1;
        if (iv >= 0) return cls->ivar_types[iv];
        return TY_POLY;
      }
    }
    /* attr reader (resolve alias so `alias v access_token` returns @access_token type) */
    { int rdcls = -1;
      if (comp_reader_in_chain(c, cid, name, &rdcls)) {
        const char *rname = comp_resolve_alias(c, cid, name);
        char ivn[256];
        snprintf(ivn, sizeof ivn, "@%s", rname);
        ClassInfo *rci = (rdcls >= 0 && rdcls < c->nclasses) ? &c->classes[rdcls] : cls;
        int iv = comp_ivar_index(rci, ivn);
        if (iv >= 0) return rci->ivar_types[iv];
      }
    }
    /* attr writer: obj.x= returns the assigned value */
    size_t ln = strlen(name);
    if (ln >= 2 && name[ln - 1] == '=') {
      char base[256];
      if (ln - 1 < sizeof base) {
        memcpy(base, name, ln - 1); base[ln - 1] = '\0';
        int wdefc = -1;
        if (comp_writer_in_chain(c, cid, base, &wdefc) && argc >= 1) {
          TyKind rhsk = infer_type(c, argv[0]);
          /* codegen boxes a scalar rhs into a poly ivar slot, so the assignment
             expression's C value is that boxed poly -- report poly to match. */
          char wivn[258]; snprintf(wivn, sizeof wivn, "@%s", base);
          int wcid = wdefc < 0 ? cid : wdefc;
          int wivx = comp_ivar_index(&c->classes[wcid], wivn);
          TyKind wivt = wivx >= 0 ? c->classes[wcid].ivar_types[wivx] : TY_UNKNOWN;
          if (wivt == TY_POLY && rhsk != TY_POLY) return TY_POLY;
          return rhsk;
        }
      }
    }
    int mi = comp_method_in_chain(c, cid, name, NULL);
    if (mi >= 0) {
      TyKind r = method_call_ret(c, mi, id);
      /* Unify with descendant direct overrides: codegen dispatch emits a
         cls_id switch over all overrides, so the result type must cover all. */
      for (int k = 0; k < c->nclasses; k++) {
        int is_desc = 0;
        for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
          if (p == cid) { is_desc = 1; break; }
        if (!is_desc) continue;
        int dmi = comp_method_in_class(c, k, name);
        if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
      }
      return r;
    }
    if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return TY_STRING;
  }

  /* implicit-self call inside an instance method */
  if (recv < 0) {
    Scope *self = comp_scope_of(c, id);
    if (self->class_id >= 0) {
      { int rdcls2 = -1;
        if (comp_reader_in_chain(c, self->class_id, name, &rdcls2)) {
          const char *rname2 = comp_resolve_alias(c, self->class_id, name);
          char ivn[256];
          snprintf(ivn, sizeof ivn, "@%s", rname2);
          ClassInfo *rci2 = (rdcls2 >= 0 && rdcls2 < c->nclasses) ? &c->classes[rdcls2] : &c->classes[self->class_id];
          int iv = comp_ivar_index(rci2, ivn);
          if (iv >= 0) return rci2->ivar_types[iv];
        }
      }
      /* bare `new` inside a class method returns an instance of self's class */
      if (self->is_cmethod && sp_streq(name, "new"))
        return ty_object(self->class_id);
      int mi = comp_method_in_chain(c, self->class_id, name, NULL);
      if (mi < 0 && self->is_cmethod)
        mi = comp_cmethod_in_chain(c, self->class_id, name, NULL);
      if (mi >= 0) {
        TyKind r = method_call_ret(c, mi, id);
        /* Unify with descendant direct overrides: codegen dispatch will
           emit a cls_id switch over all overrides, so the return type
           must accommodate every override's return type. */
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = self->is_cmethod ? comp_cmethod_in_class(c, k, name) :
                                       comp_method_in_class(c, k, name);
          if (dmi >= 0) r = ty_unify(r, (TyKind)c->scopes[dmi].ret);
        }
        return r;
      }
      /* Built-in class reopening: implicit self → delegate to built-in type lookup */
      if (mi < 0 && !self->is_cmethod) {
        const char *bcn = c->classes[self->class_id].name;
        TyKind brt = TY_UNKNOWN;
        if (sp_streq(bcn, "String"))        brt = TY_STRING;
        else if (sp_streq(bcn, "Integer"))  brt = TY_INT;
        else if (sp_streq(bcn, "Float"))    brt = TY_FLOAT;
        else if (sp_streq(bcn, "Symbol"))   brt = TY_SYMBOL;
        if (brt != TY_UNKNOWN) {
          /* Temporarily set rt to the built-in type and recursively call infer_call
             is not safe. Instead inline key return types for common method names. */
          if (brt == TY_STRING) {
            if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
                sp_streq(name, "capitalize") || sp_streq(name, "reverse") || sp_streq(name, "strip") ||
                sp_streq(name, "lstrip") || sp_streq(name, "rstrip") || sp_streq(name, "chomp") ||
                sp_streq(name, "chop") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
                sp_streq(name, "to_s") || sp_streq(name, "inspect") || sp_streq(name, "succ") ||
                sp_streq(name, "next") || sp_streq(name, "chr") || sp_streq(name, "encode") ||
                sp_streq(name, "b") || sp_streq(name, "force_encoding") || sp_streq(name, "scrub") ||
                sp_streq(name, "squeeze") || sp_streq(name, "tr") || sp_streq(name, "delete"))
              return TY_STRING;
            if ((sp_streq(name, "+") || sp_streq(name, "*")) && argc >= 1) return TY_STRING;
            if (sp_streq(name, "gsub") || sp_streq(name, "sub")) return TY_STRING;
            if (sp_streq(name, "[]") || sp_streq(name, "slice") || sp_streq(name, "slice!")) return TY_STRING;
            if (sp_streq(name, "length") || sp_streq(name, "size") || sp_streq(name, "bytesize") ||
                sp_streq(name, "to_i") || sp_streq(name, "count") || sp_streq(name, "ord") ||
                sp_streq(name, "hex") || sp_streq(name, "oct") || sp_streq(name, "rindex") ||
                sp_streq(name, "index"))
              return TY_INT;
            if (sp_streq(name, "to_f")) return TY_FLOAT;
            if (sp_streq(name, "to_sym") || sp_streq(name, "intern")) return TY_SYMBOL;
            if (sp_streq(name, "empty?") || sp_streq(name, "include?") ||
                sp_streq(name, "start_with?") || sp_streq(name, "end_with?") ||
                sp_streq(name, "==") || sp_streq(name, "!="))
              return TY_BOOL;
            if (sp_streq(name, "split") || sp_streq(name, "chars") || sp_streq(name, "lines") ||
                sp_streq(name, "bytes"))
              return TY_STR_ARRAY;
          }
          else if (brt == TY_INT) {
            if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
                sp_streq(name, "/") || sp_streq(name, "%") || sp_streq(name, "**") ||
                sp_streq(name, "abs") || sp_streq(name, "succ") || sp_streq(name, "next") ||
                sp_streq(name, "pred") || sp_streq(name, "gcd") || sp_streq(name, "lcm") ||
                sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
                sp_streq(name, "<<") || sp_streq(name, ">>"))
              return TY_INT;
            if (sp_streq(name, "to_f")) return TY_FLOAT;
            if (sp_streq(name, "to_s")) return TY_STRING;
            if (sp_streq(name, "to_r")) return TY_POLY;
            if (sp_streq(name, "odd?") || sp_streq(name, "even?") || sp_streq(name, "zero?") ||
                sp_streq(name, "==") || sp_streq(name, "!=") || sp_streq(name, "<") ||
                sp_streq(name, "<=") || sp_streq(name, ">") || sp_streq(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_FLOAT) {
            if (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
                sp_streq(name, "/") || sp_streq(name, "**") || sp_streq(name, "abs") ||
                sp_streq(name, "floor") || sp_streq(name, "ceil") || sp_streq(name, "round") ||
                sp_streq(name, "truncate"))
              return TY_FLOAT;
            if (sp_streq(name, "to_i")) return TY_INT;
            if (sp_streq(name, "to_s")) return TY_STRING;
            if (sp_streq(name, "zero?") || sp_streq(name, "nan?") || sp_streq(name, "infinite?") ||
                sp_streq(name, "finite?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
                sp_streq(name, "<") || sp_streq(name, "<=") || sp_streq(name, ">") ||
                sp_streq(name, ">="))
              return TY_BOOL;
          }
          else if (brt == TY_SYMBOL) {
            if (sp_streq(name, "to_s") || sp_streq(name, "id2name") || sp_streq(name, "inspect"))
              return TY_STRING;
            if (sp_streq(name, "to_sym") || sp_streq(name, "itself")) return TY_SYMBOL;
            if (sp_streq(name, "length") || sp_streq(name, "size")) return TY_INT;
            if (sp_streq(name, "empty?") || sp_streq(name, "==") || sp_streq(name, "!="))
              return TY_BOOL;
          }
        }
      }
      /* Method defined only in descendants (not in base chain):
         unify return types of all descendant implementations. */
      if (self->is_cmethod) {
        TyKind r = TY_UNKNOWN; int found = 0;
        for (int k = 0; k < c->nclasses; k++) {
          int is_desc = 0;
          for (int p = c->classes[k].parent; p >= 0; p = c->classes[p].parent)
            if (p == self->class_id) { is_desc = 1; break; }
          if (!is_desc) continue;
          int dmi = comp_cmethod_in_class(c, k, name);
          if (dmi < 0) continue;
          r = found ? ty_unify(r, (TyKind)c->scopes[dmi].ret) : (TyKind)c->scopes[dmi].ret;
          found = 1;
        }
        if (found) return r;
      }
    }
  }

  /* bare call inside a module/class body -> class method of that module/class.
     Use the per-node enclosing-cbody: g_cbody_class_id is only set during the
     scope pass, not the inference fixpoint, so relying on it leaves a bare
     module-body cmethod call (e.g. `take(mk)` where mk is `def self.mk`) typed
     void. The scope pass records the enclosing cbody per node in node_cbody[id]
     (cf. analyze_scope.c, analyze.c which already read it during inference). */
  if (recv < 0) {
    int cbody = c->node_cbody[id];
    if (cbody < 0) cbody = g_cbody_class_id;
    if (cbody >= 0) {
      int smi = comp_cmethod_in_chain(c, cbody, name, NULL);
      if (smi >= 0) return method_call_ret(c, smi, id);
    }
  }
  /* bare call inside an instance_eval/exec block: dispatch on receiver class */
  if (recv < 0) {
    int iec = ie_class_of(c, id);
    if (iec >= 0) {
      int imi = comp_method_in_chain(c, iec, name, NULL);
      if (imi >= 0) return method_call_ret(c, imi, id);
    }
  }
  /* Kernel conversion with an explicit user-object receiver: obj.send(:Float, x)
     desugars to obj.Float(x); the private Kernel method is available on every
     object, so it types like the receiverless form when the receiver's chain
     does not define the name (mirrors the codegen dispatch).
     The NAME gate must come first: inferring the receiver for every 1/2-arg
     call added an infer_call<->infer_type recursion edge that other arms
     avoid structurally, and looped forever on whole-program shapes with
     zero conversion calls (the tep regression). Only the six capitalized
     conversion names ever pay the receiver inference. */
  if (recv >= 0 && (argc == 1 || argc == 2) && name[0] >= 'A' && name[0] <= 'Z' &&
      (sp_streq(name, "Integer") || sp_streq(name, "Float") ||
       sp_streq(name, "String") || sp_streq(name, "Rational") ||
       sp_streq(name, "Complex") || sp_streq(name, "Array"))) {
    TyKind krt = infer_type(c, recv);
    int kdisp = (ty_is_object(krt) &&
                 comp_method_in_chain(c, ty_object_class(krt), name, NULL) < 0) ||
                ((krt == TY_NIL || krt == TY_POLY || krt == TY_UNKNOWN) &&
                 comp_method_index(c, name) < 0);
    if (kdisp) {
      if (sp_streq(name, "Integer") && (argc == 1 || argc == 2)) return TY_INT;
      if (argc == 1) {
        if (sp_streq(name, "Float"))    return TY_FLOAT;
        if (sp_streq(name, "String"))   return TY_STRING;
        if (sp_streq(name, "Rational")) return TY_RATIONAL;
        if (sp_streq(name, "Complex"))  return TY_COMPLEX;
        if (sp_streq(name, "Array")) {
          TyKind kat = infer_type(c, argv[0]);
          if (ty_is_array(kat)) return kat;
          if (kat == TY_INT)    return TY_INT_ARRAY;
          if (kat == TY_FLOAT)  return TY_FLOAT_ARRAY;
          if (kat == TY_STRING) return TY_STR_ARRAY;
        }
      }
    }
  }

  /* user-defined free-function call (no receiver) */
  if (recv < 0) {
    int mi = comp_method_index(c, name);
    if (mi < 0) mi = comp_included_method_index(c, name);
    if (mi >= 0) return method_call_ret(c, mi, id);
    /* Kernel conversions */
    if (sp_streq(name, "Integer") && (argc == 1 || argc == 2)) return TY_INT;
    if (sp_streq(name, "Float") && argc == 1) return TY_FLOAT;
    if (sp_streq(name, "String") && argc == 1) return TY_STRING;
    if (sp_streq(name, "Array") && argc == 1) {
      TyKind at = infer_type(c, argv[0]);
      if (ty_is_array(at)) return at;
      if (at == TY_INT)    return TY_INT_ARRAY;    /* Array(int)   -> [int]   */
      if (at == TY_FLOAT)  return TY_FLOAT_ARRAY;  /* Array(float) -> [float] */
      if (at == TY_STRING) return TY_STR_ARRAY;    /* Array(str)   -> [str]   */
      if (at == TY_RANGE)  return TY_INT_ARRAY;    /* Array(range) enumerates */
      return TY_POLY_ARRAY;
    }
    if (sp_streq(name, "Hash") && argc == 1) {
      TyKind at = infer_type(c, argv[0]);
      if (ty_is_hash(at)) return at;              /* Hash(hash) -> the hash */
      if (at == TY_POLY) return TY_POLY;          /* nil-or-hash decided at runtime */
      return TY_POLY_POLY_HASH;                   /* Hash(nil) / Hash([]) -> {} */
    }
    if ((sp_streq(name, "format") || sp_streq(name, "sprintf")) && argc >= 1) return TY_STRING;
    if (sp_streq(name, "system") && argc >= 1) return TY_BOOL;
    if (sp_streq(name, "trap") && argc >= 1) return TY_POLY;  /* the previous handler: a command string or a Proc */
    if (sp_streq(name, "rand")) {
      if (argc == 0) return TY_FLOAT;
      if (infer_type(c, argv[0]) == TY_FLOAT_RANGE) return TY_FLOAT;   /* rand(float range) */
      const char *atype = nt_type(nt, argv[0]);
      if (atype && sp_streq(atype, "RangeNode")) {
        int lo = nt_ref(nt, argv[0], "left");
        int hi = nt_ref(nt, argv[0], "right");
        if (lo >= 0 && infer_type(c, lo) == TY_FLOAT) return TY_FLOAT;   /* rand(float_range) */
        /* a statically empty/reversed int range yields nil (#2519) */
        if (lo >= 0 && hi >= 0 &&
            nt_type(nt, lo) && sp_streq(nt_type(nt, lo), "IntegerNode") &&
            nt_type(nt, hi) && sp_streq(nt_type(nt, hi), "IntegerNode")) {
          long long lov = nt_int(nt, lo, "value", 0);
          long long hiv = nt_int(nt, hi, "value", 0);
          int excl = (nt_int(nt, argv[0], "flags", 0) & 4) ? 1 : 0;
          if ((excl ? hiv - 1 : hiv) < lov) return TY_POLY;   /* nil */
        }
        return TY_INT;
      }
      /* rand(literal 0) is a Float in [0,1) like rand(); nonzero -> Integer */
      if (atype && sp_streq(atype, "IntegerNode") && nt_int(nt, argv[0], "value", 0) == 0)
        return TY_FLOAT;
      /* rand(Float max): CRuby truncates a positive max to an Integer range and
         returns an Integer, but max 0.0 falls back to a Float in [0,1) -- so a
         Float argument is a runtime-chosen poly (#2549). */
      if (infer_type(c, argv[0]) == TY_FLOAT) return TY_POLY;
      /* a literal nonzero Integer is an Integer; a dynamic Integer could be 0
         (Float) or nonzero (Integer), so its result is a runtime-chosen poly. */
      if (atype && sp_streq(atype, "IntegerNode")) return TY_INT;
      if (infer_type(c, argv[0]) == TY_INT) return TY_POLY;
      return TY_INT;
    }
    if (sp_streq(name, "srand")) return TY_INT;
    if (sp_streq(name, "sleep") && argc <= 1) return TY_INT;
  }
  /* Kernel.sleep(seconds) / ::Kernel.sleep -> Integer seconds slept */
  if (recv >= 0 && sp_streq(name, "sleep") && argc <= 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && (sp_streq(rty, "ConstantReadNode") || sp_streq(rty, "ConstantPathNode"))) {
      const char *rname = nt_str(nt, recv, "name");
      if (rname && sp_streq(rname, "Kernel")) return TY_INT;
    }
  }
  /* Signal.trap / Signal.list / Signal.signame; Process.kill (#2735, #2750) */
  if (recv >= 0 && nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "ConstantReadNode") || sp_streq(nt_type(nt, recv), "ConstantPathNode"))) {
    const char *rname = nt_str(nt, recv, "name");
    if (rname && sp_streq(rname, "ENV")) {
      if (sp_streq(name, "class")) return TY_CLASS;
      if (sp_streq(name, "frozen?")) return TY_BOOL;
      if (sp_streq(name, "shift")) return TY_POLY;
      if (sp_streq(name, "reject!") || sp_streq(name, "select!") || sp_streq(name, "filter!"))
        return TY_POLY;   /* nil when nothing changed (#2844) */
      if (sp_streq(name, "clear") || sp_streq(name, "update") || sp_streq(name, "merge!") ||
          sp_streq(name, "replace") || sp_streq(name, "delete_if") || sp_streq(name, "keep_if"))
        return TY_STR_STR_HASH;   /* filter-block params seed in mark_proc_captures */
    }
    if (rname && sp_streq(rname, "Signal")) {
      if (sp_streq(name, "trap") && argc >= 1) return TY_POLY;
      if (sp_streq(name, "list") && argc == 0) return TY_STR_INT_HASH;
      if (sp_streq(name, "signame") && argc == 1) return TY_STRING;
    }
    if (rname && sp_streq(rname, "Process") && sp_streq(name, "kill") && argc >= 2)
      return TY_INT;
  }

  /* Fiber storage: Fiber[:k] and Fiber.current[:k] -> poly */
  if (recv >= 0 && sp_streq(name, "[]") && argc == 1) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "Fiber")) return TY_POLY;
    }
    if (rty && sp_streq(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && sp_streq(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && sp_streq(rrty, "ConstantReadNode") && rrn && sp_streq(rrn, "Fiber"))
          return TY_POLY;
      }
    }
  }
  /* Fiber[:k] = v -> returns v's type */
  if (recv >= 0 && sp_streq(name, "[]=") && argc == 2) {
    const char *rty = nt_type(nt, recv);
    int is_fiber = 0;
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "Fiber")) is_fiber = 1;
    }
    else if (rty && sp_streq(rty, "CallNode")) {
      const char *rn = nt_str(nt, recv, "name");
      int rr = nt_ref(nt, recv, "receiver");
      if (rn && sp_streq(rn, "current") && rr >= 0) {
        const char *rrty = nt_type(nt, rr);
        const char *rrn = nt_str(nt, rr, "name");
        if (rrty && sp_streq(rrty, "ConstantReadNode") && rrn && sp_streq(rrn, "Fiber"))
          is_fiber = 1;
      }
    }
    if (is_fiber) return infer_type(c, argv[1]);
  }
  /* ENV[key] -> string or nil (use TY_STRING; null means nil) */
  if (recv >= 0 && argc >= 1 && (sp_streq(name, "[]") || sp_streq(name, "fetch"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) return TY_STRING;
    }
  }
  /* ENV.key?/has_key?/include?/member?(key) -> bool */
  if (recv >= 0 && argc == 1 &&
      (sp_streq(name, "key?") || sp_streq(name, "has_key?") ||
       sp_streq(name, "include?") || sp_streq(name, "member?"))) {
    const char *rty = nt_type(nt, recv);
    if (rty && sp_streq(rty, "ConstantReadNode")) {
      const char *rn = nt_str(nt, recv, "name");
      if (rn && sp_streq(rn, "ENV")) return TY_BOOL;
    }
  }
  /* ENV direct arms (#2743, #2746): typed results so p/interp accept them */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *rn = nt_str(nt, recv, "name");
    if (rn && sp_streq(rn, "ENV")) {
      if (sp_streq(name, "to_s")) return TY_STRING;
      /* a non-String key raises TypeError; type the (diverging) call poly */
      if (argc >= 1 &&
          (sp_streq(name, "[]") || sp_streq(name, "fetch") || sp_streq(name, "key?") ||
           sp_streq(name, "has_key?") || sp_streq(name, "include?") || sp_streq(name, "member?") ||
           sp_streq(name, "delete") || sp_streq(name, "store"))) {
        TyKind kt0 = infer_type(c, argv[0]);
        if (kt0 == TY_SYMBOL || kt0 == TY_INT || kt0 == TY_FLOAT ||
            kt0 == TY_BOOL || kt0 == TY_NIL) return TY_POLY;
      }
      /* wrong arity raises ArgumentError; poly likewise */
      if (sp_streq(name, "[]") && argc != 1) return TY_POLY;
      if (sp_streq(name, "fetch") && (argc == 0 || argc > 2)) return TY_POLY;
      if (sp_streq(name, "delete")) return TY_STRING;   /* nullable string: NULL is nil */
      if (sp_streq(name, "store") || sp_streq(name, "[]=")) return TY_STRING;
      if (sp_streq(name, "dup") || sp_streq(name, "clone") || sp_streq(name, "freeze")) return TY_POLY;
    }
  }

  /* each_slice(n).map/collect { |...| } chain: return array of block result type.
     The blockless each_slice receiver types as TY_ENUMERATOR (a first-class
     enumerator value); the codegen fold still unrolls this chain syntactically,
     so accept that receiver type here too and keep the array result. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) && (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_slice") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_es = nt_ref(nt, id, "block");
    if (blk_es >= 0) {
      int body_es = nt_ref(nt, blk_es, "body");
      int bn_es = 0; const int *bb_es = body_es >= 0 ? nt_arr(nt, body_es, "body", &bn_es) : NULL;
      return ty_array_of(bn_es > 0 ? infer_type(c, bb_es[bn_es - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).map/collect { |...| } chain: return array of block result type.
     Same as each_slice above -- the blockless each_cons receiver is now
     TY_ENUMERATOR, but the codegen fold unrolls this chain syntactically. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) && (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "each_cons") &&
      nt_ref(nt, recv, "block") < 0) {
    int blk_ec = nt_ref(nt, id, "block");
    if (blk_ec >= 0) {
      int body_ec = nt_ref(nt, blk_ec, "body");
      int bn_ec = 0; const int *bb_ec = body_ec >= 0 ? nt_arr(nt, body_ec, "body", &bn_ec) : NULL;
      return ty_array_of(bn_ec > 0 ? infer_type(c, bb_ec[bn_ec - 1]) : TY_UNKNOWN);
    }
  }

  /* each_cons(n).with_index(off).map/collect { |...| } chain. The receiver used
     to infer TY_UNKNOWN; a blockless enum.with_index is now itself a
     materialized Enumerator, so accept that type here too -- the chain arm must
     keep winning over the generic enumerator surface. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_ENUMERATOR) &&
      (ty_iter_shape(name) == TY_ITER_MAP) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "with_index") &&
      nt_ref(nt, recv, "block") < 0) {
    int wi_recv = nt_ref(nt, recv, "receiver");
    if (wi_recv >= 0 && nt_type(nt, wi_recv) && sp_streq(nt_type(nt, wi_recv), "CallNode") &&
        nt_str(nt, wi_recv, "name") && sp_streq(nt_str(nt, wi_recv, "name"), "each_cons") &&
        nt_ref(nt, wi_recv, "block") < 0) {
      int blk_wi = nt_ref(nt, id, "block");
      if (blk_wi >= 0) {
        int body_wi = nt_ref(nt, blk_wi, "body");
        int bn_wi = 0; const int *bb_wi = body_wi >= 0 ? nt_arr(nt, body_wi, "body", &bn_wi) : NULL;
        return ty_array_of(bn_wi > 0 ? infer_type(c, bb_wi[bn_wi - 1]) : TY_UNKNOWN);
      }
    }
  }

  /* array.{map,each,select,...}.with_index(off) { |x, i| } result: map collects
     the block value (array of body type); each yields the receiver; select/reject
     filter, preserving the receiver's array type. */
  if (recv >= 0 && sp_streq(name, "with_index") &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_ref(nt, recv, "block") < 0) {
    const char *inner = nt_str(nt, recv, "name");
    int arr_recv = nt_ref(nt, recv, "receiver");
    TyKind arr_t = arr_recv >= 0 ? infer_type(c, arr_recv) : TY_UNKNOWN;
    if (inner && ty_is_array(arr_t)) {
      if (sp_streq(inner, "map") || sp_streq(inner, "collect")) {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          return ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN);
        }
      }
      else if (sp_streq(inner, "each") || sp_streq(inner, "select") ||
               sp_streq(inner, "filter") || sp_streq(inner, "reject") ||
               sp_streq(inner, "map!") || sp_streq(inner, "collect!"))
        return arr_t;   /* map! mutates in place and returns the receiver */
    }
  }

  /* arr.each.with_index(off).<terminal> / arr.each_with_index.<terminal>:
     a blockless [elem, index]-pair enumerator consumed by the terminal.
     (matz/spinel#1481 inject/reduce result; #1483 others.) */
  if (recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_ref(nt, recv, "block") < 0) {
    const char *rn = nt_str(nt, recv, "name");
    int chain_arr = -1;
    if (rn && sp_streq(rn, "each_with_index")) {
      chain_arr = nt_ref(nt, recv, "receiver");
    }
    else if (rn && sp_streq(rn, "with_index")) {
      int wir = nt_ref(nt, recv, "receiver");
      if (wir >= 0 && nt_type(nt, wir) && sp_streq(nt_type(nt, wir), "CallNode") &&
          nt_str(nt, wir, "name") && sp_streq(nt_str(nt, wir, "name"), "each") &&
          nt_ref(nt, wir, "block") < 0)
        chain_arr = nt_ref(nt, wir, "receiver");
    }
    TyKind chain_at = chain_arr >= 0 ? infer_type(c, chain_arr) : TY_UNKNOWN;
    if (ty_is_array(chain_at)) {
      TyKind elem = ty_array_elem(chain_at);
      if (sp_streq(name, "inject") || sp_streq(name, "reduce")) {
        int args = nt_ref(nt, id, "arguments");
        int argc = 0; const int *argv = args >= 0 ? nt_arr(nt, args, "arguments", &argc) : NULL;
        TyKind acc = (argc > 0 && argv) ? infer_type(c, argv[0]) : elem;
        if (acc == TY_UNKNOWN) acc = elem;
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          if (bn > 0) { TyKind bt = infer_type(c, bb[bn - 1]); if (ty_is_numeric(bt)) acc = ty_promote_numeric(acc, bt); }
        }
        return acc;
      }
      int blk = nt_ref(nt, id, "block");
      int body = blk >= 0 ? nt_ref(nt, blk, "body") : -1;
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      /* The codegen path only handles the |v, i| two-param block form; gate the
         result type on it so single-param forms fall to their normal rules. */
      int two_param = blk >= 0 && !block_param_is_multi(c, blk, 0) &&
                      block_param_name(c, blk, 0) && block_param_name(c, blk, 1);
      if (two_param && (sp_streq(name, "map") || sp_streq(name, "collect")))
        return ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN);
      /* filter_map collects the truthy block values (like map, then compact) */
      if (two_param && sp_streq(name, "filter_map"))
        return ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN);
      if (sp_streq(name, "to_a") || sp_streq(name, "entries") ||
          (two_param && (sp_streq(name, "select") || sp_streq(name, "filter") || sp_streq(name, "reject"))))
        return TY_POLY_ARRAY;   /* an array of [element, index] pairs */
      if (blk < 0 && sp_streq(name, "to_h")) {
        /* an array of [element, index] pairs collected into {element => index};
           the block form instead maps each pair, so leave it to its own rule. */
        TyKind h = ty_hash_of(elem, TY_INT);
        return h != TY_UNKNOWN ? h : TY_POLY_POLY_HASH;
      }
      if (two_param && sp_streq(name, "count")) return TY_INT;
      if (two_param && (sp_streq(name, "any?") || sp_streq(name, "all?") || sp_streq(name, "none?")))
        return TY_BOOL;
    }
  }

  /* homogeneous object array (sp_PtrArray of unboxed sp_X*): the typed
     counterpart of the poly-array block below, for the narrowed TY_OBJ_ARRAY
     type. Only the ops narrow_object_arrays admits appear here. */
  if (recv >= 0 && ty_is_obj_array(rt)) {
    int ecls = ty_obj_array_class(rt);
    if ((sp_streq(name, "[]") || sp_streq(name, "at")) && argc == 1) return ty_object(ecls);
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 0) return ty_object(ecls);
    if (sp_streq(name, "[]=") && argc == 2) return ty_object(ecls);
    if (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append")) return rt;
    if ((sp_streq(name, "length") || sp_streq(name, "size")) && argc == 0) return TY_INT;
    if (sp_streq(name, "empty?") && argc == 0) return TY_BOOL;
    /* no-block comparisons (admitted by the narrowing pass only for element
       classes with `<=>`): sort keeps the array type, min/max yield an
       element (NULL-encoded nil when empty). */
    if ((sp_streq(name, "sort") || sp_streq(name, "sort!")) && argc == 0) return rt;
    if ((sp_streq(name, "min") || sp_streq(name, "max")) && argc == 0) return ty_object(ecls);
  }

  /* array receiver methods */
  /* a bare [] literal receiver types UNKNOWN until pushes promote it, but
     its blockless each is still an Enumerator */
  if (recv >= 0 && rt == TY_UNKNOWN && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode") &&
      nt_ref(nt, id, "block") < 0 && argc == 0 &&
      (sp_streq(name, "each") || sp_streq(name, "reverse_each"))) return TY_ENUMERATOR;
  if (recv >= 0 && ty_is_array(rt)) {
    int block = nt_ref(nt, id, "block");
    /* arr.each with no block -> an external Enumerator (#next/#peek/#rewind).
       Block-form chains (each.with_index, each.map) are matched as the outer
       call above and never reach this. */
    if (block < 0 && argc == 0 &&
        (sp_streq(name, "each") || sp_streq(name, "reverse_each") ||
         sp_streq(name, "each_entry") ||
         sp_streq(name, "each_with_index") || sp_streq(name, "each_index"))) return TY_ENUMERATOR;
    /* a blockless map/collect is a usable Enumerator too (size/class/next);
       chained block forms (map.with_index { }) are typed by their own arms
       before this one. */
    if (block < 0 && argc == 0 && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(name, "map") || sp_streq(name, "collect")) &&
        !call_is_chain_receiver_with_block(c, id)) return TY_ENUMERATOR;
    /* arr.each_slice(n) / arr.each_cons(n) with no block -> a materialized
       Enumerator of slices / windows. The direct-block form has block >= 0 and
       is excluded; a .map/.collect chain consumer is typed by its own arm above
       (which accepts this TY_ENUMERATOR receiver and keeps the array result).
       cycle(n) and slice_before/slice_after(pattern) materialize the same way. */
    if (block < 0 && argc == 1 &&
        (sp_streq(name, "each_slice") || sp_streq(name, "each_cons") ||
         sp_streq(name, "cycle") ||
         sp_streq(name, "slice_before") || sp_streq(name, "slice_after"))) return TY_ENUMERATOR;
    /* chunk { } with no chained consumer is an enumerator of [key, run]
       pairs; the desugar interposes to_a so .map/.count chains compose. */
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "chunk")) return TY_ENUMERATOR;
    if (block >= 0) {
      if (ty_iter_shape(name) == TY_ITER_MAP) {
        int body = nt_ref(nt, block, "body");
        int bn = 0;
        const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind bt = bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN;
        /* A value-carrying next widens the element type past the tail
           (e.g. `next "s"` string vs trailing `x` int -> poly array), so the
           collected value is boxed rather than assigned to a typed temp. */
        TyKind bnt = ie_block_break_next_ty(c, body);
        if (bnt != TY_UNKNOWN) bt = (bt == TY_UNKNOWN) ? bnt : ty_unify(bt, bnt);
        return ty_array_of(bt);
      }
      if (sp_streq(name, "flat_map") || sp_streq(name, "collect_concat")) {
        int body = nt_ref(nt, block, "body");
        int bn = 0;
        const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind bret = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        /* block returns an array -> flatten one level keeps its element type;
           a scalar block return behaves like map (each wrapped element). */
        return ty_is_array(bret) ? bret : ty_array_of(bret);
      }
      if (sp_streq(name, "to_h") && argc == 0) {
        /* array.to_h { |x| [k, v] } -> a boxed-value hash, keyed by the
           block's [k, v] tail-pair key type (string/symbol get their own
           hash kind; anything else falls back to a fully boxed hash). */
        int body = nt_ref(nt, block, "body");
        int bn = 0;
        const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        int tail = bn > 0 ? bb[bn - 1] : -1;
        const char *tty = tail >= 0 ? nt_type(nt, tail) : NULL;
        if (tty && sp_streq(tty, "ArrayNode")) {
          int en = 0; const int *el = nt_arr(nt, tail, "elements", &en);
          if (en == 2) {
            TyKind kt = infer_type(c, el[0]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING) return TY_STR_POLY_HASH;
            /* the key's type hasn't settled yet (mid-fixpoint) -- stay
               unknown rather than locking in poly_poly_hash prematurely,
               which would then never narrow once kt resolves. */
            if (kt == TY_UNKNOWN) return TY_UNKNOWN;
          }
        }
        return TY_POLY_POLY_HASH;
      }
      if (sp_streq(name, "select") || sp_streq(name, "reject") ||
          sp_streq(name, "filter") || sp_streq(name, "find_all") ||
          sp_streq(name, "sort_by") ||
          sp_streq(name, "sort_by!") ||
          sp_streq(name, "take_while") || sp_streq(name, "drop_while"))
        return rt;
      if ((sp_streq(name, "max_by") || sp_streq(name, "min_by")) && argc >= 1)
        return TY_POLY_ARRAY;  /* count form: n elements as a generic Array */
      if ((sp_streq(name, "find") || sp_streq(name, "detect")) && argc >= 1)
        return TY_POLY;  /* find(ifnone): the element or the proc's value */
      if (sp_streq(name, "max_by") || sp_streq(name, "min_by") ||
          sp_streq(name, "find") || sp_streq(name, "detect"))
        return ty_array_elem(rt);  /* returns an element */
      if (sp_streq(name, "minmax_by")) return TY_POLY_ARRAY;  /* [min, max], or [nil, nil] when empty */
      if (sp_streq(name, "partition")) return TY_POLY_ARRAY;  /* [[truthy...],[falsy...]] */
      if (sp_streq(name, "filter_map")) return TY_POLY_ARRAY;  /* map then drop falsy */
    }
    /* grep/grep_v without a block filter by `pattern === e`, preserving the
       receiver's array type. */
    if ((sp_streq(name, "grep") || sp_streq(name, "grep_v")) && argc == 1) {
      int gblk = nt_ref(nt, id, "block");
      if (gblk < 0) return rt;
      /* block form: an array of the block's results */
      int gbd = nt_ref(nt, gblk, "body");
      int gbn = 0; const int *gbb = gbd >= 0 ? nt_arr(nt, gbd, "body", &gbn) : NULL;
      TyKind gbt = gbn > 0 ? infer_type(c, gbb[gbn - 1]) : TY_UNKNOWN;
      return gbt == TY_INT ? TY_INT_ARRAY : gbt == TY_FLOAT ? TY_FLOAT_ARRAY
           : gbt == TY_STRING ? TY_STR_ARRAY : TY_POLY_ARRAY;
    }
    if (sp_streq(name, "[]")) {
      /* arr[range] / arr[start, len] -> a subarray; arr[i] -> an element. The
         range index may be a literal RangeNode or a variable/param typed
         TY_RANGE. */
      if (argc == 2) return rt;
      if (argc == 1 && ((nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RangeNode")) ||
                        a0 == TY_RANGE)) return rt;
      return ty_array_elem(rt);
    }
    if (sp_streq(name, "at") && argc == 1) return ty_array_elem(rt);  /* like [i] */
    if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) {
      TyKind et = ty_array_elem(rt);
      if (argc == 2) {
        TyKind dt = infer_type(c, argv[1]);
        return (dt == TY_UNKNOWN || dt == et) ? et : TY_POLY;
      }
      int fblk = nt_ref(nt, id, "block");
      if (fblk >= 0) {
        int fb = nt_ref(nt, fblk, "body");
        int fn = 0; const int *fbb = fb >= 0 ? nt_arr(nt, fb, "body", &fn) : NULL;
        TyKind bt = fn > 0 ? infer_type(c, fbb[fn - 1]) : TY_NIL;
        return (bt == TY_UNKNOWN || bt == et) ? et : TY_POLY;
      }
      return et;
    }
    if (sp_streq(name, "dig") && argc >= 1) {
      if (argc == 1) return ty_array_elem(rt);
      return TY_POLY;
    }
    /* index returns nil on a miss -> poly (int-or-nil) */
    if ((sp_streq(name, "index") || sp_streq(name, "find_index") || sp_streq(name, "rindex")) &&
        (rt == TY_INT_ARRAY || rt == TY_STR_ARRAY)) return TY_POLY;
    if (sp_streq(name, "length") || sp_streq(name, "size") ||
        sp_streq(name, "count") || sp_streq(name, "index") || sp_streq(name, "find_index")) return TY_INT;
    if (sp_streq(name, "sum")) {
      int blk = nt_ref(nt, id, "block");
      /* a float initial value promotes the whole sum to Float (e.g.
         ints.sum(0.0) or ints.sum(0.0) { |x| x }), regardless of the block. */
      if (argc == 1 && infer_type(c, argv[0]) == TY_FLOAT) return TY_FLOAT;
      /* a String / Array initial value folds by concatenation */
      if (argc == 1 && infer_type(c, argv[0]) == TY_STRING) return TY_STRING;
      if (argc == 1 && blk < 0) {
        TyKind sit = infer_type(c, argv[0]);
        if (sit == TY_STRING) return TY_STRING;
        if (ty_is_array(sit) ||
            (sit == TY_UNKNOWN && nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "ArrayNode")))
          return TY_POLY_ARRAY;
      }
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        return bn > 0 ? infer_type(c, bb[bn - 1]) : ty_array_elem(rt);
      }
      return ty_array_elem(rt);
    }
    if (sp_streq(name, "inject") || sp_streq(name, "reduce")) {
      /* inject(&:&|:||:-) over a literal array of int arrays: set operation
         folding the inner arrays -> an int array. */
      if (rt == TY_POLY_ARRAY && comp_is_nested_int_array_literal(c, recv)) {
        int blk = nt_ref(nt, id, "block");
        const char *sop = NULL;
        if (blk >= 0 && nt_type(nt, blk) && sp_streq(nt_type(nt, blk), "BlockArgumentNode")) {
          int ex = nt_ref(nt, blk, "expression");
          if (ex >= 0 && nt_type(nt, ex) && sp_streq(nt_type(nt, ex), "SymbolNode")) sop = nt_str(nt, ex, "value");
        }
        if (sop && (sp_streq(sop, "&") || sp_streq(sop, "|") || sp_streq(sop, "-"))) return TY_INT_ARRAY;
      }
      /* When an init argument is provided, the return type matches the init type.
         inject(:op) is the no-init operator form — the sole symbol arg is the
         operator, NOT an init value, so skip the "return argv[0] type" path. */
      if (argc > 0 && argv) {
        const char *a0ty = nt_type(nt, argv[0]);
        /* a symbol literal, or a local statically holding one (s = :+) */
        int is_sym_op = argc == 1 && sym_static_value(c, argv[0]) != NULL;
        (void)a0ty;
        if (!is_sym_op) {
          TyKind it = infer_type(c, argv[0]);
          /* An empty array-literal seed accumulates an array: poly, since the
             block decides the element mix (`reduce([]) { |a, x| a << x }`). */
          if (it == TY_UNKNOWN && a0ty && sp_streq(a0ty, "ArrayNode")) {
            int sen = 0; nt_arr(nt, argv[0], "elements", &sen);
            if (sen == 0) it = TY_POLY_ARRAY;
          }
          if (it != TY_UNKNOWN) {
            /* The accumulator is reassigned to the block body each iteration,
               so an int seed folded over floats accumulates float. An array
               seed never numeric-promotes (`a << x` pre-types as an int shift
               before the accumulator's type shadows the block param). */
            int rblk = nt_ref(nt, id, "block");
            int rbody = rblk >= 0 ? nt_ref(nt, rblk, "body") : -1;
            int rbn = 0; const int *rbb = rbody >= 0 ? nt_arr(nt, rbody, "body", &rbn) : NULL;
            if (rbn > 0 && !ty_is_array(it)) { TyKind bt = infer_type(c, rbb[rbn - 1]); if (ty_is_numeric(bt)) it = ty_promote_numeric(it, bt); }
            return it;
          }
        }
      }
      /* empty array literal `[]` with sym op: codegen treats as int_array → returns int */
      if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; nt_arr(nt, recv, "elements", &en);
        if (en == 0) return TY_INT;
      }
      /* Block body last expression determines the return type when available. */
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int body = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        if (bn > 0) { TyKind bt = infer_type(c, bb[bn - 1]); if (bt != TY_UNKNOWN) return bt; }
      }
      return ty_array_elem(rt);
    }
    if (sp_streq(name, "each_with_object") && argc > 0 && argv && block < 0)
      return TY_ENUMERATOR;   /* blockless each_with_object -> Enumerator (#2540) */
    if (sp_streq(name, "each_with_object") && argc > 0 && argv) {
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_UNKNOWN) {
        const char *a0ty = nt_type(nt, argv[0]);
        int an0 = 0;
        if (a0ty && sp_streq(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
        if (a0ty && sp_streq(a0ty, "ArrayNode") && an0 == 0) {
          /* empty `[]`: element type from how the memo is filled, else int. */
          TyKind me = ewo_memo_elem_type(c, id);
          return (me != TY_UNKNOWN) ? ty_array_of(me) : TY_INT_ARRAY;
        }
        /* empty `{}` memo: a general (boxed key/value) hash builder. */
        if (a0ty && sp_streq(a0ty, "HashNode") &&
            (nt_arr(nt, argv[0], "elements", &an0), an0 == 0))
          return TY_POLY_POLY_HASH;
      }
      return at;
    }
    if (sp_streq(name, "tally") && argc == 0) {
      if (rt == TY_INT_ARRAY) return TY_INT_INT_HASH;
      if (rt == TY_STR_ARRAY) return TY_STR_INT_HASH;
      /* poly array: keys are the elements (any type), counts are ints. */
      if (rt == TY_POLY_ARRAY) return TY_POLY_POLY_HASH;
    }
    if (sp_streq(name, "tally") && argc == 1)   /* tally(hash) returns the accumulator, boxed (#2533) */
      return TY_POLY;
    if (sp_streq(name, "group_by") && block >= 0 && ty_is_array(rt))
      return TY_POLY_POLY_HASH;
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 1) return rt;  /* first(n)/last(n) -> subarray */
    /* `arr.take(n)`/`drop(n)` is a subarray, but `arr.lazy.take(n)` stays a lazy
       stage -- let the lazy pipeline (below) type the forced chain, not this
       eager subarray arm. */
    {
      int rcv_is_lazy = recv >= 0 && nt_type(nt, recv) &&
                        sp_streq(nt_type(nt, recv), "CallNode") &&
                        nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "lazy");
      if ((sp_streq(name, "drop") || sp_streq(name, "take")) && argc == 1 && !rcv_is_lazy)
        return rt;  /* subarray */
    }
    /* min(n)/max(n) (no comparator block) take the n extreme elements -> a
       subarray; sample(n) likewise. With a comparator block the n-arg form is
       not lowered, so don't type it as an array (that would mis-drive codegen
       into returning a scalar through an array type) -- leave it to reject. */
    if (((sp_streq(name, "min") || sp_streq(name, "max")) && block < 0 && argc == 1) ||
        (sp_streq(name, "sample") && argc == 1))
      return rt;  /* n-arg form -> subarray */
    if (sp_streq(name, "slice") && argc == 2) return rt;
    if ((sp_streq(name, "pop") || sp_streq(name, "shift")) && argc == 1)
      return rt;  /* pop(n)/shift(n): the removed subarray */
    if (sp_streq(name, "cycle") && argc == 1 && nt_ref(nt, id, "block") < 0)
      return rt;  /* blockless cycle(n): the receiver repeated n times */
    if (sp_streq(name, "cycle") && nt_ref(nt, id, "block") >= 0)
      return TY_NIL;  /* the block form returns nil (a valued break widens) */
    if (sp_streq(name, "first") || sp_streq(name, "last") ||
        sp_streq(name, "min") || sp_streq(name, "max") ||
        sp_streq(name, "sample") ||
        sp_streq(name, "pop") || sp_streq(name, "shift")) return ty_array_elem(rt);
    if (sp_streq(name, "minmax")) return rt;  /* [min, max], same element kind */
    if (sp_streq(name, "join"))                        return TY_STRING;
    if (sp_streq(name, "pack") && argc == 1)           return TY_STRING;
    if (sp_streq(name, "inspect") || sp_streq(name, "to_s")) return TY_STRING;
    if (sp_streq(name, "empty?") || sp_streq(name, "include?")) return TY_BOOL;
    if ((sp_streq(name, "all?") || sp_streq(name, "any?") ||
         sp_streq(name, "none?") || sp_streq(name, "one?")) && argc <= 1) return TY_BOOL;
    if ((sp_streq(name, "find") || sp_streq(name, "detect")) && block >= 0 && argc >= 1)
      return TY_POLY;  /* find(ifnone): the element or the proc's value */
    if ((sp_streq(name, "bsearch") || sp_streq(name, "find") || sp_streq(name, "detect")) && block >= 0)
      return ty_array_elem(rt);  /* element or nil */
    if (sp_streq(name, "bsearch_index") && block >= 0) return TY_INT;  /* index, or nil */
    if ((sp_streq(name, "map!") || sp_streq(name, "collect!")) && block >= 0) {
      /* Typed arrays (int/str/float): in-place mutation preserves element type.
         The block param may be widened to TY_POLY when shared with other blocks,
         but the array type is determined by the receiver, not the block body. */
      if (ty_array_elem(rt) != TY_POLY)
        return rt;
      int body = nt_ref(nt, block, "body");
      int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      TyKind bt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
      return bt != TY_UNKNOWN ? ty_array_of(bt) : rt;
    }
    if ((sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "reject!")) &&
        block >= 0) return TY_POLY;  /* self, or nil when nothing was removed */
    if (sp_streq(name, "flatten!") && argc == 1) return TY_POLY;   /* self or nil */
    if (sp_streq(name, "flatten") && argc == 1)
      return rt == TY_POLY_ARRAY ? TY_POLY_ARRAY : rt;  /* typed arrays have no nesting */
    if ((sp_streq(name, "uniq!") || sp_streq(name, "compact!") || sp_streq(name, "flatten!")) &&
        argc == 0 && block < 0) return TY_POLY;  /* self, or nil when a no-op */
    if ((sp_streq(name, "keep_if") || sp_streq(name, "delete_if")) && block >= 0)
      return rt;  /* always self */
    if (sp_streq(name, "find_index") || sp_streq(name, "index") || sp_streq(name, "rindex")) return TY_INT;  /* int or nil */
    if (sp_streq(name, "each_index")) return rt;
    if ((sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append") ||
         sp_streq(name, "unshift") || sp_streq(name, "prepend")) &&
        argc >= 1 && argv && rt != TY_POLY_ARRAY && ty_array_elem(rt) != TY_UNKNOWN) {
      /* Heterogeneous push/unshift on a typed-array literal: lift to poly. */
      TyKind elem_t = ty_array_elem(rt);
      const char *rty = nt_type(nt, recv);
      if (rty && sp_streq(rty, "ArrayNode")) {
        for (int ai = 0; ai < argc; ai++) {
          TyKind at = infer_type(c, argv[ai]);
          if (at != TY_UNKNOWN && at != elem_t) return TY_POLY_ARRAY;
        }
      }
      return rt;
    }
    if (sp_streq(name, "push") || sp_streq(name, "<<") || sp_streq(name, "append") ||
        sp_streq(name, "reverse") || sp_streq(name, "sort") || sp_streq(name, "uniq") ||
        sp_streq(name, "to_a") || sp_streq(name, "to_ary") || sp_streq(name, "deconstruct") ||
        sp_streq(name, "entries") || sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "compact") || sp_streq(name, "flatten") || sp_streq(name, "clear") ||
        sp_streq(name, "transpose") ||
        sp_streq(name, "shuffle") ||
        (sp_streq(name, "union") && argc == 0) ||
        sp_streq(name, "reverse!") || sp_streq(name, "sort!") || sp_streq(name, "shuffle!") ||

        sp_streq(name, "rotate!") || sp_streq(name, "rotate") || sp_streq(name, "insert") || sp_streq(name, "unshift") || sp_streq(name, "prepend") || sp_streq(name, "concat") || sp_streq(name, "freeze") ||
        (sp_streq(name, "fill") && ((block < 0 && argc >= 1 && argc <= 3 &&
                                     /* a fill VALUE incompatible with the element type
                                        makes the result a poly array (see below) */
                                     ({ TyKind _fv = infer_type(c, argv[0]);
                                        TyKind _fe = ty_array_elem(rt);
                                        _fe == TY_POLY || _fv == TY_UNKNOWN || _fv == _fe ||
                                        (ty_is_numeric(_fv) && ty_is_numeric(_fe)); })) ||
                                    (block >= 0 && argc <= 2))) ||
        sp_streq(name, "replace") ||
        sp_streq(name, "values_at") ||
        (sp_streq(name, "fetch_values") && block < 0)) return rt;
    /* the block form mixes fallback values in -> poly array (#2368) */
    if (sp_streq(name, "fetch_values") && block >= 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "fill") && block < 0 && argc >= 1 && argc <= 3)
      return TY_POLY_ARRAY;   /* the incompatible-value fill fell through above */
    if (sp_streq(name, "zip") && block < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "zip") && block >= 0) return TY_NIL;  /* block form returns nil */
    if (sp_streq(name, "product") && argc >= 1)
      return nt_ref(nt, id, "block") >= 0 ? rt : TY_POLY_ARRAY;  /* block form returns self */
    if (sp_streq(name, "product") && argc == 0 && nt_ref(nt, id, "block") < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "combination") && argc == 1 && block < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "permutation") && (argc == 1 || argc == 0) && block < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "repeated_permutation") && argc == 1 && block < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "repeated_combination") && argc == 1 && block < 0) return TY_POLY_ARRAY;
    /* block forms: combination family returns self, each_slice/cons nil */
    if (block >= 0 &&
        (sp_streq(name, "combination") || sp_streq(name, "permutation") ||
         sp_streq(name, "repeated_combination") || sp_streq(name, "repeated_permutation")))
      return rt;
    if (block >= 0 && (sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) && argc == 1)
      return rt;  /* Ruby >= 3.1: block form returns self */
    if (sp_streq(name, "frozen?")) return TY_BOOL;
    /* delete(v) { fallback }: the not-found block's value mixes in -> poly */
    if (sp_streq(name, "delete") && argc == 1 && nt_ref(nt, id, "block") >= 0)
      return TY_POLY;
    if ((sp_streq(name, "delete_at") || sp_streq(name, "delete")) && argc == 1)
      return ty_array_elem(rt);
    if (sp_streq(name, "shift") && argc == 0) return ty_array_elem(rt);
    if ((sp_streq(name, "shift") || sp_streq(name, "pop")) && argc == 1) return rt;  /* removed subarray */
    if (sp_streq(name, "slice") && argc == 1 && nt_ref(nt, id, "block") < 0)
      /* slice(range) is a subarray; slice(i) one element (mirrors #[]) */
      return infer_type(c, argv[0]) == TY_RANGE ? rt : ty_array_elem(rt);
    if (sp_streq(name, "slice!") && argc == 2) return rt;  /* removed subarray */
    if (sp_streq(name, "slice!") && argc == 1)
      /* slice!(range) removes a subarray; slice!(i) removes one element */
      return infer_type(c, argv[0]) == TY_RANGE ? rt : ty_array_elem(rt);
    /* a[i] = v -> v's type (== element type); a[range] = v / a[s,l] = v is a
       splice and returns the RHS as written. The poly-array splice emitter
       yields the RHS BOXED (its `_t` temp is an sp_RbVal), so a poly receiver
       infers TY_POLY; a typed receiver's emitter yields the raw RHS value. */
    if (sp_streq(name, "[]=") && argc == 2) {
      if (infer_type(c, argv[0]) == TY_RANGE)
        return rt == TY_POLY_ARRAY ? TY_POLY : infer_type(c, argv[1]);
      return ty_array_elem(rt);
    }
    if (sp_streq(name, "[]=") && argc == 3)
      return rt == TY_POLY_ARRAY ? TY_POLY : infer_type(c, argv[2]);
    if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && rt == TY_POLY_ARRAY)
      return TY_POLY_ARRAY;  /* the matching sub-array, or nil (NULL ptr) */
    if (sp_streq(name, "to_h") && argc == 0 && block < 0) {
      /* Infer hash type from the first pair element of an array literal */
      if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ArrayNode")) {
        int en = 0; const int *els = nt_arr(nt, recv, "elements", &en);
        if (en > 0 && nt_type(nt, els[0]) && sp_streq(nt_type(nt, els[0]), "ArrayNode")) {
          int en2 = 0; const int *els2 = nt_arr(nt, els[0], "elements", &en2);
          if (en2 >= 2) {
            TyKind kt = infer_type(c, els2[0]);
            TyKind vt = infer_type(c, els2[1]);
            if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
            if (kt == TY_STRING) {
              TyKind h = ty_hash_of(TY_STRING, vt);
              return h != TY_UNKNOWN ? h : TY_STR_POLY_HASH;
            }
            TyKind h = ty_hash_of(kt, vt);
            if (h != TY_UNKNOWN) return h;
          }
        }
      }
      /* Non-literal receiver: the pair element types are not statically known
         (e.g. `a.to_h` for a method param), so a fully boxed hash preserves
         whatever keys/values the pairs hold instead of mis-typing them. */
      return TY_POLY_POLY_HASH;
    }
  }

  /* Exception class-level methods: Cls.exception(msg) is Cls.new (#2740);
     Exception.to_tty? answers whether stderr is a terminal (#2757). */
  if (recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode")) {
    const char *ecn = nt_str(nt, recv, "name");
    if (ecn && is_builtin_exception_name(ecn)) {
      if (sp_streq(name, "exception")) return TY_EXCEPTION;
      if (sp_streq(name, "to_tty?")) return TY_BOOL;
    }
  }

  /* exception receiver methods */
  /* A specialized rescue var is typed as the exception subclass object, but
     its exception-shaped queries still answer as on a base exception, unless
     the subclass defines its own override (#1415). */
  int exc_shaped = rt == TY_EXCEPTION ||
                   (ty_is_object(rt) && class_is_exc_subclass(c, ty_object_class(rt)) &&
                    comp_method_in_chain(c, ty_object_class(rt), name, NULL) < 0);
  if (recv >= 0 && exc_shaped) {
    if (sp_streq(name, "message") || sp_streq(name, "to_s") ||
        sp_streq(name, "to_str") || sp_streq(name, "inspect") ||
        sp_streq(name, "full_message") || sp_streq(name, "detailed_message"))
      return TY_STRING;
    if (sp_streq(name, "class")) return TY_CLASS;  /* a Class object, carried by name */
    if (sp_streq(name, "backtrace")) return TY_STR_ARRAY;  /* empty: no frames captured */
    if (sp_streq(name, "cause")) return TY_EXCEPTION;      /* the threaded cause, nil if none */
    if (sp_streq(name, "result")) return TY_POLY;          /* StopIteration#result, nil otherwise */
    if (sp_streq(name, "name")) return TY_POLY;            /* NameError#name, nil otherwise */
    if (sp_streq(name, "dup") || sp_streq(name, "clone")) return rt;  /* a copy keeps the (subclass) type */
    if (sp_streq(name, "key") || sp_streq(name, "receiver") || sp_streq(name, "args") ||
        sp_streq(name, "reason") || sp_streq(name, "exit_value") ||
        sp_streq(name, "tag") || sp_streq(name, "value"))
      return TY_POLY;   /* class-gated introspection accessors (#2753-#2756, #2770) */
    if (sp_streq(name, "private_call?")) return TY_BOOL;
    if (sp_streq(name, "status")) return TY_INT;       /* SystemExit#status */
    if (sp_streq(name, "success?")) return TY_BOOL;    /* SystemExit#success? */
    if (sp_streq(name, "signo")) return TY_INT;        /* SignalException#signo */
    if (sp_streq(name, "signm")) return TY_STRING;     /* SignalException#signm */
    if (rt == TY_EXCEPTION && sp_streq(name, "exception")) return TY_EXCEPTION;  /* self, or a copy carrying a new message */
    if (sp_streq(name, "eql?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "equal?"))
      return TY_BOOL;
  }

  /* poly receiver / poly operand: result type of operations on sp_RbVal */
  if (recv >= 0 && (rt == TY_POLY || a0 == TY_POLY)) {
    /* array * n is repetition (yielding the same array type), not poly
       arithmetic, even when the count `n` widened to poly under promote. */
    if ((ty_is_array(rt) || rt == TY_POLY_ARRAY) && sp_streq(name, "*") && argc == 1)
      return rt;
    /* String operators with a poly operand are NOT poly arithmetic: `str % x`
       is printf formatting, `str + x` is concatenation, `str * n` is repeat --
       all yield a string. Defer them to the rt==TY_STRING path below. */
    if (!(rt == TY_STRING && (sp_streq(name, "%") || sp_streq(name, "+") || sp_streq(name, "*"))) &&
        (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") ||
         sp_streq(name, "/") || sp_streq(name, "%") || sp_streq(name, "**")))
      return TY_POLY;
    /* unary numeric operators on a poly receiver: negation/unary-plus stay
       poly, bitwise complement yields int. Resolve them here so the poly
       method-dispatch below does not bind `-@`/`+@` to a user class that
       happens to define one (e.g. `-@cents` with @cents widened to poly must
       not infer the enclosing Money type). */
    if (argc == 0 && (sp_streq(name, "-@") || sp_streq(name, "+@"))) return TY_POLY;
    if (argc == 0 && sp_streq(name, "~")) return TY_INT;
    if (sp_streq(name, "<") || sp_streq(name, ">") || sp_streq(name, "<=") ||
        sp_streq(name, ">=") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "nil?") || sp_streq(name, "is_a?") || sp_streq(name, "kind_of?") ||
        sp_streq(name, "include?"))
      return TY_BOOL;
    if (rt == TY_POLY) {
      /* &. on a poly receiver may short-circuit to nil at runtime → always poly */
      {
        const char *call_op = nt_str(nt, id, "call_operator");
        if (recv >= 0 && call_op && sp_streq(call_op, "&.")) return TY_POLY;
      }
      if (sp_streq(name, "to_s") || sp_streq(name, "inspect")) return TY_STRING;
      if ((sp_streq(name, "gsub") || sp_streq(name, "sub")) && argc == 2) return TY_STRING;
      if (sp_streq(name, "join")) return TY_STRING;
      if (sp_streq(name, "to_i") || sp_streq(name, "length") || sp_streq(name, "size")) return TY_INT;
      if (sp_streq(name, "to_f")) return TY_FLOAT;
      /* Hash#keys / #values on a poly hash -> a poly array (boxed elements),
         unless a user class defines that method (then its return type wins).
         to_a on a poly value follows the same rule: nil -> [], arrays and
         hashes materialize, anything else raises (sp_poly_to_a_arr). */
      if ((sp_streq(name, "keys") || sp_streq(name, "values") ||
           sp_streq(name, "to_a")) && argc == 0) {
        int has_user = 0;
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
        if (!has_user) return TY_POLY_ARRAY;
      }
      if (sp_streq(name, "clamp")) return TY_POLY;  /* boxed numeric clamp -> poly */
      /* nil-aware conversions (a nil local widens to poly): boxed results */
      if (argc == 0 && nt_ref(nt, id, "block") < 0 &&
          (sp_streq(name, "to_a") || sp_streq(name, "to_h") ||
           sp_streq(name, "to_r") || sp_streq(name, "rationalize") ||
           sp_streq(name, "to_c"))) {
        int has_user = 0;
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
        if (!has_user) {
          /* concrete result types, matching the TY_NIL receiver arm so a
             local settled on an early (pre-widening) pass stays consistent */
          if (sp_streq(name, "to_a")) return TY_POLY_ARRAY;
          if (sp_streq(name, "to_h")) return TY_SYM_POLY_HASH;
          if (sp_streq(name, "to_r") || sp_streq(name, "rationalize")) return TY_RATIONAL;
          return TY_COMPLEX;
        }
      }
      if (argc == 1 && sp_streq(name, "===")) {
        int has_user = 0;
        for (int k = 0; k < c->nclasses && !has_user; k++)
          if (comp_method_in_chain(c, k, name, NULL) >= 0) has_user = 1;
        if (!has_user) return TY_BOOL;
      }
      /* & | ^ on a poly receiver dispatch on the runtime tag (nil/bool take
         the boolean ops, ints the bitwise ones) via sp_poly_bitop, whose
         result is a boxed value -- so the static type stays poly (#2401). */
      if (argc == 1 && (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
        return TY_POLY;
      /* String transforms on a boxed value: emit_poly_call routes these
         through sp_poly_to_s and re-boxes the result, so the value stays
         poly (mirrors the codegen list in codegen_call_recv.c). */
      if (argc == 0 &&
          (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
           sp_streq(name, "capitalize") || sp_streq(name, "swapcase") ||
           sp_streq(name, "strip") || sp_streq(name, "reverse") ||
           sp_streq(name, "chomp") || sp_streq(name, "chop") ||
           sp_streq(name, "succ") || sp_streq(name, "next") ||
           sp_streq(name, "chr")))
        return TY_POLY;
      /* poly.bytes / poly.codepoints on a value that is really a String (a
         binary lump read whose method widened to poly): a concrete int array,
         emitted via sp_str_bytes(sp_poly_to_s(...)) with no boxing. */
      if ((sp_streq(name, "bytes") || sp_streq(name, "codepoints")) && argc == 0 &&
          nt_ref(nt, id, "block") < 0)
        return TY_INT_ARRAY;
      /* poly.unpack1(fmt): String#unpack1 on a value that widened to poly
         (pervasive in doom's binary WAD parsing). Mirrors the rt==TY_STRING
         rule so a single-directive int format stays int, not poly. */
      if (sp_streq(name, "unpack1") && argc == 1) return an_unpack1_lit_type(nt, argv[0]);
      /* poly.delete(chars): String#delete on a value that widened to poly
         (`data[offset, 8].delete("\x00").upcase` stripping NUL padding off a
         fixed-width WAD name field in doom's texture parser). Resolve it here
         so the poly method-dispatch below does not bind `delete` to whatever
         user class happens to define one: the receiver can still be a string,
         so a user-class `delete` (e.g. the bundled Set's) unifies WITH
         TY_STRING (-> poly) instead of replacing it. No user class keeps the
         concrete TY_STRING, like the rt==TY_STRING rule. */
      if (sp_streq(name, "delete") && argc == 1) {
        TyKind dr = TY_STRING;
        for (int k = 0; k < c->nclasses; k++) {
          int mi = comp_method_in_chain(c, k, name, NULL);
          if (mi >= 0) dr = ty_unify(dr, c->scopes[mi].ret);
        }
        return dr;
      }
      if (sp_streq(name, "[]") && argc == 1) return TY_POLY;  /* boxed array element access */
      if (sp_streq(name, "[]") && argc == 2) return TY_POLY;  /* 2-arg poly slice */
      /* fetch on a poly Hash yields a boxed (poly) value, like `[]` -- the
         hash-value type is not statically known through the poly widening. Type
         it here so the boxed dispatch result is not discarded as nil (without
         this, `fetch` fell through to the non-hash `fetch(k, default)` rule or
         to nil, and its value-position result was dropped). */
      if (sp_streq(name, "fetch") && (argc == 1 || argc == 2)) return TY_POLY;
      /* []= on a poly receiver yields the assigned value, emitted boxed */
      if (sp_streq(name, "[]=") && (argc == 2 || argc == 3)) return TY_POLY;
      if (sp_streq(name, "dig") && argc >= 1) return TY_POLY;
      {
        int blk = nt_ref(nt, id, "block");
        if (blk >= 0 && (ty_iter_shape(name) == TY_ITER_MAP)) {
          int body = nt_ref(nt, blk, "body");
          int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
          TyKind et = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
          return et != TY_UNKNOWN ? ty_array_of(et) : TY_POLY_ARRAY;
        }
      }
      /* poly method dispatch: unify the return type over every class that
         defines `name` (the runtime cls_id picks the impl). */
      TyKind r = TY_UNKNOWN; int found = 0;
      for (int k = 0; k < c->nclasses; k++) {
        if (c->classes[k].is_native_class) {
          int nmk = comp_native_method_find(c, k, name, argc, 0);
          if (nmk >= 0) {
            TyKind nr = sp_streq(c->native_methods[nmk].ret, "self")
                          ? ty_object(k) : native_spec_to_ty(c->native_methods[nmk].ret);
            r = found ? ty_unify(r, nr) : nr; found = 1;
          }
          continue;
        }
        int mi = comp_method_in_chain(c, k, name, NULL);
        if (mi >= 0) { r = found ? ty_unify(r, c->scopes[mi].ret) : c->scopes[mi].ret; found = 1; continue; }
        int rdcls = -1;
        if (comp_reader_in_chain(c, k, name, &rdcls)) {
          char ivn[256]; snprintf(ivn, sizeof ivn, "@%s", name);
          int iv = comp_ivar_index(&c->classes[rdcls], ivn);
          TyKind rt2 = iv >= 0 ? c->classes[rdcls].ivar_types[iv] : TY_UNKNOWN;
          r = found ? ty_unify(r, rt2) : rt2; found = 1;
        }
      }
      if (found) return r;
      /* Numeric queries / rounding on a boxed value: the sp_poly_* helpers
         dispatch on the runtime tag (a non-numeric tag raises CRuby's
         NoMethodError). abs keeps the receiver's class and floor/... can
         return a bigint unchanged, so those stay boxed. */
      if (argc == 0) {
        if (sp_streq(name, "nan?") || sp_streq(name, "finite?") ||
            sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
            sp_streq(name, "negative?")) return TY_BOOL;
        if (sp_streq(name, "abs") || sp_streq(name, "infinite?") ||
            sp_streq(name, "floor") || sp_streq(name, "ceil") ||
            sp_streq(name, "round") || sp_streq(name, "truncate")) return TY_POLY;
        if (sp_streq(name, "bytesize") || sp_streq(name, "ord") ||
            sp_streq(name, "bit_length")) return TY_INT;
      }
      /* String#getbyte on a boxed value: int byte or nil on out-of-range. */
      if (argc == 1 && sp_streq(name, "getbyte")) return TY_POLY;
      /* Array-reduction methods on a boxed array element (a run from
         chunk_while etc.): the concrete element type is erased to poly, so the
         result is a boxed poly value resolved at runtime by cls_id. */
      if (argc == 0 &&
          (sp_streq(name, "sum") || sp_streq(name, "min") || sp_streq(name, "max") ||
           sp_streq(name, "first") || sp_streq(name, "last") || sp_streq(name, "sample")))
        return TY_POLY;
      /* Block iterators on a poly value that holds an array at runtime (a
         recursive param, a `case` whose arms mix arrays and scalars): the result
         is a poly array. codegen coerces the receiver via sp_poly_to_poly_array. */
      if (nt_ref(nt, id, "block") >= 0 &&
          (sp_streq(name, "flat_map") || sp_streq(name, "collect_concat")))
        return TY_POLY_ARRAY;
      /* Fiber/Thread/IO/File instance methods: fallback when no user class defines `name`. */
      if (sp_streq(name, "resume") || sp_streq(name, "value") || sp_streq(name, "join"))
        return TY_POLY;
      if (sp_streq(name, "alive?") || sp_streq(name, "dead?") || sp_streq(name, "closed?") ||
          sp_streq(name, "eof?")) return TY_BOOL;
      if (sp_streq(name, "read") || sp_streq(name, "gets") ||
          sp_streq(name, "readline")) return TY_STRING;
      if (sp_streq(name, "write")) return TY_INT;   /* IO#write: the byte count */
      if (sp_streq(name, "close") || sp_streq(name, "flush")) return TY_NIL;
      if (sp_streq(name, "fileno")) return TY_INT;
      if (sp_streq(name, "synchronize")) {
        int blk_id = nt_ref(nt, id, "block");
        if (blk_id >= 0) {
          int bdy = nt_ref(nt, blk_id, "body");
          int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
          if (bbn > 0) return infer_type(c, bbb[bbn - 1]);
        }
        return TY_NIL;
      }
    }
  }

  /* symbol receiver methods */
  if (recv >= 0 && rt == TY_SYMBOL) {
    if (sp_streq(name, "to_s") || sp_streq(name, "id2name") || sp_streq(name, "name")) return TY_STRING;
    if (sp_streq(name, "inspect")) return TY_STRING;
    if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
        sp_streq(name, "capitalize") || sp_streq(name, "swapcase") ||
        sp_streq(name, "to_sym") || sp_streq(name, "intern") ||
        sp_streq(name, "itself")) return TY_SYMBOL;
    if (sp_streq(name, "length") || sp_streq(name, "size")) return TY_INT;
    if (sp_streq(name, "empty?") || sp_streq(name, "==") || sp_streq(name, "!=")) return TY_BOOL;
    if (sp_streq(name, "succ") || sp_streq(name, "next")) return TY_SYMBOL;
    if ((sp_streq(name, "[]") || sp_streq(name, "slice")) && (argc == 1 || argc == 2)) return TY_STRING;
    if ((sp_streq(name, "start_with?") || sp_streq(name, "end_with?") || sp_streq(name, "match?")) && argc == 1)
      return TY_BOOL;
    /* casecmp/casecmp? against a non-symbol operand answer nil */
    if (sp_streq(name, "casecmp") && argc == 1)
      return infer_type(c, argv[0]) == TY_SYMBOL ? TY_INT : TY_NIL;
    if (sp_streq(name, "casecmp?") && argc == 1)
      return infer_type(c, argv[0]) == TY_SYMBOL ? TY_BOOL : TY_NIL;
  }

  /* range receiver methods */
  if (recv >= 0 && rt == TY_RANGE) {
    /* a literal string range ("a".."z") yields strings, not ints */
    if (sp_streq(name, "to_a")) {
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 && infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING)
          return TY_STR_ARRAY;
      }
    }
    /* String-endpoint range accessors read/return strings, not ints (#2467) */
    {
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      if (rn >= 0 && nt_type(nt, rn) && !sp_streq(nt_type(nt, rn), "RangeNode")) {
        int sl = local_sole_range_node(c, rn);
        if (sl >= 0) rn = sl;
      }
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        if (lo >= 0 && hi >= 0 &&
            infer_type(c, lo) == TY_STRING && infer_type(c, hi) == TY_STRING) {
          if (argc == 0 && (sp_streq(name, "begin") || sp_streq(name, "end") ||
                            sp_streq(name, "first") || sp_streq(name, "last") ||
                            sp_streq(name, "min") || sp_streq(name, "max")))
            return TY_STRING;
          if (argc == 1 && (sp_streq(name, "first") || sp_streq(name, "last")))
            return TY_STR_ARRAY;
        }
      }
    }
    if (sp_streq(name, "to_a") || sp_streq(name, "entries")) return TY_INT_ARRAY;  /* (#2414) */
    if (sp_streq(name, "minmax")) return TY_POLY_ARRAY;   /* [nil, nil] when empty (#2412) */
    /* an ENDLESS literal range: #end is nil (#2413) */
    if (sp_streq(name, "end") && ({ int _rn = recv;
        while (_rn >= 0 && nt_type(nt, _rn) && sp_streq(nt_type(nt, _rn), "ParenthesesNode")) {
          int _bd = nt_ref(nt, _rn, "body"); int _bn = 0;
          const int *_bb = _bd >= 0 ? nt_arr(nt, _bd, "body", &_bn) : NULL;
          _rn = _bn == 1 ? _bb[0] : -1;
        }
        _rn >= 0 && nt_type(nt, _rn) && sp_streq(nt_type(nt, _rn), "RangeNode") &&
        nt_ref(nt, _rn, "right") < 0; })) return TY_POLY;
    /* step { } in value position returns the receiver range (#2415) */
    if (sp_streq(name, "step") && nt_ref(nt, id, "block") >= 0) return TY_RANGE;
    if (sp_streq(name, "include?") || sp_streq(name, "member?") ||
        sp_streq(name, "cover?") || sp_streq(name, "exclude_end?") ||
        sp_streq(name, "eql?") || sp_streq(name, "==") || sp_streq(name, "!=") ||
        sp_streq(name, "overlap?")) return TY_BOOL;
    if (sp_streq(name, "step")) {
      /* step with a block walks the range and returns self */
      if (nt_ref(nt, id, "block") >= 0) return rt;
      /* a float step, or a literal range with float bounds, yields floats */
      int sfloat = argc >= 1 && infer_type(c, argv[0]) == TY_FLOAT;
      int rn = recv;
      while (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "ParenthesesNode")) {
        int body = nt_ref(nt, rn, "body"); int bn = 0;
        const int *bd = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        rn = bn == 1 ? bd[0] : -1;
      }
      int bfloat = 0;
      if (rn >= 0 && nt_type(nt, rn) && sp_streq(nt_type(nt, rn), "RangeNode")) {
        int lo = nt_ref(nt, rn, "left"), hi = nt_ref(nt, rn, "right");
        bfloat = (lo >= 0 && infer_type(c, lo) == TY_FLOAT) ||
                 (hi >= 0 && infer_type(c, hi) == TY_FLOAT);
      }
      return (sfloat || bfloat) ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
    }
    if (sp_streq(name, "all?") || sp_streq(name, "any?") ||
        sp_streq(name, "none?") || sp_streq(name, "one?")) return TY_BOOL;
    if (sp_streq(name, "each") && nt_ref(nt, id, "block") < 0)
      return range_each_is_external(c, id) ? TY_ENUMERATOR : TY_INT_ARRAY;
    if ((sp_streq(name, "each_slice") || sp_streq(name, "each_cons")) &&
        argc == 1 && nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    if ((sp_streq(name, "first") || sp_streq(name, "last")) && argc == 1) return TY_INT_ARRAY;
    if (sp_streq(name, "sum") || sp_streq(name, "min") || sp_streq(name, "max") ||
        sp_streq(name, "first") || sp_streq(name, "last") ||
        sp_streq(name, "size") || sp_streq(name, "count") ||
        sp_streq(name, "begin") || sp_streq(name, "end"))  return TY_INT;
    if (sp_streq(name, "bsearch")) {
      /* a float-bounded range yields a float member (or nil); an int range an
         int member. The bound types are on the receiver's RangeNode. */
      int brn = recv;
      while (brn >= 0 && nt_type(nt, brn) && sp_streq(nt_type(nt, brn), "ParenthesesNode")) {
        int pb = nt_ref(nt, brn, "body"); int pbn = 0;
        const int *pbd = pb >= 0 ? nt_arr(nt, pb, "body", &pbn) : NULL;
        brn = pbn == 1 ? pbd[0] : -1;
      }
      if (brn >= 0 && nt_type(nt, brn) && sp_streq(nt_type(nt, brn), "RangeNode")) {
        int bl = nt_ref(nt, brn, "left"), br = nt_ref(nt, brn, "right");
        /* both bounds must be real NUMERIC nodes (the float-bisection branch
           needs a finite interval; a beginless/endless bound is nil) and at
           least one float -> a float member */
        TyKind blt = bl >= 0 ? infer_type(c, bl) : TY_NIL;
        TyKind brt = br >= 0 ? infer_type(c, br) : TY_NIL;
        if ((blt == TY_INT || blt == TY_FLOAT) && (brt == TY_INT || brt == TY_FLOAT) &&
            (blt == TY_FLOAT || brt == TY_FLOAT)) return TY_FLOAT;
      }
      return TY_INT;  /* a member, or nil (nullable int) */
    }
    int block = nt_ref(nt, id, "block");
    /* finite-range Enumerable methods that materialize to an int array in
       codegen: select/reject/filter (fused loop) and min_by/max_by. */
    if ((sp_streq(name, "min_by") || sp_streq(name, "max_by")) && argc >= 1) return TY_POLY_ARRAY;
    if ((sp_streq(name, "min_by") || sp_streq(name, "max_by")) && block >= 0) return TY_INT;
    if ((ty_iter_shape(name) == TY_ITER_SELECT || ty_iter_shape(name) == TY_ITER_REJECT) &&
        block >= 0) return TY_INT_ARRAY;
    if (block >= 0 && (ty_iter_shape(name) == TY_ITER_MAP)) {
      int body = nt_ref(nt, block, "body");
      int bn = 0;
      const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
      return ty_array_of(bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN);
    }
  }

  /* (range).lazy[.select/reject{blk}].first(n) / .first */
  if ((sp_streq(name, "first") || sp_streq(name, "last")) &&
      recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
    const char *rname = nt_str(nt, recv, "name");
    int lazy_src = -1;
    if (rname && sp_streq(rname, "lazy")) {
      lazy_src = nt_ref(nt, recv, "receiver");
    }
    else if (rname && (sp_streq(rname, "select") || sp_streq(rname, "reject") || sp_streq(rname, "filter"))) {
      int inner = nt_ref(nt, recv, "receiver");
      if (inner >= 0 && nt_type(nt, inner) && sp_streq(nt_type(nt, inner), "CallNode")) {
        const char *iname = nt_str(nt, inner, "name");
        if (iname && sp_streq(iname, "lazy")) lazy_src = nt_ref(nt, inner, "receiver");
      }
    }
    TyKind lst = lazy_src >= 0 ? infer_type(c, lazy_src) : TY_UNKNOWN;
    if (lazy_src >= 0 && lst == TY_RANGE)
      return (argc == 1) ? TY_POLY_ARRAY : TY_INT;  /* emit_lazy_pipeline_expr -> PolyArray */
    /* An array-source lazy first(n) (e.g. the `arr.lazy.take(n).to_a` that the
       take->first desugar produces) materializes n boxed elements. */
    if (lazy_src >= 0 && argc == 1 && ty_is_array(lst))
      return TY_POLY_ARRAY;
  }

  /* `<source>.lazy.<ops>.size` -> the propagated source size: nil if any stage
     changes the element count, Float::INFINITY for an unbounded endless source,
     else Integer. (#2485) */
  if (sp_streq(name, "size") && argc == 0 && nt_ref(nt, id, "block") < 0 &&
      recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode")) {
    int cur = recv, lazy_src = -1, ok = 1, kill = 0, has_take = 0;
    while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
      const char *nm = nt_str(nt, cur, "name");
      if (!nm) { ok = 0; break; }
      if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) { lazy_src = nt_ref(nt, cur, "receiver"); break; }
      if ((sp_streq(nm, "take") || sp_streq(nm, "drop")) && nt_ref(nt, cur, "block") < 0) {
        if (sp_streq(nm, "take")) has_take = 1;
        cur = nt_ref(nt, cur, "receiver"); continue;
      }
      if (nt_ref(nt, cur, "block") < 0) { ok = 0; break; }
      if (sp_streq(nm, "map") || sp_streq(nm, "collect")) { cur = nt_ref(nt, cur, "receiver"); continue; }
      if (sp_streq(nm, "select") || sp_streq(nm, "filter") || sp_streq(nm, "find_all") ||
          sp_streq(nm, "reject") || sp_streq(nm, "take_while") || sp_streq(nm, "drop_while") ||
          sp_streq(nm, "filter_map") || sp_streq(nm, "flat_map") || sp_streq(nm, "collect_concat")) {
        kill = 1; cur = nt_ref(nt, cur, "receiver"); continue;
      }
      ok = 0; break;
    }
    /* unwrap `(1..n)` parentheses so the endless check sees the RangeNode */
    while (lazy_src >= 0 && nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "ParenthesesNode")) {
      int pb = nt_ref(nt, lazy_src, "body"); int pn = 0;
      const int *pd = pb >= 0 ? nt_arr(nt, pb, "body", &pn) : NULL;
      lazy_src = pn == 1 ? pd[0] : -1;
    }
    if (ok && lazy_src >= 0) {
      TyKind st = infer_type(c, lazy_src);
      int is_arr_lit = nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "ArrayNode");
      if (st == TY_RANGE || ty_is_array(st) || is_arr_lit) {
        if (kill) return TY_POLY;   /* nil */
        int endless = 0;
        if (st == TY_RANGE && nt_type(nt, lazy_src) && sp_streq(nt_type(nt, lazy_src), "RangeNode"))
          endless = lazy_endpoint_is_infinite(c, nt_ref(nt, lazy_src, "right"));
        if (endless && !has_take) return TY_FLOAT;   /* Float::INFINITY */
        return TY_INT;
      }
    }
  }

  /* General lazy pipeline: <int range | int array>.lazy.<map/select/reject/
     filter/take_while...>.{first(n) | take(n) | to_a | force} -> an int array. */
  if ((sp_streq(name, "first") ||
       sp_streq(name, "to_a") || sp_streq(name, "force")) &&
      recv >= 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_ref(nt, id, "block") < 0 &&
      !(sp_streq(name, "first") && argc != 1) &&
      !((sp_streq(name, "to_a") || sp_streq(name, "force")) && argc != 0)) {
    int cur = recv, lazy_src = -1, ok = 1, saw_op = 0;
    while (cur >= 0 && nt_type(nt, cur) && sp_streq(nt_type(nt, cur), "CallNode")) {
      const char *nm = nt_str(nt, cur, "name");
      if (!nm) { ok = 0; break; }
      if (sp_streq(nm, "lazy") && nt_ref(nt, cur, "block") < 0) { lazy_src = nt_ref(nt, cur, "receiver"); break; }
      /* blockless counter stages fuse into the pipeline (codegen re-validates
         the single integer argument). */
      if ((sp_streq(nm, "take") || sp_streq(nm, "drop") ||
           (sp_streq(nm, "each_slice") && cur == recv)) && nt_ref(nt, cur, "block") < 0) {
        saw_op = 1; cur = nt_ref(nt, cur, "receiver"); continue;
      }
      if (nt_ref(nt, cur, "block") < 0) { ok = 0; break; }
      if (!sp_streq(nm, "map") && !sp_streq(nm, "collect") && !sp_streq(nm, "select") &&
          !sp_streq(nm, "filter") && !sp_streq(nm, "reject") && !sp_streq(nm, "take_while") &&
          !sp_streq(nm, "drop_while") &&
          !sp_streq(nm, "filter_map") && !sp_streq(nm, "flat_map") &&
          !sp_streq(nm, "collect_concat")) { ok = 0; break; }
      saw_op = 1;
      cur = nt_ref(nt, cur, "receiver");
    }
    if (ok && saw_op && lazy_src >= 0) {
      TyKind st = infer_type(c, lazy_src);
      if (st == TY_RANGE || st == TY_INT_ARRAY || st == TY_ENUMERATOR ||
          st == TY_POLY_ARRAY || st == TY_STR_ARRAY || st == TY_FLOAT_ARRAY) return TY_POLY_ARRAY;
    }
  }

  /* hash receiver methods */
  if (recv >= 0 && sp_streq(name, "default") && argc <= 1 &&
      nt_type(nt, recv) && (sp_streq(nt_type(nt, recv), "HashNode") ||
                             sp_streq(nt_type(nt, recv), "KeywordHashNode"))) {
    return TY_POLY; /* {}.default -> nil (poly nil) */
  }
  /* a literal (or single-assignment variable) Hash.new(d) receiver that
     never narrowed: .default and [] both yield the default (no write can
     have reached it). The variable form widens to poly, hence both gates. */
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_POLY) &&
      ((sp_streq(name, "default") && argc <= 1) ||
       (sp_streq(name, "[]") && argc == 1))) {
    int dn = hash_new_default_arg(c, recv);
    if (dn >= 0) return infer_type(c, dn);
  }
  if (recv >= 0 && (rt == TY_UNKNOWN || rt == TY_POLY) &&
      sp_streq(name, "values_at") && argc >= 1 &&
      hash_new_default_arg(c, recv) >= 0) return TY_POLY_ARRAY;  /* (#2408) */
  /* h.default = v as a value: the assigned value (works for the untyped {}
     literal receiver too) */
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && ty_is_hash(rt))
    return infer_type(c, argv[0]);
  if (recv >= 0 && sp_streq(name, "default=") && argc == 1 && rt == TY_UNKNOWN &&
      nt_type(nt, recv) &&
      (sp_streq(nt_type(nt, recv), "HashNode") || sp_streq(nt_type(nt, recv), "KeywordHashNode")))
    return infer_type(c, argv[0]);
  /* fetch(key, default) on an unknown/empty hash: return the default type */
  if (recv >= 0 && sp_streq(name, "fetch") && argc >= 2 && !ty_is_hash(rt)) {
    TyKind dt = infer_type(c, argv[1]);
    if (dt != TY_UNKNOWN) return dt;
  }
  if (recv >= 0 && ty_is_hash(rt)) {
    /* a blockless each/each_pair/each_key/each_value/each_with_index is an
       external Enumerator (the block forms iterate and are handled below). */
    if (nt_ref(nt, id, "block") < 0 && argc == 0 &&
        (sp_streq(name, "each") || sp_streq(name, "each_pair") ||
         sp_streq(name, "each_key") || sp_streq(name, "each_value") ||
         sp_streq(name, "each_with_index") ||
         /* blockless Enumerable methods are external Enumerators over the pairs */
         sp_streq(name, "map") || sp_streq(name, "collect") ||
         sp_streq(name, "select") || sp_streq(name, "filter") ||
         sp_streq(name, "reject") || sp_streq(name, "find") ||
         sp_streq(name, "detect") || sp_streq(name, "find_all") ||
         sp_streq(name, "flat_map") || sp_streq(name, "filter_map") ||
         sp_streq(name, "sort_by") || sp_streq(name, "min_by") ||
         sp_streq(name, "max_by") || sp_streq(name, "group_by") ||
         sp_streq(name, "partition")))
      return TY_ENUMERATOR;
    if (argc <= 1 && nt_ref(nt, id, "block") < 0 &&
        (sp_streq(name, "any?") || sp_streq(name, "none?") ||
         sp_streq(name, "all?") || sp_streq(name, "one?")))
      return TY_BOOL;
    if (sp_streq(name, "deconstruct_keys") && argc == 1) return rt;
    if (sp_streq(name, "compact!") && argc == 0) return TY_POLY;  /* self or nil */
    /* chunk { |k, v| key } is an enumerator of [key, [[k, v], ...]] pairs;
       the .to_a consumer arm types the materialized chain as a poly array. */
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "chunk")) return TY_ENUMERATOR;
    if (sp_streq(name, "to_proc")) return TY_PROC;
    if (sp_streq(name, "key") && argc == 1 && rt == TY_SYM_POLY_HASH) return TY_SYMBOL;
    if (sp_streq(name, "key") && argc == 1) return TY_POLY;  /* the key (boxed) or nil */
    /* Enumerable first/take/drop over the [key, value] pair list */
    if (sp_streq(name, "first") && argc == 0 && nt_ref(nt, id, "block") < 0) return TY_POLY;
    if ((sp_streq(name, "first") || sp_streq(name, "take") || sp_streq(name, "drop")) &&
        argc == 1 && nt_ref(nt, id, "block") < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_h") && argc == 0 && nt_ref(nt, id, "block") < 0) return rt;  /* identity */
    if (sp_streq(name, "slice") && argc >= 1) return rt;  /* key-subset hash */
    if (sp_streq(name, "[]"))     return ty_hash_val(rt);
    if (sp_streq(name, "[]=") || sp_streq(name, "store"))
      return argc >= 2 ? ty_unify(infer_type(c, argv[1]), ty_hash_val(rt)) : ty_hash_val(rt);
    if (sp_streq(name, "fetch")) {
      TyKind vt = ty_hash_val(rt);
      if (argc == 2) {
        TyKind dt = infer_type(c, argv[1]);
        /* A hash literal default `{}` infers TY_UNKNOWN but is still a hash value
           — incompatible with a non-hash hash-val type like TY_INT. */
        if (dt == TY_UNKNOWN) {
          const char *atn = nt_type(nt, argv[1]);
          if (atn && (sp_streq(atn, "HashNode") || sp_streq(atn, "KeywordHashNode")))
            dt = TY_POLY_POLY_HASH;
        }
        if (ty_unify(vt, dt) == TY_POLY) return TY_POLY;
      }
      int blk = nt_ref(nt, id, "block");
      if (blk >= 0) {
        int bbody = nt_ref(nt, blk, "body");
        int bn = 0; const int *bb = bbody >= 0 ? nt_arr(nt, bbody, "body", &bn) : NULL;
        TyKind bvt = bn > 0 ? infer_type(c, bb[bn - 1]) : vt;
        if (bvt != vt) return TY_POLY;
      }
      return vt;
    }
    if (sp_streq(name, "delete")) return ty_hash_val(rt);
    if (sp_streq(name, "dig") && argc >= 1) {
      if (argc == 1) return ty_hash_val(rt);
      return TY_POLY;
    }
    if (sp_streq(name, "default") && argc <= 1) return TY_POLY;  /* default(key) too (#2409) */
    if (sp_streq(name, "length") || sp_streq(name, "size") ||
        sp_streq(name, "count")) return TY_INT;
    if ((sp_streq(name, "<") || sp_streq(name, "<=") ||
         sp_streq(name, ">") || sp_streq(name, ">=")) && argc == 1)
      return TY_BOOL;  /* subset/superset comparisons */
    if (sp_streq(name, "delete") && argc == 1 && nt_ref(nt, id, "block") >= 0)
      return TY_POLY;  /* deleted value, or the block's fallback */
    if (sp_streq(name, "keys"))   return ty_array_of(ty_hash_key(rt));
    if (sp_streq(name, "values")) return ty_array_of(ty_hash_val(rt));
    if (sp_streq(name, "values_at") || sp_streq(name, "fetch_values")) return TY_POLY_ARRAY;
    int block = nt_ref(nt, id, "block");
    if ((sp_streq(name, "to_a") || sp_streq(name, "entries") || sp_streq(name, "sort")) && block < 0)
      return TY_POLY_ARRAY;
    if (block >= 0 &&
        (sp_streq(name, "min_by") || sp_streq(name, "max_by") ||
         sp_streq(name, "find") || sp_streq(name, "detect")))
      return TY_POLY_ARRAY;   /* the winning [k, v] pair, or nil */
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "sort_by"))
      return TY_POLY_ARRAY;   /* [k, v] pairs ordered by the block value */
    if (nt_ref(nt, id, "block") >= 0 && (sp_streq(name, "all?") || sp_streq(name, "any?")))
      return TY_BOOL;
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "sum"))
      return TY_POLY;   /* boxed accumulation via sp_poly_add */
    /* blockless Hash#sum: folds each [k, v] pair into the init, which is only
       well-defined for an empty hash (else `init + [k,v]` raises TypeError).
       The result is the init's type -- int by default. */
    if (nt_ref(nt, id, "block") < 0 && sp_streq(name, "sum") && argc <= 1)
      return TY_INT;
    if (nt_ref(nt, id, "block") >= 0 &&
        (sp_streq(name, "flat_map") || sp_streq(name, "collect_concat") ||
         sp_streq(name, "filter_map") || sp_streq(name, "partition")))
      return TY_POLY_ARRAY;
    if (nt_ref(nt, id, "block") >= 0 && sp_streq(name, "group_by"))
      return TY_POLY_POLY_HASH;
    {
      if (block >= 0 && (ty_iter_shape(name) == TY_ITER_MAP)) {
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind bt = bn > 0 ? yield_aware_elem_ty(c, bb[bn - 1]) : TY_UNKNOWN;
        /* a value-carrying next widens the element type past the tail */
        TyKind bnt = ie_block_break_next_ty(c, body);
        if (bnt != TY_UNKNOWN) bt = (bt == TY_UNKNOWN) ? bnt : ty_unify(bt, bnt);
        return ty_array_of(bt);
      }
      if (block >= 0 &&
          (sp_streq(name, "select!") || sp_streq(name, "filter!") || sp_streq(name, "reject!")))
        return TY_POLY;  /* self, or nil when nothing was removed */
      if (block >= 0 && (sp_streq(name, "keep_if") || sp_streq(name, "delete_if")))
        return rt;  /* always self */
      if (block >= 0 && (ty_iter_shape(name) == TY_ITER_SELECT || sp_streq(name, "reject"))) return rt;
      if (block >= 0 && sp_streq(name, "transform_keys")) {
        /* a PolyPoly receiver boxes any key: the variant is stable */
        if (rt == TY_POLY_POLY_HASH) return rt;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nkt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        /* Symbol keys have no scalar-valued hash variant, so keys becoming
           symbols (e.g. transform_keys(&:to_sym)) yield a SymPolyHash regardless
           of the value type. */
        if (nkt == TY_SYMBOL) return TY_SYM_POLY_HASH;
        TyKind r = ty_hash_of(nkt, ty_hash_val(rt));
        return r != TY_UNKNOWN ? r : rt;
      }
      if (block >= 0 && sp_streq(name, "transform_values")) {
        if (rt == TY_POLY_POLY_HASH) return rt;
        int body = nt_ref(nt, block, "body");
        int bn = 0; const int *bb = body >= 0 ? nt_arr(nt, body, "body", &bn) : NULL;
        TyKind nvt = bn > 0 ? infer_type(c, bb[bn - 1]) : TY_UNKNOWN;
        TyKind r = ty_hash_of(ty_hash_key(rt), nvt);
        return r != TY_UNKNOWN ? r : rt;
      }
    }
    if (sp_streq(name, "merge") && argc == 1) {
      TyKind at = argc >= 1 ? infer_type(c, argv[0]) : TY_UNKNOWN;
      if (at == rt) return rt;  /* same type: trivial */
      /* cross-variant str-keyed merge: promote to str_poly_hash */
      if (ty_hash_key(rt) == TY_STRING && ty_is_hash(at) && ty_hash_key(at) == TY_STRING)
        return TY_STR_POLY_HASH;
      /* cross-variant sym-keyed merge: both sym → sym_poly (only sym_poly exists) */
      if (ty_hash_key(rt) == TY_SYMBOL && ty_is_hash(at) && ty_hash_key(at) == TY_SYMBOL)
        return TY_SYM_POLY_HASH;
      return rt;
    }
    /* replace(other) returns self, but self's SLOT widens to the universal
       PolyPoly hash when other is a different variant (the receiver-mutation
       evidence in infer_write_types does the same to the local) -- mirror it
       here so a `r = h.replace(o)` capture agrees from the first fixpoint
       iteration instead of lagging one phase behind (#2374 hang: a SymPoly-
       declared r holding a PolyPoly pointer made inspect walk garbage). */
    if (sp_streq(name, "replace") && argc == 1) {
      TyKind ot = infer_type(c, argv[0]);
      if (ty_is_hash(ot) && ot != rt) return TY_POLY_POLY_HASH;
      return rt;
    }
    if (sp_streq(name, "dup") || sp_streq(name, "clone") ||
        sp_streq(name, "merge")) return rt;
    /* #2340/#2349/#2351: no-arg merge / slice / clear / to_hash / rehash keep
       the receiver's variant (an emptied or copied hash of the same shape) */
    if ((sp_streq(name, "merge") || sp_streq(name, "slice")) && argc == 0) return rt;
    if ((sp_streq(name, "clear") || sp_streq(name, "to_hash") || sp_streq(name, "rehash")) && argc == 0)
      return rt;
    /* Hash#shift -> [key, value] pair, or nil (boxed poly) (#2349) */
    if (sp_streq(name, "shift") && argc == 0) return TY_POLY;
    /* Hash#key(value) -> a key, or nil: poly (the key type, nullable) (#2352) */
    if (sp_streq(name, "key") && argc == 1) return TY_POLY;
    /* blockless one? -> bool (exactly one pair) (#2354) */
    if (sp_streq(name, "one?") && argc == 0 && nt_ref(nt, id, "block") < 0) return TY_BOOL;
    /* in-place merge mutates and returns the receiver (its variant is fixed) */
    if ((sp_streq(name, "merge!") || sp_streq(name, "update")) && argc >= 1) return rt;
    if (sp_streq(name, "has_key?") || sp_streq(name, "key?") ||
        sp_streq(name, "include?") || sp_streq(name, "member?") ||
        sp_streq(name, "has_value?") || sp_streq(name, "value?") ||
        sp_streq(name, "empty?")) return TY_BOOL;
    if (sp_streq(name, "each_with_object") && argc > 0 && argv) {
      /* the memo block-param's settled type IS the result: the widening pass
         narrows it from what the block pushes (a string push into an []
         seed makes it a StrArray -- the seed-based guess mistyped it) */
      int ewo_blk = nt_ref(nt, id, "block");
      if (ewo_blk >= 0 && nt_type(nt, ewo_blk) && sp_streq(nt_type(nt, ewo_blk), "BlockNode")) {
        const char *mn = block_param_name(c, ewo_blk, 1);
        if (!mn) mn = block_param_name(c, ewo_blk, 2);   /* |k, v, memo| */
        if (mn) {
          LocalVar *ml = scope_local(comp_scope_of(c, ewo_blk), mn);
          if (ml && ml->type != TY_UNKNOWN && (ty_is_array(ml->type) || ty_is_hash(ml->type)))
            return ml->type;
        }
      }
      TyKind at = infer_type(c, argv[0]);
      if (at == TY_UNKNOWN) {
        const char *a0ty = nt_type(nt, argv[0]);
        int an0 = 0;
        if (a0ty && sp_streq(a0ty, "ArrayNode")) nt_arr(nt, argv[0], "elements", &an0);
        if (a0ty && sp_streq(a0ty, "ArrayNode") && an0 == 0) {
          /* When hash values are poly the block pushes poly values, so the
             accumulator widens to poly_array */
          return ty_hash_val(rt) == TY_POLY ? TY_POLY_ARRAY : TY_INT_ARRAY;
        }
      }
      return at;
    }
    if (sp_streq(name, "flatten") && argc <= 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "invert") && argc == 0) {
      /* swap key/value types where we have a typed variant */
      if (rt == TY_STR_STR_HASH) return TY_STR_STR_HASH;
      return TY_POLY_POLY_HASH;
    }
    if ((sp_streq(name, "assoc") || sp_streq(name, "rassoc")) && argc == 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "compact") && argc == 0) return rt;
    if (sp_streq(name, "except")) return rt;  /* a copy minus the given keys */
  }

  /* <str>.encoding.name -> the encoding name string */
  if (sp_streq(name, "name") && argc == 0 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "CallNode") &&
      nt_str(nt, recv, "name") && sp_streq(nt_str(nt, recv, "name"), "encoding"))
    return TY_STRING;

  /* string receiver methods */
  if (recv >= 0 && rt == TY_STRING) {
    if (sp_streq(name, "clear") && argc == 0) return TY_STRING;  /* empties + returns self (#2332) */
    /* s[i] = v / s[i, n] = v / s[range] = v / s["sub"] = v as a VALUE: the
       assigned string (#2370) */
    if (sp_streq(name, "[]=") && (argc == 2 || argc == 3)) return TY_STRING;
    if (sp_streq(name, "concat") && argc == 0) return TY_STRING;  /* self (#2309) */
    if (sp_streq(name, "clone") && argc == 1) return TY_STRING;  /* clone(freeze: ...) */
    if (sp_streq(name, "encoding") && argc == 0) return TY_POLY;  /* an Encoding value */
    if (sp_streq(name, "upcase") || sp_streq(name, "downcase") ||
        sp_streq(name, "capitalize") || sp_streq(name, "swapcase") ||
        sp_streq(name, "reverse") ||
        ((sp_streq(name, "delete_prefix") || sp_streq(name, "delete_suffix")) && argc == 1) ||
        sp_streq(name, "strip") || sp_streq(name, "lstrip") ||
        sp_streq(name, "rstrip") || sp_streq(name, "chomp") ||
        sp_streq(name, "chop") || sp_streq(name, "chr") || sp_streq(name, "clamp") ||
        sp_streq(name, "squeeze") || sp_streq(name, "tr") || sp_streq(name, "tr_s") ||
        sp_streq(name, "succ") || sp_streq(name, "next") ||
        sp_streq(name, "delete")) return TY_STRING;
    if (sp_streq(name, "slice!")) return TY_STRING;  /* removed part, or nil */
    if (sp_streq(name, "[]") || sp_streq(name, "slice") || sp_streq(name, "byteslice") ||
        sp_streq(name, "bytesplice") || sp_streq(name, "append_as_bytes") ||
        sp_streq(name, "force_encoding") || sp_streq(name, "b") || sp_streq(name, "encode")) return TY_STRING;
    if ((sp_streq(name, "dump") || sp_streq(name, "undump")) && argc == 0) return TY_STRING;
    if (sp_streq(name, "index") && argc == 1) {
      const char *aty = nt_type(nt, argv[0]);
      /* nullable int (SP_INT_NIL on no match), matching the string-needle
         form -- the emitter carries the same sentinel for a regexp needle */
      if (aty && sp_streq(aty, "RegularExpressionNode")) return TY_INT;
      if (infer_type(c, argv[0]) == TY_REGEX) return TY_INT;
    }
    /* casecmp/casecmp? with a statically non-string argument: CRuby answers
       nil rather than raising, so the call types nil (the emitter drops the
       comparison and evaluates the argument for effect). */
    if ((sp_streq(name, "casecmp") || sp_streq(name, "casecmp?")) && argc == 1) {
      TyKind at0 = infer_type(c, argv[0]);
      if (at0 == TY_POLY) return TY_POLY;  /* runtime tag decides: boxed result or nil */
      if (at0 != TY_STRING && at0 != TY_UNKNOWN) return TY_NIL;
      return sp_streq(name, "casecmp") ? TY_INT : TY_BOOL;
    }
    if (sp_streq(name, "index") || sp_streq(name, "to_i") || sp_streq(name, "count") ||
        sp_streq(name, "oct") || sp_streq(name, "hex") || sp_streq(name, "ord") ||
        sp_streq(name, "bytesize") || sp_streq(name, "setbyte") || sp_streq(name, "getbyte")) return TY_INT;
    if (sp_streq(name, "scrub") || sp_streq(name, "crypt")) return TY_STRING;
    if (sp_streq(name, "sum") && argc <= 1) return TY_INT;
    if (sp_streq(name, "unpack1") && (argc == 1 || argc == 2)) return an_unpack1_lit_type(nt, argv[0]);
    if (sp_streq(name, "rindex")) return TY_INT;
    /* byteindex/byterindex over a String or Regexp needle -> byte offset or
       nil (SP_INT_NIL). */
    if ((sp_streq(name, "byteindex") || sp_streq(name, "byterindex")) &&
        (argc == 1 || argc == 2) &&
        (comp_ntype(c, argv[0]) == TY_STRING || comp_ntype(c, argv[0]) == TY_REGEX))
      return TY_INT;
    if (sp_streq(name, "partition") || sp_streq(name, "rpartition")) return TY_STR_ARRAY;
    /* value-form mutators: the post-mutation string; the no-change bang
       contract carries nil as NULL through the nullable string. */
    if (sp_streq(name, "gsub!") || sp_streq(name, "sub!") || sp_streq(name, "upcase!") ||
        sp_streq(name, "downcase!") || sp_streq(name, "capitalize!") || sp_streq(name, "swapcase!") ||
        sp_streq(name, "strip!") || sp_streq(name, "lstrip!") || sp_streq(name, "rstrip!") ||
        sp_streq(name, "chomp!") || sp_streq(name, "chop!") || sp_streq(name, "squeeze!") ||
        sp_streq(name, "tr!") || sp_streq(name, "delete!") || sp_streq(name, "reverse!") ||
        sp_streq(name, "tr_s!") || sp_streq(name, "delete_prefix!") ||
        sp_streq(name, "delete_suffix!") || sp_streq(name, "dedup") ||
        sp_streq(name, "succ!") || sp_streq(name, "next!") ||
        sp_streq(name, "concat") || sp_streq(name, "<<") || sp_streq(name, "prepend") ||
        sp_streq(name, "insert") || sp_streq(name, "replace"))
      return TY_STRING;
    if (sp_streq(name, "ascii_only?") || sp_streq(name, "valid_encoding?")) return TY_BOOL;
    if (sp_streq(name, "to_f"))  return TY_FLOAT;
    if (sp_streq(name, "to_r") && argc == 0) return TY_RATIONAL;
    if ((sp_streq(name, "each_char") || sp_streq(name, "each_line") ||
         sp_streq(name, "each_byte") || sp_streq(name, "each_codepoint")) && argc == 0 &&
        nt_ref(nt, id, "block") < 0) return TY_ENUMERATOR;
    if (sp_streq(name, "each_line") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "KeywordHashNode"))
      return TY_ENUMERATOR;  /* each_line(chomp: ...) blockless */
    if (sp_streq(name, "each_line") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        infer_type(c, argv[0]) == TY_STRING)
      return TY_ENUMERATOR;  /* each_line(sep) blockless */
    if (sp_streq(name, "lines") && argc == 1 && infer_type(c, argv[0]) == TY_STRING)
      return TY_STR_ARRAY;   /* lines(sep) */
    if (sp_streq(name, "each_char") || sp_streq(name, "each_line") || sp_streq(name, "each_byte")) return TY_STRING;
    { int blk = nt_ref(nt, id, "block");
      if (blk >= 0 && (sp_streq(name, "chars") || sp_streq(name, "lines"))) return TY_STRING;
      if (blk >= 0 && (sp_streq(name, "bytes") || sp_streq(name, "codepoints"))) return TY_STRING; }
    if (sp_streq(name, "split") || sp_streq(name, "lines")) return TY_STR_ARRAY;
    if (sp_streq(name, "scan") && argc == 1) {
      /* the block form iterates and returns self (the receiver string) */
      if (nt_ref(nt, id, "block") >= 0) return TY_STRING;
      /* scan with capture groups returns poly_array (array of arrays or strings) */
      const char *aty = nt_type(nt, argv[0]);
      if (aty && sp_streq(aty, "RegularExpressionNode")) {
        const char *src = nt_str(nt, argv[0], "unescaped");
        if (src && an_re_has_captures(src)) return TY_POLY_ARRAY;
      }
      return TY_STR_ARRAY;
    }
    if (sp_streq(name, "upto") && argc == 1) return TY_STR_ARRAY;  /* blockless: materialized sequence */
    if (sp_streq(name, "bytes") || sp_streq(name, "codepoints")) return TY_INT_ARRAY;
    if (sp_streq(name, "unpack") && (argc == 1 || argc == 2)) return TY_POLY_ARRAY;
    if (sp_streq(name, "chars")) return TY_STR_ARRAY;
    if (sp_streq(name, "intern") && argc == 0) return TY_SYMBOL;
    if (sp_streq(name, "to_c") && argc == 0) return TY_COMPLEX;
    if (sp_streq(name, "gsub") && argc == 1 && nt_ref(nt, id, "block") < 0 &&
        nt_type(nt, argv[0]) && sp_streq(nt_type(nt, argv[0]), "RegularExpressionNode"))
      return TY_ENUMERATOR;  /* blockless gsub(/re/): an Enumerator of matches */
    if (sp_streq(name, "gsub") || sp_streq(name, "sub") || sp_streq(name, "tr") ||
        sp_streq(name, "center") || sp_streq(name, "ljust") || sp_streq(name, "rjust"))
      return TY_STRING;
    if (sp_streq(name, "*")) return TY_STRING;
    /* in-place append / concat reassign the receiver and evaluate to it */
    if ((sp_streq(name, "<<") || sp_streq(name, "concat") || sp_streq(name, "prepend")) && argc == 1)
      return TY_STRING;
  }
  /* <int_array>.product(<int_array>)[.to_a].inspect -> a string */
  if (sp_streq(name, "inspect") && argc == 0 && recv >= 0) {
    int pr = recv;
    if (nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "CallNode") &&
        nt_str(nt, pr, "name") && sp_streq(nt_str(nt, pr, "name"), "to_a"))
      pr = nt_ref(nt, pr, "receiver");
    if (pr >= 0 && nt_type(nt, pr) && sp_streq(nt_type(nt, pr), "CallNode") && nt_str(nt, pr, "name") &&
        (sp_streq(nt_str(nt, pr, "name"), "product") || sp_streq(nt_str(nt, pr, "name"), "slice_before") ||
         sp_streq(nt_str(nt, pr, "name"), "slice_after") || sp_streq(nt_str(nt, pr, "name"), "slice_when") ||
         sp_streq(nt_str(nt, pr, "name"), "chunk")))
      return TY_STRING;
  }

  /* numeric.step(...) without a block materializes the sequence as an array */
  if (recv >= 0 && ty_is_numeric(rt) && sp_streq(name, "step") && nt_ref(nt, id, "block") < 0) {
    int args = nt_ref(nt, id, "arguments");
    int sc = 0; const int *sv = args >= 0 ? nt_arr(nt, args, "arguments", &sc) : NULL;
    int isf = (rt == TY_FLOAT) || (sc >= 1 && infer_type(c, sv[0]) == TY_FLOAT) ||
              (sc >= 2 && infer_type(c, sv[1]) == TY_FLOAT);
    return isf ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
  }
  /* integer receiver methods */
  if (recv >= 0 && rt == TY_INT) {
    if (sp_streq(name, "ceil") || sp_streq(name, "floor") ||
        sp_streq(name, "round") || sp_streq(name, "truncate")) return TY_INT;  /* no precision arg -> self */
    if (sp_streq(name, "divmod") && argc == 1) return TY_INT_ARRAY;  /* [quotient, remainder] */
    if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) && argc == 1) return TY_BOOL;
    if (sp_streq(name, "even?") || sp_streq(name, "odd?") || sp_streq(name, "zero?") ||
        sp_streq(name, "positive?") || sp_streq(name, "negative?") ||
        sp_streq(name, "integer?") || sp_streq(name, "finite?") ||
        sp_streq(name, "real?")) return TY_BOOL;
    if (sp_streq(name, "infinite?") && argc == 0) return TY_INT;  /* always nil (nullable int) */
    /* Numeric / Complex-projection on a real Integer (#2328) */
    if ((sp_streq(name, "abs2") || sp_streq(name, "real") || sp_streq(name, "imaginary") ||
         sp_streq(name, "imag") || sp_streq(name, "conj") || sp_streq(name, "conjugate")) && argc == 0)
      return TY_INT;
    if (sp_streq(name, "i") && argc == 0) return TY_COMPLEX;
    if ((sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) && argc == 0)
      return TY_POLY;  /* Integer 0 or Float PI */
    if ((sp_streq(name, "rect") || sp_streq(name, "rectangular")) && argc == 0) return TY_INT_ARRAY;
    if (sp_streq(name, "polar") && argc == 0) return TY_POLY_ARRAY;
    if ((sp_streq(name, "ord") || sp_streq(name, "to_int")) && argc == 0) return TY_INT;
    /* pow with a literal negative exponent yields the exact Rational */
    if (sp_streq(name, "pow") && argc == 1 && nt_type(nt, argv[0]) &&
        sp_streq(nt_type(nt, argv[0]), "IntegerNode") &&
        nt_int(nt, argv[0], "value", 0) < 0) return TY_RATIONAL;
    if (sp_streq(name, "pow") && argc == 1 && infer_type(c, argv[0]) == TY_FLOAT) return TY_FLOAT;
    if ((sp_streq(name, "ceildiv") || sp_streq(name, "pow")) && argc >= 1) return TY_INT;
    if ((sp_streq(name, "pred") || sp_streq(name, "succ") || sp_streq(name, "next")) && argc == 0) return TY_INT;
    if (sp_streq(name, "nonzero?") && argc == 0) return TY_INT;  /* self or nil (nullable int) */
    /* Integer as a Rational: numerator is self, denominator is 1. */
    if ((sp_streq(name, "numerator") || sp_streq(name, "denominator")) && argc == 0) return TY_INT;
    if ((sp_streq(name, "to_r") && argc == 0) ||
        (sp_streq(name, "rationalize") && (argc == 0 || argc == 1))) return TY_RATIONAL;
    if (sp_streq(name, "to_c") && argc == 0) return TY_COMPLEX;
    /* times/upto/downto/step with a block return the receiver (self) */
    if ((sp_streq(name, "times") || sp_streq(name, "upto") || sp_streq(name, "downto") ||
         sp_streq(name, "step")) && nt_ref(nt, id, "block") >= 0) return TY_INT;
    /* times/upto/downto without a block return a range-like enumerator */
    if ((sp_streq(name, "times") || sp_streq(name, "upto") || sp_streq(name, "downto")) &&
        nt_ref(nt, id, "block") < 0) return TY_RANGE;
    if (sp_streq(name, "chr")) return TY_STRING;
    if (sp_streq(name, "[]") && argc == 1) return TY_INT;  /* bit access */
    if (sp_streq(name, "bit_length") && argc == 0) return TY_INT;
    if (sp_streq(name, "fdiv") && argc == 1) return TY_FLOAT;
    if (sp_streq(name, "[]") && (argc == 1 || argc == 2)) return TY_INT;  /* bit access / bit-range field */
    if (sp_streq(name, "div") && argc == 1) return TY_INT;  /* floor division */
    if (sp_streq(name, "gcd") || sp_streq(name, "lcm")) return TY_INT;
    /* clamp keeps the applied operand's class: a Float bound can be returned, so
       the mixed int-receiver/float-bound form is poly; pure-int stays Integer. */
    if (sp_streq(name, "clamp")) {
      if (argc == 2) {
        TyKind b0 = infer_type(c, argv[0]), b1 = infer_type(c, argv[1]);
        if (b0 == TY_FLOAT || b1 == TY_FLOAT || b0 == TY_POLY || b1 == TY_POLY) return TY_POLY;
      }
      return TY_INT;
    }
    if (sp_streq(name, "magnitude") && argc == 0) return TY_INT;  /* alias for abs */
    if ((sp_streq(name, "modulo") || sp_streq(name, "remainder")) && argc == 1) return TY_INT;
    if (sp_streq(name, "gcdlcm") && argc == 1) return TY_INT_ARRAY;  /* [gcd, lcm] */
    if (sp_streq(name, "digits")) return TY_INT_ARRAY;
    if (sp_streq(name, "to_s") && argc == 1) return TY_STRING;
    if (sp_streq(name, "coerce") && argc == 1) {
      TyKind a0 = infer_type(c, argv[0]);
      if (a0 == TY_BIGINT) return TY_POLY_ARRAY;   /* [big, big] boxed pair (#2419) */
      return (a0 == TY_FLOAT || a0 == TY_RATIONAL || a0 == TY_COMPLEX) ? TY_FLOAT_ARRAY : TY_INT_ARRAY;
    }
  }
  /* float receiver methods */
  if (recv >= 0 && rt == TY_FLOAT) {
    if ((sp_streq(name, "arg") || sp_streq(name, "angle") || sp_streq(name, "phase")) && argc == 0)
      return TY_POLY;  /* Integer 0 or Float PI (#2316) */
    if (sp_streq(name, "to_c") && argc == 0) return TY_COMPLEX;
    if (sp_streq(name, "coerce") && argc == 1) return TY_FLOAT_ARRAY;  /* [Float(other), self] */
    if (sp_streq(name, "divmod") && argc == 1) return TY_POLY_ARRAY;  /* [Integer, Float] */
    if (sp_streq(name, "infinite?")) return TY_INT;   /* nil / 1 / -1 (nullable int) */
    if (sp_streq(name, "nan?") || sp_streq(name, "finite?") ||
        sp_streq(name, "positive?") || sp_streq(name, "negative?") ||
        sp_streq(name, "zero?") || sp_streq(name, "integer?") ||
        sp_streq(name, "real?")) return TY_BOOL;
    if (sp_streq(name, "nonzero?")) return TY_POLY;   /* self (Float) or nil */
    if (sp_streq(name, "div") && argc == 1) return TY_INT;  /* integer floor-division */
    /* Complex-view methods on a real Float */
    if (sp_streq(name, "abs2") || sp_streq(name, "real") ||
        sp_streq(name, "conj") || sp_streq(name, "conjugate")) return TY_FLOAT;
    if (sp_streq(name, "imag") || sp_streq(name, "imaginary")) return TY_INT;
    if (sp_streq(name, "rect") || sp_streq(name, "rectangular") ||
        sp_streq(name, "polar")) return TY_POLY_ARRAY;
    if (sp_streq(name, "i")) return TY_COMPLEX;
    /* Float <=> Rational: compare via the rational's float value (#2596) */
    if (sp_streq(name, "<=>") && argc == 1 && comp_ntype(c, argv[0]) == TY_RATIONAL) return TY_INT;
    if (sp_streq(name, "next_float") || sp_streq(name, "prev_float") ||
        sp_streq(name, "abs") || sp_streq(name, "magnitude") ||
        sp_streq(name, "modulo") || sp_streq(name, "remainder") || sp_streq(name, "to_f") ||
        (sp_streq(name, "fdiv") && argc == 1)) return TY_FLOAT;
    if ((sp_streq(name, "numerator") || sp_streq(name, "denominator")) && argc == 0) return TY_INT;
    if ((sp_streq(name, "to_r") && argc == 0) ||
        (sp_streq(name, "rationalize") && (argc == 0 || argc == 1))) return TY_RATIONAL;
    if (sp_streq(name, "eql?") && argc == 1) return TY_BOOL;
    /* clamp with float bounds returns a float (matches codegen in codegen_call.c);
       a mixed/int bound can return the Integer bound, so leave that poly. */
    if (sp_streq(name, "clamp") && argc == 2 &&
        infer_type(c, argv[0]) == TY_FLOAT && infer_type(c, argv[1]) == TY_FLOAT)
      return TY_FLOAT;
    if (sp_streq(name, "floor") || sp_streq(name, "ceil") ||
        sp_streq(name, "round") || sp_streq(name, "truncate")) {
      /* CRuby chooses the return class from the runtime ndigits value: Integer
         when ndigits <= 0, Float when ndigits > 0. With a literal ndigits we
         match it exactly. A NON-literal ndigits can't be classified statically,
         so the result stays Float and the value is still computed exactly (x
         rounded to n places); only #class differs from CRuby when n turns out
         <= 0 -- the documented residual divergence (docs/float-rounding.md). */
      /* a trailing `half:` keyword only picks the tie-break mode; peel it
         off the positional count for the class choice */
      int fr_argc = argc;
      if (fr_argc >= 1 && nt_type(nt, argv[fr_argc - 1]) &&
          sp_streq(nt_type(nt, argv[fr_argc - 1]), "KeywordHashNode"))
        fr_argc--;
      if (fr_argc == 1) {
        const char *aty = nt_type(nt, argv[0]);
        /* Non-literal ndigits: the class (Integer when <= 0, Float when > 0) is
           only known at runtime, so the result is a boxed poly chosen there. */
        if (!aty || !sp_streq(aty, "IntegerNode")) return TY_POLY;
        return nt_int(nt, argv[0], "value", 0) > 0 ? TY_FLOAT : TY_INT;
      }
      return TY_INT;  /* no arg -> self truncated to Integer */
    }
  }

  /* /re/ === str -> match boolean */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "RegularExpressionNode"))
    return TY_BOOL;
  /* Class.===(obj) is always bool */
  if (sp_streq(name, "===") && argc == 1 && recv >= 0 &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "ConstantReadNode"))
    return TY_BOOL;

  if ((sp_streq(name, "-@") || sp_streq(name, "+@")) && recv >= 0 && argc == 0) {
    if (rt == TY_STRING) return TY_STRING;  /* +str = mutable copy; -str = frozen self */
    return ty_is_numeric(rt) ? rt : rt == TY_POLY ? TY_POLY : TY_UNKNOWN;
  }
  /* unary bitwise complement: ~int / ~poly -> int (poly value coerced via to_i) */
  if (sp_streq(name, "~") && recv >= 0 && argc == 0 && (rt == TY_INT || rt == TY_POLY))
    return TY_INT;
  if (sp_streq(name, "!")) return TY_BOOL;
  if (sp_streq(name, "respond_to?") && recv >= 0) return TY_BOOL;
  if ((sp_streq(name, "method_defined?") || sp_streq(name, "const_defined?") ||
       sp_streq(name, "public_method_defined?") || sp_streq(name, "private_method_defined?") ||
       sp_streq(name, "protected_method_defined?")) && recv >= 0) return TY_BOOL;
  /* const_get(:K) with a literal name resolves to the constant's type (codegen
     emits cst_<K>); a literal name that does not resolve raises NameError at
     runtime, so its value type is poly. A dynamic name is left unresolved. */
  if (sp_streq(name, "const_get") && recv >= 0 && argc >= 1) {
    const char *cgt = nt_type(nt, argv[0]);
    const char *cgn = NULL;
    if (cgt && sp_streq(cgt, "SymbolNode")) cgn = nt_str(nt, argv[0], "value");
    else if (cgt && sp_streq(cgt, "StringNode")) cgn = nt_str(nt, argv[0], "content");
    if (cgn) { LocalVar *cv = comp_const(c, cgn); if (cv && cv->type != TY_UNKNOWN) return cv->type; return TY_POLY; }
  }
  if (sp_streq(name, "nil?") && recv >= 0 && argc == 0) return TY_BOOL;
  if ((sp_streq(name, "object_id") || sp_streq(name, "__id__")) && recv >= 0 && argc == 0) return TY_INT;
  /* #hash on a primitive returns an Integer (CRuby's any_hash contract): the
     value is the receiver boxed through sp_rbval_hash_key, the same hashing the
     Hash container uses, so a user `def hash = v.hash` composes consistently. A
     concrete user object is left to its own #hash method dispatch (which may
     return any type -- honored via the sp_obj_hash_hook for keys). */
  if (sp_streq(name, "hash") && recv >= 0 && argc == 0 && !ty_is_object(rt)) return TY_INT;
  if (sp_streq(name, "between?") && argc == 2 && (rt == TY_STRING || ty_is_numeric(rt))) return TY_BOOL;
  /* int | / ^ a Bignum operand promotes (#2422) */
  if (recv >= 0 && argc == 1 && (sp_streq(name, "|") || sp_streq(name, "^")) &&
      infer_type(c, recv) == TY_INT && infer_type(c, argv[0]) == TY_BIGINT) return TY_BIGINT;
  if ((sp_streq(name, "match?") || sp_streq(name, "!~")) && recv >= 0) return TY_BOOL;
  if (sp_streq(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    const char *rrt = nt_type(nt, recv), *art = argc > 0 ? nt_type(nt, argv[0]) : NULL;
    if ((rrt && sp_streq(rrt, "RegularExpressionNode")) ||
        (art && sp_streq(art, "RegularExpressionNode")))
      /* the block form evaluates to the block's value (nil on a miss) */
      return nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA;
  }
  if (sp_streq(name, "=~") && recv >= 0 && argc == 1) {
    const char *rrt = nt_type(nt, recv), *art = nt_type(nt, argv[0]);
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if ((rrt && sp_streq(rrt, "RegularExpressionNode")) ||
        (art && sp_streq(art, "RegularExpressionNode")) ||
        rt == TY_REGEX || a0t == TY_REGEX) return TY_POLY;
  }
  if (sp_streq(name, "match") && recv >= 0 && (argc == 1 || argc == 2)) {
    TyKind a0t = argc > 0 ? infer_type(c, argv[0]) : TY_UNKNOWN;
    if (rt == TY_REGEX || a0t == TY_REGEX) return TY_MATCHDATA;
    /* String#match with a String pattern (regexp source) -> MatchData */
    if ((rt == TY_STRING || rt == TY_STRBUF) && a0t == TY_STRING)
      return nt_ref(nt, id, "block") >= 0 ? TY_POLY : TY_MATCHDATA;
  }
  /* /re/.source -> String, /re/.options -> Integer (compile-time constants) */
  if (recv >= 0 && argc == 0 && nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "RegularExpressionNode")) {
    if (sp_streq(name, "source")) return TY_STRING;
    if (sp_streq(name, "options")) return TY_INT;
  }

  /* array set operations: &, intersection, |, union, -, difference. The named
     forms are variadic (fold over each argument); the operators are binary. */
  if (recv >= 0 && argc >= 1 &&
      (sp_streq(name, "&") || sp_streq(name, "intersection") ||
       sp_streq(name, "|") || sp_streq(name, "union") ||
       sp_streq(name, "-") || sp_streq(name, "difference"))) {
    if (ty_is_array(rt) && a0 == rt) return rt;
    /* empty array [] arg (TY_UNKNOWN): result is same kind as receiver */
    if (ty_is_array(rt) && a0 == TY_UNKNOWN) return rt;
    /* any array receiver with a different-kind (or poly) array argument: the
       codegen boxes both operands to poly and runs the poly set op, so the
       result is a poly array. */
    if (ty_is_array(rt) && ty_is_array(a0) && a0 != rt) return TY_POLY_ARRAY;
  }
  /* Array#intersect?(other) -> bool */
  if (recv >= 0 && argc == 1 && sp_streq(name, "intersect?") && ty_is_array(rt))
    return TY_BOOL;
  if (recv >= 0 && argc == 1 && is_arith_op(name)) {
    if (rt == TY_STRING) {
      if (sp_streq(name, "%")) return TY_STRING;  /* sprintf (array or single value) */
      if (sp_streq(name, "+") || sp_streq(name, "*")) {
        /* `str + x` / `str * n` always yield a String; a poly operand (which
           holds a string at runtime) is coerced via sp_poly_to_s in codegen. */
        return TY_STRING;
      }
      return TY_UNKNOWN;
    }
    /* array + same-kind -> same kind; different-kind -> poly_array */
    if (sp_streq(name, "+") && ty_is_array(rt) && a0 == rt) return rt;
    if (sp_streq(name, "+") && ty_is_array(rt) && ty_is_array(a0) && a0 != rt) return TY_POLY_ARRAY;
    /* typed array +/- an empty literal [] (a0 UNKNOWN) keeps the receiver kind */
    if ((sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "&") ||
         sp_streq(name, "|") || sp_streq(name, "union") || sp_streq(name, "difference") ||
         sp_streq(name, "intersection")) && ty_is_array(rt) && a0 == TY_UNKNOWN) return rt;
    /* array * int -> same array type (repeat); array * string -> join string */
    if (sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_INT) return rt;
    if (sp_streq(name, "*") && (ty_is_array(rt) || rt == TY_POLY_ARRAY) && a0 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(rt) && ty_is_numeric(a0)) {
      if (rt == TY_FLOAT || a0 == TY_FLOAT) return TY_FLOAT;
      if (rt == TY_BIGINT || a0 == TY_BIGINT) return TY_BIGINT;
      return TY_INT;
    }
    /* numeric receiver <op> a coercing user object: the result is what the
       object's own <op> returns (coerce yields a pair of that class). */
    if ((rt == TY_INT || rt == TY_FLOAT) && ty_is_object(a0)) {
      int acls = ty_object_class(a0);
      if (comp_method_in_chain(c, acls, "coerce", NULL) >= 0) {
        int op_mi = comp_method_in_chain(c, acls, name, NULL);
        if (op_mi >= 0) return (TyKind)c->scopes[op_mi].ret;
      }
    }
    /* a poly operand makes the +,-,*,/ result poly: codegen lowers these to
       sp_poly_<op>, which returns a (boxed) poly, so the static type must agree. */
    if ((rt == TY_POLY || a0 == TY_POLY) &&
        (sp_streq(name, "+") || sp_streq(name, "-") || sp_streq(name, "*") || sp_streq(name, "/")))
      return TY_POLY;
    /* An Integer/Bignum arith op with a non-coercible (String/Symbol/nil/bool/
       Array/Hash/Range) argument raises TypeError at run time; type the raising
       expression as int so any value position (p, assignment) can emit it -- the
       codegen raise-expr is int-typed too (#2471). */
    if ((rt == TY_INT || rt == TY_BIGINT) &&
        (a0 == TY_STRING || a0 == TY_SYMBOL || a0 == TY_NIL || a0 == TY_BOOL ||
         ty_is_array(a0) || ty_is_hash(a0) || a0 == TY_RANGE))
      return TY_INT;
    return TY_UNKNOWN;
  }
  if (recv >= 0 && argc == 1 && sp_streq(name, "<=>")) return TY_INT;
  if (recv >= 0 && argc == 1 && is_cmp_op(name)) return TY_BOOL;
  if (argc == 1 && is_eq_op(name)) return TY_BOOL;

  /* integer bitwise operators */
  if (recv >= 0 && argc == 1 && rt == TY_INT &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
       sp_streq(name, "<<") || sp_streq(name, ">>")))
    return TY_INT;
  /* bigint bitwise ops keep arbitrary precision (a `<<` widening that overflows
     int is exactly why the receiver was promoted to bigint; and `bignum & MASK`
     can still exceed int64, e.g. 0x9e37…c16 & ((1<<64)-1)). */
  if (recv >= 0 && argc == 1 && rt == TY_BIGINT &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^") ||
       sp_streq(name, "<<") || sp_streq(name, ">>")))
    return TY_BIGINT;
  /* Integer#bit_length on a Bignum answers an int (the bit count fits int64). */
  if (recv >= 0 && argc == 0 && rt == TY_BIGINT && sp_streq(name, "bit_length"))
    return TY_INT;
  if (recv >= 0 && rt == TY_BIGINT) {
    if ((sp_streq(name, "even?") || sp_streq(name, "odd?")) && argc == 0) return TY_BOOL;
    if (sp_streq(name, "abs") && argc == 0) return TY_BIGINT;
    if ((sp_streq(name, "magnitude") || sp_streq(name, "abs2")) && argc == 0) return TY_BIGINT;  /* (#2418/#2424) */
    /* Bignum#downto/#upto with no block: materialized poly array of Bignums (#2305) */
    if ((sp_streq(name, "downto") || sp_streq(name, "upto")) && argc == 1 &&
        nt_ref(nt, id, "block") < 0) return TY_POLY_ARRAY;
    if (sp_streq(name, "to_s") && argc == 1) return TY_STRING;
    if (sp_streq(name, "digits") && argc <= 1) return TY_INT_ARRAY;
    /* #2318 / #2319: query + reflection on a Bignum */
    if ((sp_streq(name, "zero?") || sp_streq(name, "positive?") ||
         sp_streq(name, "negative?") || sp_streq(name, "integer?")) && argc == 0) return TY_BOOL;
    /* to_i / to_int is self (the full Bignum, not a truncated int); succ/pred
       stay Bignum */
    if ((sp_streq(name, "to_i") || sp_streq(name, "to_int") || sp_streq(name, "succ") ||
         sp_streq(name, "next") || sp_streq(name, "pred")) && argc == 0) return TY_BIGINT;
    if (sp_streq(name, "class") && argc == 0) return TY_CLASS;
    if ((sp_streq(name, "round") || sp_streq(name, "ceil") || sp_streq(name, "floor")) &&
        (argc == 0 || argc == 1)) return TY_BIGINT;  /* #2303 */
    /* Integer/Float/bool-returning Bignum methods that need no Rational (#2469).
       to_r/rationalize/quo would need a bigint-backed Rational and stay
       unsupported. */
    if (sp_streq(name, "~") && argc == 0) return TY_BIGINT;
    if ((sp_streq(name, "numerator") || sp_streq(name, "ord")) && argc == 0) return TY_BIGINT;
    if ((sp_streq(name, "denominator") || sp_streq(name, "size")) && argc == 0) return TY_INT;
    if (sp_streq(name, "nonzero?") && argc == 0) return TY_POLY;   /* self or nil */
    if (sp_streq(name, "fdiv") && argc == 1) return TY_FLOAT;
    if (sp_streq(name, "pow") && argc == 1) return TY_BIGINT;
    /* modulo/%/remainder/modular-pow stay Bignum; divmod is a [q, r] pair;
       #[] is a single bit (0/1) (#2594) */
    if ((sp_streq(name, "modulo") || sp_streq(name, "%") || sp_streq(name, "remainder")) &&
        argc == 1) return TY_BIGINT;
    if (sp_streq(name, "pow") && argc == 2) return TY_BIGINT;
    if (sp_streq(name, "divmod") && argc == 1) return TY_POLY_ARRAY;
    if (sp_streq(name, "[]") && argc == 1) return TY_INT;
    if ((sp_streq(name, "div") || sp_streq(name, "gcd") || sp_streq(name, "lcm") ||
         sp_streq(name, "ceildiv")) && argc == 1) return TY_BIGINT;
    if ((sp_streq(name, "allbits?") || sp_streq(name, "anybits?") || sp_streq(name, "nobits?")) &&
        argc == 1) return TY_BOOL;
    if (sp_streq(name, "gcdlcm") && argc == 1) return TY_POLY_ARRAY;
    /* to_r/rationalize/quo on a Bignum produce a boxed big Rational (#2469) */
    if ((sp_streq(name, "to_r") || sp_streq(name, "rationalize")) && argc == 0) return TY_POLY;
    if (sp_streq(name, "quo") && argc == 1) return TY_POLY;
  }
  /* poly recv bitwise op: result is int (sp_poly_to_i applied). */
  if (recv >= 0 && argc == 1 && rt == TY_POLY &&
      (sp_streq(name, ">>") || sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
    return TY_INT;
  /* poly recv `<<` is ambiguous (Integer#<< shift vs Array#push append); the
     runtime sp_poly_shl dispatches on the tag and returns a boxed result either
     way, so the static type is poly. A downstream bitwise op coerces it back to
     int, and an append keeps its (boxed) array -- both stay consistent. */
  if (recv >= 0 && argc == 1 && rt == TY_POLY && sp_streq(name, "<<"))
    return TY_POLY;
  /* boolean &/|/^ */
  if (recv >= 0 && argc == 1 && rt == TY_BOOL &&
      (sp_streq(name, "&") || sp_streq(name, "|") || sp_streq(name, "^")))
    return TY_BOOL;

  size_t nl = strlen(name);
  if (nl > 0 && name[nl - 1] == '?') return TY_BOOL;

  if (sp_streq(name, "to_s") || sp_streq(name, "inspect") ||
      sp_streq(name, "chr") || sp_streq(name, "to_str")) return TY_STRING;
  if (sp_streq(name, "to_i") || sp_streq(name, "to_int") ||
      sp_streq(name, "length") || sp_streq(name, "size") ||
      sp_streq(name, "count") ||
      sp_streq(name, "ord") || sp_streq(name, "abs")) return TY_INT;
  if (sp_streq(name, "to_f")) return TY_FLOAT;
  if (sp_streq(name, "to_sym")) return TY_SYMBOL;

  if (is_void_call(name) && recv < 0) return TY_VOID;

  /* $stdout/$stderr.puts/print/write return nil (so a value-position use --
     an if/else arm or assignment -- unifies and boxes as nil). */
  if (recv >= 0 && (sp_streq(name, "puts") || sp_streq(name, "print") || sp_streq(name, "write") ||
                    sp_streq(name, "syswrite")) &&
      nt_type(nt, recv) && sp_streq(nt_type(nt, recv), "GlobalVariableReadNode")) {
    const char *gv = nt_str(nt, recv, "name");
    if (gv && (sp_streq(gv, "$stdout") || sp_streq(gv, "$stderr")))
      return (sp_streq(name, "write") || sp_streq(name, "syswrite")) ? TY_INT : TY_NIL;
  }

  /* tap: run block, return self */
  if (sp_streq(name, "tap") && recv >= 0) return rt;
  /* then / yield_self: run block, return block result */
  if (sp_streq(name, "then") || sp_streq(name, "yield_self")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      /* Pin block param to receiver type so body inference uses the right type */
      const char *bp0 = block_param_name(c, blk_id, 0);
      Scope *bs = bp0 ? comp_scope_of(c, blk_id) : NULL;
      LocalVar *blv = (bs && bp0) ? scope_local(bs, bp0) : NULL;
      TyKind saved_blv = blv ? blv->type : TY_UNKNOWN;
      if (blv && rt != TY_UNKNOWN) blv->type = rt;
      TyKind result = infer_type(c, bbb[bbn - 1]);
      if (blv) blv->type = saved_blv;
      return result;
    }
  }
  if (sp_streq(name, "instance_eval")) {
    int blk_id = nt_ref(nt, id, "block");
    if (blk_id >= 0 && ty_is_object(rt) &&
        comp_method_in_chain(c, ty_object_class(rt), "instance_eval", NULL) < 0) {
      int bdy = nt_ref(nt, blk_id, "body");
      int bbn = 0; const int *bbb = bdy >= 0 ? nt_arr(nt, bdy, "body", &bbn) : NULL;
      if (bbn <= 0) return TY_NIL;
      int saved_ie = an_ie_class_id;
      an_ie_class_id = ty_object_class(rt);
      TyKind result = infer_type(c, bbb[bbn - 1]);
      an_ie_class_id = saved_ie;
      return result;
    }
    return TY_POLY;
  }

  /* safe navigation &. with unresolved type: return poly (receiver may be nil at runtime) */
  {
    const char *call_op = nt_str(nt, id, "call_operator");
    if (recv >= 0 && call_op && sp_streq(call_op, "&.")) return TY_POLY;
  }

  /* Builtin class reopening: look up user-defined methods on Array/Numeric/Object
     receivers where no builtin method matched. */
  if (recv >= 0) {
    /* Array reopening: any array-typed receiver */
    if (ty_is_array(rt)) {
      int oc_ci = comp_class_index(c, "Array");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Numeric reopening: integers and floats */
    if (rt == TY_INT || rt == TY_FLOAT) {
      int oc_ci = comp_class_index(c, "Numeric");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* FalseClass methods (TrueClass already checked earlier for TY_BOOL) */
    if (rt == TY_BOOL) {
      int oc_ci = comp_class_index(c, "FalseClass");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
    /* Object reopening: universal fallback for any receiver type */
    {
      int oc_ci = comp_class_index(c, "Object");
      if (oc_ci >= 0) {
        int oc_mi = comp_method_in_chain(c, oc_ci, name, NULL);
        if (oc_mi >= 0) return c->scopes[oc_mi].ret;
      }
    }
  }

  return TY_UNKNOWN;
}

/* ---- core inference ---- */

TyKind infer_uncached(Compiler *c, int id) {
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return TY_UNKNOWN;
  NodeKind nk = nt_kind(nt, id);

  if (nk == NK_IntegerNode)             return nt_str(nt, id, "bigval") ? TY_BIGINT : TY_INT;
  if (nk == NK_FloatNode)               return TY_FLOAT;
  if (nk == NK_ImaginaryNode)           return TY_COMPLEX;
  if (nk == NK_RationalNode)            return TY_RATIONAL;
  if (nk == NK_StringNode)              return TY_STRING;
  if (nk == NK_SourceFileNode)          return TY_STRING;
  if (nk == NK_SourceLineNode)          return TY_INT;
  if (nk == NK_SourceEncodingNode)      return TY_POLY;
  if (nk == NK_RegularExpressionNode ||
      nk == NK_InterpolatedRegularExpressionNode) return TY_REGEX;
  if (nk == NK_MatchPredicateNode)      return TY_BOOL;   /* `expr in pattern` */
  /* `/(?<n>..)/ =~ str` -- the value is the `=~` result (match index or nil). */
  if (sp_streq(ty, "MatchWriteNode")) return TY_POLY;
  if (nk == NK_InterpolatedStringNode)  return TY_STRING;
  if (nk == NK_XStringNode || nk == NK_InterpolatedXStringNode) return TY_STRING;
  if (nk == NK_InterpolatedSymbolNode)  return TY_SYMBOL;
  if (nk == NK_SymbolNode)              return TY_SYMBOL;
  if (nk == NK_TrueNode)                return TY_BOOL;
  if (nk == NK_FalseNode)               return TY_BOOL;
  if (nk == NK_NilNode)                 return TY_NIL;
  /* A while/until loop in value position evaluates to nil (a valued `break`
     is a separate gap); type it as poly so the slot holds a boxed nil. */
  if (nk == NK_WhileNode || nk == NK_UntilNode) return TY_POLY;
  if (nk == NK_RangeNode) {
    /* (:a..:e): symbols enumerate by name succession -- the whole range
       lowers to a poly array of boxed symbols (see codegen_expr) */
    {
      int slo = nt_ref(nt, id, "left"), shi = nt_ref(nt, id, "right");
      const char *slt = slo >= 0 ? nt_type(nt, slo) : NULL;
      const char *sht = shi >= 0 ? nt_type(nt, shi) : NULL;
      if (slt && sht && sp_streq(slt, "SymbolNode") && sp_streq(sht, "SymbolNode"))
        return TY_POLY_ARRAY;
    }
    /* infer the bounds so codegen can tell an int range from a string range */
    int lo = nt_ref(nt, id, "left"), hi = nt_ref(nt, id, "right");
    TyKind lt = lo >= 0 ? infer_type(c, lo) : TY_UNKNOWN;
    TyKind ht = hi >= 0 ? infer_type(c, hi) : TY_UNKNOWN;
    /* (1.0..3.0): a BOUNDED range with both endpoints float lowers to the
       distinct sp_FloatRange type. A mixed int/float range (1..3.0) and any
       beginless/endless float range stay on the ordinary int TY_RANGE path,
       which already serves their (narrow, mostly-raising) method surface. */
    if (lo >= 0 && hi >= 0 && lt == TY_FLOAT && ht == TY_FLOAT)
      return TY_FLOAT_RANGE;
    return TY_RANGE;
  }
  /* A splat inside an array literal (`[*0..10]`, `[*arr]`) contributes the
     element type of the splatted collection, so the literal stays a typed
     array instead of widening to poly_array. The value returned here is the
     would-be element type, which the ArrayNode arm unifies in. */
  if (nk == NK_SplatNode) {
    int inner = nt_ref(nt, id, "expression");
    if (inner < 0) return TY_UNKNOWN;
    const char *ity = nt_type(nt, inner);
    if (ity && sp_streq(ity, "RangeNode")) {
      int lo = nt_ref(nt, inner, "left");
      return (lo >= 0 && infer_type(c, lo) == TY_STRING) ? TY_STRING : TY_INT;
    }
    TyKind it = infer_type(c, inner);
    if (ty_is_array(it)) return ty_array_elem(it);
    return TY_POLY;
  }
  if (nk == NK_LambdaNode)              return TY_PROC;
  /* an assignment expression evaluates to the assigned value -- but codegen
     lowers `x = expr` to `({ lv_x = ...; lv_x; })`, so the chain value IS the
     slot. Return the local's slot type (when known) so a chained `a = b = expr`
     boxes consistently with the slot, mirroring the ivar-write rule below. */
  if (nk == NK_LocalVariableWriteNode) {
    const char *lwn = nt_str(nt, id, "name");
    Scope *lws = comp_scope_of(c, id);
    LocalVar *lwv = lwn ? scope_local(lws, lwn) : NULL;
    if (lwv && lwv->type != TY_UNKNOWN) return lwv->type;
    return infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_InstanceVariableWriteNode ||
      nk == NK_InstanceVariableOrWriteNode ||
      nk == NK_InstanceVariableAndWriteNode ||
      nk == NK_InstanceVariableOperatorWriteNode) {
    /* expression evaluates to the ivar slot's type (same as a read): codegen
       lowers `@a = expr` to `({ iv_a = ...; iv_a; })`, so the chain value IS the
       slot, and inference must match to keep `@x = @a = expr` boxing consistent. */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* inside an instance_eval/exec splice the block scope has no class_id; the
       ivar belongs to the rebound receiver class (an_ie_class_id). */
    int wcls = s->class_id >= 0 ? s->class_id : an_ie_class_id;
    if (wcls < 0) return infer_type(c, nt_ref(nt, id, "value"));
    ClassInfo *ci = &c->classes[wcls];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (nk == NK_LocalVariableOperatorWriteNode) {
    const char *nm2 = nt_str(nt, id, "name");
    Scope *s2 = comp_scope_of(c, id);
    LocalVar *lv2 = nm2 ? scope_local(s2, nm2) : NULL;
    TyKind ct2 = lv2 ? lv2->type : TY_UNKNOWN;
    TyKind vt2 = infer_type(c, nt_ref(nt, id, "value"));
    if (ct2 == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct2) && ty_is_numeric(vt2))
      return (ct2 == TY_FLOAT || vt2 == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct2 != TY_UNKNOWN ? ct2 : vt2;
  }
  if (nk == NK_LocalVariableOrWriteNode || nk == NK_LocalVariableAndWriteNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    return ty_unify(ct, infer_type(c, nt_ref(nt, id, "value")));
  }

  if (nk == NK_LocalVariableReadNode) {
    /* a `return .. if p.nil?`-guarded param read: the non-nil type (#1661) */
    if (c->nilnarrow[id] != TY_UNKNOWN) return c->nilnarrow[id];
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    /* &block param that escapes (not yield-inlined): the LocalVar slot type is
       TY_UNKNOWN, but the value is a Proc object when the method does not inline
       the block (yields==0). Return TY_PROC so callers can type the return value. */
    if (nm && s && s->blk_param && s->blk_param[0] && sp_streq(nm, s->blk_param)
        && !s->yields)
      return TY_PROC;
    LocalVar *lv = nm ? scope_local(s, nm) : NULL;
    if (lv) return lv->type;
    return TY_UNKNOWN;
  }
  if (nk == NK_GlobalVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    /* $stdin is the IO handle over the C stdin stream (gets/read/tty?/... via
       the TY_IO dispatch). $stdout/$stderr keep their dedicated AST-shape
       emission arm and stay untyped here. */
    if (nm && (sp_streq(nm, "$stdin") || sp_streq(nm, "$stdout") ||
               sp_streq(nm, "$stderr"))) return TY_IO;   /* the C stream handles (#2818) */
    /* predefined punctuation globals: $/ defaults to "\n"; $! / $; / $, read nil */
    if (nm && sp_streq(nm, "$/")) return TY_STRING;
    if (nm && sp_streq(nm, "$?")) return TY_INT;  /* last child exit status */
    if (nm && (sp_streq(nm, "$PROGRAM_NAME") || sp_streq(nm, "$0"))) return TY_STRING;
    if (nm && sp_streq(nm, "$!")) return TY_EXCEPTION;  /* the exception being handled, or nil (NULL) outside a rescue */
    if (nm && (sp_streq(nm, "$;") || sp_streq(nm, "$,"))) return TY_NIL;
    /* regex match globals: $~ is the last MatchData (NULL = nil); the
       text back-references are nullable strings */
    if (nm && sp_streq(nm, "$~")) return TY_MATCHDATA;
    if (nm && (sp_streq(nm, "$&") || sp_streq(nm, "$`") ||
               sp_streq(nm, "$'") || sp_streq(nm, "$+"))) return TY_STRING;
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    return lv ? lv->type : TY_UNKNOWN;
  }
  if (nk == NK_GlobalVariableOperatorWriteNode) {
    /* `$g += v` evaluates to the updated value (the local/ivar op-write forms
       above already do; #1484). Plain `$g = v` and `||=`/`&&=` stay untyped
       statements, mirroring the local-variable policy. */
    const char *nm = nt_str(nt, id, "name");
    const char *rn = nm ? comp_resolve_gvar(c, nm + 1) : NULL;
    LocalVar *lv = rn ? comp_gvar(c, rn) : NULL;
    TyKind ct = lv ? lv->type : TY_UNKNOWN;
    TyKind vt = infer_type(c, nt_ref(nt, id, "value"));
    if (ct == TY_STRING) return TY_STRING;
    if (ty_is_numeric(ct) && ty_is_numeric(vt))
      return (ct == TY_FLOAT || vt == TY_FLOAT) ? TY_FLOAT : TY_INT;
    return ct != TY_UNKNOWN ? ct : vt;
  }
  if (nk == NK_ConstantReadNode) {
    const char *nm = nt_str(nt, id, "name");
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    /* a registered constant whose type never settled (e.g. an anonymous
       Struct class assignment) must not shadow the class-table fallbacks
       below -- Pt = Struct.new(:x) reads as the class value, not unknown */
    if (lv && lv->type != TY_UNKNOWN) return lv->type;
    /* `include Math` exposes bare PI/E as Float constants (#2600) */
    if (c->has_include_math && !lv && nm && (sp_streq(nm, "PI") || sp_streq(nm, "E")))
      return TY_FLOAT;
    if (nm && (sp_streq(nm, "RUBY_DESCRIPTION") || sp_streq(nm, "RUBY_VERSION") ||
               sp_streq(nm, "RUBY_PLATFORM") || sp_streq(nm, "RUBY_ENGINE") ||
               sp_streq(nm, "RUBY_ENGINE_VERSION") || sp_streq(nm, "RUBY_RELEASE_DATE") ||
               sp_streq(nm, "RUBY_REVISION") || sp_streq(nm, "RUBY_COPYRIGHT"))) return TY_STRING;
    if (nm && sp_streq(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && sp_streq(nm, "ARGF")) return TY_ARGF;
    /* STDOUT/STDERR/STDIN are IO handles wrapping the C standard streams, so
       puts/print/write/flush -- and gets/read/tty? for STDIN -- route through
       the existing TY_IO dispatch. */
    if (nm && (sp_streq(nm, "STDOUT") || sp_streq(nm, "STDERR") ||
               sp_streq(nm, "STDIN"))) return TY_IO;
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    return TY_UNKNOWN;
  }
  if (nk == NK_DefinedNode) return TY_STRING;  /* a label string, or nil (NULL) */
  if (nk == NK_NumberedReferenceReadNode) return TY_STRING;  /* $1..$9: capture, or nil (NULL) */
  if (nk == NK_BackReferenceReadNode) return TY_STRING;  /* $&/$`/$'/$~/$+: nullable string */
  if (nk == NK_ConstantPathNode) {
    /* M::CONST -> resolve by the final path component (constants register
       under their unqualified name) */
    const char *nm = nt_str(nt, id, "name");
    /* An ffi_const is parent-qualified; resolve it BEFORE the leaf-keyed
       plain-constant table, or a same-leaf plain constant in another module
       silently claims the reference (and its type). */
    {
      int fpar = nt_ref(nt, id, "parent");
      const char *fpty = fpar >= 0 ? nt_type(nt, fpar) : NULL;
      /* the qualifying module is the parent's LEAF name, so a nested path
         (Outer::CSql::TEXT) qualifies by CSql -- the same unqualified name
         the ffi decl registered under */
      const char *fpnm = (fpty && (sp_streq(fpty, "ConstantReadNode") ||
                                   sp_streq(fpty, "ConstantPathNode")))
                         ? nt_str(nt, fpar, "name") : NULL;
      if (fpnm && nm)
        for (int fci = 0; fci < c->n_ffi_consts; fci++)
          if (sp_streq(c->ffi_consts[fci].mod, fpnm) &&
              sp_streq(c->ffi_consts[fci].name, nm))
            return TY_INT;
    }
    LocalVar *lv = nm ? comp_const(c, nm) : NULL;
    if (lv) return lv->type;
    /* A top-level scoped constant `::Name` (no parent) names the same thing as
       the bare constant `Name`; resolve it as a class when it is one so is_a?,
       case/when, etc. treat `::Integer` exactly like `Integer` (#2683). */
    if (nm && nt_ref(nt, id, "parent") < 0 &&
        (comp_class_index(c, nm) >= 0 || is_builtin_class_name(nm)))
      return TY_CLASS;
    if (nm && sp_streq(nm, "ARGV")) return TY_STR_ARRAY;
    if (nm && sp_streq(nm, "ARGF")) return TY_ARGF;
    /* well-known module constants */
    int par_id = nt_ref(nt, id, "parent");
    /* a ::-scoped BUILTIN class (Math::DomainError, Process::Status) is a
       first-class Class value like its bare siblings (#2840) */
    {
      int qpar = par_id;
      const char *qpty = qpar >= 0 ? nt_type(nt, qpar) : NULL;
      const char *qpnm = (qpty && (sp_streq(qpty, "ConstantReadNode") ||
                                   sp_streq(qpty, "ConstantPathNode")))
                         ? nt_str(nt, qpar, "name") : NULL;
      if (qpnm && nm) {
        char qbuf[160];
        snprintf(qbuf, sizeof qbuf, "%s::%s", qpnm, nm);
        if (builtin_class_id(qbuf) != 0) return TY_CLASS;
      }
    }
    const char *par_ty = par_id >= 0 ? nt_type(nt, par_id) : NULL;
    /* the qualifying module is the parent's leaf name, so a nested / root
       path (`::Float::MAX`) qualifies by `Float` too -- match codegen's
       par_nmc, which already accepts a ConstantPathNode parent here */
    const char *par_nm = (par_ty && (sp_streq(par_ty, "ConstantReadNode") ||
                                     sp_streq(par_ty, "ConstantPathNode")))
                         ? nt_str(nt, par_id, "name") : NULL;
    if (par_nm && sp_streq(par_nm, "Float")) {
      if (nm && (sp_streq(nm, "MAX") || sp_streq(nm, "MIN") || sp_streq(nm, "EPSILON") ||
                 sp_streq(nm, "INFINITY") || sp_streq(nm, "NAN"))) return TY_FLOAT;
      /* DIG/MANT_DIG/RADIX and the exponent limits are Integer constants */
      if (nm && (sp_streq(nm, "DIG") || sp_streq(nm, "MANT_DIG") || sp_streq(nm, "RADIX") ||
                 sp_streq(nm, "MAX_EXP") || sp_streq(nm, "MIN_EXP") ||
                 sp_streq(nm, "MAX_10_EXP") || sp_streq(nm, "MIN_10_EXP"))) return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Math")) {
      if (nm && (sp_streq(nm, "PI") || sp_streq(nm, "E"))) return TY_FLOAT;
    }
    if (par_nm && sp_streq(par_nm, "Regexp")) {
      if (nm && (sp_streq(nm, "IGNORECASE") || sp_streq(nm, "EXTENDED") ||
                 sp_streq(nm, "MULTILINE"))) return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Encoding") && nm &&
        (sp_streq(nm, "UTF_8") || sp_streq(nm, "UTF8") || sp_streq(nm, "US_ASCII") ||
         sp_streq(nm, "ASCII") || sp_streq(nm, "ANSI_X3_4_1968") ||
         sp_streq(nm, "BINARY") || sp_streq(nm, "ASCII_8BIT")))
      return TY_POLY;  /* a boxed Encoding value */
    if (par_nm && sp_streq(par_nm, "File")) {
      if (nm && (sp_streq(nm, "SEPARATOR") || sp_streq(nm, "PATH_SEPARATOR") ||
                 sp_streq(nm, "ALT_SEPARATOR"))) return TY_STRING;
      if (nm && (sp_streq(nm, "RDONLY") || sp_streq(nm, "WRONLY") || sp_streq(nm, "RDWR") ||
                 sp_streq(nm, "CREAT") || sp_streq(nm, "EXCL") || sp_streq(nm, "TRUNC") ||
                 sp_streq(nm, "APPEND") || sp_streq(nm, "NONBLOCK") || sp_streq(nm, "BINARY") ||
                 sp_streq(nm, "LOCK_SH") || sp_streq(nm, "LOCK_EX") || sp_streq(nm, "LOCK_UN") ||
                 sp_streq(nm, "LOCK_NB")))
        return TY_INT;   /* the open(2)/flock(2) flag constants (#2788, #2808) */
    }
    if (par_nm && (sp_streq(par_nm, "IO") || sp_streq(par_nm, "File"))) {
      /* IO#seek whence constants (File inherits them from IO) */
      if (nm && (sp_streq(nm, "SEEK_SET") || sp_streq(nm, "SEEK_CUR") ||
                 sp_streq(nm, "SEEK_END"))) return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Process")) {
      /* clock ids (codegen emits their integer values) */
      if (nm && (sp_streq(nm, "CLOCK_MONOTONIC") || sp_streq(nm, "CLOCK_REALTIME") ||
                 sp_streq(nm, "CLOCK_PROCESS_CPUTIME_ID") || sp_streq(nm, "CLOCK_THREAD_CPUTIME_ID")))
        return TY_INT;
    }
    if (par_nm && sp_streq(par_nm, "Integer")) {
      if (nm && (sp_streq(nm, "MAX") || sp_streq(nm, "MIN"))) return TY_UNKNOWN; /* raises NameError */
    }
    if (nm && comp_class_index(c, nm) >= 0) return TY_CLASS;
    if (nm && is_builtin_class_name(nm)) return TY_CLASS;
    /* FFI const: Module::NAME -> int */
    if (par_nm && nm) {
      for (int fci = 0; fci < c->n_ffi_consts; fci++) {
        if (sp_streq(c->ffi_consts[fci].mod, par_nm) &&
            sp_streq(c->ffi_consts[fci].name, nm))
          return TY_INT;
      }
    }
    return TY_UNKNOWN;
  }
  if (nk == NK_SelfNode) {
    Scope *s = comp_scope_of(c, id);
    int self_cls = s->class_id;
    /* inside a class method, bare `self` is the Class object (#2443) */
    if (self_cls >= 0 && s->is_cmethod) return TY_CLASS;
    /* `self` inside an instance_eval/exec block is the rebound receiver. */
    if (self_cls < 0) self_cls = (an_ie_class_id >= 0) ? an_ie_class_id : ie_class_of(c, id);
    if (self_cls < 0) return TY_UNKNOWN;
    const char *cn = c->classes[self_cls].name;
    if (sp_streq(cn, "String"))  return TY_STRING;
    if (sp_streq(cn, "Integer")) return TY_INT;
    if (sp_streq(cn, "Float"))   return TY_FLOAT;
    if (sp_streq(cn, "Symbol"))  return TY_SYMBOL;
    if (sp_streq(cn, "TrueClass") || sp_streq(cn, "FalseClass") || sp_streq(cn, "NilClass")) return TY_BOOL;
    if (sp_streq(cn, "Array"))   return TY_POLY_ARRAY;
    if (sp_streq(cn, "Object"))  return TY_POLY;  /* dynamic: called on any receiver type */
    return ty_object(self_cls);
  }
  if (nk == NK_InstanceVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cls_id = (s->class_id >= 0) ? s->class_id : an_ie_class_id;
    if (cls_id < 0) cls_id = ie_class_of(c, id);
    if (cls_id < 0) cls_id = comp_class_index(c, "Toplevel");
    if (cls_id < 0) return TY_UNKNOWN;
    ClassInfo *ci = &c->classes[cls_id];
    int iv = nm ? comp_ivar_index(ci, nm) : -1;
    return iv >= 0 ? ci->ivar_types[iv] : TY_UNKNOWN;
  }
  if (nk == NK_ClassVariableReadNode) {
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s->class_id;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    if (cid < 0) return TY_UNKNOWN;
    int idx = nm ? comp_cvar_index(&c->classes[cid], nm) : -1;
    return idx >= 0 ? c->classes[cid].cvar_types[idx] : TY_UNKNOWN;
  }
  if (nk == NK_ClassVariableOperatorWriteNode || nk == NK_ClassVariableWriteNode ||
      nk == NK_ClassVariableOrWriteNode || nk == NK_ClassVariableAndWriteNode) {
    /* `@@x op= v` / `@@x = v` / `@@x ||= v` / `@@x &&= v` write the cvar and yield
       the stored value, which the codegen coerces to the cvar's slot type (poly
       when widened under promote) -- so the expression's type is the cvar's, not
       v's. (For a non-widened cvar this equals v's type, so default mode is
       unchanged.) */
    const char *nm = nt_str(nt, id, "name");
    Scope *s = comp_scope_of(c, id);
    int cid = s ? s->class_id : -1;
    if (cid < 0) cid = comp_class_index(c, "Toplevel");
    int idx = (cid >= 0 && nm) ? comp_cvar_index(&c->classes[cid], nm) : -1;
    if (idx >= 0) return c->classes[cid].cvar_types[idx];
    return infer_type(c, nt_ref(nt, id, "value"));
  }
  if (nk == NK_IndexOrWriteNode || nk == NK_IndexAndWriteNode ||
      nk == NK_IndexOperatorWriteNode) {
    /* all three yield the slot's (post-write) value, so the expression's type
       is the slot type -- op-write included (`a[i] += x` used as a value, e.g.
       the tail of a block whose proc result is consumed). */
    int recv = nt_ref(nt, id, "receiver");
    if (recv < 0) return TY_UNKNOWN;
    TyKind rt = infer_type(c, recv);
    if (ty_is_array(rt)) return ty_array_elem(rt);
    if (ty_is_hash(rt)) return ty_hash_val(rt);
    return TY_POLY;
  }
  if (nk == NK_ParenthesesNode) {
    int body = nt_ref(nt, id, "body");
    if (body < 0) return TY_NIL;
    int n = 0;
    const int *b = nt_arr(nt, body, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_StatementsNode) {
    int n = 0;
    const int *b = nt_arr(nt, id, "body", &n);
    return n > 0 ? infer_type(c, b[n - 1]) : TY_NIL;
  }
  if (nk == NK_CaseNode) {
    /* value = unify of each when's body; a missing else means a no-match
       falls through to nil */
    int nw = 0; const int *whens = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, whens[w], "statements");
      r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL);
    }
    if (else_c >= 0) { int st = nt_ref(nt, else_c, "statements"); r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL); }
    else r = ty_unify(r, TY_NIL);
    return r;
  }
  if (nk == NK_CaseMatchNode) {
    /* case X; in PATTERN; ... — value = unify of each arm's body (+ else). */
    int nw = 0; const int *conds = nt_arr(nt, id, "conditions", &nw);
    int else_c = nt_ref(nt, id, "else_clause");
    TyKind r = TY_UNKNOWN;
    for (int w = 0; w < nw; w++) {
      int st = nt_ref(nt, conds[w], "statements");
      r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL);
    }
    if (else_c >= 0) { int st = nt_ref(nt, else_c, "statements"); r = ty_unify(r, st >= 0 ? infer_type(c, st) : TY_NIL); }
    return r;
  }
  if (nk == NK_IfNode || nk == NK_UnlessNode) {
    int is_unless = nk == NK_UnlessNode;
    int then_b = nt_ref(nt, id, "statements");
    int else_b = nt_ref(nt, id, is_unless ? "else_clause" : "subsequent");
    /* a statically-answered defined? predicate types as its live arm alone
       (the dead arm is never emitted -- see the codegen folds) */
    int dpred = nt_ref(nt, id, "predicate");
    int df = comp_defined_guard_false(c, dpred);
    int dt = df ? 0 : comp_defined_guard_true(c, dpred);
    if (df || dt) {
      int take_then = is_unless ? df : dt;
      if (take_then) return then_b >= 0 ? infer_type(c, then_b) : TY_NIL;
      return else_b >= 0 ? infer_type(c, else_b) : TY_NIL;
    }
    TyKind tt = then_b >= 0 ? infer_type(c, then_b) : TY_NIL;
    TyKind et = else_b >= 0 ? infer_type(c, else_b) : TY_NIL;
    return ty_unify(tt, et);
  }
  if (nk == NK_ElseNode) {
    int s = nt_ref(nt, id, "statements");
    return s >= 0 ? infer_type(c, s) : TY_NIL;
  }
  if (nk == NK_ArrayNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) {
      /* An empty `[]` used as a whitelisted iterator's receiver must still
         dispatch (`[].each { }`): type it as an empty poly array. Elsewhere it
         stays UNKNOWN so `x = []; x << 1` can back-fill the element type and the
         non-block empty-literal folds keep working. */
      if (c->empty_arr_recv && id < c->node_cap && c->empty_arr_recv[id])
        return TY_POLY_ARRAY;
      return TY_UNKNOWN;  /* empty: element type comes from usage */
    }
    TyKind e = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      TyKind et = infer_type(c, els[k]);
      /* A nested container literal whose own type is still open (an empty
         `[]` / `{}` element) is a non-scalar value all the same: treat it as
         poly, exactly like the HashNode arm below does for `{}`. Otherwise
         UNKNOWN unifies away and `[[], 1]` collapses to an IntArray, whose
         emit pushes the nested array POINTER as an int element (silent
         garbage). */
      if (et == TY_UNKNOWN) {
        const char *ety = nt_type(nt, els[k]);
        if (ety && (sp_streq(ety, "ArrayNode") || sp_streq(ety, "HashNode") ||
                    sp_streq(ety, "KeywordHashNode")))
          et = TY_POLY;
      }
      e = ty_unify(e, et);
    }
    return ty_array_of(e);
  }
  if (nk == NK_HashNode || nk == NK_KeywordHashNode) {
    int n = 0;
    const int *els = nt_arr(nt, id, "elements", &n);
    if (n == 0) {
      /* a bare `{}` used directly as a hash block-method receiver dispatches as
         the STR_POLY hash (mirrors the empty-array-receiver mark) (#2336). */
      if (c->empty_hash_recv && id < c->node_cap && c->empty_hash_recv[id])
        return TY_STR_POLY_HASH;
      /* a bare `{}` passed as a user-method arg (an accumulator seed) is the
         widest hash, so the callee may write any key/value type (#2860). */
      if (c->empty_hash_arg && id < c->node_cap && c->empty_hash_arg[id])
        return TY_POLY_POLY_HASH;
      return TY_UNKNOWN;
    }
    TyKind kt = TY_UNKNOWN, vt = TY_UNKNOWN;
    for (int k = 0; k < n; k++) {
      const char *aty = nt_type(nt, els[k]);
      if (aty && sp_streq(aty, "AssocSplatNode")) {
        /* `{ **h, ... }`: merge the spread source's key/value types so the
           rebuilt literal keeps a concrete typed-hash variant instead of
           erasing to UNKNOWN. */
        int src = nt_ref(nt, els[k], "value");
        TyKind sh = src >= 0 ? infer_type(c, src) : TY_UNKNOWN;
        if (ty_is_hash(sh)) {
          kt = ty_unify(kt, ty_hash_key(sh));
          vt = ty_unify(vt, ty_hash_val(sh));
        } else if (sh == TY_POLY) {
          /* a poly spread source (a hash reached through a poly binding) merges
             at runtime into a fully-poly hash. */
          kt = ty_unify(kt, TY_POLY);
          vt = ty_unify(vt, TY_POLY);
        } else {
          return TY_UNKNOWN;  /* unresolved or non-hash splat */
        }
        continue;
      }
      if (!aty || !sp_streq(aty, "AssocNode")) return TY_UNKNOWN;
      kt = ty_unify(kt, infer_type(c, nt_ref(nt, els[k], "key")));
      int vnode = nt_ref(nt, els[k], "value");
      TyKind vt_elem = infer_type(c, vnode);
      /* A nested hash literal (even empty `{}`) is a non-scalar value; treat
         it as poly so the outer hash promotes to a poly-valued variant. */
      if (vt_elem == TY_UNKNOWN) {
        const char *vnode_ty = nt_type(nt, vnode);
        if (vnode_ty && (sp_streq(vnode_ty, "HashNode") || sp_streq(vnode_ty, "KeywordHashNode")))
          vt_elem = TY_POLY;
      }
      vt = ty_unify(vt, vt_elem);
    }
    /* symbol keys -> SymPolyHash (boxed values), regardless of value type */
    if (kt == TY_SYMBOL) return TY_SYM_POLY_HASH;
    TyKind hv = ty_hash_of(kt, vt);
    if (hv != TY_UNKNOWN) return hv;
    /* No scalar (key,val) variant: the value is poly-stored (a nested hash/
       array/object, or a mix). The key type still selects the hash variant --
       string keys stay a str-keyed poly hash rather than collapsing to a
       fully-poly-keyed one, so the literal matches a `Hash[String, untyped]`
       (StrPolyHash) parameter without a layout-mismatching pointer cast. */
    if (vt != TY_UNKNOWN) {
      if (kt == TY_STRING) return TY_STR_POLY_HASH;
      return TY_POLY_POLY_HASH;
    }
    return hv;
  }
  if (nk == NK_DefNode) return TY_SYMBOL;  /* `def` evaluates to :name */
  if (nk == NK_CallOrWriteNode || nk == NK_CallAndWriteNode) {
    /* `a.v ||= x` evaluates to the attribute's (assigned-or-existing) value:
       the backing ivar's type when the receiver class is known. */
    int recv = nt_ref(nt, id, "receiver");
    const char *attr = nt_str(nt, id, "name");
    TyKind rt2 = recv >= 0 ? infer_type(c, recv) : TY_UNKNOWN;
    if (attr && ty_is_object(rt2)) {
      int cid2 = ty_object_class(rt2);
      char ivn2[300]; snprintf(ivn2, sizeof ivn2, "@%s", attr);
      int ii2 = comp_ivar_index(&c->classes[cid2], ivn2);
      if (ii2 >= 0) return c->classes[cid2].ivar_types[ii2];
    }
    int v2 = nt_ref(nt, id, "value");
    return v2 >= 0 ? infer_type(c, v2) : TY_UNKNOWN;
  }
  if (nk == NK_NextNode) {
    /* `next v` produces the BLOCK's value: its type is v's type (nil when
       bare). Leaving it untyped made a yield whose block ends in `next v`
       infer nil, so the delivered value was discarded at the call site. */
    int nargs = nt_ref(nt, id, "arguments");
    int nvc = 0; const int *nv = nargs >= 0 ? nt_arr(nt, nargs, "arguments", &nvc) : NULL;
    if (nvc > 0) {
      const char *aty = nt_type(nt, nv[0]);
      /* `next *x` delivers the splat-built ARRAY (the SplatNode arm above
         answers with the ELEMENT type, for array-literal splices). */
      if (aty && sp_streq(aty, "SplatNode")) return TY_POLY_ARRAY;
      return infer_type(c, nv[0]);
    }
    return TY_NIL;
  }
  if (nk == NK_YieldNode) {
    int ymi = (int)(comp_scope_of(c, id) - c->scopes);
    /* When the block value diverges across call sites (string block at one,
       int at another) AND this yield is the value of an assignment (its result
       flows into a LOCAL), the local settles its type from the first site and
       the other site miscompiles into that slot. Type the yield poly so the
       local is a boxed carrier and each inlined site boxes its own value. A
       bare-yield tail is handled per-site by emit_block_invoke_coerced /
       method_call_ret and must keep its concrete first-site type. */
    if (yield_value_diverges(c, ymi)) {
      for (int w = 0; w < nt->count; w++) {
        NodeKind wk = nt_kind(nt, w);
        if ((wk == NK_LocalVariableWriteNode || wk == NK_LocalVariableOperatorWriteNode ||
             wk == NK_LocalVariableOrWriteNode || wk == NK_LocalVariableAndWriteNode) &&
            nt_ref(nt, w, "value") == id) return TY_POLY;
      }
    }
    return yield_value_type(c, ymi);
  }
  if (nk == NK_SuperNode || nk == NK_ForwardingSuperNode) {
    Scope *s = comp_scope_of(c, id);
    if (s->class_id < 0 || !s->name) return TY_UNKNOWN;
    const char *shadow = comp_prep_chain_target(c, s->class_id, s->name);
    if (shadow) {
      int mi = comp_method_in_class(c, s->class_id, shadow);
      return mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
    }
    const char *uname = comp_prep_user_name(s->name);
    int p = c->classes[s->class_id].parent;
    if (p < 0) return TY_UNKNOWN;
    /* super inside a class method resolves through the parent's CLASS-method
       chain (the instance chain would miss `def self.x` entirely). */
    int mi = s->is_cmethod ? comp_cmethod_in_chain(c, p, uname, NULL)
                           : comp_method_in_chain(c, p, uname, NULL);
    return mi >= 0 ? c->scopes[mi].ret : TY_UNKNOWN;
  }
  if (nk == NK_AndNode || nk == NK_OrNode) {
    TyKind lt = infer_type(c, nt_ref(nt, id, "left"));
    TyKind rt = infer_type(c, nt_ref(nt, id, "right"));
    if (lt == TY_BOOL && rt == TY_BOOL) return TY_BOOL;
    return ty_unify(lt, rt);  /* value form: a || b -> common type */
  }
  if (nk == NK_BeginNode) {
    /* value = body value unified with each rescue handler's value */
    int body = nt_ref(nt, id, "statements");
    TyKind r = body >= 0 ? infer_type(c, body) : TY_NIL;
    TyKind body_t = r;
    /* a body whose last statement is a bare raise diverges: the begin's value
       comes from the rescue arms alone, so their type must not widen (#2739) */
    if (body >= 0 && nt_ref(nt, id, "rescue_clause") >= 0) {
      int bn = 0; const int *bs = nt_arr(nt, body, "body", &bn);
      if (bn > 0 && bs) {
        const char *lt = nt_type(nt, bs[bn - 1]);
        const char *lnm = (lt && sp_streq(lt, "CallNode")) ? nt_str(nt, bs[bn - 1], "name") : NULL;
        if (lnm && (sp_streq(lnm, "raise") || sp_streq(lnm, "fail")) &&
            nt_ref(nt, bs[bn - 1], "receiver") < 0)
          r = TY_VOID;
      }
    }
    int have = !(r == TY_UNKNOWN || r == TY_VOID);
    for (int rs = nt_ref(nt, id, "rescue_clause"); rs >= 0; rs = nt_ref(nt, rs, "subsequent")) {
      int st = nt_ref(nt, rs, "statements");
      TyKind at = st >= 0 ? infer_type(c, st) : TY_NIL;
      if (at == TY_UNKNOWN || at == TY_VOID) continue;  /* this arm diverges too */
      r = have ? ty_unify(r, at) : at;
      have = 1;
    }
    return have ? r : body_t;
  }
  if (nk == NK_CallNode) return infer_call(c, id);

  if (nk == NK_RescueModifierNode) {
    int e = nt_ref(nt, id, "expression");
    int r = nt_ref(nt, id, "rescue_expression");
    TyKind et = e >= 0 ? infer_type(c, e) : TY_NIL;
    TyKind rt = r >= 0 ? infer_type(c, r) : TY_NIL;
    /* a diverging expression like raise has no real type; use the rescue arm's type */
    if (et == TY_UNKNOWN || et == TY_VOID || et == TY_NIL) return rt;
    return ty_unify(et, rt);
  }

  /* MultiWriteNode as expression: value is the RHS array. */
  if (nk == NK_MultiWriteNode)
    return infer_type(c, nt_ref(nt, id, "value"));

  return TY_UNKNOWN;
}

TyKind infer_type(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return TY_UNKNOWN;
  TyKind t = infer_uncached(c, id);
  c->ntype[id] = t;
  return t;
}

/* See analyze.h. Children first, so a parent whose inference reads child
   caches (comp_ntype) sees the refreshed values. */
void infer_subtree(Compiler *c, int id) {
  if (id < 0 || id >= c->nt->count) return;
  const NodeTable *nt = c->nt;
  const char *ty = nt_type(nt, id);
  if (!ty) return;
  if (sp_streq(ty, "DefNode") || sp_streq(ty, "ClassNode") || sp_streq(ty, "ModuleNode")) return;
  int nr = nt_num_refs(nt, id);
  for (int i = 0; i < nr; i++) infer_subtree(c, nt_ref_at(nt, id, i));
  int na = nt_num_arrs(nt, id);
  for (int i = 0; i < na; i++) {
    int n = 0; const int *ids = nt_arr_at(nt, id, i, &n);
    for (int k = 0; k < n; k++) infer_subtree(c, ids[k]);
  }
  infer_type(c, id);
}

/* ---- scope assignment ---- */

void scope_add_param(Scope *s, const char *name, int defnode) {
  if (s->nparams % 8 == 0) {
    s->pnames = realloc(s->pnames, sizeof(char *) * (size_t)(s->nparams + 8));
    s->pdefault = realloc(s->pdefault, sizeof(int) * (size_t)(s->nparams + 8));
  }
  s->pdefault[s->nparams] = defnode;
  s->pnames[s->nparams++] = strdup(name);
  if (defnode < 0) s->nrequired = s->nparams;
  LocalVar *lv = scope_local_intern(s, name);
  lv->is_param = 1;
}

/* Collect parameters from a DefNode into scope s. */
