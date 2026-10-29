#include "dvs_bbsolver.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "dvs_aig.h"
#include "dvs_affine.h"
#include "dvs_aigsim.h"
#include "dvs_aig_cnf.h"
#include "dvs_bitblast.h"
#include "dvs_bbsum.h"
#include "dvs_bbword.h"
#include "dvs_bvdom.h"
#include "dvs_pool.h"
#include "dvs_sat.h"
#include "dvs_sv.h"
#include "dvs_thread.h"
#include "dvs_stackinfo.h"
#include "dvs_components.h"

/* The per-kind blasters stay out of bb_expr, so a level of nesting costs
 * bb_expr's frame plus the one kind's, not the union of all of them. */
#if defined(_MSC_VER)
#define BB_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define BB_NOINLINE __attribute__((noinline))
#else
#define BB_NOINLINE
#endif

/* ----------------------------- types -------------------------------------- */

#define BB_INVALID_WIDTH 0

typedef struct {
    uint32_t width;
    uint8_t  is_signed;
    uint8_t  defined;   /* 0 if var_id not seen */
    int64_t  lo;
    int64_t  hi;
    dvs_bv_t bv;        /* lazily filled on first reference */
    int      bv_built;
    int      bounds_asserted; /* lo<=v<=hi already emitted (incremental reuse) */
} bb_var_t;

/* A slice substitution: bits [hi:lo] of `var` are defined as `eref`, from a
 * top-level (= (extract var hi lo) eref). Yosys `-stbv` output relates whole
 * state vectors this way: each register is a slice of the next state, set
 * from the current one. Chained per variable through `next`. */
typedef struct {
    dvs_expr_t eref;
    uint32_t   next;
    uint32_t   hi, lo;
} bb_slice_t;

#define BB_NO_SLICE UINT32_MAX

/* A session scope (dvs_bbsolver_sess_push). Its assertions are clauses
 * (-sel | a), live while `sel` is assumed; pop adds the unit -sel. */
typedef struct {
    dvs_aig_node_t sel;
    uint32_t       var_mark;    /* variables from here up belong to the scope */
    uint32_t       n_asserts;   /* assert_nodes at the push */
    uint32_t       n_slices;    /* slices at the push */
} bb_frame_t;

/* Memoization cache entry — one per dvs_expr_t byte offset in the pool. */
typedef struct {
    dvs_bv_t bv;      /* size == 0 marks "not yet bit-blasted" */
} bb_cache_entry_t;

struct dvs_bbsolver_s {
    dvs_alloc_t    *alloc;
    /* The expression walks below stop when the stack runs low: blasting
     * reports the problem unsupported (so another engine, or "unknown",
     * answers), the cycle checks answer "may cycle". */
    dvs_stack_guard_t sg;
    dvs_problem_t   *problem;
    /* The caller's problem when `problem` is an SV-elaborated copy we own
     * (dvs_sv_elaborate); NULL when `problem` is the caller's. */
    dvs_problem_t   *orig_problem;
    uint32_t        orig_synced;   /* orig_problem->pool.used already copied */
    dvs_aig_t      *aig;
    dvs_sat_t      *sat;
    dvs_aig_cnf_t  *cnf;
    dvs_bitblast_t *bb;
    dvs_bbsum_t    *sum;     /* word-level sum normalization (piece 5.1) */
    dvs_bbword_t   *word;    /* word-level facts: inverses, residues (5.4) */

    uint64_t        seed;    /* from dvs_bbsolver_check; 0 = deterministic */

    bb_var_t       *vars;
    uint32_t        n_vars;

    /* Equality-substitution map: subst[var_id] = dvs_expr_t of the RHS to
     * bit-blast in place of the variable, or EXPR_NULL if no substitution
     * applies. Populated during the preprocessing pass from top-level
     * (= var expr) sub-asserts (descending through DVS_BIN_AND chains).
     *
     * resolving[var_id] is a re-entrancy guard: if bb_var_expr() is called
     * for a var while already resolving its substitution, fall back to a
     * fresh BV. Protects against transitive cycles in the substitution
     * graph that the linear-scan acyclicity check might miss. */
    dvs_expr_t        *subst;       /* size n_vars, or NULL */
    uint8_t        *resolving;   /* size n_vars, or NULL */
    /* Visited stamps for subst_reaches_var: var v is visited when
     * reach_mark[v] == reach_epoch (no clearing per query). */
    uint32_t       *reach_mark;  /* size reach_cap, or NULL */
    uint32_t        reach_cap;
    uint32_t        reach_epoch;
    /* rhs_var[v]: v occurs in the right side of a recorded substitution or
     * slice. Only then can a new substitution's right side reach v through
     * the substitution graph, so subst_reaches_var runs only for those. */
    uint8_t        *rhs_var;     /* size n_vars, or NULL (always walk) */

    /* Slice substitutions (piece 5.1): slice_head[var_id] starts the var's
     * chain in slices[], BB_NO_SLICE if none. A var has slices or a whole
     * substitution, never both; its slices never overlap. */
    uint32_t       *slice_head;  /* size n_vars, or NULL */
    bb_slice_t     *slices;
    uint32_t        n_slices, cap_slices;

    /* Fanout of each expression node (saturating), with zero-extensions,
     * `concat` of a zero and substituted variables passed through to what
     * they wrap. A sum used once may be flattened into its user
     * (dvs_bbsum); one used more often is kept, so it stays shared. */
    uint8_t        *uses;        /* size uses_cap, or NULL */
    uint32_t        uses_cap;
    uint8_t        *fo_seen;     /* count_fanout's visited set, size uses_cap */

    /* Bitmap of constraint indices to skip (those consumed by substitution).
     * Indexed by enumeration order of constraints_head walk. */
    uint8_t        *constraint_skip;
    uint32_t        n_constraints;

    /* Independent components of the kept constraints (analysis only). */
    dvs_components_t comps;
    uint32_t        *cons_comp;   /* constraint -> comp (UINT32_MAX: none/skipped) */
    uint32_t         ladder_enum_zero; /* claimed comps with 0 solutions (stats) */

    uint64_t        n_substs;    /* statistics */

    /* Memoization of bit-blast results by dvs_expr_t. Sparse array indexed
     * by dvs_expr_t (byte offset into the pool). EXPR_CONST nodes are not
     * memoized because their result depends on the caller's hint_width;
     * every other node has a natural width that doesn't depend on hint
     * (variables ignore hint, ops widen to max(operands)). */
    bb_cache_entry_t *cache;
    uint32_t          cache_cap;

    int             last_result;
    int             had_error;
    /* Set when a construct is recognized but cannot be soundly bit-blasted
     * (e.g. signed div/mod — see A-4 soundness guards). Distinct from
     * had_error: maps to DVS_BB_UNKNOWN so the caller defers to its fallback
     * rather than treating the result as a real verdict. */
    int             had_unsupported;
    /* had_unsupported because an expression nests deeper than the stack. */
    int             too_deep;

    /* DSE-2 soft-aware serve. soft_keep[i] (indexed by softs_head walk order)
     * selects which soft constraints to encode as *hard* top-level assertions
     * for this check. NULL (the default) honors no soft — the legacy soft-less
     * behavior. The MaxSAT wrapper (dvs_bbsolver_solve_maxsat) sets it to the
     * maximal priority-respecting kept set. */
    const uint8_t  *soft_keep;
    uint32_t        n_softs;

    /* Diversity flip-check (post-solve). assert_nodes collects every top-level
     * asserted AIG node (hard constraints, kept softs, var bounds) so the
     * flip-check can re-verify them after tentatively flipping a bit. node_val
     * is the AIG node valuation after the flip-check pass (NULL if it did not
     * run, i.e. seed 0); the readback reads variable bits from it. */
    dvs_aig_node_t *assert_nodes;
    uint32_t        n_asserts;
    uint32_t        asserts_cap;
    int8_t         *node_val;
    /* _diversify_sweeps' AND children (L/R) and fanout CSR (fo_off/fo_adj), built
     * for the first div_n nodes. The AIG is append-only, so they stay valid until
     * the node count changes. */
    dvs_aig_node_t *div_L, *div_R;
    uint32_t       *div_fo_off, *div_fo_adj;
    uint32_t        div_n;

    /* DV_BB_NO_MEMO, read once at construction: getenv per blasted node was
     * ~7% of an enumeration-heavy Verilator run. */
    int             no_memo;
    /* Skip the post-solve flip-check (dvs_bbsolver_set_diversify). */
    int             no_diversify;
    /* Simulate before SAT (dvs_bbsolver_set_presim). */
    int             presim;
    /* Record the clauses for dvs_bbsolver_resample (dvs_bbsolver_set_resample). */
    int             resample;
    /* Rediversify calls over the current base model; every DVS_REDIV_EPOCH
     * of them draws a fresh base (dvs_bbsolver_rediversify). */
    uint32_t        redivs_since_base;
    /* The last check's model came from simulation (exhaustive or random). */
    int             base_sim;
    /* The last SAT verdict's model is the assignment of S->sat. Clear when it
     * came from simulation, the affine check or a portfolio/cube worker: then
     * node_val is the model and S->sat's assignment is stale or absent. */
    int             model_in_sat;
    /* An assertion could not be recorded in assert_nodes (out of memory), so
     * the list is not the whole problem: no simulation verdicts. */
    int             asserts_lost;
    /* Popcount sampler (_popsamp): per-assert R-only k masks, and its tries
     * and hits over the instance's life (it turns itself off when it rarely
     * answers). */
    struct bb_psk  *psk;
    uint32_t        n_psk, cap_psk;
    uint32_t        ps_tries, ps_hits;
    /* Unique sampler (_uniqsamp): tries and hits, as for _popsamp. */
    uint32_t        us_tries, us_hits;
    /* Sorted sampler (_sortsamp): tries and hits, as for _popsamp. */
    uint32_t        ss_tries, ss_hits;
    /* Div/mul sampler (_divsamp): tries and hits, as for _popsamp. */
    uint32_t        ds_tries, ds_hits;

    /* Session mode (dvs_bbsolver_sess_*): constraints arrive in batches, each
     * at the depth of its scope, and the instance outlives check-sats. */
    int             sess;
    bb_frame_t     *frames;
    uint32_t        n_frames, cap_frames;
    /* A substitution may define only variables from sub_floor up: those of
     * the innermost scope, which go when it does. */
    uint32_t        sub_floor;
    /* Variables substituted in the current batch (their bits are encoded so
     * the model reads them). */
    uint32_t       *new_subst;
    uint32_t        n_new_subst, cap_new_subst;
    /* Variable-list entries bb_grow_vars has seen; a pop sets refill. */
    uint32_t        seen_vars;
    int             refill;
    /* assert_top guards its assertion with the selector of scope
     * assert_frame - 1 (0: none), and tags it in assert_tags. */
    uint32_t        assert_frame;
    uint32_t       *assert_tags;
    uint32_t        tags_cap;
    /* The source expression of each assert_nodes entry (EXPR_NULL: none
     * known), for _popsamp; NULL once it could not grow. */
    dvs_expr_t     *assert_roots;
    uint32_t        roots_cap;
    int             roots_lost;
    /* dvs_bbsolver_sample has encoded the problem (it encodes once and then
     * only adds parity planes, which it assumes rather than asserts). */
    int             sample_ready;
    uint64_t        sample_base_ands;   /* AIG size after that encode */
    /* Work done outside the SAT solver, in AIG node visits: the diversity
     * pass and pre-solve simulation. Deterministic, for engine routing
     * (dvs_bbsolver_work). */
    uint64_t        aux_work;
};

/* ----------------------------- helpers ------------------------------------ */

static void *xalloc(dvs_alloc_t *a, size_t sz) {
    return a ? DVS_ALLOC(a, sz) : malloc(sz);
}
static void xfree(dvs_alloc_t *a, void *p, size_t sz) {
    if (a) DVS_RELEASE(a, p, sz); else free(p);
}

/* Encode `node` as a hard top-level assertion AND remember it, so the diversity
 * flip-check can re-verify every assertion after a tentative bit flip. */
static void assert_top_r(dvs_bbsolver_t *S, dvs_aig_node_t node, dvs_expr_t root);

static void assert_top(dvs_bbsolver_t *S, dvs_aig_node_t node) {
    assert_top_r(S, node, EXPR_NULL);
}

/* assert_top of the predicate of expression `root`. */
static void assert_top_r(dvs_bbsolver_t *S, dvs_aig_node_t node, dvs_expr_t root) {
    if (S->assert_frame) {
        /* Inside a session scope: the clause (-sel | node), live while sel is
         * assumed. Encoding TRUE first gives FALSE its variable. */
        dvs_aig_node_t sel = S->frames[S->assert_frame - 1].sel;
        dvs_aig_cnf_encode(S->cnf, DVS_AIG_TRUE, /*top_level=*/0);
        dvs_aig_cnf_encode(S->cnf, node, /*top_level=*/0);
        dvs_sat_add_binary(S->sat, (dvs_sat_lit_t)-sel, (dvs_sat_lit_t)node);
    } else {
        dvs_aig_cnf_encode(S->cnf, node, /*top_level=*/1);
    }
    if (S->sess && !S->asserts_lost) {
        /* Which scope owns each entry, so a pop keeps the others. */
        if (S->n_asserts == S->tags_cap) {
            uint32_t nc = S->tags_cap ? 2 * S->tags_cap : 64;
            uint32_t *t = (uint32_t *)realloc(S->assert_tags, nc * sizeof(*t));
            if (!t) { S->asserts_lost = 1; return; }
            S->assert_tags = t; S->tags_cap = nc;
        }
        S->assert_tags[S->n_asserts] = S->assert_frame;
    }
    if (S->n_asserts == S->asserts_cap) {
        uint32_t nc = S->asserts_cap ? S->asserts_cap * 2 : 64;
        dvs_aig_node_t *na = (dvs_aig_node_t *)xalloc(S->alloc,
                                                      nc * sizeof(dvs_aig_node_t));
        if (!na) { S->asserts_lost = 1; return; }   /* flip-check and presim no-op */
        if (S->assert_nodes) {
            memcpy(na, S->assert_nodes, S->n_asserts * sizeof(dvs_aig_node_t));
            xfree(S->alloc, S->assert_nodes, S->asserts_cap * sizeof(dvs_aig_node_t));
        }
        S->assert_nodes = na;
        S->asserts_cap = nc;
    }
    if (!S->roots_lost && S->n_asserts == S->roots_cap) {
        uint32_t nc = S->roots_cap ? 2 * S->roots_cap : 64;
        dvs_expr_t *t = (dvs_expr_t *)realloc(S->assert_roots, nc * sizeof(*t));
        if (t) { S->assert_roots = t; S->roots_cap = nc; }
        else   { free(S->assert_roots); S->assert_roots = NULL; S->roots_lost = 1; }
    }
    if (!S->roots_lost) S->assert_roots[S->n_asserts] = root;
    S->assert_nodes[S->n_asserts++] = node;
}

static uint32_t max_w(uint32_t a, uint32_t b) { return a > b ? a : b; }

/* Self-determined signedness of `ref` under the SystemVerilog typing rules
 * (dvs_sv.h): a variable / sized constant has its own; arithmetic and bitwise
 * ops are signed only when both operands are; a shift takes its left operand's;
 * comparisons and connectives are unsigned 1-bit; an extend keeps its operand's;
 * an SV cast names its own; extract / concat are unsigned.
 *
 * The problem is SV-elaborated before it is bit-blasted, so every operator's
 * operands already share one signedness -- this is what the operator works
 * in, and what a narrower operand is extended by. (An SMT-LIB problem has no
 * signed leaves at all, so everything there is unsigned, as before.) */
static int bb_signed(dvs_bbsolver_t *S, dvs_expr_t ref, int depth) {
    if (ref == EXPR_NULL) return 0;
    /* Too deep to tell: guessing would extend by the wrong signedness. */
    if (dvs_stack_deep(&S->sg, depth, 4096)) { S->had_unsupported = 1; return 0; }
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_CONST: {
        uint32_t w; uint8_t sg;
        dvs_sv_const_type((ExprConst *)kp, &w, &sg);
        return sg;
    }
    case EXPR_CONST_WIDE:
        return ((ExprConstWide *)kp)->is_signed != 0;
    case EXPR_VAR: {
        uint32_t id = ((ExprVar *)kp)->var_id;
        return id < S->n_vars && S->vars[id].is_signed;
    }
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)kp;
        switch (b->op) {
        case DVS_BIN_EQ: case DVS_BIN_NEQ:
        case DVS_BIN_LT: case DVS_BIN_LTE: case DVS_BIN_GT: case DVS_BIN_GTE:
        case DVS_BIN_AND: case DVS_BIN_OR:
            return 0;
        case DVS_BIN_LSHIFT: case DVS_BIN_RSHIFT: case DVS_BIN_ASHR:
            return bb_signed(S, b->lhs, depth + 1);
        default:
            return bb_signed(S, b->lhs, depth + 1)
                && bb_signed(S, b->rhs, depth + 1);
        }
    }
    case EXPR_UNARY: {
        ExprUnary *u = (ExprUnary *)kp;
        if (u->op == DVS_UN_NOT) return 0;
        return bb_signed(S, u->operand, depth + 1);
    }
    case EXPR_ITE: {
        ExprITE *e = (ExprITE *)kp;
        return bb_signed(S, e->then_e, depth + 1)
            && bb_signed(S, e->else_e, depth + 1);
    }
    case EXPR_EXTEND:
        return bb_signed(S, ((ExprExtend *)kp)->operand, depth + 1);
    case EXPR_SV_CAST:
        return ((ExprSvCast *)kp)->dst_signed != 0;
    default:
        return 0;
    }
}

/* Self-determined width of `ref` (0 = an unsized constant, which adapts). */
static uint32_t bb_self_width(dvs_bbsolver_t *S, dvs_expr_t ref, int depth) {
    if (ref == EXPR_NULL) return 0;
    if (dvs_stack_deep(&S->sg, depth, 4096)) { S->had_unsupported = 1; return 0; }
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_CONST: return ((ExprConst *)kp)->width;
    case EXPR_CONST_WIDE: return (uint32_t)((ExprConstWide *)kp)->width;
    case EXPR_VAR: {
        uint32_t id = ((ExprVar *)kp)->var_id;
        return id < S->n_vars ? S->vars[id].width : 0;
    }
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)kp;
        switch (b->op) {
        case DVS_BIN_EQ: case DVS_BIN_NEQ:
        case DVS_BIN_LT: case DVS_BIN_LTE: case DVS_BIN_GT: case DVS_BIN_GTE:
        case DVS_BIN_AND: case DVS_BIN_OR:
            return 1;
        case DVS_BIN_LSHIFT: case DVS_BIN_RSHIFT: case DVS_BIN_ASHR:
            return bb_self_width(S, b->lhs, depth + 1);
        default: {
            uint32_t l = bb_self_width(S, b->lhs, depth + 1);
            uint32_t r = bb_self_width(S, b->rhs, depth + 1);
            return l > r ? l : r;
        }
        }
    }
    case EXPR_UNARY: {
        ExprUnary *u = (ExprUnary *)kp;
        if (u->op == DVS_UN_NOT) return 1;
        return bb_self_width(S, u->operand, depth + 1);
    }
    case EXPR_ITE: {
        ExprITE *e = (ExprITE *)kp;
        uint32_t t = bb_self_width(S, e->then_e, depth + 1);
        uint32_t f = bb_self_width(S, e->else_e, depth + 1);
        return t > f ? t : f;
    }
    case EXPR_EXTEND: return ((ExprExtend *)kp)->to_bits;
    case EXPR_SV_CAST: return ((ExprSvCast *)kp)->to_bits;
    case EXPR_EXTRACT: {
        ExprExtract *x = (ExprExtract *)kp;
        return (uint32_t)(x->hi_bit - x->lo_bit + 1);
    }
    case EXPR_CONCAT: {
        ExprConcat *c = (ExprConcat *)kp;
        uint32_t h = bb_self_width(S, c->hi, depth + 1);
        return h ? (uint32_t)(h + c->lo_width) : 0;
    }
    default:
        return 1;
    }
}

static dvs_bv_t zext_to(dvs_bbsolver_t *S, dvs_bv_t v, uint32_t target) {
    if (v.size >= target) return v;
    return dvs_bb_zero_ext(S->bb, v, (uint32_t)(target - v.size));
}

static dvs_bv_t sext_to(dvs_bbsolver_t *S, dvs_bv_t v, uint32_t target) {
    if (v.size >= target) return v;
    return dvs_bb_sign_ext(S->bb, v, (uint32_t)(target - v.size));
}

/* Extend the value of `ref` (already blasted to `v`) to `target` bits by
 * ref's OWN signedness -- the SV rule for an operand of a wider context. */
static dvs_bv_t ext_ref(dvs_bbsolver_t *S, dvs_bv_t v, dvs_expr_t ref,
                        uint32_t target) {
    if (v.size >= target) return v;
    return bb_signed(S, ref, 0) ? sext_to(S, v, target) : zext_to(S, v, target);
}

/* Forward decl — needed because bv_for_var may recurse through bb_expr. */
static dvs_bv_t bb_expr(dvs_bbsolver_t *S, dvs_expr_t ref, uint32_t hint_width);
static dvs_bv_t bb_popcount(dvs_bbsolver_t *S, dvs_expr_t operand, uint32_t W);

/* True iff `ref`'s subtree (transitively following the current subst[]
 * map for any EXPR_VAR encountered) reaches EXPR_VAR with id `target_var`.
 *
 * The transitive walk through subst[] is what makes this sound under the
 * chained-substitution scenario: x := f(y), y := g(x) would slip past
 * a direct reference check (neither RHS mentions the LHS directly), but
 * following subst[y] from the f(y) branch reaches x.
 *
 * `visited` (S->reach_mark, stamped S->reach_epoch) avoids revisiting (and infinite
 * recursion through pre-existing cycles in subst, although those
 * shouldn't exist if every prior insertion was guarded by this check). */
static int subst_reaches_var(dvs_bbsolver_t *S,
                             dvs_expr_t ref,
                             uint32_t target_var,
                             uint32_t *visited);

/* subst_reaches_var for a variable named by id: an EXPR_VAR, or an element
 * of an array select (which names its elements by id). */
static int reach_var_id(dvs_bbsolver_t *S, uint32_t id, uint32_t target_var,
                        uint32_t *visited) {
    if (id == target_var) return 1;
    if (id >= S->n_vars) return 0;
    if (visited[id] == S->reach_epoch) return 0;
    visited[id] = S->reach_epoch;
    /* A built variable's bits are fixed: bit-blasting never resolves
     * through its substitution again, so no cycle passes through it.
     * (Only a session builds variables between substitution passes.) */
    if (S->vars[id].bv_built) return 0;
    /* If this variable has its own substitution, look through it. */
    if (S->subst[id] != EXPR_NULL)
        return subst_reaches_var(S, S->subst[id], target_var, visited);
    if (S->slice_head)
        for (uint32_t i = S->slice_head[id]; i != BB_NO_SLICE; i = S->slices[i].next)
            if (subst_reaches_var(S, S->slices[i].eref, target_var, visited))
                return 1;
    return 0;
}

static int subst_reaches_var(dvs_bbsolver_t *S,
                             dvs_expr_t ref,
                             uint32_t target_var,
                             uint32_t *visited) {
    if (ref == EXPR_NULL) return 0;
    if (dvs_stack_low(&S->sg)) return 1;   /* cannot rule a cycle out */
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_CONST: return 0;
    case EXPR_VAR:
        return reach_var_id(S, ((ExprVar *)kp)->var_id, target_var, visited);
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)kp;
        return subst_reaches_var(S, b->lhs, target_var, visited)
            || subst_reaches_var(S, b->rhs, target_var, visited);
    }
    case EXPR_UNARY:
        return subst_reaches_var(S, ((ExprUnary *)kp)->operand, target_var, visited);
    case EXPR_ITE: {
        ExprITE *e = (ExprITE *)kp;
        return subst_reaches_var(S, e->cond, target_var, visited)
            || subst_reaches_var(S, e->then_e, target_var, visited)
            || subst_reaches_var(S, e->else_e, target_var, visited);
    }
    case EXPR_IN_RANGE: {
        ExprInRange *r = (ExprInRange *)kp;
        return subst_reaches_var(S, r->value, target_var, visited)
            || subst_reaches_var(S, r->lo, target_var, visited)
            || subst_reaches_var(S, r->hi, target_var, visited);
    }
    case EXPR_IN_SET: {
        ExprInSet *iset = (ExprInSet *)kp;
        if (subst_reaches_var(S, iset->value, target_var, visited)) return 1;
        dvs_expr_t *elems = expr_in_set_elems(S->problem, ref);
        for (uint32_t i = 0; i < iset->n_elems; i++)
            if (subst_reaches_var(S, elems[i], target_var, visited)) return 1;
        return 0;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *irs = (ExprInRanges *)kp;
        if (subst_reaches_var(S, irs->value, target_var, visited)) return 1;
        dvs_expr_t *los = expr_in_ranges_los(S->problem, ref);
        dvs_expr_t *his = expr_in_ranges_his(S->problem, ref);
        for (uint32_t i = 0; i < irs->n_ranges; i++)
            if (subst_reaches_var(S, los[i], target_var, visited) ||
                subst_reaches_var(S, his[i], target_var, visited)) return 1;
        return 0;
    }
    case EXPR_EXTEND:
        return subst_reaches_var(S, ((ExprExtend *)kp)->operand, target_var, visited);
    case EXPR_SV_CAST:
        return subst_reaches_var(S, ((ExprSvCast *)kp)->operand, target_var, visited);
    case EXPR_EXTRACT:
        return subst_reaches_var(S, ((ExprExtract *)kp)->operand, target_var, visited);
    case EXPR_CONCAT: {
        ExprConcat *c = (ExprConcat *)kp;
        return subst_reaches_var(S, c->hi, target_var, visited)
            || subst_reaches_var(S, c->lo, target_var, visited);
    }
    case EXPR_SUM: {
        const ExprSum *es = (const ExprSum *)kp;
        const dvs_expr_t *xs = (const dvs_expr_t *)(es + 1);
        if (subst_reaches_var(S, es->result, target_var, visited)) return 1;
        for (uint32_t i = 0; i < es->n_vars; i++)
            if (subst_reaches_var(S, xs[i], target_var, visited)) return 1;
        return 0;
    }
    case EXPR_COUNTONES: {
        const ExprCountones *ec = (const ExprCountones *)kp;
        return subst_reaches_var(S, ec->result, target_var, visited)
            || subst_reaches_var(S, ec->operand, target_var, visited);
    }
    case EXPR_CLOG2: {
        const ExprClog2 *ec = (const ExprClog2 *)kp;
        return subst_reaches_var(S, ec->result, target_var, visited)
            || subst_reaches_var(S, ec->operand, target_var, visited);
    }
    case EXPR_ARRAY_SELECT: {
        const ExprArraySelect *as = (const ExprArraySelect *)kp;
        if (subst_reaches_var(S, as->result, target_var, visited) ||
            subst_reaches_var(S, as->index, target_var, visited)) return 1;
        for (uint32_t i = 0; i < as->n_elems; i++)
            if (reach_var_id(S, as->base_var_id + i, target_var, visited)) return 1;
        return 0;
    }
    default: return 0;
    }
}

/* Start a subst_reaches_var query: a fresh epoch over n_vars stamps.
 * Returns S->reach_mark, or NULL when it cannot be allocated. */
static uint32_t *reach_begin(dvs_bbsolver_t *S) {
    if (S->reach_cap < S->n_vars) {
        uint32_t *m = (uint32_t *)calloc(S->n_vars, sizeof(uint32_t));
        if (!m) return NULL;
        free(S->reach_mark);
        S->reach_mark = m;
        S->reach_cap = S->n_vars;
        S->reach_epoch = 0;
    }
    if (++S->reach_epoch == 0) {
        memset(S->reach_mark, 0, S->reach_cap * sizeof(uint32_t));
        S->reach_epoch = 1;
    }
    return S->reach_mark;
}

/* Walk `ref` down to its variables, not looking through substitutions. With
 * `flag`, mark each in rhs_var and return 0; otherwise return whether
 * `target` occurs. */
static int direct_var_id(dvs_bbsolver_t *S, uint32_t id, uint32_t target, int flag) {
    if (!flag) return id == target;
    if (S->rhs_var && id < S->n_vars) S->rhs_var[id] = 1;
    return 0;
}

static int expr_direct_vars(dvs_bbsolver_t *S, dvs_expr_t ref, uint32_t target,
                            int flag) {
    if (ref == EXPR_NULL) return 0;
    if (dvs_stack_low(&S->sg)) {
        /* Cannot finish: a query answers "occurs"; a marking walk leaves
         * rhs_var incomplete, so drop it (subst_cycles then always walks). */
        if (!flag) return 1;
        free(S->rhs_var);
        S->rhs_var = NULL;
        return 0;
    }
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_VAR:
        return direct_var_id(S, ((ExprVar *)kp)->var_id, target, flag);
    case EXPR_BINARY: {
        ExprBinary *b = (ExprBinary *)kp;
        return expr_direct_vars(S, b->lhs, target, flag)
            || expr_direct_vars(S, b->rhs, target, flag);
    }
    case EXPR_UNARY:
        return expr_direct_vars(S, ((ExprUnary *)kp)->operand, target, flag);
    case EXPR_ITE: {
        ExprITE *e = (ExprITE *)kp;
        return expr_direct_vars(S, e->cond, target, flag)
            || expr_direct_vars(S, e->then_e, target, flag)
            || expr_direct_vars(S, e->else_e, target, flag);
    }
    case EXPR_IN_RANGE: {
        ExprInRange *r = (ExprInRange *)kp;
        return expr_direct_vars(S, r->value, target, flag)
            || expr_direct_vars(S, r->lo, target, flag)
            || expr_direct_vars(S, r->hi, target, flag);
    }
    case EXPR_IN_SET: {
        ExprInSet *iset = (ExprInSet *)kp;
        if (expr_direct_vars(S, iset->value, target, flag)) return 1;
        dvs_expr_t *elems = expr_in_set_elems(S->problem, ref);
        for (uint32_t i = 0; i < iset->n_elems; i++)
            if (expr_direct_vars(S, elems[i], target, flag)) return 1;
        return 0;
    }
    case EXPR_IN_RANGES: {
        ExprInRanges *irs = (ExprInRanges *)kp;
        if (expr_direct_vars(S, irs->value, target, flag)) return 1;
        dvs_expr_t *los = expr_in_ranges_los(S->problem, ref);
        dvs_expr_t *his = expr_in_ranges_his(S->problem, ref);
        for (uint32_t i = 0; i < irs->n_ranges; i++)
            if (expr_direct_vars(S, los[i], target, flag) ||
                expr_direct_vars(S, his[i], target, flag)) return 1;
        return 0;
    }
    case EXPR_EXTEND:
        return expr_direct_vars(S, ((ExprExtend *)kp)->operand, target, flag);
    case EXPR_SV_CAST:
        return expr_direct_vars(S, ((ExprSvCast *)kp)->operand, target, flag);
    case EXPR_EXTRACT:
        return expr_direct_vars(S, ((ExprExtract *)kp)->operand, target, flag);
    case EXPR_CONCAT: {
        ExprConcat *c = (ExprConcat *)kp;
        return expr_direct_vars(S, c->hi, target, flag)
            || expr_direct_vars(S, c->lo, target, flag);
    }
    case EXPR_SUM: {
        const ExprSum *es = (const ExprSum *)kp;
        const dvs_expr_t *xs = (const dvs_expr_t *)(es + 1);
        if (expr_direct_vars(S, es->result, target, flag)) return 1;
        for (uint32_t i = 0; i < es->n_vars; i++)
            if (expr_direct_vars(S, xs[i], target, flag)) return 1;
        return 0;
    }
    case EXPR_COUNTONES: {
        const ExprCountones *ec = (const ExprCountones *)kp;
        return expr_direct_vars(S, ec->result, target, flag)
            || expr_direct_vars(S, ec->operand, target, flag);
    }
    case EXPR_CLOG2: {
        const ExprClog2 *ec = (const ExprClog2 *)kp;
        return expr_direct_vars(S, ec->result, target, flag)
            || expr_direct_vars(S, ec->operand, target, flag);
    }
    case EXPR_ARRAY_SELECT: {
        const ExprArraySelect *as = (const ExprArraySelect *)kp;
        if (expr_direct_vars(S, as->result, target, flag) ||
            expr_direct_vars(S, as->index, target, flag)) return 1;
        for (uint32_t i = 0; i < as->n_elems; i++)
            if (direct_var_id(S, as->base_var_id + i, target, flag)) return 1;
        return 0;
    }
    default: return 0;
    }
}

/* Would var := eref close a cycle in the substitution graph? Everything
 * eref reaches is a variable of eref itself or of some recorded right side,
 * so a variable in no right side is reached only by occurring in eref. */
static int subst_cycles(dvs_bbsolver_t *S, dvs_expr_t eref, uint32_t var) {
    if (S->rhs_var && !S->rhs_var[var]) return expr_direct_vars(S, eref, var, 0);
    uint32_t *mark = reach_begin(S);
    return !mark || subst_reaches_var(S, eref, var, mark);
}

/* May a substitution define `var`? Not once its bits are built (a session's
 * earlier batch referenced it), nor below the innermost scope's variables. */
static int subst_definable(const dvs_bbsolver_t *S, uint32_t var) {
    return var >= S->sub_floor && !S->vars[var].bv_built;
}

/* Bookkeeping for a recorded substitution of `var` by `eref`. */
static void note_subst(dvs_bbsolver_t *S, dvs_expr_t eref, uint32_t var) {
    if (S->rhs_var) expr_direct_vars(S, eref, 0, 1);
    if (!S->sess) return;
    if (S->n_new_subst == S->cap_new_subst) {
        uint32_t c = S->cap_new_subst ? 2 * S->cap_new_subst : 256;
        uint32_t *p = (uint32_t *)realloc(S->new_subst, c * sizeof(*p));
        if (!p) { S->had_error = 1; return; }
        S->new_subst = p; S->cap_new_subst = c;
    }
    S->new_subst[S->n_new_subst++] = var;
}

/* Attempt to register a substitution var(vref) := eref. Returns 1 if
 * a new substitution was recorded. Rejects if eref transitively
 * (through the current subst graph) reaches the var being substituted —
 * this is the soundness gate. */
static int try_record_subst(dvs_bbsolver_t *S, dvs_expr_t vref, dvs_expr_t eref) {
    ExprKind *vk = (ExprKind *)POOL_PTR(S->problem, vref);
    if (!vk || *vk != EXPR_VAR) return 0;
    ExprVar *v = (ExprVar *)vk;
    if (v->var_id >= S->n_vars) return 0;
    if (!subst_definable(S, v->var_id)) return 0;
    if (S->subst[v->var_id] != EXPR_NULL) return 0;
    if (S->slice_head && S->slice_head[v->var_id] != BB_NO_SLICE) return 0;
    /* `v == e` DEFINES v only when e is no wider than v (v then equals e
     * extended by e's own signedness). A wider e makes it a comparison in e's
     * width -- v is extended, e's high bits must match -- not a definition. */
    if (bb_self_width(S, eref, 0) > S->vars[v->var_id].width) return 0;
    /* Transitive cycle check: would substituting create a cycle in the
     * subst graph reachable from this var? */
    if (subst_cycles(S, eref, v->var_id)) return 0;
    S->subst[v->var_id] = eref;
    S->n_substs++;
    note_subst(S, eref, v->var_id);
    return 1;
}

/* `ref`, looking through whole substitutions, as (extract var hi lo) of an
 * unsubstituted variable. */
static int as_var_slice(dvs_bbsolver_t *S, dvs_expr_t ref,
                        uint32_t *var, uint32_t *hi, uint32_t *lo) {
    for (int guard = 0; guard < 64 && ref != EXPR_NULL; guard++) {
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
        if (!kp) return 0;
        if (*kp == EXPR_VAR) {
            uint32_t id = ((ExprVar *)kp)->var_id;
            if (id >= S->n_vars || S->subst[id] == EXPR_NULL) return 0;
            ref = S->subst[id];
            continue;
        }
        if (*kp != EXPR_EXTRACT) return 0;
        const ExprExtract *x = (const ExprExtract *)kp;
        ExprKind *ok = (ExprKind *)POOL_PTR(S->problem, x->operand);
        if (!ok || *ok != EXPR_VAR) return 0;
        uint32_t id = ((ExprVar *)ok)->var_id;
        if (id >= S->n_vars || !S->vars[id].defined) return 0;
        if (S->subst[id] != EXPR_NULL) return 0;
        if (x->hi_bit < x->lo_bit || x->hi_bit >= S->vars[id].width) return 0;
        *var = id; *hi = x->hi_bit; *lo = x->lo_bit;
        return 1;
    }
    return 0;
}

/* Attempt to register a slice substitution: `sref` is (extract var hi lo)
 * and bits [hi:lo] of var := eref. The same gates as a whole substitution
 * (no wider than the slice, no cycle through the substitution graph), and
 * the var's slices must not overlap. */
static int try_record_slice(dvs_bbsolver_t *S, dvs_expr_t sref, dvs_expr_t eref) {
    if (!S->slice_head) return 0;
    uint32_t id; uint32_t hi, lo;
    if (!as_var_slice(S, sref, &id, &hi, &lo)) return 0;
    if (!subst_definable(S, id)) return 0;
    if (bb_self_width(S, eref, 0) > (uint32_t)(hi - lo + 1)) return 0;
    for (uint32_t i = S->slice_head[id]; i != BB_NO_SLICE; i = S->slices[i].next)
        if (!(hi < S->slices[i].lo || lo > S->slices[i].hi)) return 0;
    if (subst_cycles(S, eref, id)) return 0;
    if (S->n_slices == S->cap_slices) {
        uint32_t c = S->cap_slices ? 2 * S->cap_slices : 64;
        bb_slice_t *p = (bb_slice_t *)realloc(S->slices, c * sizeof(*p));
        if (!p) return 0;
        S->slices = p; S->cap_slices = c;
    }
    bb_slice_t *sl = &S->slices[S->n_slices];
    sl->eref = eref; sl->hi = hi; sl->lo = lo; sl->next = S->slice_head[id];
    S->slice_head[id] = S->n_slices++;
    S->n_substs++;
    note_subst(S, eref, id);
    return 1;
}

/* `ref` as the constant 0 or 1 (unsized or 1-bit); -1 otherwise. */
static int const_bit(dvs_bbsolver_t *S, dvs_expr_t ref) {
    ExprKind *k = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!k || *k != EXPR_CONST) return -1;
    const ExprConst *c = (const ExprConst *)k;
    if (c->width > 1 || (c->value != 0 && c->value != 1)) return -1;
    return (int)c->value;
}

/* For a leaf `v == (ite c 1 0)` whose 1-bit v is already substituted by 1
 * (the front end's form of a Boolean assert), the condition c holds in every
 * model; return it so its equalities can be substituted too. EXPR_NULL
 * otherwise. The leaf itself stays encoded. */
static dvs_expr_t forced_ite_cond(dvs_bbsolver_t *S, dvs_expr_t vref, dvs_expr_t eref) {
    ExprKind *vk = (ExprKind *)POOL_PTR(S->problem, vref);
    ExprKind *ek = (ExprKind *)POOL_PTR(S->problem, eref);
    if (!vk || *vk != EXPR_VAR || !ek || *ek != EXPR_ITE) return EXPR_NULL;
    uint32_t id = ((ExprVar *)vk)->var_id;
    if (id >= S->n_vars || S->vars[id].width != 1) return EXPR_NULL;
    if (S->subst[id] == EXPR_NULL || const_bit(S, S->subst[id]) != 1) return EXPR_NULL;
    const ExprITE *t = (const ExprITE *)ek;
    if (const_bit(S, t->then_e) != 1 || const_bit(S, t->else_e) != 0) return EXPR_NULL;
    return t->cond;
}

/* Walk `root` descending through DVS_BIN_AND chains, looking for DVS_BIN_EQ
 * sub-asserts of the shape (= var expr). Records substitutions. Returns
 * 1 iff every sub-assert under this root was consumed (the whole root
 * can be skipped); 0 if any sub-assert remained (root must still be
 * encoded). */
/* `ref` is a constant, possibly through extract, extend and concat
 * (depth-capped; deeper counts as not constant). */
static int is_const_tree(dvs_bbsolver_t *S, dvs_expr_t ref, int depth) {
    if (depth > 16) return 0;
    ExprKind *k = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!k) return 0;
    switch (*k) {
    case EXPR_CONST: return 1;
    case EXPR_EXTRACT: return is_const_tree(S, ((ExprExtract *)k)->operand, depth + 1);
    case EXPR_EXTEND:  return is_const_tree(S, ((ExprExtend *)k)->operand, depth + 1);
    case EXPR_CONCAT:
        return is_const_tree(S, ((ExprConcat *)k)->hi, depth + 1)
            && is_const_tree(S, ((ExprConcat *)k)->lo, depth + 1);
    default: return 0;
    }
}

/* `ref` is wiring: variables and constants through extract, extend and
 * concat (depth-capped). */
static int is_wiring_tree(dvs_bbsolver_t *S, dvs_expr_t ref, int depth) {
    if (depth > 16) return 0;
    ExprKind *k = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!k) return 0;
    switch (*k) {
    case EXPR_CONST: case EXPR_VAR: return 1;
    case EXPR_EXTRACT: return is_wiring_tree(S, ((ExprExtract *)k)->operand, depth + 1);
    case EXPR_EXTEND:  return is_wiring_tree(S, ((ExprExtend *)k)->operand, depth + 1);
    case EXPR_CONCAT:
        return is_wiring_tree(S, ((ExprConcat *)k)->hi, depth + 1)
            && is_wiring_tree(S, ((ExprConcat *)k)->lo, depth + 1);
    default: return 0;
    }
}

/* Variable `t` is defined by a top-level `t == e` whose e is not wiring. */
static int var_def_is_arith(dvs_bbsolver_t *S, uint32_t t) {
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; ) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        ExprKind *rk = (ExprKind *)POOL_PTR(S->problem, cs->root);
        if (rk && *rk == EXPR_BINARY && ((ExprBinary *)rk)->op == DVS_BIN_EQ) {
            ExprBinary *b = (ExprBinary *)rk;
            for (int s = 0; s < 2; s++) {
                ExprKind *a = (ExprKind *)POOL_PTR(S->problem, s ? b->rhs : b->lhs);
                if (a && *a == EXPR_VAR && ((ExprVar *)a)->var_id == t
                        && !is_wiring_tree(S, s ? b->lhs : b->rhs, 0))
                    return 1;
            }
        }
        cur = cs->next;
    }
    return 0;
}

/* Under a forced condition only constants and wiring aliases are
 * substituted. Aliasing a variable to one defined by arithmetic (arit6:
 * v := t, t == 1 << k) makes v's other uses share that circuit and slows
 * the solve ~1.2x; substituting arithmetic itself does the same. */
static int try_record_forced(dvs_bbsolver_t *S, dvs_expr_t vref, dvs_expr_t eref) {
    ExprKind *ek = (ExprKind *)POOL_PTR(S->problem, eref);
    if (ek && *ek == EXPR_VAR) {
        if (var_def_is_arith(S, ((ExprVar *)ek)->var_id)) return 0;
        return try_record_subst(S, vref, eref);
    }
    return is_const_tree(S, eref, 0) && try_record_subst(S, vref, eref);
}

static int collect_substs_from_m(dvs_bbsolver_t *S, dvs_expr_t root, int forced);

static int collect_substs_from(dvs_bbsolver_t *S, dvs_expr_t root) {
    return collect_substs_from_m(S, root, 0);
}

/* `countones(r, x)` with r a plain variable DEFINES r: substitute r by the
 * countones node itself. bv_for_var and bb_eval read such a substitution as
 * the VALUE popcount(x) (see popcount_def), so r is no search input. Only
 * when popcount(x) always fits r (non-negative if r is signed): then
 * r == popcount(x) holds by construction and the constraint is consumed. */
static int try_record_popcount(dvs_bbsolver_t *S, dvs_expr_t cref) {
    const ExprCountones *ec = (const ExprCountones *)POOL_PTR(S->problem, cref);
    const ExprKind *vk = (const ExprKind *)POOL_PTR(S->problem, ec->result);
    if (!vk || *vk != EXPR_VAR) return 0;
    uint32_t id = ((const ExprVar *)vk)->var_id;
    if (id >= S->n_vars || !subst_definable(S, id)) return 0;
    if (S->subst[id] != EXPR_NULL) return 0;
    if (S->slice_head && S->slice_head[id] != BB_NO_SLICE) return 0;
    uint32_t xs = bb_self_width(S, ec->operand, 0);
    uint32_t rw = S->vars[id].width - (S->vars[id].is_signed ? 1u : 0u);
    if (xs == 0 || (rw < 32 && xs >= (1u << rw))) return 0;
    if (subst_cycles(S, ec->operand, id)) return 0;
    S->subst[id] = cref;
    S->n_substs++;
    note_subst(S, ec->operand, id);
    return 1;
}

/* The countones node defining var `id` by try_record_popcount, or NULL. */
static const ExprCountones *popcount_def(dvs_bbsolver_t *S, uint32_t id) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, S->subst[id]);
    if (!k || *k != EXPR_COUNTONES) return NULL;
    const ExprCountones *ec = (const ExprCountones *)k;
    const ExprKind *rk = (const ExprKind *)POOL_PTR(S->problem, ec->result);
    return (rk && *rk == EXPR_VAR && ((const ExprVar *)rk)->var_id == id) ? ec : NULL;
}

/* forced: root is a forced condition (see forced_ite_cond). Its equalities
 * are recorded only through try_record_forced; forced conditions met at the
 * top level are deferred to a second pass, so recording order for the
 * ordinary leaves is unchanged. */
static int collect_substs_from_m(dvs_bbsolver_t *S, dvs_expr_t root, int forced) {
    /* Flatten the DVS_BIN_AND spine ITERATIVELY. A deep conjunction of equalities
     * (large graph-colouring / sudoku instances) would otherwise recurse one
     * C-stack frame per conjunct and overflow the stack (was crash B10, subst
     * pre-pass site). Leaves are visited LEFT-TO-RIGHT, exactly as the former
     * recursion, so subst-recording order — hence the cycle-gate outcome in
     * try_record_subst — is byte-for-byte unchanged. Returns 1 iff EVERY
     * sub-assert under root was consumed as a substitution (root can be
     * skipped); every leaf is still visited even after one fails to consume, so
     * all recordable substitutions are recorded (matching the old `l && r`).
     * The stack starts on the C stack: most roots are not conjunctions, and
     * this runs once per constraint per solve. */
    dvs_expr_t local[32];
    dvs_expr_t *stk = local;
    size_t n = 0, cap = sizeof local / sizeof local[0];
    int all_consumed = 1;
    dvs_expr_t fc[8];   /* forced conditions, for the second pass */
    size_t n_fc = 0;
    #define CS_PUSH(R) do {                                                 \
        if (n == cap) { size_t nc = cap * 2;                                \
            dvs_expr_t *t = (dvs_expr_t *)malloc(nc * sizeof(dvs_expr_t));  \
            if (!t) { if (stk != local) free(stk); return 0; }              \
            memcpy(t, stk, n * sizeof(dvs_expr_t));                         \
            if (stk != local) free(stk);                                    \
            stk = t; cap = nc; }                                            \
        stk[n++] = (R);                                                     \
    } while (0)
    CS_PUSH(root);
    while (n > 0) {
        dvs_expr_t r = stk[--n];
        if (r == EXPR_NULL) { all_consumed = 0; continue; }
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, r);
        if (!kp) { all_consumed = 0; continue; }
        if (*kp == EXPR_BINARY) {
            ExprBinary *b = (ExprBinary *)kp;
            if (b->op == DVS_BIN_AND) {
                CS_PUSH(b->rhs);   /* push rhs first so lhs pops (is visited) first */
                CS_PUSH(b->lhs);
                continue;
            }
            if (b->op == DVS_BIN_EQ) {
                if (forced) {
                    if (try_record_forced(S, b->lhs, b->rhs)) continue;
                    if (try_record_forced(S, b->rhs, b->lhs)) continue;
                    if (is_const_tree(S, b->rhs, 0) && try_record_slice(S, b->lhs, b->rhs)) continue;
                    if (is_const_tree(S, b->lhs, 0) && try_record_slice(S, b->rhs, b->lhs)) continue;
                } else {
                    if (try_record_subst(S, b->lhs, b->rhs)) continue;  /* consumed */
                    if (try_record_subst(S, b->rhs, b->lhs)) continue;  /* consumed */
                    if (try_record_slice(S, b->lhs, b->rhs)) continue;  /* consumed */
                    if (try_record_slice(S, b->rhs, b->lhs)) continue;  /* consumed */
                }
                dvs_expr_t c = forced_ite_cond(S, b->lhs, b->rhs);
                if (c == EXPR_NULL) c = forced_ite_cond(S, b->rhs, b->lhs);
                if (c != EXPR_NULL) {
                    if (forced) CS_PUSH(c);
                    else if (n_fc < sizeof fc / sizeof fc[0]) fc[n_fc++] = c;
                }
            }
        } else if (*kp == EXPR_COUNTONES && !forced) {
            if (try_record_popcount(S, r)) continue;   /* consumed */
        }
        all_consumed = 0;   /* this leaf was not consumable as a substitution */
    }
    #undef CS_PUSH
    if (stk != local) free(stk);
    for (size_t i = 0; i < n_fc; i++) (void)collect_substs_from_m(S, fc[i], 1);
    return all_consumed;
}

/* Is `ref` a node that only widens what it wraps with zeros? Returns the
 * wrapped node, or EXPR_NULL. A substituted variable counts: its uses are
 * uses of its definition. */
static dvs_expr_t fanout_inner(dvs_bbsolver_t *S, dvs_expr_t ref) {
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return EXPR_NULL;
    switch (*kp) {
    case EXPR_EXTEND: {
        const ExprExtend *e = (const ExprExtend *)kp;
        return (!e->sign_extend && e->to_bits >= e->from_bits) ? e->operand : EXPR_NULL;
    }
    case EXPR_CONCAT: {
        const ExprConcat *c = (const ExprConcat *)kp;
        ExprKind *hk = (ExprKind *)POOL_PTR(S->problem, c->hi);
        return (hk && *hk == EXPR_CONST && ((const ExprConst *)hk)->value == 0)
            ? c->lo : EXPR_NULL;
    }
    case EXPR_VAR: {
        uint32_t id = ((const ExprVar *)kp)->var_id;
        return (S->subst && id < S->n_vars) ? S->subst[id] : EXPR_NULL;
    }
    default:
        return EXPR_NULL;
    }
}

/* Size uses[] and fo_seen[] to the memo cache (a session's grows). */
static int fanout_reserve(dvs_bbsolver_t *S) {
    uint32_t cap = S->cache_cap, old = S->uses ? S->uses_cap : 0;
    if (S->uses && old >= cap) return 1;
    uint8_t *u = (uint8_t *)realloc(S->uses, cap);
    if (u) S->uses = u;
    uint8_t *f = u ? (uint8_t *)realloc(S->fo_seen, cap) : NULL;
    if (f) S->fo_seen = f;
    if (!u || !f) {
        free(S->uses); free(S->fo_seen);
        S->uses = NULL; S->fo_seen = NULL; S->uses_cap = 0;
        return 0;
    }
    memset(u + old, 0, cap - old);
    memset(f + old, 0, cap - old);
    S->uses_cap = cap;
    return 1;
}

/* Count the fanout of every node reachable from `roots` (skipping those with
 * skip[i] set) into S->uses. Nodes counted by an earlier call are not
 * descended again, so a session counts each batch's new nodes once.
 * Iterative: the DAG can be deep. */
static void count_fanout_from(dvs_bbsolver_t *S, const dvs_expr_t *roots,
                              const uint8_t *skip, uint32_t n_roots) {
    if (!S->cache_cap || getenv("DV_BB_SUM_NO_FANOUT")) return;
    if (!fanout_reserve(S)) return;
    uint32_t cap = S->uses_cap;
    uint8_t *seen = S->fo_seen;
    dvs_expr_t *stk = NULL;
    size_t n = 0, scap = 0;
    #define FO_EDGE(C) do {                                                    \
        dvs_expr_t c_ = (C);                                                   \
        for (int g_ = 0; g_ < 64 && c_ != EXPR_NULL; g_++) {                   \
            if (c_ >= cap) break;                                              \
            if (S->uses[c_] < 255) S->uses[c_]++;                              \
            dvs_expr_t in_ = fanout_inner(S, c_);                              \
            if (in_ != EXPR_NULL) { c_ = in_; continue; }                      \
            if (!seen[c_]) {                                                   \
                seen[c_] = 1;                                                  \
                if (n == scap) { size_t nc_ = scap ? 2 * scap : 256;           \
                    dvs_expr_t *t_ = (dvs_expr_t *)realloc(stk, nc_ * sizeof(*t_)); \
                    if (!t_) goto fail;                                        \
                    stk = t_; scap = nc_; }                                    \
                stk[n++] = c_;                                                 \
            }                                                                  \
            break;                                                             \
        }                                                                      \
    } while (0)
    for (uint32_t i = 0; i < n_roots; i++)
        if (!(skip && skip[i])) FO_EDGE(roots[i]);
    while (n > 0) {
        dvs_expr_t r = stk[--n];
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, r);
        if (!kp) continue;
        switch (*kp) {
        case EXPR_BINARY:
            FO_EDGE(((ExprBinary *)kp)->lhs);
            FO_EDGE(((ExprBinary *)kp)->rhs);
            break;
        case EXPR_UNARY:   FO_EDGE(((ExprUnary *)kp)->operand); break;
        case EXPR_EXTEND:  FO_EDGE(((ExprExtend *)kp)->operand); break;
        case EXPR_SV_CAST: FO_EDGE(((ExprSvCast *)kp)->operand); break;
        case EXPR_EXTRACT: {
            /* An extract of a sliced variable reads only the slices it
             * overlaps (Yosys reads each register out of the state). */
            const ExprExtract *x = (const ExprExtract *)kp;
            ExprKind *ok = (ExprKind *)POOL_PTR(S->problem, x->operand);
            uint32_t id = (ok && *ok == EXPR_VAR) ? ((ExprVar *)ok)->var_id : UINT32_MAX;
            if (S->slice_head && id < S->n_vars && S->slice_head[id] != BB_NO_SLICE) {
                for (uint32_t i = S->slice_head[id]; i != BB_NO_SLICE; i = S->slices[i].next)
                    if (!(x->hi_bit < S->slices[i].lo || x->lo_bit > S->slices[i].hi))
                        FO_EDGE(S->slices[i].eref);
            } else {
                FO_EDGE(x->operand);
            }
            break;
        }
        case EXPR_CONCAT:
            FO_EDGE(((ExprConcat *)kp)->hi);
            FO_EDGE(((ExprConcat *)kp)->lo);
            break;
        case EXPR_ITE:
            FO_EDGE(((ExprITE *)kp)->cond);
            FO_EDGE(((ExprITE *)kp)->then_e);
            FO_EDGE(((ExprITE *)kp)->else_e);
            break;
        case EXPR_IN_RANGE:
            FO_EDGE(((ExprInRange *)kp)->value);
            FO_EDGE(((ExprInRange *)kp)->lo);
            FO_EDGE(((ExprInRange *)kp)->hi);
            break;
        case EXPR_IN_SET: {
            ExprInSet *iset = (ExprInSet *)kp;
            FO_EDGE(iset->value);
            dvs_expr_t *el = expr_in_set_elems(S->problem, r);
            for (uint32_t i = 0; i < iset->n_elems; i++) FO_EDGE(el[i]);
            break;
        }
        case EXPR_IN_RANGES: {
            ExprInRanges *irs = (ExprInRanges *)kp;
            FO_EDGE(irs->value);
            dvs_expr_t *los = expr_in_ranges_los(S->problem, r);
            dvs_expr_t *his = expr_in_ranges_his(S->problem, r);
            for (uint32_t i = 0; i < irs->n_ranges; i++) { FO_EDGE(los[i]); FO_EDGE(his[i]); }
            break;
        }
        case EXPR_VAR: {
            /* A sliced variable: every use of it may read every slice. */
            uint32_t id = ((ExprVar *)kp)->var_id;
            if (S->slice_head && id < S->n_vars)
                for (uint32_t i = S->slice_head[id]; i != BB_NO_SLICE; i = S->slices[i].next)
                    FO_EDGE(S->slices[i].eref);
            break;
        }
        case EXPR_SUM: {
            const ExprSum *es = (const ExprSum *)kp;
            const dvs_expr_t *xs = (const dvs_expr_t *)(es + 1);
            FO_EDGE(es->result);
            for (uint32_t i = 0; i < es->n_vars; i++) FO_EDGE(xs[i]);
            break;
        }
        case EXPR_COUNTONES:
            FO_EDGE(((ExprCountones *)kp)->result);
            FO_EDGE(((ExprCountones *)kp)->operand);
            break;
        case EXPR_CLOG2:
            FO_EDGE(((ExprClog2 *)kp)->result);
            FO_EDGE(((ExprClog2 *)kp)->operand);
            break;
        case EXPR_ARRAY_SELECT: {
            /* The elements are named by id; a sliced one reads its slices. */
            const ExprArraySelect *as = (const ExprArraySelect *)kp;
            FO_EDGE(as->result);
            FO_EDGE(as->index);
            for (uint32_t e = 0; e < as->n_elems && S->slice_head; e++) {
                uint32_t id = as->base_var_id + e;
                if (id < S->n_vars)
                    for (uint32_t i = S->slice_head[id]; i != BB_NO_SLICE; i = S->slices[i].next)
                        FO_EDGE(S->slices[i].eref);
            }
            break;
        }
        default:
            break;
        }
    }
    #undef FO_EDGE
    free(stk);
    return;
fail:
    free(stk);
    free(S->uses); free(S->fo_seen);
    S->uses = NULL; S->fo_seen = NULL; S->uses_cap = 0;
}

/* Union the variables each kept constraint reads into components. Elements
 * are vars [0,n_vars) then constraints; an expr node shared by two
 * constraints links them through seen_by. Iterative; nothing reads the
 * result yet, so answers do not change. */
static void compute_components(dvs_bbsolver_t *S) {
    dvs_components_free(&S->comps);
    free(S->cons_comp);
    S->cons_comp = NULL;
    uint32_t nv = S->n_vars, nc = S->n_constraints, cap = S->cache_cap;
    if (!S->problem || !cap) return;
    dvs_uf_t uf;
    uint32_t *seen_by = (uint32_t *)malloc(cap * sizeof(uint32_t));
    uint8_t *used = (uint8_t *)calloc(nv ? nv : 1, 1);
    dvs_expr_t *stk = NULL;
    size_t n = 0, scap = 0;
    /* Softs take uf nodes after the constraints (nv + nc + i). */
    uint32_t ns = 0;
    for (dvs_expr_t s = S->problem->softs_head; s != EXPR_NULL; ns++)
        s = ((SoftSpec *)POOL_PTR(S->problem, s))->next;
    if (!seen_by || !used || !dvs_uf_init(&uf, nv + nc + ns)) {
        free(seen_by); free(used);
        return;
    }
    memset(seen_by, 0xff, cap * sizeof(uint32_t));
    uint32_t me = 0;
    #define CC_EDGE(C) do {                                                    \
        dvs_expr_t c_ = (C);                                                   \
        if (c_ == EXPR_NULL || c_ >= cap) break;                               \
        if (seen_by[c_] != UINT32_MAX) {                                       \
            dvs_uf_union(&uf, nv + me, nv + seen_by[c_]); break; }             \
        seen_by[c_] = me;                                                      \
        if (n == scap) { size_t nc_ = scap ? 2 * scap : 256;                   \
            dvs_expr_t *t_ = (dvs_expr_t *)realloc(stk, nc_ * sizeof(*t_));    \
            if (!t_) goto done;                                                \
            stk = t_; scap = nc_; }                                            \
        stk[n++] = c_;                                                         \
    } while (0)
    #define CC_VAR(ID) do {                                                    \
        uint32_t v_ = (ID);                                                    \
        if (v_ < nv) { used[v_] = 1; dvs_uf_union(&uf, v_, nv + me); }         \
        /* A substituted var stands for its subst expr: walk it too. */        \
        if (S->subst && v_ < nv && S->subst[v_] != EXPR_NULL)                  \
            CC_EDGE(S->subst[v_]);                                             \
        if (S->slice_head && v_ < nv)                                          \
            for (uint32_t i_ = S->slice_head[v_]; i_ != BB_NO_SLICE;           \
                 i_ = S->slices[i_].next) CC_EDGE(S->slices[i_].eref);         \
    } while (0)
    for (int phase = 0; phase < 2; phase++) {
    dvs_expr_t cur = phase ? S->problem->softs_head : S->problem->constraints_head;
    uint32_t lim = phase ? nc + ns : nc;
    if (phase) me = nc;
    for (; cur != EXPR_NULL && me < lim; me++) {
        dvs_expr_t root;
        if (phase) {
            SoftSpec *ss = (SoftSpec *)POOL_PTR(S->problem, cur);
            cur = ss->next;
            root = ss->root;
        } else {
            ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
            cur = cs->next;
            if (S->constraint_skip && S->constraint_skip[me]) continue;
            root = cs->root;
        }
        CC_EDGE(root);
        while (n > 0) {
            dvs_expr_t r = stk[--n];
            ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, r);
            if (!kp) continue;
            switch (*kp) {
            case EXPR_BINARY:
                CC_EDGE(((ExprBinary *)kp)->lhs);
                CC_EDGE(((ExprBinary *)kp)->rhs);
                break;
            case EXPR_UNARY:   CC_EDGE(((ExprUnary *)kp)->operand); break;
            case EXPR_EXTEND:  CC_EDGE(((ExprExtend *)kp)->operand); break;
            case EXPR_SV_CAST: CC_EDGE(((ExprSvCast *)kp)->operand); break;
            case EXPR_EXTRACT: CC_EDGE(((ExprExtract *)kp)->operand); break;
            case EXPR_CONCAT:
                CC_EDGE(((ExprConcat *)kp)->hi);
                CC_EDGE(((ExprConcat *)kp)->lo);
                break;
            case EXPR_ITE:
                CC_EDGE(((ExprITE *)kp)->cond);
                CC_EDGE(((ExprITE *)kp)->then_e);
                CC_EDGE(((ExprITE *)kp)->else_e);
                break;
            case EXPR_IN_RANGE:
                CC_EDGE(((ExprInRange *)kp)->value);
                CC_EDGE(((ExprInRange *)kp)->lo);
                CC_EDGE(((ExprInRange *)kp)->hi);
                break;
            case EXPR_IN_SET: {
                ExprInSet *iset = (ExprInSet *)kp;
                CC_EDGE(iset->value);
                dvs_expr_t *el = expr_in_set_elems(S->problem, r);
                for (uint32_t i = 0; i < iset->n_elems; i++) CC_EDGE(el[i]);
                break;
            }
            case EXPR_IN_RANGES: {
                ExprInRanges *irs = (ExprInRanges *)kp;
                CC_EDGE(irs->value);
                dvs_expr_t *los = expr_in_ranges_los(S->problem, r);
                dvs_expr_t *his = expr_in_ranges_his(S->problem, r);
                for (uint32_t i = 0; i < irs->n_ranges; i++) { CC_EDGE(los[i]); CC_EDGE(his[i]); }
                break;
            }
            case EXPR_VAR: CC_VAR(((ExprVar *)kp)->var_id); break;
            case EXPR_SUM: {
                const ExprSum *es = (const ExprSum *)kp;
                const dvs_expr_t *xs = (const dvs_expr_t *)(es + 1);
                CC_EDGE(es->result);
                for (uint32_t i = 0; i < es->n_vars; i++) CC_EDGE(xs[i]);
                break;
            }
            case EXPR_COUNTONES:
                CC_EDGE(((ExprCountones *)kp)->result);
                CC_EDGE(((ExprCountones *)kp)->operand);
                break;
            case EXPR_CLOG2:
                CC_EDGE(((ExprClog2 *)kp)->result);
                CC_EDGE(((ExprClog2 *)kp)->operand);
                break;
            case EXPR_ARRAY_SELECT: {
                const ExprArraySelect *as = (const ExprArraySelect *)kp;
                CC_EDGE(as->result);
                CC_EDGE(as->index);
                for (uint32_t e = 0; e < as->n_elems; e++) CC_VAR(as->base_var_id + e);
                break;
            }
            default:
                break;
            }
        }
    }
    }
    /* A dist only constrains its own var; an alldiff group ties its vars. */
    for (dvs_expr_t d = S->problem->dists_head; d != EXPR_NULL; ) {
        DistSpec *ds = (DistSpec *)POOL_PTR(S->problem, d);
        if (ds->var_id < nv) used[ds->var_id] = 1;
        d = ds->next;
    }
    for (dvs_expr_t a = S->problem->allDiff_head; a != EXPR_NULL; ) {
        AllDiffSpec *as = (AllDiffSpec *)POOL_PTR(S->problem, a);
        uint32_t *ids = (uint32_t *)(as + 1);
        for (uint32_t i = 0; i < as->n_vars; i++) {
            if (ids[i] >= nv) continue;
            used[ids[i]] = 1;
            if (ids[0] < nv) dvs_uf_union(&uf, ids[0], ids[i]);
        }
        a = as->next;
    }
    dvs_components_from_uf(&S->comps, &uf, used, nv);
    /* Map each kept constraint to the comp of its uf root. */
    {
        uint32_t nn = nv + nc + ns;
        uint32_t *rc = (uint32_t *)malloc((nn ? nn : 1) * sizeof(uint32_t));
        S->cons_comp = (uint32_t *)malloc((nc ? nc : 1) * sizeof(uint32_t));
        if (rc && S->cons_comp && S->comps.var_comp) {
            memset(rc, 0xff, (nn ? nn : 1) * sizeof(uint32_t));
            for (uint32_t v = 0; v < nv; v++)
                if (used[v]) rc[dvs_uf_find(&uf, v)] = S->comps.var_comp[v];
            for (uint32_t i = 0; i < nc; i++)
                S->cons_comp[i] = (S->constraint_skip && S->constraint_skip[i])
                    ? UINT32_MAX : rc[dvs_uf_find(&uf, nv + i)];
        } else { free(S->cons_comp); S->cons_comp = NULL; }
        free(rc);
    }
done:
    #undef CC_EDGE
    #undef CC_VAR
    free(stk);
    free(seen_by);
    free(used);
    dvs_uf_free(&uf);
}

/* Fanout over the constraints that are still encoded (see S->uses). */
static void count_fanout(dvs_bbsolver_t *S) {
    uint32_t n = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; n++)
        cur = ((ConstraintSpec *)POOL_PTR(S->problem, cur))->next;
    dvs_expr_t *roots = (dvs_expr_t *)malloc((n ? n : 1) * sizeof(dvs_expr_t));
    if (!roots) return;
    uint32_t i = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; i++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        roots[i] = cs->root;
        cur = cs->next;
    }
    count_fanout_from(S, roots, S->n_constraints == n ? S->constraint_skip : NULL, n);
    free(roots);
}

/* Pre-pass: scan all top-level constraints, populate S->subst, mark
 * fully-consumed constraints in S->constraint_skip. */
static void run_subst_pass(dvs_bbsolver_t *S) {
    /* Count constraints first so we can size constraint_skip. */
    uint32_t n = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; ) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        n++;
        cur = cs->next;
    }
    S->n_constraints = n;
    if (n == 0 || S->n_vars == 0) return;
    S->subst = (dvs_expr_t *)xalloc(S->alloc, S->n_vars * sizeof(dvs_expr_t));
    S->resolving = (uint8_t *)xalloc(S->alloc, S->n_vars * sizeof(uint8_t));
    S->constraint_skip = (uint8_t *)xalloc(S->alloc, n * sizeof(uint8_t));
    if (!S->subst || !S->resolving || !S->constraint_skip) return;
    for (uint32_t i = 0; i < S->n_vars; i++) S->subst[i] = EXPR_NULL;
    S->rhs_var = (uint8_t *)calloc(S->n_vars, 1);
    if (getenv("DV_BB_NO_SLICE") == NULL) {
        S->slice_head = (uint32_t *)malloc(S->n_vars * sizeof(uint32_t));
        if (S->slice_head)
            for (uint32_t i = 0; i < S->n_vars; i++) S->slice_head[i] = BB_NO_SLICE;
    }
    memset(S->resolving, 0, S->n_vars);
    memset(S->constraint_skip, 0, n);

    uint32_t idx = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        if (collect_substs_from(S, cs->root)) {
            S->constraint_skip[idx] = 1;
        }
        cur = cs->next;
    }
}

/* Build (or fetch) the bit-vector for a declared variable.
 *
 * If the variable's bounds [lo, hi] (interpreted unsigned) yield a useful
 * bit-fix mask — e.g. an 8-bit variable bounded to [0, 7] has its top 5
 * bits fixed-0 — those bits are emitted as AIG constants instead of fresh
 * inputs. Saves SAT variables and clauses on every bounded variable.
 *
 * Signed variables with mixed-sign ranges are not folded (the unsigned
 * range straddles the midpoint and no bit is uniformly fixed); they get
 * the all-fresh behavior. */
static dvs_bv_t bv_for_var(dvs_bbsolver_t *S, uint32_t var_id) {
    assert(var_id < S->n_vars);
    bb_var_t *v = &S->vars[var_id];
    if (!v->bv_built) {
        /* If a substitution applies and we aren't already resolving it
         * (cycle guard), bit-blast the substituted expression and use
         * its result. The expression's natural width may differ from
         * the variable's declared width — adjust via zext or extract. */
        if (S->subst && S->subst[var_id] != EXPR_NULL && !S->resolving[var_id]) {
            S->resolving[var_id] = 1;
            const ExprCountones *pc = popcount_def(S, var_id);
            dvs_bv_t e = pc ? bb_popcount(S, pc->operand, v->width)
                            : bb_expr(S, S->subst[var_id], v->width);
            S->resolving[var_id] = 0;
            if (!S->had_error) {
                if (e.size < v->width) {
                    /* Extend per the *expression's* result signedness, not the
                     * variable's: a boolean result (e.g. `var == (a == 5)`) is
                     * unsigned 0/1 and must zero-extend even into a signed var,
                     * else true (1) becomes all-ones (-1). */
                    e = ext_ref(S, e, S->subst[var_id], v->width);
                } else if (e.size > v->width) {
                    e = dvs_bb_extract(S->bb, e, v->width - 1, 0);
                }
                v->bv = e;
                v->bv_built = 1;
                return v->bv;
            }
            /* fall through on error to allocate fresh */
        }
        int sliced = S->slice_head && S->slice_head[var_id] != BB_NO_SLICE
                     && !S->resolving[var_id];

        /* Bit-fix folding is a 64-bit-domain optimization: the [lo,hi] range and
         * the fixed/unknown masks are uint64. For width > 64 it cannot represent
         * the true range and would wrongly fix the bits above 63 to 0 (e.g. a
         * 128-bit unconstrained var passed [0, INT64_MAX] would have bits 63..127
         * pinned). Skip it for wide vars — they get all-fresh bits, and any real
         * sub-range is enforced by assert_var_bounds / explicit constraints. */
        int use_fix = !v->is_signed && v->lo >= 0 && v->hi >= v->lo
                      && v->width <= 64;
        if (use_fix) {
            dvs_bvdom_t d;
            dvs_bvdom_init_from_range_u(&d, v->width,
                                        (uint64_t)v->lo, (uint64_t)v->hi);
            uint64_t mask = dvs_bvdom_mask(v->width);
            /* fixed bit mask = bits where lo == hi (within width). */
            uint64_t fixed_mask = ~(d.lo ^ d.hi) & mask;
            uint64_t unknown_mask = ~fixed_mask & mask;
            v->bv = dvs_bb_constant_dom(S->bb, v->width, d.lo, unknown_mask);
        } else {
            v->bv = dvs_bb_constant(S->bb, v->width);
        }
        if (sliced) {
            /* Replace each substituted slice's fresh bits by its expression's.
             * MSB first: bit `hi` is at index width-1-hi. */
            S->resolving[var_id] = 1;
            dvs_aig_node_t *bits =
                (dvs_aig_node_t *)malloc(v->width * sizeof(dvs_aig_node_t));
            if (!bits) { S->resolving[var_id] = 0; S->had_error = 1; return v->bv; }
            memcpy(bits, v->bv.bits, v->width * sizeof(dvs_aig_node_t));
            for (uint32_t i = S->slice_head[var_id]; i != BB_NO_SLICE;
                 i = S->slices[i].next) {
                const bb_slice_t *sl = &S->slices[i];
                uint32_t sw = (uint32_t)(sl->hi - sl->lo + 1);
                dvs_bv_t e = bb_expr(S, sl->eref, sw);
                if (S->had_error) break;
                if (e.size < sw)      e = ext_ref(S, e, sl->eref, sw);
                else if (e.size > sw) e = dvs_bb_extract(S->bb, e, sw - 1, 0);
                memcpy(bits + (v->width - 1 - sl->hi), e.bits,
                       sw * sizeof(dvs_aig_node_t));
            }
            S->resolving[var_id] = 0;
            if (!S->had_error) v->bv = dvs_bb_value_bits(S->bb, v->width, bits);
            free(bits);
        }
        v->bv_built = 1;
    }
    return v->bv;
}

/* ----------------------------- expression dispatch ------------------------ */

/* Returns the bit-blasted value of expression `ref`. `hint_width` may be
 * passed to size EXPR_CONST values to context; pass 0 to use the natural
 * width (64 — the engine's int64 value domain — for unconstrained constants).
 * On error, sets S->had_error = 1 and returns {NULL, 0}. */
static dvs_bv_t bb_expr(dvs_bbsolver_t *S, dvs_expr_t ref, uint32_t hint_width);

/* Same as bb_expr but the result is a 1-bit predicate. */
static dvs_bv_t bb_predicate(dvs_bbsolver_t *S, dvs_expr_t ref);

/* Build a width-`w` bit-vector for the int64 `value`. For w <= 64 this is the
 * exact low-w-bit pattern (dvs_bb_value_u64). For w > 64, value_u64 alone would
 * zero-fill bits 64+, corrupting a negative value; build the 64-bit pattern and
 * extend — sign-extend when `is_signed` (a negative value must set the high
 * bits), zero-extend otherwise. */
static dvs_bv_t bb_value_i64(dvs_bbsolver_t *S, uint32_t w, int64_t value,
                             int is_signed) {
    if (w <= 64) return dvs_bb_value_u64(S->bb, w, (uint64_t)value);
    dvs_bv_t lo64 = dvs_bb_value_u64(S->bb, 64, (uint64_t)value);
    return is_signed ? sext_to(S, lo64, w) : zext_to(S, lo64, w);
}

static dvs_bv_t bb_const(dvs_bbsolver_t *S, const ExprConst *c, uint32_t hint) {
    /* A sized constant is exactly its own width. */
    if (c->width) return bb_value_i64(S, c->width, c->value, c->is_signed);
    /* A width-less constant defaults to 64 bits — the engine's int64 value
     * domain — NOT 32: a constant carrying a full 64-bit pattern (e.g. a limb of
     * a wide >64-bit literal, requested by bb_concat with hint=0) must not be
     * truncated to 32 bits. Where a constant has real context (a comparison /
     * arithmetic operand) the caller passes a non-zero hint, and width
     * reconciliation (max_w + zext/sext) sizes it to the other operand anyway,
     * so a wider default never changes a value outcome. */
    uint32_t w = hint ? hint : 64;
    return bb_value_i64(S, w, c->value, c->is_signed);
}

/* A wide constant is exactly its own width, like a sized one. */
static dvs_bv_t bb_const_wide(dvs_bbsolver_t *S, const ExprConstWide *c) {
    dvs_bv_t r = dvs_bb_value_u64(S->bb, c->width, 0);
    for (uint32_t i = 0; i < c->width; i++)          /* bits[0] is the MSB */
        if (dvs_const_wide_bit(c, i)) r.bits[c->width - 1 - i] = DVS_AIG_TRUE;
    return r;
}

static BB_NOINLINE dvs_bv_t bb_var_expr(dvs_bbsolver_t *S, const ExprVar *v) {
    return bv_for_var(S, v->var_id);
}

static dvs_bv_t err_bv(dvs_bbsolver_t *S, const char *msg) {
    fprintf(stderr, "[dvs_bbsolver] error: %s\n", msg);
    S->had_error = 1;
    dvs_bv_t empty = { NULL, 0 };
    return empty;
}

/* True iff `rf` is an in-range AND/OR EXPR_BINARY node — i.e. one the iterative
 * AND/OR evaluator manages on its explicit stack (and memoizes), as opposed to a
 * leaf operand it blasts directly via bb_predicate. */
static int bb_is_andor_node(dvs_bbsolver_t *S, dvs_expr_t rf) {
    if (rf == EXPR_NULL || rf >= S->cache_cap) return 0;
    ExprKind *kk = (ExprKind *)POOL_PTR(S->problem, rf);
    return kk && *kk == EXPR_BINARY &&
           (((const ExprBinary *)kk)->op == DVS_BIN_AND ||
            ((const ExprBinary *)kk)->op == DVS_BIN_OR);
}

/* Logical shift (left if `shl`) of `l` by the unsigned amount `r`, whose
 * width may exceed l's: an amount >= 2^l.size is >= l.size, i.e. result 0. */
static dvs_bv_t bb_shift(dvs_bbsolver_t *S, int shl, dvs_bv_t l, dvs_bv_t r) {
    if (r.size > l.size) {
        dvs_bv_t hi = dvs_bb_extract(S->bb, r, r.size - 1, l.size);
        dvs_bv_t big = dvs_bb_not(S->bb, dvs_bb_eq(S->bb, hi,
                           dvs_bb_value_u64(S->bb, hi.size, 0)));
        dvs_bv_t lo = dvs_bb_extract(S->bb, r, l.size - 1, 0);
        dvs_bv_t sh = shl ? dvs_bb_shl(S->bb, l, lo) : dvs_bb_shr(S->bb, l, lo);
        return dvs_bb_ite(S->bb, big.bits[0],
                          dvs_bb_value_u64(S->bb, l.size, 0), sh);
    }
    if (r.size != l.size) r = zext_to(S, r, l.size);
    return shl ? dvs_bb_shl(S->bb, l, r) : dvs_bb_shr(S->bb, l, r);
}

/* Bit offsets (from the LSB) where `ref`, placed at `off`, is a concat of
 * two parts; appended to cuts[*n], at most `cap`. */
static void bb_concat_cuts(dvs_bbsolver_t *S, dvs_expr_t ref, uint32_t off,
                           uint32_t *cuts, uint32_t *n, uint32_t cap, int depth) {
    if (ref == EXPR_NULL || depth > 64 || *n >= cap) return;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (kp && *kp == EXPR_VAR) {               /* through a substituted var */
        uint32_t id = ((const ExprVar *)kp)->var_id;
        if (S->subst && id < S->n_vars && S->subst[id] != EXPR_NULL)
            bb_concat_cuts(S, S->subst[id], off, cuts, n, cap, depth + 1);
        return;
    }
    if (!kp || *kp != EXPR_CONCAT) return;
    const ExprConcat *c = (const ExprConcat *)kp;
    cuts[(*n)++] = off + c->lo_width;
    bb_concat_cuts(S, c->lo, off, cuts, n, cap, depth + 1);
    bb_concat_cuts(S, c->hi, off + c->lo_width, cuts, n, cap, depth + 1);
}

static int bb_cut_cmp(const void *a, const void *b) {
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return (x > y) - (x < y);
}

/* l == r through the word-level facts (dvs_bbword), one concat part at a
 * time: each side's concat structure splits the comparison so a part such
 * as a reduced checksum byte meets its counterpart. */
static dvs_bv_t bb_word_eq(dvs_bbsolver_t *S, dvs_expr_t lref, dvs_expr_t rref,
                           dvs_bv_t l, dvs_bv_t r) {
    enum { MAX_CUTS = 64 };
    uint32_t cuts[2 * MAX_CUTS + 2], n = 0;
    bb_concat_cuts(S, lref, 0, cuts, &n, MAX_CUTS, 0);
    bb_concat_cuts(S, rref, 0, cuts, &n, n + MAX_CUTS, 0);
    if (n == 0) return dvs_bbword_eq(S->word, S->bb, l, r);
    qsort(cuts, n, sizeof(uint32_t), bb_cut_cmp);
    uint64_t before = dvs_bbword_num_decided(S->word) + dvs_bbword_num_peeled(S->word);
    dvs_bv_t all = dvs_bb_value_u64(S->bb, 1, 1);
    uint32_t lo = 0;
    for (uint32_t i = 0; i <= n; i++) {
        uint32_t hi = i < n ? cuts[i] : l.size;
        if (hi <= lo || hi > l.size) continue;
        dvs_bv_t part = dvs_bbword_eq(S->word, S->bb,
                                      dvs_bb_extract(S->bb, l, hi - 1, lo),
                                      dvs_bb_extract(S->bb, r, hi - 1, lo));
        all = dvs_bb_and(S->bb, all, part);
        lo = hi;
    }
    /* Nothing known about any part: compare whole, so the gates are as before
     * (the parts' gates are left unused). */
    if (dvs_bbword_num_decided(S->word) + dvs_bbword_num_peeled(S->word) == before)
        return dvs_bb_eq(S->bb, l, r);
    return all;
}

static BB_NOINLINE dvs_bv_t bb_binary(dvs_bbsolver_t *S, const ExprBinary *b, dvs_expr_t ref,
                          uint32_t hint) {
    /* Comparison / logical ops always produce 1-bit; arithmetic / bitwise
     * widen lhs and rhs to a common width = max(lhs, rhs, hint). */
    switch (b->op) {
    case DVS_BIN_AND:
    case DVS_BIN_OR: {
        /* Evaluate the AND/OR expression tree ITERATIVELY with the SAME
         * memoization as the recursive path, so the produced AIG is
         * byte-identical (DAG-shared sub-formulas stay shared) while the C stack
         * stays O(1) on deep spines. A deep (and c1 (and c2 (and c3 ...))) chain
         * — thousands of conjuncts, as large graph-colouring / edge-matching /
         * sudoku instances build — otherwise recurses one C-stack frame per
         * conjunct (bb_binary->bb_predicate->bb_expr->bb_binary) and overflows
         * the stack (was crash B10). A NAIVE flatten avoids the crash but
         * dissolves shared sub-conjunctions (they memoize once in the recursive
         * path), changing the CNF and REGRESSING heavy-tail solves (mcm/70
         * 40s->timeout) — hence this structure-preserving version instead.
         *
         * Only AND/OR nodes go on the explicit stack; non-AND/OR operands are
         * blasted via bb_predicate (bounded recursion). Each AND/OR node memoizes
         * into S->cache — the exact slot bb_expr uses — so a shared node is
         * computed once. Falls back to plain recursion when memoization is off
         * (DV_BB_NO_MEMO) or the ref is out of cache range (the only path that
         * can still deep-recurse, both non-default). */
        int memo = !S->no_memo && S->cache != NULL;
        if (!memo || ref >= S->cache_cap) {
            dvs_bv_t a = bb_predicate(S, b->lhs);
            dvs_bv_t c = bb_predicate(S, b->rhs);
            if (S->had_error) return a;
            return (b->op == DVS_BIN_AND) ? dvs_bb_and(S->bb, a, c)
                                      : dvs_bb_or (S->bb, a, c);
        }
        typedef struct { dvs_expr_t ref; int phase; } bb_fr;
        bb_fr *stk = NULL; size_t n = 0, cap = 0;
        #define BB_PUSH(R,P) do {                                              \
            if (n == cap) { size_t nc = cap ? cap * 2 : 64;                    \
                bb_fr *t = (bb_fr *)realloc(stk, nc * sizeof(bb_fr));          \
                if (!t) { free(stk); return err_bv(S, "oom: and/or eval"); }   \
                stk = t; cap = nc; }                                           \
            stk[n].ref = (R); stk[n].phase = (P); n++;                         \
        } while (0)
        /* operand predicate: cached AND/OR result, else blast the leaf. */
        #define BB_OPND(RF) ( (bb_is_andor_node(S, (RF)) &&                    \
            S->cache[(RF)].bv.size != 0) ? S->cache[(RF)].bv                   \
                                         : bb_predicate(S, (RF)) )
        BB_PUSH(ref, 0);
        while (n > 0) {
            bb_fr it = stk[--n];
            const ExprBinary *nb =
                (const ExprBinary *)POOL_PTR(S->problem, it.ref);
            if (it.phase == 0) {
                if (S->cache[it.ref].bv.size != 0) continue;   /* shared: done */
                BB_PUSH(it.ref, 1);                            /* combine later */
                if (bb_is_andor_node(S, nb->rhs)) BB_PUSH(nb->rhs, 0);
                if (bb_is_andor_node(S, nb->lhs)) BB_PUSH(nb->lhs, 0); /* lhs 1st */
            } else {
                dvs_bv_t a = BB_OPND(nb->lhs);
                if (S->had_error) { free(stk); return a; }
                dvs_bv_t c = BB_OPND(nb->rhs);
                if (S->had_error) { free(stk); return c; }
                S->cache[it.ref].bv = (nb->op == DVS_BIN_AND)
                    ? dvs_bb_and(S->bb, a, c) : dvs_bb_or(S->bb, a, c);
            }
        }
        #undef BB_PUSH
        #undef BB_OPND
        free(stk);
        return S->cache[ref].bv;
    }
    case DVS_BIN_EQ:
    case DVS_BIN_NEQ: {
        /* Recurse with hint=0 on lhs to learn its natural width, then size
         * rhs to match. */
        dvs_bv_t l = bb_expr(S, b->lhs, 0);
        if (S->had_error) return l;
        dvs_bv_t r = bb_expr(S, b->rhs, l.size);
        if (S->had_error) return r;
        uint32_t w = max_w(l.size, r.size);
        l = ext_ref(S, l, b->lhs, w);
        r = ext_ref(S, r, b->rhs, w);
        dvs_bv_t eq = bb_word_eq(S, b->lhs, b->rhs, l, r);
        if (b->op == DVS_BIN_NEQ) eq = dvs_bb_not(S->bb, eq);
        return eq;
    }
    case DVS_BIN_LT:
    case DVS_BIN_LTE:
    case DVS_BIN_GT:
    case DVS_BIN_GTE: {
        dvs_bv_t l = bb_expr(S, b->lhs, 0);
        if (S->had_error) return l;
        dvs_bv_t r = bb_expr(S, b->rhs, l.size);
        if (S->had_error) return r;
        /* SV: the comparison is signed only when both sides are; each side
         * is first extended by its own signedness. */
        int is_signed = bb_signed(S, b->lhs, 0) && bb_signed(S, b->rhs, 0);
        uint32_t w = max_w(l.size, r.size);
        l = ext_ref(S, l, b->lhs, w);
        r = ext_ref(S, r, b->rhs, w);
        dvs_bv_t lt;
        switch (b->op) {
        case DVS_BIN_LT:  lt = is_signed ? dvs_bb_slt(S->bb, l, r) : dvs_bb_ult(S->bb, l, r); break;
        case DVS_BIN_LTE: lt = is_signed ? dvs_bb_slt(S->bb, l, r) : dvs_bb_ult(S->bb, l, r);
                      lt = dvs_bb_or(S->bb, lt, dvs_bb_eq(S->bb, l, r)); break;
        case DVS_BIN_GT:  lt = is_signed ? dvs_bb_slt(S->bb, r, l) : dvs_bb_ult(S->bb, r, l); break;
        case DVS_BIN_GTE: lt = is_signed ? dvs_bb_slt(S->bb, r, l) : dvs_bb_ult(S->bb, r, l);
                      lt = dvs_bb_or(S->bb, lt, dvs_bb_eq(S->bb, l, r)); break;
        default: lt = err_bv(S, "unreachable cmp"); break;
        }
        return lt;
    }
    case DVS_BIN_LSHIFT:
    case DVS_BIN_RSHIFT:
    case DVS_BIN_ASHR: {
        /* SV elaboration turns a signed `>>` into an explicit unsigned
         * (logical) shift between two casts, so a signed operand of `>>` here
         * means an un-elaborated problem: defer rather than guess. A `>>>`
         * (DVS_BIN_ASHR) is arithmetic exactly when its operand is signed. */
        if (b->op == DVS_BIN_RSHIFT && bb_signed(S, b->lhs, 0))
            S->had_unsupported = 1;
        int arith = (b->op == DVS_BIN_ASHR) && bb_signed(S, b->lhs, 0);
        dvs_bv_t l = bb_expr(S, b->lhs, hint);
        if (S->had_error) return l;
        dvs_bv_t r = bb_expr(S, b->rhs, l.size);
        if (S->had_error) return r;
        if (!arith) {
            dvs_bv_t sh = bb_shift(S, b->op == DVS_BIN_LSHIFT, l, r);
            if (b->op != DVS_BIN_LSHIFT) dvs_bbword_note_shr(S->word, l, r, sh);
            return sh;
        }
        /* Arithmetic shift: ~(~x >> s) when x is negative, x >> s otherwise
         * -- which also gives all sign bits for s >= width. */
        dvs_bv_t neg = dvs_bb_not(S->bb, bb_shift(S, 0, dvs_bb_not(S->bb, l), r));
        dvs_bv_t pos = bb_shift(S, 0, l, r);
        return dvs_bb_ite(S->bb, l.bits[0], neg, pos);   /* bits[0] = MSB */
    }
    case DVS_BIN_ADD:
    case DVS_BIN_SUB:
    case DVS_BIN_MUL:
    case DVS_BIN_BAND:
    case DVS_BIN_BOR:
    case DVS_BIN_BXOR: {
        dvs_bv_t l = bb_expr(S, b->lhs, hint);
        if (S->had_error) return l;
        dvs_bv_t r = bb_expr(S, b->rhs, l.size > hint ? l.size : hint);
        if (S->had_error) return r;
        uint32_t w = max_w(l.size, r.size);
        if (hint > w) w = hint;
        l = ext_ref(S, l, b->lhs, w);
        r = ext_ref(S, r, b->rhs, w);
        switch (b->op) {
        case DVS_BIN_ADD: {
            dvs_bv_t sum = dvs_bbsum_add(S->sum, S->bb, l, r,
                                 S->uses && ref < S->uses_cap && S->uses[ref] <= 1);
            dvs_bbword_note_add(S->word, l, r, sum);
            return sum;
        }
        case DVS_BIN_SUB:  return dvs_bb_sub(S->bb, l, r);
        case DVS_BIN_MUL:  return dvs_bbword_mul(S->word, S->bb, l, r);
        case DVS_BIN_BAND: return dvs_bb_and(S->bb, l, r);
        case DVS_BIN_BOR:  return dvs_bb_or (S->bb, l, r);
        case DVS_BIN_BXOR: return dvs_bbword_xor(S->word, S->bb, l, r);
        default: break;
        }
        return err_bv(S, "unreachable arith");
    }
    case DVS_BIN_DIV:
    case DVS_BIN_MOD: {
        /* Unsigned integer division/modulo only. Signed div/mod would need
         * the SMT-LIB signed-div semantics (round-toward-zero), unimplemented.
         * A-4 guard: if either operand is signed, mark the problem unsupported
         * so check() returns DVS_BB_UNKNOWN and the caller defers (the primary
         * engine / Boolector handle signed div correctly) rather than computing
         * a wrong unsigned quotient. */
        int sgn = bb_signed(S, b->lhs, 0) && bb_signed(S, b->rhs, 0);
        dvs_bv_t l = bb_expr(S, b->lhs, hint);
        if (S->had_error) return l;
        dvs_bv_t r = bb_expr(S, b->rhs, l.size > hint ? l.size : hint);
        if (S->had_error) return r;
        uint32_t w = max_w(l.size, r.size);
        if (hint > w) w = hint;
        l = ext_ref(S, l, b->lhs, w);
        r = ext_ref(S, r, b->rhs, w);
        if (!sgn) {
            if (b->op == DVS_BIN_DIV) return dvs_bb_udiv(S->bb, l, r);
            dvs_bv_t m = dvs_bb_urem(S->bb, l, r);
            dvs_bbword_note_urem(S->word, l, r, m);
            return m;
        }
        /* Signed: truncate toward zero. Divide the magnitudes; the quotient
         * is negative when the signs differ, the remainder takes the
         * dividend's sign. MIN / -1 wraps to MIN, its remainder is 0. */
        {
            dvs_aig_node_t sa = l.bits[0], sb = r.bits[0];   /* MSB-first */
            dvs_bv_t al = dvs_bb_ite(S->bb, sa, dvs_bb_neg(S->bb, l), l);
            dvs_bv_t ar = dvs_bb_ite(S->bb, sb, dvs_bb_neg(S->bb, r), r);
            if (b->op == DVS_BIN_DIV) {
                dvs_bv_t q = dvs_bb_udiv(S->bb, al, ar);
                dvs_bv_t diff = dvs_bb_xor(S->bb,
                    dvs_bb_extract(S->bb, l, w - 1, w - 1),
                    dvs_bb_extract(S->bb, r, w - 1, w - 1));
                return dvs_bb_ite(S->bb, diff.bits[0], dvs_bb_neg(S->bb, q), q);
            }
            dvs_bv_t m = dvs_bb_urem(S->bb, al, ar);
            return dvs_bb_ite(S->bb, sa, dvs_bb_neg(S->bb, m), m);
        }
    }
    default:
        return err_bv(S, "unknown dvs_binop_t");
    }
}

static BB_NOINLINE dvs_bv_t bb_unary(dvs_bbsolver_t *S, const ExprUnary *u, uint32_t hint) {
    switch (u->op) {
    case DVS_UN_NOT: {
        dvs_bv_t a = bb_predicate(S, u->operand);
        if (S->had_error) return a;
        return dvs_bb_not(S->bb, a);
    }
    case DVS_UN_NEG: {
        dvs_bv_t a = bb_expr(S, u->operand, hint);
        if (S->had_error) return a;
        return dvs_bb_neg(S->bb, a);
    }
    case DVS_UN_INVERT: {
        dvs_bv_t a = bb_expr(S, u->operand, hint);
        if (S->had_error) return a;
        return dvs_bb_not(S->bb, a);
    }
    }
    return err_bv(S, "unknown dvs_unop_t");
}

static BB_NOINLINE dvs_bv_t bb_ite(dvs_bbsolver_t *S, const ExprITE *e, uint32_t hint) {
    dvs_bv_t c = bb_predicate(S, e->cond);
    if (S->had_error) return c;
    dvs_bv_t t = bb_expr(S, e->then_e, hint);
    if (S->had_error) return t;
    dvs_bv_t f = bb_expr(S, e->else_e, t.size > hint ? t.size : hint);
    if (S->had_error) return f;
    uint32_t w = max_w(t.size, f.size);
    if (hint > w) w = hint;
    t = ext_ref(S, t, e->then_e, w);
    f = ext_ref(S, f, e->else_e, w);
    return dvs_bb_ite(S->bb, c.bits[0], t, f);
}

static BB_NOINLINE dvs_bv_t bb_in_range(dvs_bbsolver_t *S, const ExprInRange *r) {
    /* in_range(value, lo, hi) := value >= lo AND value <= hi.
     * For signedness, peek at `value` like in cmp. */
    dvs_bv_t v = bb_expr(S, r->value, 0);
    if (S->had_error) return v;
    dvs_bv_t lo = bb_expr(S, r->lo, v.size);
    if (S->had_error) return lo;
    dvs_bv_t hi = bb_expr(S, r->hi, v.size);
    if (S->had_error) return hi;
    uint32_t w = max_w(max_w(v.size, lo.size), hi.size);
    /* Elaboration keeps a membership node only when every bound shares the
     * value's signedness, so one flag orders all three. */
    int is_signed = bb_signed(S, r->value, 0);
    v  = ext_ref(S, v,  r->value, w);
    lo = ext_ref(S, lo, r->lo, w);
    hi = ext_ref(S, hi, r->hi, w);
    /* v >= lo  <=>  NOT (v <_u lo)   (and signed analog) */
    dvs_bv_t v_lt_lo = is_signed ? dvs_bb_slt(S->bb, v, lo) : dvs_bb_ult(S->bb, v, lo);
    dvs_bv_t hi_lt_v = is_signed ? dvs_bb_slt(S->bb, hi, v) : dvs_bb_ult(S->bb, hi, v);
    dvs_bv_t ge_lo = dvs_bb_not(S->bb, v_lt_lo);
    dvs_bv_t le_hi = dvs_bb_not(S->bb, hi_lt_v);
    return dvs_bb_and(S->bb, ge_lo, le_hi);
}

static BB_NOINLINE dvs_bv_t bb_in_set(dvs_bbsolver_t *S, dvs_expr_t ref) {
    ExprInSet *node = (ExprInSet *)POOL_PTR(S->problem, ref);
    dvs_expr_t *elems = expr_in_set_elems(S->problem, ref);
    dvs_bv_t v = bb_expr(S, node->value, 0);
    if (S->had_error) return v;
    /* OR of (v == e[i]) */
    dvs_bv_t acc = { NULL, 0 };
    for (uint32_t i = 0; i < node->n_elems; i++) {
        dvs_bv_t ei = bb_expr(S, elems[i], v.size);
        if (S->had_error) return ei;
        uint32_t w = max_w(v.size, ei.size);
        dvs_bv_t vw = ext_ref(S, v, node->value, w);
        dvs_bv_t ew = ext_ref(S, ei, elems[i], w);
        dvs_bv_t eq = dvs_bb_eq(S->bb, vw, ew);
        if (i == 0) acc = eq;
        else        acc = dvs_bb_or(S->bb, acc, eq);
    }
    if (acc.size == 0) {
        /* Empty set is always false. */
        acc = dvs_bb_value_u64(S->bb, 1, 0);
    }
    return acc;
}

static BB_NOINLINE dvs_bv_t bb_in_ranges(dvs_bbsolver_t *S, dvs_expr_t ref) {
    /* OR over ranges of (value >= lo_i AND value <= hi_i). */
    ExprInRanges *node = (ExprInRanges *)POOL_PTR(S->problem, ref);
    dvs_expr_t *los = expr_in_ranges_los(S->problem, ref);
    dvs_expr_t *his = expr_in_ranges_his(S->problem, ref);
    dvs_bv_t v = bb_expr(S, node->value, 0);
    if (S->had_error) return v;
    int is_signed = bb_signed(S, node->value, 0);
    dvs_bv_t acc = { NULL, 0 };
    for (uint32_t i = 0; i < node->n_ranges; i++) {
        dvs_bv_t lo = bb_expr(S, los[i], v.size);
        if (S->had_error) return lo;
        dvs_bv_t hi = bb_expr(S, his[i], v.size);
        if (S->had_error) return hi;
        uint32_t w = max_w(max_w(v.size, lo.size), hi.size);
        dvs_bv_t vw = ext_ref(S, v, node->value, w);
        dvs_bv_t lw = ext_ref(S, lo, los[i], w);
        dvs_bv_t hw = ext_ref(S, hi, his[i], w);
        dvs_bv_t v_lt_lo = is_signed ? dvs_bb_slt(S->bb, vw, lw) : dvs_bb_ult(S->bb, vw, lw);
        dvs_bv_t hi_lt_v = is_signed ? dvs_bb_slt(S->bb, hw, vw) : dvs_bb_ult(S->bb, hw, vw);
        dvs_bv_t in_i = dvs_bb_and(S->bb, dvs_bb_not(S->bb, v_lt_lo),
                                          dvs_bb_not(S->bb, hi_lt_v));
        acc = (i == 0) ? in_i : dvs_bb_or(S->bb, acc, in_i);
    }
    if (acc.size == 0) acc = dvs_bb_value_u64(S->bb, 1, 0);  /* empty -> false */
    return acc;
}

static BB_NOINLINE dvs_bv_t bb_extend(dvs_bbsolver_t *S, const ExprExtend *e) {
    /* Also EXPR_SV_CAST (same layout): the bits are the same, only the
     * signedness the result is read with differs, and bb_signed knows it. */
    dvs_bv_t op = bb_expr(S, e->operand, e->from_bits);
    if (S->had_error) return op;
    if (op.size > e->from_bits) {
        /* Truncate down — shouldn't happen but be safe. */
        op = dvs_bb_extract(S->bb, op, e->from_bits - 1, 0);
    } else if (op.size < e->from_bits) {
        op = ext_ref(S, op, e->operand, e->from_bits);
    }
    if (e->to_bits < e->from_bits) {
        dvs_bv_t lo = dvs_bb_extract(S->bb, op, e->to_bits - 1, 0);
        dvs_bbword_note_extract(S->word, op, e->to_bits - 1, 0, lo);
        return lo;
    }
    uint32_t n = (uint32_t)e->to_bits - (uint32_t)e->from_bits;
    if (n == 0) return op;
    return e->sign_extend ? dvs_bb_sign_ext(S->bb, op, n)
                          : dvs_bb_zero_ext(S->bb, op, n);
}

static BB_NOINLINE dvs_bv_t bb_extract(dvs_bbsolver_t *S, const ExprExtract *e) {
    /* operand width must be at least hi_bit + 1 — infer from operand. */
    dvs_bv_t op = bb_expr(S, e->operand, 0);
    if (S->had_error) return op;
    if (op.size <= e->hi_bit) {
        /* zero-extend operand up to hi_bit+1 bits */
        op = zext_to(S, op, e->hi_bit + 1);
    }
    dvs_bv_t x = dvs_bb_extract(S->bb, op, e->hi_bit, e->lo_bit);
    dvs_bbword_note_extract(S->word, op, e->hi_bit, e->lo_bit, x);
    return x;
}

static BB_NOINLINE dvs_bv_t bb_concat(dvs_bbsolver_t *S, const ExprConcat *e) {
    dvs_bv_t lo = bb_expr(S, e->lo, e->lo_width);
    if (S->had_error) return lo;
    if (lo.size > e->lo_width) {
        lo = dvs_bb_extract(S->bb, lo, e->lo_width - 1, 0);
    } else if (lo.size < e->lo_width) {
        lo = zext_to(S, lo, e->lo_width);
    }
    dvs_bv_t hi = bb_expr(S, e->hi, 0);
    if (S->had_error) return hi;
    return dvs_bb_concat(S->bb, hi, lo);
}

/* The aggregate relations. Each is a truth value (1 bit) with the meaning
 * the SMT-LIB2 emitter gives it (dvs_smt2_emit.c, _aggregate), and the
 * primary engine enforces when the relation is a top-level constraint. */

/* Smallest width holding the unsigned value n. */
static uint32_t bb_bits_for(uint64_t n) {
    uint32_t b = 1;
    while (b < 64 && (n >> b)) b++;
    return b;
}

/* `result == base[index]`: some element i has index == i and result equal
 * to it, each comparison in its SV context (the index against an int
 * literal, at least 32 bits; result and element signed only if both are).
 * An index outside the array, and so any index of an empty one, is false.
 * One equality per element, then their disjunction: the multiplexer a
 * select of a fixed-size array is. */
static dvs_bv_t bb_array_select(dvs_bbsolver_t *S, const ExprArraySelect *as) {
    dvs_bv_t any = dvs_bb_value_u64(S->bb, 1, 0);
    if (as->n_elems == 0) return any;
    dvs_bv_t idx = bb_expr(S, as->index, 0);
    if (S->had_error) return idx;
    dvs_bv_t res = bb_expr(S, as->result, 0);
    if (S->had_error) return res;
    uint32_t iw = idx.size > 32 ? (uint32_t)idx.size : 32;
    idx = ext_ref(S, idx, as->index, iw);
    const int rs = bb_signed(S, as->result, 0);
    for (uint32_t i = 0; i < as->n_elems; i++) {
        uint32_t id = as->base_var_id + i;
        if (id >= S->n_vars || !S->vars[id].defined) {
            S->had_unsupported = 1;
            return dvs_bb_value_u64(S->bb, 1, 0);
        }
        dvs_bv_t el = bv_for_var(S, id);
        if (S->had_error) return el;
        const int sg = rs && S->vars[id].is_signed;
        uint32_t w = res.size > el.size ? (uint32_t)res.size : (uint32_t)el.size;
        dvs_bv_t r = sg ? sext_to(S, res, w) : zext_to(S, res, w);
        dvs_bv_t e = sg ? sext_to(S, el, w) : zext_to(S, el, w);
        dvs_bv_t hit = dvs_bb_and(S->bb,
                                  dvs_bb_eq(S->bb, idx, dvs_bb_value_u64(S->bb, iw, i)),
                                  dvs_bb_eq(S->bb, r, e));
        any = dvs_bb_or(S->bb, any, hit);
    }
    return any;
}

/* `result == x0 + x1 + ...` as integers: every term extended by its own
 * signedness to a width the sum cannot overflow. */
static dvs_bv_t bb_sum(dvs_bbsolver_t *S, const ExprSum *es) {
    const dvs_expr_t *xs = (const dvs_expr_t *)(es + 1);   /* refs follow the node */
    dvs_bv_t res = bb_expr(S, es->result, 0);
    if (S->had_error) return res;
    uint32_t mw = res.size;
    dvs_bv_t *t = es->n_vars ? (dvs_bv_t *)malloc(es->n_vars * sizeof(dvs_bv_t)) : NULL;
    if (es->n_vars && !t) return err_bv(S, "out of memory");
    for (uint32_t i = 0; i < es->n_vars; i++) {
        t[i] = bb_expr(S, xs[i], 0);
        if (S->had_error) { dvs_bv_t e = t[i]; free(t); return e; }
        if (t[i].size > mw) mw = t[i].size;
    }
    uint32_t W = mw + bb_bits_for((uint64_t)es->n_vars + 1) + 1;
    if (W > DVS_MAX_WIDTH) { free(t); S->had_unsupported = 1; return dvs_bb_value_u64(S->bb, 1, 0); }
    dvs_bv_t acc = dvs_bb_value_u64(S->bb, W, 0);
    for (uint32_t i = 0; i < es->n_vars; i++)
        acc = dvs_bb_add(S->bb, acc, ext_ref(S, t[i], xs[i], (uint32_t)W));
    free(t);
    return dvs_bb_eq(S->bb, ext_ref(S, res, es->result, (uint32_t)W), acc);
}

/* popcount(x) as a W-bit sum of x's bits; W must hold it (callers ensure). */
static dvs_bv_t bb_popcount_bits(dvs_bbsolver_t *S, dvs_bv_t x, uint32_t W) {
    dvs_bv_t acc = dvs_bb_value_u64(S->bb, W, 0);
    for (uint32_t i = 0; i < x.size; i++)
        acc = dvs_bb_add(S->bb, acc,
                         zext_to(S, dvs_bb_extract(S->bb, x, i, i), W));
    return acc;
}

/* The value popcount(operand) in W bits, for a var try_record_popcount
 * defined (popcount fits W there). */
static dvs_bv_t bb_popcount(dvs_bbsolver_t *S, dvs_expr_t operand, uint32_t W) {
    dvs_bv_t x = bb_expr(S, operand, 0);
    if (S->had_error) return x;
    uint32_t xb = bb_bits_for(x.size) + 1;
    dvs_bv_t acc = bb_popcount_bits(S, x, W > xb ? W : xb);
    return acc.size > W ? dvs_bb_extract(S->bb, acc, W - 1, 0) : acc;
}

/* `result == popcount(x)`: the bits of x, each widened, summed. */
static dvs_bv_t bb_countones(dvs_bbsolver_t *S, const ExprCountones *ec) {
    dvs_bv_t x = bb_expr(S, ec->operand, 0);
    if (S->had_error) return x;
    dvs_bv_t res = bb_expr(S, ec->result, 0);
    if (S->had_error) return res;
    uint32_t xb = bb_bits_for(x.size);
    uint32_t W = (uint32_t)((res.size > xb ? res.size : xb) + 1);
    dvs_bv_t acc = bb_popcount_bits(S, x, W);
    return dvs_bb_eq(S->bb, ext_ref(S, res, ec->result, W), acc);
}

/* `result == clog2(x)`, x read unsigned: the least k with x <= 2^k (0 for
 * x <= 1), as a chain from the widest k down. */
static dvs_bv_t bb_clog2(dvs_bbsolver_t *S, const ExprClog2 *ec) {
    dvs_bv_t x = bb_expr(S, ec->operand, 0);
    if (S->had_error) return x;
    dvs_bv_t res = bb_expr(S, ec->result, 0);
    if (S->had_error) return res;
    if (x.size > 64) { S->had_unsupported = 1; return dvs_bb_value_u64(S->bb, 1, 0); }
    uint32_t xb = bb_bits_for(x.size);
    uint32_t W = (uint32_t)((res.size > xb ? res.size : xb) + 1);
    uint32_t XW = (uint32_t)(x.size + 1);
    dvs_bv_t xx = zext_to(S, x, XW);
    dvs_bv_t val = dvs_bb_value_u64(S->bb, W, x.size);
    for (uint32_t k = x.size; k-- > 0;) {
        dvs_bv_t gt = dvs_bb_ult(S->bb, dvs_bb_value_u64(S->bb, XW, (uint64_t)1 << k), xx);
        val = dvs_bb_ite(S->bb, gt.bits[0], val, dvs_bb_value_u64(S->bb, W, k));
    }
    return dvs_bb_eq(S->bb, ext_ref(S, res, ec->result, W), val);
}

static dvs_bv_t bb_expr(dvs_bbsolver_t *S, dvs_expr_t ref, uint32_t hint_width) {
    if (S->had_error) { dvs_bv_t e = {NULL, 0}; return e; }
    if (ref == EXPR_NULL) return err_bv(S, "EXPR_NULL");

    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return err_bv(S, "bad dvs_expr_t");

    /* Memoization: most ExprKinds have a hint-independent natural width.
     * Cache them by dvs_expr_t alone. EXPR_CONST is hint-dependent (sized to
     * caller context) so it stays uncached and cheap. DV_BB_NO_MEMO
     * disables the cache for debugging. */
    int memoize = (*kp != EXPR_CONST) && !S->no_memo;
    if (memoize && ref < S->cache_cap && S->cache[ref].bv.size != 0) {
        return S->cache[ref].bv;
    }
    /* Nested deeper than the stack can hold: unsupported, as for SUM below. */
    if (dvs_stack_low(&S->sg)) {
        S->had_unsupported = S->too_deep = 1;
        return dvs_bb_value_u64(S->bb, 1, 0);
    }

    dvs_bv_t out;
    switch (*kp) {
    case EXPR_CONST:    return bb_const(S, (ExprConst *)kp, hint_width);
    case EXPR_CONST_WIDE: out = bb_const_wide(S, (ExprConstWide *)kp); break;
    case EXPR_VAR:      out = bb_var_expr(S, (ExprVar *)kp); break;
    case EXPR_BINARY:   out = bb_binary(S, (ExprBinary *)kp, ref, hint_width); break;
    case EXPR_UNARY:    out = bb_unary(S, (ExprUnary *)kp, hint_width); break;
    case EXPR_ITE:      out = bb_ite(S, (ExprITE *)kp, hint_width); break;
    case EXPR_IN_RANGE: out = bb_in_range(S, (ExprInRange *)kp); break;
    case EXPR_IN_SET:   out = bb_in_set(S, ref); break;
    case EXPR_IN_RANGES: out = bb_in_ranges(S, ref); break;
    case EXPR_EXTEND:   out = bb_extend(S, (ExprExtend *)kp); break;
    case EXPR_SV_CAST:  out = bb_extend(S, (ExprExtend *)(void *)kp); break;
    case EXPR_EXTRACT:  out = bb_extract(S, (ExprExtract *)kp); break;
    case EXPR_CONCAT:   out = bb_concat(S, (ExprConcat *)kp); break;
    case EXPR_SUM:      out = bb_sum(S, (ExprSum *)kp); break;
    case EXPR_COUNTONES: out = bb_countones(S, (ExprCountones *)kp); break;
    case EXPR_CLOG2:    out = bb_clog2(S, (ExprClog2 *)kp); break;
    case EXPR_ARRAY_SELECT: out = bb_array_select(S, (ExprArraySelect *)kp); break;
    default:
        fprintf(stderr, "[dvs_bbsolver] error: unknown ExprKind %d\n", (int)*kp);
        S->had_error = 1;
        { dvs_bv_t e = { NULL, 0 }; return e; }
    }

    if (memoize && !S->had_error && ref < S->cache_cap) {
        S->cache[ref].bv = out;
    }
    return out;
}

static dvs_bv_t bb_predicate(dvs_bbsolver_t *S, dvs_expr_t ref) {
    dvs_bv_t r = bb_expr(S, ref, 1);
    if (S->had_error) return r;
    if (r.size != 1) {
        /* Reduce to (r != 0) */
        dvs_bv_t zero = dvs_bb_value_u64(S->bb, r.size, 0);
        dvs_bv_t eq0 = dvs_bb_eq(S->bb, r, zero);
        r = dvs_bb_not(S->bb, eq0);
    }
    return r;
}

/* ----------------------------- bound assertions --------------------------- */

/* Encode `lo <= v <= hi` for variable v of width w. If is_signed, use signed
 * comparison; else unsigned. Caller asserts at top level. Returns 1-bit BV
 * or no-op (returns NULL bits) if bounds are vacuous. */
static int assert_var_bounds(dvs_bbsolver_t *S, uint32_t var_id) {
    bb_var_t *v = &S->vars[var_id];
    /* Natural full range for the width. */
    int64_t natural_lo, natural_hi;
    if (v->is_signed) {
        if (v->width >= 64) { natural_lo = INT64_MIN; natural_hi = INT64_MAX; }
        else {
            natural_lo = -((int64_t)1 << (v->width - 1));
            natural_hi =  ((int64_t)1 << (v->width - 1)) - 1;
        }
    } else {
        natural_lo = 0;
        natural_hi = (v->width >= 64) ? INT64_MAX : (((int64_t)1 << v->width) - 1);
    }
    int need_lo = v->lo != natural_lo;
    int need_hi = v->hi != natural_hi;
    if (!need_lo && !need_hi) return 0;

    dvs_bv_t bv = bv_for_var(S, var_id);
    /* bb_value_i64 sign/zero-extends correctly for width > 64 (value_u64 alone
     * would zero-fill bits 64+, corrupting a negative bound). */
    dvs_bv_t lo = bb_value_i64(S, v->width, v->lo, v->is_signed);
    dvs_bv_t hi = bb_value_i64(S, v->width, v->hi, v->is_signed);

    dvs_bv_t pred = dvs_bb_value_u64(S->bb, 1, 1);
    if (need_lo) {
        dvs_bv_t v_lt_lo = v->is_signed ? dvs_bb_slt(S->bb, bv, lo)
                                        : dvs_bb_ult(S->bb, bv, lo);
        dvs_bv_t ge_lo = dvs_bb_not(S->bb, v_lt_lo);
        pred = dvs_bb_and(S->bb, pred, ge_lo);
    }
    if (need_hi) {
        dvs_bv_t hi_lt_v = v->is_signed ? dvs_bb_slt(S->bb, hi, bv)
                                        : dvs_bb_ult(S->bb, hi, bv);
        dvs_bv_t le_hi = dvs_bb_not(S->bb, hi_lt_v);
        pred = dvs_bb_and(S->bb, pred, le_hi);
    }
    assert_top(S, pred.bits[0]);
    return 0;
}

/* ----------------------------- public API --------------------------------- */

/* Select the SAT backend for a fresh bbsolver. Defaults to KISSAT (the
 * historical one-shot behavior). DV_SAT_BACKEND=cadical|bb forces the
 * incremental CaDiCaL backend (audit/benchmark knob, mirroring DV_ENGINE);
 * =kissat forces kissat. When CaDiCaL is not compiled in, a cadical request
 * transparently falls back to kissat (see dvs_sat_new_backend). This is the
 * single creation site; interaction-shape routing (BMC/incremental → cadical)
 * layers on top of this later. */
/* prefer_cadical: -1 = env-driven (DV_SAT_BACKEND, default kissat), 0 = force
 * kissat, 1 = force CaDiCaL. A forced CaDiCaL request still falls back to
 * kissat transparently when CaDiCaL is not compiled in (see
 * dvs_sat_new_backend); callers that require incrementality re-check via
 * dvs_bbsolver_is_incremental. */
static dvs_sat_t *bb_new_sat(dvs_alloc_t *alloc, int prefer_cadical) {
    if (prefer_cadical < 0) {
        const char *e = getenv("DV_SAT_BACKEND");
        if (e && (strcmp(e, "cadical") == 0 || strcmp(e, "cd") == 0 ||
                  strcmp(e, "bb") == 0)) {
            prefer_cadical = 1;
        } else {
            prefer_cadical = 0;
        }
    }
    /* Lazy: a check answered by simulation or a fast path never builds the
     * backend. */
    dvs_sat_t *sat = dvs_sat_new_lazy(alloc, prefer_cadical ? DVS_SAT_BACKEND_CADICAL
                                                            : DVS_SAT_BACKEND_KISSAT);
    if (sat && prefer_cadical == 2) dvs_sat_set_sampling(sat, 1);
    return sat;
}

static dvs_bbsolver_t *bbsolver_new_ex(dvs_alloc_t *alloc, dvs_problem_t *problem,
                                       int prefer_cadical) {
    if (!problem) return NULL;
    dvs_bbsolver_t *S = (dvs_bbsolver_t *)xalloc(alloc, sizeof(*S));
    if (!S) return NULL;
    memset(S, 0, sizeof(*S));
    S->alloc = alloc;
    S->no_memo = getenv("DV_BB_NO_MEMO") != NULL;
    /* Builder-API expressions follow SystemVerilog sizing rules: blast the
     * elaborated form, where those rules are explicit (dvs_sv.h). An SMT-LIB
     * problem is flagged explicit already and is used as-is. */
    {
        int err = 0;
        dvs_problem_t *esp = dvs_sv_elaborate(problem, NULL, NULL, &err);
        if (err) {
            /* Not in explicit form: blasting it would apply different sizing
             * rules from the problem's, or read an undeclared variable. */
            xfree(alloc, S, sizeof(*S));
            return NULL;
        }
        if (esp != problem) {
            S->orig_problem = problem;
            S->orig_synced = problem->pool.used;
        }
        problem = esp;
    }
    S->problem = problem;
    S->aig = dvs_aig_new(alloc);
    S->sat = bb_new_sat(alloc, prefer_cadical);
    S->cnf = dvs_aig_cnf_new(alloc, S->aig, S->sat);
    S->bb  = dvs_bitblast_new(alloc, S->aig);
    S->sum = dvs_bbsum_new(alloc);
    S->word = dvs_bbword_new(alloc);
    if (!S->aig || !S->sat || !S->cnf || !S->bb) {
        dvs_bbsolver_free(S);
        return NULL;
    }

    /* Collect variables — find the max var_id, allocate table. */
    uint32_t max_id = 0;
    dvs_expr_t cur = problem->vars_head;
    while (cur != EXPR_NULL) {
        VarSpec *vs = (VarSpec *)POOL_PTR(problem, cur);
        if (vs->var_id > max_id) max_id = vs->var_id;
        cur = vs->next;
    }
    uint32_t n = problem->n_vars > 0 ? max_id + 1 : 0;
    S->n_vars = n;
    if (n > 0) {
        S->vars = (bb_var_t *)xalloc(alloc, n * sizeof(bb_var_t));
        memset(S->vars, 0, n * sizeof(bb_var_t));
        cur = problem->vars_head;
        while (cur != EXPR_NULL) {
            VarSpec *vs = (VarSpec *)POOL_PTR(problem, cur);
            bb_var_t *v = &S->vars[vs->var_id];
            v->width = vs->width;
            v->is_signed = vs->is_signed;
            v->defined = 1;
            v->lo = vs->lo;
            v->hi = vs->hi;
            cur = vs->next;
        }
    }

    /* Memoization cache: sized to the pool's current used range, since
     * ExprRefs are byte offsets and never exceed used. Zero-initialized
     * (bv.size == 0 marks "not yet bit-blasted"). */
    S->cache_cap = dvs_pool_used(&problem->pool);
    if (S->cache_cap > 0) {
        S->cache = (bb_cache_entry_t *)xalloc(alloc,
                                              S->cache_cap * sizeof(bb_cache_entry_t));
        if (S->cache) memset(S->cache, 0, S->cache_cap * sizeof(bb_cache_entry_t));
    }
    return S;
}

dvs_bbsolver_t *dvs_bbsolver_new(dvs_alloc_t *alloc, dvs_problem_t *problem) {
    return bbsolver_new_ex(alloc, problem, /*prefer_cadical=*/-1);
}

dvs_bbsolver_t *dvs_bbsolver_new_backend(dvs_alloc_t *alloc,
                                         dvs_problem_t *problem,
                                         int prefer_cadical) {
    return bbsolver_new_ex(alloc, problem, prefer_cadical);
}

void dvs_bbsolver_free(dvs_bbsolver_t *S) {
    if (!S) return;
    if (S->orig_problem) dvs_sv_release(S->orig_problem, S->problem);
    if (S->bb)  dvs_bitblast_free(S->bb);
    dvs_bbsum_free(S->sum);
    dvs_bbword_free(S->word);
    if (S->cnf) dvs_aig_cnf_free(S->cnf);
    if (S->sat) dvs_sat_free(S->sat);
    if (S->aig) dvs_aig_free(S->aig);
    xfree(S->alloc, S->vars, S->n_vars * sizeof(bb_var_t));
    xfree(S->alloc, S->cache, S->cache_cap * sizeof(bb_cache_entry_t));
    xfree(S->alloc, S->subst, S->n_vars * sizeof(dvs_expr_t));
    xfree(S->alloc, S->resolving, S->n_vars * sizeof(uint8_t));
    free(S->reach_mark);
    free(S->rhs_var);
    free(S->slice_head);
    free(S->uses);
    free(S->fo_seen);
    free(S->frames);
    free(S->new_subst);
    free(S->assert_tags);
    free(S->slices);
    xfree(S->alloc, S->constraint_skip, S->n_constraints * sizeof(uint8_t));
    dvs_components_free(&S->comps);
    free(S->cons_comp);
    xfree(S->alloc, S->assert_nodes, S->asserts_cap * sizeof(dvs_aig_node_t));
    free(S->assert_roots);
    free(S->node_val);
    free(S->psk);
    free(S->div_L); free(S->div_R); free(S->div_fo_off); free(S->div_fo_adj);
    xfree(S->alloc, S, sizeof(*S));
}

static uint64_t _free_var_fill(uint64_t seed, uint32_t var_id, uint32_t limb);

/* Value of AIG literal `lit` under node valuation nv[] (nv indexed by |id|). */
static inline int _lit_val(const int8_t *nv, dvs_aig_node_t lit) {
    if (lit == DVS_AIG_TRUE)  return 1;
    if (lit == DVS_AIG_FALSE) return 0;
    uint32_t i = (uint32_t)(lit < 0 ? -lit : lit);
    int v = nv[i];
    return lit < 0 ? !v : v;
}

/* Binary min-heap over node ids (the worklist of _prop_cone). */
static void _heap_push(uint32_t *h, uint32_t *n, uint32_t v) {
    uint32_t i = (*n)++;
    while (i && h[(i - 1) / 2] > v) { h[i] = h[(i - 1) / 2]; i = (i - 1) / 2; }
    h[i] = v;
}
static uint32_t _heap_pop(uint32_t *h, uint32_t *n) {
    uint32_t top = h[0], v = h[--(*n)], i = 0;
    for (;;) {
        uint32_t c = 2 * i + 1;
        if (c >= *n) break;
        if (c + 1 < *n && h[c + 1] < h[c]) c++;
        if (h[c] >= v) break;
        h[i] = h[c]; i = c;
    }
    if (*n) h[i] = v;
    return top;
}

/* Propagate a just-changed node value through its fanout cone. nv[start] has
 * already been set to its new value; recompute the AND nodes reachable upward
 * (via the fo_off/fo_adj CSR fanout index) until the valuation reaches its fixed
 * point. Only nodes in the changed cone are touched.
 *
 * The worklist is a min-heap on node id. Ids are topological (children before
 * parents), so every node is recomputed at most once, after all of its changed
 * children. A LIFO worklist re-evaluated a node once per changed path reaching
 * it: on reconvergent logic (a bit-blasted divider) that is exponential, and a
 * single flip of a 122-bit packed struct feeding a 32-bit bvudiv never finished.
 * `inq` guards against duplicates and is left all-zero on return.
 *
 * Every changed node id is appended to trail[*nt] so the caller can undo the
 * flip by inverting exactly those nodes. areq[id] marks top-level assertions
 * (bit 0: must be 1, bit 1: must be 0); because each node's value is final when
 * it is computed, the first assertion to take a violating value rejects the
 * flip on the spot, and the rest of the cone is never visited. Returns 1 when
 * every assertion still holds, 0 on a rejection. */
static int _prop_cone(int8_t *nv, const dvs_aig_node_t *L, const dvs_aig_node_t *R,
                      const uint32_t *fo_off, const uint32_t *fo_adj,
                      const uint8_t *areq, uint32_t *wl, uint8_t *inq,
                      uint32_t *trail, uint32_t *nt, uint32_t start) {
    uint32_t n = 0;
    for (uint32_t e = fo_off[start]; e < fo_off[start + 1]; e++)
        if (!inq[fo_adj[e]]) { inq[fo_adj[e]] = 1; _heap_push(wl, &n, fo_adj[e]); }
    while (n) {
        uint32_t pn = _heap_pop(wl, &n);
        inq[pn] = 0;
        int np = _lit_val(nv, L[pn]) & _lit_val(nv, R[pn]);
        if (np == nv[pn]) continue;
        nv[pn] = (int8_t)np;
        trail[(*nt)++] = pn;
        if (areq[pn] & (np ? 2u : 1u)) {
            for (uint32_t i = 0; i < n; i++) inq[wl[i]] = 0;
            return 0;
        }
        for (uint32_t e = fo_off[pn]; e < fo_off[pn + 1]; e++)
            if (!inq[fo_adj[e]]) { inq[fo_adj[e]] = 1; _heap_push(wl, &n, fo_adj[e]); }
    }
    return 1;
}

/* Diversity flip-check: after a SAT solve, randomize the don't-care variable
 * bits. Each rand bit is set toward a seeded-random target; the flip is kept
 * only if EVERY top-level assertion still holds under the updated model. Flips
 * are applied sequentially, so each is verified against the running model and
 * coupled constraints (e.g. a sum bound) are never violated -- the result stays
 * a valid solution, just a more diverse one. node_val[] holds that model for the
 * readback. Each flip is a Gibbs step for the uniform distribution (the bit
 * takes a random value where both values satisfy, else stays), so a model the
 * simulation drew uniformly stays uniform.
 *
 * Cost per candidate bit is one *incremental* re-evaluation: only the flipped
 * bit's fanout cone is repropagated (built once as the fo_off/fo_adj CSR index),
 * not the entire AIG. For the localized cones typical of constraint bits this is
 * far cheaper than the old full O(nodes) re-simulation. Skipped on very large
 * problems (where BMC-style seed 0 is used anyway). */
#define DVS_DIVERSIFY_MAX_NODES 200000u
/* Distinct candidate inputs up to which the flip loop memoizes validity. */
#define DVS_DIVERSIFY_MEMO_BITS 16u
/* Flip sweeps over the candidate bits, each with fresh random targets. The
 * sampler's XOR planes randomize only the low BB_SAMPLE_BITS_PER_VAR bits of
 * each variable; the bits above come from the SAT solver's model, and one
 * sweep moves each bit at most once, so they kept the solver's bias. A 64-bit
 * mixed-sign a < b drew a negative a 14% of the time instead of 25% under
 * CaDiCaL 3.0.0; under 3.0.1 it was 25%, by luck of a changed preprocessing
 * default, while a 128-bit a < b < c gave P(a.msb) = 0.215 instead of 0.125.
 * Each flip is a Gibbs step that preserves the uniform distribution, so more
 * sweeps forget the solver's starting model. Every sweep visits ALL the bits:
 * sweeping only the bits above the planes leaves both shapes exactly as
 * biased as before, because a top bit can only move once the low bits have.
 * Sweeps after the first stop once DVS_DIVERSIFY_SWEEP_BUDGET flip visits are
 * spent, so a large problem pays about one sweep, as before. Only
 * dvs_bbsolver_sample asks for them; the other paths keep one (_diversify). */
#ifndef DVS_DIVERSIFY_SWEEPS
#define DVS_DIVERSIFY_SWEEPS 16u
#endif
#define DVS_DIVERSIFY_SWEEP_BUDGET (1u << 18)

/* Recompute every AND node of nv[] from its inputs, ascending id. Returns 1
 * when every assertion in areq holds; with `stop`, returns 0 at the first
 * violated one (nv is then partly updated). */
static int _eval_all(int8_t *nv, const dvs_aig_node_t *L, const dvs_aig_node_t *R,
                     const uint8_t *areq, uint32_t N, int stop) {
    int ok = 1;
    for (uint32_t id = 1; id <= N; id++) {
        if (L[id]) nv[id] = (int8_t)(_lit_val(nv, L[id]) & _lit_val(nv, R[id]));
        if (areq[id] & (nv[id] ? 2u : 1u)) {
            ok = 0;
            if (stop) return 0;
        }
    }
    return ok;
}

/* The flip loop when the candidate bits cover only k <= DVS_DIVERSIFY_MEMO_BITS
 * distinct inputs but visit them more often than once (builder intermediates
 * alias the bits of the variables they are built from: an 8-bit CRC filter
 * has ~1800 vars over 8 inputs, so each input is visited ~2800 times). A flip
 * is kept exactly when the flipped assignment satisfies every assertion, which
 * depends only on the k candidate inputs (no other input changes here), so
 * valid[] caches that per assignment and the accept/reject sequence is the
 * same as the cone-propagating loop's, given a starting model that satisfies
 * (checked here). Each new assignment costs one full pass, at most 2^k of
 * them, so the caller uses this only when that is at most a quarter of the
 * visits. cand[] holds (base, desired) pairs in visit order, slot[] the
 * input's bit in the assignment. Returns 0, nv unchanged, when the memo
 * cannot be allocated or the start does not satisfy (the caller then runs
 * the plain loop). */
static int _diversify_memo(int8_t *nv, const dvs_aig_node_t *L, const dvs_aig_node_t *R,
                           const uint8_t *areq, uint32_t N,
                           const uint32_t *cand, uint32_t ncand,
                           const uint32_t *inputs, uint32_t k, const uint32_t *slot,
                           uint64_t *work) {
    *work += N;
    if (!_eval_all(nv, L, R, areq, N, /*stop=*/1)) return 0;   /* nv already consistent */
    uint8_t *valid = (uint8_t *)calloc((size_t)1 << k, 1);   /* 0 ?, 1 yes, 2 no */
    if (!valid) return 0;
    uint32_t st = 0;
    for (uint32_t i = 0; i < k; i++) if (nv[inputs[i]]) st |= 1u << i;
    valid[st] = 1;                       /* the starting model satisfies */
    uint32_t nv_st = st;                 /* the assignment nv[] holds; ~0 = stale */
    for (uint32_t c = 0; c < ncand; c++) {
        uint32_t base = cand[2 * c], desired = cand[2 * c + 1];
        uint32_t bit = 1u << slot[base];
        if (((st & bit) != 0) == desired) continue;
        if (areq[base] & (desired ? 2u : 1u)) continue;      /* asserted input */
        uint32_t nst = st ^ bit;
        if (!valid[nst]) {
            for (uint32_t i = 0; i < k; i++) nv[inputs[i]] = (int8_t)((nst >> i) & 1u);
            *work += N;
            int ok = _eval_all(nv, L, R, areq, N, /*stop=*/1);
            valid[nst] = ok ? 1 : 2;
            nv_st = ok ? nst : ~0u;
        }
        if (valid[nst] == 1) st = nst;
    }
    if (nv_st != st) {
        for (uint32_t i = 0; i < k; i++) nv[inputs[i]] = (int8_t)((st >> i) & 1u);
        *work += N;
        _eval_all(nv, L, R, areq, N, /*stop=*/0);
    }
    free(valid);
    return 1;
}

/* `sweeps` flip sweeps (at least one; see DVS_DIVERSIFY_SWEEPS). */
static void _diversify_sweeps(dvs_bbsolver_t *S, uint32_t sweeps) {
    if (S->seed == 0 || !S->aig || S->no_diversify) return;
    uint32_t N = (uint32_t)dvs_aig_num_nodes(S->aig);
    if (N == 0 || N > DVS_DIVERSIFY_MAX_NODES) return;

    dvs_aig_t *m = S->aig;
    /* Plain malloc for these transient per-solve buffers: node_val (nv) is freed
     * in dvs_bbsolver_free, the rest here -- all independent of the arena. */
    int8_t *nv = (int8_t *)malloc((size_t)(N + 1));
    if (!nv) return;
    if (S->div_n != N) {
        /* (Re)build the cached AND children (0 = input/const) and the fanout
         * index (CSR): fo_adj[fo_off[c]..fo_off[c+1]) lists the AND nodes that
         * reference node c as a child. Constants (|child| <= 1, i.e. TRUE=id1 /
         * FALSE) get no edge -- their value never changes. */
        free(S->div_L); free(S->div_R); free(S->div_fo_off); free(S->div_fo_adj);
        S->div_fo_adj = NULL; S->div_n = 0;
        S->div_L      = (dvs_aig_node_t *)malloc((size_t)(N + 1) * sizeof(dvs_aig_node_t));
        S->div_R      = (dvs_aig_node_t *)malloc((size_t)(N + 1) * sizeof(dvs_aig_node_t));
        S->div_fo_off = (uint32_t *)malloc(((size_t)N + 2) * sizeof(uint32_t));
        uint32_t *cnt = (uint32_t *)calloc((size_t)N + 2, sizeof(uint32_t));
        dvs_aig_node_t *cL = S->div_L, *cR = S->div_R;
        uint32_t *off = S->div_fo_off, *adj = NULL;
        if (cL && cR && off && cnt) {
            cL[0] = 0; cR[0] = 0;
            for (uint32_t id = 1; id <= N; id++) {
                if (dvs_aig_is_and(m, (dvs_aig_node_t)id))
                    dvs_aig_get_children(m, (dvs_aig_node_t)id, &cL[id], &cR[id]);
                else { cL[id] = 0; cR[id] = 0; }
            }
            for (uint32_t id = 1; id <= N; id++) {
                if (!cL[id]) continue;
                uint32_t cl = (uint32_t)(cL[id] < 0 ? -cL[id] : cL[id]);
                uint32_t cr = (uint32_t)(cR[id] < 0 ? -cR[id] : cR[id]);
                if (cl > 1) cnt[cl]++;
                if (cr > 1) cnt[cr]++;
            }
            uint32_t acc = 0;
            for (uint32_t i = 0; i <= N; i++) { off[i] = acc; acc += cnt[i]; }
            off[N + 1] = acc;
            adj = (uint32_t *)malloc((acc ? (size_t)acc : 1) * sizeof(uint32_t));
            if (adj) {
                for (uint32_t i = 0; i <= N; i++) cnt[i] = off[i];   /* fill cursor */
                for (uint32_t id = 1; id <= N; id++) {
                    if (!cL[id]) continue;
                    uint32_t cl = (uint32_t)(cL[id] < 0 ? -cL[id] : cL[id]);
                    uint32_t cr = (uint32_t)(cR[id] < 0 ? -cR[id] : cR[id]);
                    if (cl > 1) adj[cnt[cl]++] = id;
                    if (cr > 1) adj[cnt[cr]++] = id;
                }
            }
        }
        free(cnt);
        S->div_fo_adj = adj;
        if (!adj) {
            free(S->div_L); free(S->div_R); free(S->div_fo_off);
            S->div_L = S->div_R = NULL; S->div_fo_off = NULL;
            free(nv); return;
        }
        S->div_n = N;
    }
    const dvs_aig_node_t *L = S->div_L, *R = S->div_R;
    const uint32_t *fo_off = S->div_fo_off, *fo_adj = S->div_fo_adj;
    S->aux_work += N;
    /* The model to start from: a simulation's (node_val), else the SAT solver's. */
    int8_t *base_nv = S->node_val;
    S->node_val = NULL;

    /* Build the initial valuation in topological (ascending id) order --
     * children always have a smaller id. */
    nv[0] = 0;
    for (uint32_t id = 1; id <= N; id++) {
        if (L[id])
            nv[id] = (int8_t)(_lit_val(nv, L[id]) & _lit_val(nv, R[id]));
        else
            nv[id] = base_nv ? base_nv[id]
                             : (int8_t)(dvs_aig_cnf_value(S->cnf, (dvs_aig_node_t)id) == 1);
    }
    free(base_nv);

    uint8_t  *inq    = (uint8_t  *)calloc((size_t)N + 1, 1);
    uint32_t *wl     = (uint32_t *)malloc(((size_t)N + 1) * sizeof(uint32_t));
    uint32_t *trail  = (uint32_t *)malloc(((size_t)N + 1) * sizeof(uint32_t));
    uint8_t  *areq   = (uint8_t  *)calloc((size_t)N + 1, 1);
    uint32_t *cnt    = (uint32_t *)malloc(((size_t)N + 2) * sizeof(uint32_t));
    if (!inq || !wl || !trail || !areq || !cnt) {
        free(inq); free(wl); free(trail); free(areq); free(cnt);
        S->node_val = nv; return;   /* keep the valid (undiversified) model */
    }
    /* Mark each asserted node with the value it must keep. */
    for (uint32_t a = 0; a < S->n_asserts; a++) {
        dvs_aig_node_t lit = S->assert_nodes[a];
        uint32_t id = (uint32_t)(lit < 0 ? -lit : lit);
        if (id == 0 || id > N || lit == DVS_AIG_TRUE || lit == DVS_AIG_FALSE) continue;
        areq[id] |= (lit < 0) ? 2u : 1u;          /* lit>0: node must be 1 */
    }

    /* The candidate flips, in visit order: each variable bit that is a
     * primary input, with its seeded target, once per sweep; sweeps after the
     * first draw fresh targets.
     * A constant bit (TRUE, id 1) is not a candidate: readback takes constants
     * from the literal, never from nv[1]. Count the distinct inputs on the way
     * (cnt is slot[], the input's index + 1). */
    uint32_t ncand = 0, k = 0;
    for (uint32_t i = 0; i <= N; i++) cnt[i] = 0;
    for (uint32_t vi = 0; vi < S->n_vars; vi++) {
        bb_var_t *v = &S->vars[vi];
        if (!v->defined || !v->bv_built) continue;
        for (uint32_t p = 0; p < v->bv.size; p++) {
            dvs_aig_node_t lit = v->bv.bits[p];
            uint32_t base = (uint32_t)(lit < 0 ? -lit : lit);
            if (base <= 1 || base > N || L[base] != 0) continue;
            ncand++;
            if (!cnt[base]) cnt[base] = ++k;
        }
    }
    uint32_t nextra = 0;
    if (ncand && ncand < DVS_DIVERSIFY_SWEEP_BUDGET) {
        nextra = (DVS_DIVERSIFY_SWEEP_BUDGET - ncand) / ncand;
        if (nextra + 1u > sweeps) nextra = sweeps ? sweeps - 1u : 0u;
    }
    size_t nvisit = (size_t)ncand * (nextra + 1u);
    uint32_t *cand = (uint32_t *)malloc((nvisit + 1) * 2 * sizeof(uint32_t));
    uint32_t *inputs = (uint32_t *)malloc(((size_t)k + 1) * sizeof(uint32_t));
    int memo_done = 0;
    if (cand && inputs) {
        size_t c = 0;
        for (uint32_t sw = 0; sw <= nextra; sw++) {
            /* Sweep 0 keeps the targets the single sweep always used. Later
             * sweeps hash the sweep index into the seed: adding it would line
             * up with _free_var_fill's per-variable term and repeat targets. */
            uint64_t sseed = sw ? _free_var_fill(S->seed ^ 0x5851F42D4C957F2DULL,
                                                 0xFFFFFFF0u, sw)
                                : S->seed;
            for (uint32_t vi = 0; vi < S->n_vars; vi++) {
                bb_var_t *v = &S->vars[vi];
                if (!v->defined || !v->bv_built) continue;
                for (uint32_t p = 0; p < v->bv.size; p++) {
                    dvs_aig_node_t lit = v->bv.bits[p];
                    uint32_t base = (uint32_t)(lit < 0 ? -lit : lit);
                    if (base <= 1 || base > N || L[base] != 0) continue;
                    int tgt = (int)(_free_var_fill(sseed, vi, p) & 1);
                    cand[2 * c] = base;
                    cand[2 * c + 1] = (uint32_t)((lit < 0) ? !tgt : tgt);  /* input-node polarity */
                    c++;
                    if (sw == 0) inputs[cnt[base] - 1] = base;
                }
            }
        }
        for (uint32_t i = 0; i <= N; i++) if (cnt[i]) cnt[i]--;   /* slot = index */
        const char *dm = getenv("DV_DIVERSIFY_MEMO");      /* =0: plain loop (A/B tests) */
        if (k <= DVS_DIVERSIFY_MEMO_BITS && ((size_t)4 << k) <= nvisit && !(dm && *dm == '0'))
            memo_done = _diversify_memo(nv, L, R, areq, N, cand, (uint32_t)nvisit,
                                        inputs, k, cnt, &S->aux_work);
    }
    if (!memo_done && cand) {
        for (size_t c = 0; c < nvisit; c++) {
            uint32_t base = cand[2 * c], desired = cand[2 * c + 1];
            if ((uint32_t)nv[base] == desired) continue;
            if (areq[base] & (desired ? 2u : 1u)) continue;  /* asserted input */
            uint32_t nt = 0;
            nv[base] = (int8_t)desired;
            trail[nt++] = base;
            int ok = _prop_cone(nv, L, R, fo_off, fo_adj, areq, wl, inq, trail, &nt, base);
            S->aux_work += nt;
            if (!ok) {
                /* revert: every touched node was boolean-inverted */
                for (uint32_t t = 0; t < nt; t++) nv[trail[t]] = (int8_t)!nv[trail[t]];
            }
        }
    } else if (!memo_done) {
        /* No visit list (allocation failed): one sweep with targets computed
         * on the fly -- a valid, less diverse model. */
        for (uint32_t vi = 0; vi < S->n_vars; vi++) {
            bb_var_t *v = &S->vars[vi];
            if (!v->defined || !v->bv_built) continue;
            for (uint32_t p = 0; p < v->bv.size; p++) {
                dvs_aig_node_t lit = v->bv.bits[p];
                uint32_t base = (uint32_t)(lit < 0 ? -lit : lit);
                if (base <= 1 || base > N) continue;
                if (L[base] != 0) continue;                 /* not a primary input */
                int tgt = (int)(_free_var_fill(S->seed, vi, p) & 1);
                int desired = (lit < 0) ? !tgt : tgt;       /* input-node polarity */
                if (nv[base] == desired) continue;
                if (areq[base] & (desired ? 2u : 1u)) continue;  /* asserted input */
                uint32_t nt = 0;
                nv[base] = (int8_t)desired;
                trail[nt++] = base;
                int ok = _prop_cone(nv, L, R, fo_off, fo_adj, areq, wl, inq, trail, &nt, base);
                S->aux_work += nt;
                if (!ok) {
                    /* revert: every touched node was boolean-inverted */
                    for (uint32_t t = 0; t < nt; t++) nv[trail[t]] = (int8_t)!nv[trail[t]];
                }
            }
        }
    }
    free(cand);
    free(inputs);

    free(cnt);
    free(inq);
    free(wl);
    free(trail);
    free(areq);
    S->node_val = nv;
}

/* One sweep: the plain solve paths (bitblast routes, Verilator diversity
 * requests) promise a varied model, not a uniform one, and the Verilator
 * router weighs bitblast against CDCL by time, so they keep the single
 * sweep's cost. */
static void _diversify(dvs_bbsolver_t *S) { _diversify_sweeps(S, 1u); }

/* Bit-blast + CNF-encode the whole problem (constraints, kept softs, subst
 * force-builds, variable bounds) WITHOUT solving. Shared by the one-shot
 * dvs_bbsolver_check and the cube engine's dvs_bbsolver_prepare, which then
 * drives its own assumption-based solve loop over the same encoded instance.
 * Returns DVS_BB_ENCODE_READY when the instance is fully encoded and ready to
 * solve, or DVS_BB_UNKNOWN / DVS_BB_ERROR (with S->last_result set) when a
 * construct is unsupported or a hard error occurred. NOTE: the "ready" sentinel
 * is deliberately NOT 0 — DVS_BB_UNKNOWN is 0, so a 0-means-ready contract would
 * make a deferral look like success and drop the unsupported hard constraint. */
/** Encode every DistSpec as a domain restriction.
 *
 * `dist` is not just a sampling hint in this engine: the propagator side's
 * value picker (_pick_value_dist, dvs_search.c) only ever draws from the
 * NONZERO-weight ranges that intersect the feasible domain, and the suite
 * locks that as a hard property (test_dist_domain_restriction,
 * test_dist_zero_weight_excluded). So the satisfiability content of a dist is
 * `var ∈ ⋃ {[lo_i, hi_i] : weight_i > 0}`, which is the same shape bb_in_ranges
 * already encodes -- an OR over per-range conjunctions.
 *
 * Weights themselves are deliberately NOT modelled: a SAT solver returns a
 * witness, not a sample, so the bias has no meaning here. What must not happen
 * is the previous behaviour, where the dist list was never read at all and the
 * engine cheerfully returned 47 for a `dist` over {1,2,3} -- a silent wrong
 * answer, and the one failure mode worse than a refusal.
 *
 * Ranges are clipped to the variable's declared domain and a range that does
 * not overlap it is dropped. If NO nonzero-weight range overlaps, the dist is
 * left unasserted rather than made unsatisfiable, which matches the picker's
 * own fallback to a uniform draw over the feasible domain in that case.
 *
 * @return 1 on success, 0 if a spec cannot be encoded (caller defers).
 */
static int _bb_encode_dists(dvs_bbsolver_t *S) {
    for (dvs_expr_t dref = S->problem->dists_head; dref != EXPR_NULL; ) {
        DistSpec *ds = (DistSpec *)POOL_PTR(S->problem, dref);
        dvs_dist_entry_t *ents = (dvs_dist_entry_t *)(ds + 1);
        uint32_t vid = ds->var_id;
        if (vid >= S->n_vars || !S->vars[vid].defined) return 0;

        dvs_bv_t v = bv_for_var(S, vid);
        if (S->had_error || S->had_unsupported) return 0;
        int is_signed = S->vars[vid].is_signed;
        int64_t dlo = S->vars[vid].lo, dhi = S->vars[vid].hi;

        /* Bounds are bit patterns: an unsigned 64-bit var orders them
         * unsigned, as the picker does (var_b_lt), or its upper-half entries
         * would clip away. (Wider vars carry the [0, INT64_MAX] "free"
         * sentinel and take no dist.) */
        int uns = !is_signed && v.size == 64;
#define DIST_LT(a, b) (uns ? (uint64_t)(a) < (uint64_t)(b) : (a) < (b))
        dvs_bv_t acc = { NULL, 0 };
        for (uint32_t i = 0; i < ds->n_entries; i++) {
            if (ents[i].weight == 0) continue;          /* never picked */
            int64_t elo = DIST_LT(ents[i].lo, dlo) ? dlo : ents[i].lo;
            int64_t ehi = DIST_LT(dhi, ents[i].hi) ? dhi : ents[i].hi;
            if (DIST_LT(ehi, elo)) continue;            /* outside the domain */

            dvs_bv_t lo = bb_value_i64(S, v.size, elo, is_signed);
            dvs_bv_t hi = bb_value_i64(S, v.size, ehi, is_signed);
            if (S->had_error) return 0;
            dvs_bv_t v_lt_lo = is_signed ? dvs_bb_slt(S->bb, v, lo)
                                         : dvs_bb_ult(S->bb, v, lo);
            dvs_bv_t hi_lt_v = is_signed ? dvs_bb_slt(S->bb, hi, v)
                                         : dvs_bb_ult(S->bb, hi, v);
            dvs_bv_t in_i = dvs_bb_and(S->bb, dvs_bb_not(S->bb, v_lt_lo),
                                              dvs_bb_not(S->bb, hi_lt_v));
            acc = (acc.size == 0) ? in_i : dvs_bb_or(S->bb, acc, in_i);
        }
#undef DIST_LT
        if (acc.size != 0) assert_top(S, acc.bits[0]);
        dref = ds->next;
    }
    return 1;
}

/** Encode every AllDiffSpec as pairwise disequality.
 *
 * Pairwise NEQ is the obvious lowering and the one be-bc already falls back to
 * itself when a group is too large for the global propagator
 * (lower/constraints.py). It is O(n^2) gates, which is why the propagator side
 * keeps a dedicated AllDifferent instead -- but here correctness is the point:
 * bb previously DEFERRED on any all-different group, so a problem that combined
 * `unique` with a shape cdcl could not compile got INCOMPLETE from one engine
 * and UNKNOWN from the other, i.e. no answer from either.
 *
 * @return 1 on success, 0 if a spec cannot be encoded (caller defers).
 */
static int _bb_encode_alldiff(dvs_bbsolver_t *S) {
    for (dvs_expr_t aref = S->problem->allDiff_head; aref != EXPR_NULL; ) {
        AllDiffSpec *as = (AllDiffSpec *)POOL_PTR(S->problem, aref);
        uint32_t *ids = (uint32_t *)(as + 1);
        uint32_t n = as->n_vars;

        for (uint32_t i = 0; i < n; i++) {
            if (ids[i] >= S->n_vars || !S->vars[ids[i]].defined) return 0;
        }
        for (uint32_t i = 0; i < n; i++) {
            dvs_bv_t a = bv_for_var(S, ids[i]);
            if (S->had_error || S->had_unsupported) return 0;
            for (uint32_t j = i + 1; j < n; j++) {
                dvs_bv_t b = bv_for_var(S, ids[j]);
                if (S->had_error || S->had_unsupported) return 0;
                /* Widths can differ across the group; widen to the wider one,
                 * sign-extending only when the narrower operand is signed. */
                uint32_t w = max_w(a.size, b.size);
                dvs_bv_t aw = a, bw = b;
                if (aw.size < w)
                    aw = S->vars[ids[i]].is_signed ? sext_to(S, aw, w)
                                                   : zext_to(S, aw, w);
                if (bw.size < w)
                    bw = S->vars[ids[j]].is_signed ? sext_to(S, bw, w)
                                                   : zext_to(S, bw, w);
                dvs_bv_t eq = dvs_bb_eq(S->bb, aw, bw);
                assert_top(S, dvs_bb_not(S->bb, eq).bits[0]);
            }
        }
        aref = as->next;
    }
    return 1;
}

/* L0 decomposition ladder: per-component recognizers, tried in order. A
 * recognizer returns 1 if it would solve component `comp` itself, 0 to fall
 * through to the whole-problem bit-blast. None yet, so every component falls
 * through and the SAT call is unchanged. */
typedef int (*bb_comp_recognizer_t)(dvs_bbsolver_t *S, uint32_t comp,
                                    const uint64_t *lo, const uint64_t *hi);

/* Unsigned value of a non-negative 64-bit-or-narrower constant, else -1. */
static int bb_const_u(dvs_bbsolver_t *S, dvs_expr_t e, uint64_t *out) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, e);
    if (!k || *k != EXPR_CONST) return 0;
    const ExprConst *c = (const ExprConst *)k;
    if (c->value < 0) return 0;
    *out = (uint64_t)c->value;
    return 1;
}

/* Var id of a bare reference to an unsigned var of width <= 64, else -1. */
static int64_t bb_unsigned_var(dvs_bbsolver_t *S, dvs_expr_t e) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, e);
    uint32_t id;
    for (int hops = 0; ; hops++) {
        while (k && *k == EXPR_EXTEND && !((const ExprExtend *)k)->sign_extend)
            k = (const ExprKind *)POOL_PTR(S->problem,
                                           ((const ExprExtend *)k)->operand);
        if (!k || *k != EXPR_VAR) return -1;
        id = ((const ExprVar *)k)->var_id;
        /* `x == zero_extend(y)` taken as a substitution: bound y. */
        if (hops >= 8 || id >= S->n_vars || !S->subst ||
            S->subst[id] == EXPR_NULL) break;
        k = (const ExprKind *)POOL_PTR(S->problem, S->subst[id]);
    }
    if (id >= S->n_vars || S->vars[id].is_signed || S->vars[id].width > 64)
        return -1;
    return id;
}

/* `ite(c, 1, 0)`: non-zero exactly when c holds. */
static int bb_is_bool_ite(dvs_bbsolver_t *S, dvs_expr_t e, dvs_expr_t *cond) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, e);
    uint64_t t, f;
    if (!k || *k != EXPR_ITE) return 0;
    const ExprITE *i = (const ExprITE *)k;
    if (!bb_const_u(S, i->then_e, &t) || !bb_const_u(S, i->else_e, &f) ||
        t != 1 || f != 0) return 0;
    if (cond) *cond = i->cond;
    return 1;
}

static void bb_tighten(uint64_t *lo, uint64_t *hi, int64_t v,
                       uint64_t l, uint64_t h) {
    if (v < 0) return;
    if (l > lo[v]) lo[v] = l;
    if (h < hi[v]) hi[v] = h;
}

/** Unary bound tightening (stats only): intersect each unsigned var's
 * [lo,hi] with the top-level conjuncts `var cmp const`, `var inside
 * {consts}` and `var inside [const:const]` of the kept constraints.
 * Works on the copies lo[]/hi[]; S->vars is untouched. An empty result
 * (lo > hi) is left as is: a recognizer must then decline, not claim. */
static void bb_unary_bounds(dvs_bbsolver_t *S, uint64_t *lo, uint64_t *hi) {
    dvs_expr_t stk[64];
    uint32_t me = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL;
         me++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        cur = cs->next;
        if (S->constraint_skip && S->constraint_skip[me]) continue;
        size_t n = 0;
        stk[n++] = cs->root;
        while (n > 0) {
            dvs_expr_t r = stk[--n];
            const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, r);
            if (!k) continue;
            uint64_t a, b;
            dvs_expr_t cond;
            if (bb_is_bool_ite(S, r, &cond)) {     /* ite(c,1,0) != 0: c */
                if (n < 64) stk[n++] = cond;
                continue;
            }
            if (*k == EXPR_BINARY) {
                const ExprBinary *e = (const ExprBinary *)k;
                /* `a && b`, and `a & b` of two 0/1 ites: both hold. */
                if (e->op == DVS_BIN_AND ||
                    (e->op == DVS_BIN_BAND && bb_is_bool_ite(S, e->lhs, NULL) &&
                     bb_is_bool_ite(S, e->rhs, NULL))) {
                    if (n + 2 <= 64) { stk[n++] = e->lhs; stk[n++] = e->rhs; }
                    continue;
                }
                /* `1 == x`: x is non-zero (Verilator's (= #b1 ...) wrapper). */
                if (e->op == DVS_BIN_EQ && bb_const_u(S, e->lhs, &a) && a == 1 &&
                    bb_unsigned_var(S, e->rhs) < 0) {
                    if (n < 64) stk[n++] = e->rhs;
                    continue;
                }
                /* `b == ite(c,1,0)` with b substituted by 1: c holds. */
                if (e->op == DVS_BIN_EQ && S->subst) {
                    const ExprKind *lk =
                        (const ExprKind *)POOL_PTR(S->problem, e->lhs);
                    if (lk && *lk == EXPR_VAR &&
                        ((const ExprVar *)lk)->var_id < S->n_vars &&
                        S->subst[((const ExprVar *)lk)->var_id] != EXPR_NULL &&
                        bb_const_u(S, S->subst[((const ExprVar *)lk)->var_id],
                                   &a) && a == 1 &&
                        bb_is_bool_ite(S, e->rhs, &cond)) {
                        if (n < 64) stk[n++] = cond;
                        continue;
                    }
                }
                dvs_binop_t op = e->op;
                int64_t v = bb_unsigned_var(S, e->lhs);
                int ok = v >= 0 && bb_const_u(S, e->rhs, &a);
                if (!ok) {      /* const op var: mirror the comparison */
                    v = bb_unsigned_var(S, e->rhs);
                    ok = v >= 0 && bb_const_u(S, e->lhs, &a);
                    if (op == DVS_BIN_LT) op = DVS_BIN_GT;
                    else if (op == DVS_BIN_GT) op = DVS_BIN_LT;
                    else if (op == DVS_BIN_LTE) op = DVS_BIN_GTE;
                    else if (op == DVS_BIN_GTE) op = DVS_BIN_LTE;
                }
                if (!ok) continue;
                switch (op) {
                case DVS_BIN_EQ:  bb_tighten(lo, hi, v, a, a); break;
                case DVS_BIN_LT:
                    if (a == 0) bb_tighten(lo, hi, v, 1, 0);
                    else bb_tighten(lo, hi, v, 0, a - 1);
                    break;
                case DVS_BIN_LTE: bb_tighten(lo, hi, v, 0, a); break;
                case DVS_BIN_GT:
                    if (a == UINT64_MAX) bb_tighten(lo, hi, v, 1, 0);
                    else bb_tighten(lo, hi, v, a + 1, UINT64_MAX);
                    break;
                case DVS_BIN_GTE: bb_tighten(lo, hi, v, a, UINT64_MAX); break;
                default: break;
                }
            } else if (*k == EXPR_IN_RANGE) {
                const ExprInRange *e = (const ExprInRange *)k;
                int64_t v = bb_unsigned_var(S, e->value);
                if (v >= 0 && bb_const_u(S, e->lo, &a) && bb_const_u(S, e->hi, &b))
                    bb_tighten(lo, hi, v, a, b);
            } else if (*k == EXPR_IN_SET) {
                const ExprInSet *e = (const ExprInSet *)k;
                int64_t v = bb_unsigned_var(S, e->value);
                if (v < 0 || !e->n_elems) continue;
                const dvs_expr_t *el = expr_in_set_elems(S->problem, r);
                uint64_t mn = UINT64_MAX, mx = 0;
                uint32_t i;
                for (i = 0; i < e->n_elems; i++) {
                    if (!bb_const_u(S, el[i], &a)) break;
                    if (a < mn) mn = a;
                    if (a > mx) mx = a;
                }
                if (i == e->n_elems) bb_tighten(lo, hi, v, mn, mx);
            } else if (*k == EXPR_IN_RANGES) {
                const ExprInRanges *e = (const ExprInRanges *)k;
                int64_t v = bb_unsigned_var(S, e->value);
                if (v < 0 || !e->n_ranges) continue;
                const dvs_expr_t *ls = expr_in_ranges_los(S->problem, r);
                const dvs_expr_t *hs = expr_in_ranges_his(S->problem, r);
                uint64_t mn = UINT64_MAX, mx = 0;
                uint32_t i;
                for (i = 0; i < e->n_ranges; i++) {
                    if (!bb_const_u(S, ls[i], &a) || !bb_const_u(S, hs[i], &b))
                        break;
                    if (a < mn) mn = a;
                    if (b > mx) mx = b;
                }
                if (i == e->n_ranges) bb_tighten(lo, hi, v, mn, mx);
            }
        }
    }
}

static uint64_t bb_eval_mask(uint32_t w) {
    return w >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1);
}

/* Extend the low `from` bits of x to `to` bits (sign-extend if sg). */
static uint64_t bb_eval_ext(uint64_t x, uint32_t from, uint32_t to, int sg) {
    x &= bb_eval_mask(from);
    if (sg && from && from < 64 && ((x >> (from - 1)) & 1)) x |= ~bb_eval_mask(from);
    return x & bb_eval_mask(to);
}

/** Concrete value of `ref` under the var assignment val[] (bit patterns by var
 * id), sized to context width w (1..64) as bb_expr would size it. A var with a
 * substitution evaluates its S->subst entry. Returns 0 (decline) on any
 * construct it does not model or a width above 64; the ladder then leaves the
 * component to the SAT path. Covers CONST, VAR, BINARY (unsigned DIV/MOD), UNARY,
 * ITE, IN_RANGE, IN_SET, IN_RANGES, EXTEND, SV_CAST, EXTRACT, CONCAT,
 * COUNTONES. */
static int bb_eval(dvs_bbsolver_t *S, dvs_expr_t ref, uint32_t w,
                   const uint64_t *val, uint64_t *out);

/* Truth of `ref` as bb_predicate reads it: (ref at its own width) != 0. */
static int bb_eval_pred(dvs_bbsolver_t *S, dvs_expr_t ref, const uint64_t *val,
                        uint64_t *out) {
    uint32_t sw = bb_self_width(S, ref, 0);
    if (sw == 0) sw = 1;
    uint64_t x;
    if (sw > 64 || !bb_eval(S, ref, sw, val, &x)) return 0;
    *out = x != 0;
    return 1;
}

/* lo <= v <= hi at the widest of the three, ordered by v's signedness
 * (bb_in_range / bb_in_ranges; IN_SET uses lo == hi == elem). */
static int bb_eval_in(dvs_bbsolver_t *S, dvs_expr_t v, dvs_expr_t lo,
                      dvs_expr_t hi, const uint64_t *val, int *in) {
    uint32_t cw = bb_self_width(S, v, 0), lw = bb_self_width(S, lo, 0),
             hw = bb_self_width(S, hi, 0);
    if (lw > cw) cw = lw;
    if (hw > cw) cw = hw;
    if (cw == 0) cw = 64;
    if (cw > 64) return 0;
    uint64_t x, l, h;
    if (!bb_eval(S, v, cw, val, &x) || !bb_eval(S, lo, cw, val, &l)
        || !bb_eval(S, hi, cw, val, &h)) return 0;
    if (bb_signed(S, v, 0)) {
        uint64_t sb = (uint64_t)1 << (cw - 1);
        x ^= sb; l ^= sb; h ^= sb;
    }
    *in = l <= x && x <= h;
    return 1;
}

static int bb_eval(dvs_bbsolver_t *S, dvs_expr_t ref, uint32_t w,
                   const uint64_t *val, uint64_t *out) {
    if (ref == EXPR_NULL || w == 0 || w > 64) return 0;
    if (dvs_stack_low(&S->sg)) return 0;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (!kp) return 0;
    switch (*kp) {
    case EXPR_CONST: {
        const ExprConst *c = (const ExprConst *)kp;
        if (c->width > 64) return 0;
        *out = c->width ? bb_eval_ext((uint64_t)c->value, c->width, w, c->is_signed)
                        : ((uint64_t)c->value & bb_eval_mask(w));
        return 1;
    }
    case EXPR_VAR: {
        uint32_t id = ((const ExprVar *)kp)->var_id;
        if (id >= S->n_vars) return 0;
        uint32_t vw = S->vars[id].width;
        if (vw == 0 || vw > 64) return 0;
        uint64_t x;
        if (S->subst && S->subst[id] != EXPR_NULL) {
            if (S->resolving[id]) return 0;
            S->resolving[id] = 1;
            const ExprCountones *pc = popcount_def(S, id);
            uint32_t xs = pc ? bb_self_width(S, pc->operand, 0) : 0;
            int ok;
            if (!pc) ok = bb_eval(S, S->subst[id], vw, val, &x);
            else if ((ok = xs > 0 && xs <= 64
                           && bb_eval(S, pc->operand, xs, val, &x))) {
                uint64_t n = 0;
                for (x &= bb_eval_mask(xs); x; x &= x - 1) n++;
                x = n;
            }
            S->resolving[id] = 0;
            if (!ok) return 0;
        } else {
            x = val[id];
        }
        *out = bb_eval_ext(x, vw, w, S->vars[id].is_signed);
        return 1;
    }
    case EXPR_BINARY: {
        const ExprBinary *b = (const ExprBinary *)kp;
        uint64_t x, y;
        switch (b->op) {
        case DVS_BIN_AND: case DVS_BIN_OR:
            if (!bb_eval_pred(S, b->lhs, val, &x)) return 0;
            if (!bb_eval_pred(S, b->rhs, val, &y)) return 0;
            *out = (b->op == DVS_BIN_AND) ? (x & y) : (x | y);
            return 1;
        case DVS_BIN_EQ: case DVS_BIN_NEQ:
        case DVS_BIN_LT: case DVS_BIN_LTE: case DVS_BIN_GT: case DVS_BIN_GTE: {
            uint32_t lw = bb_self_width(S, b->lhs, 0), rw = bb_self_width(S, b->rhs, 0);
            uint32_t cw = lw > rw ? lw : rw;
            if (cw == 0) cw = 64;
            if (cw > 64) return 0;
            int sg = bb_signed(S, b->lhs, 0) && bb_signed(S, b->rhs, 0);
            if (!bb_eval(S, b->lhs, cw, val, &x)) return 0;
            if (!bb_eval(S, b->rhs, cw, val, &y)) return 0;
            int r;
            if (b->op == DVS_BIN_EQ) r = x == y;
            else if (b->op == DVS_BIN_NEQ) r = x != y;
            else {
                if (sg) { /* flip the sign bit: signed order as unsigned */
                    uint64_t sb = (uint64_t)1 << (cw - 1);
                    x ^= sb; y ^= sb;
                }
                r = b->op == DVS_BIN_LT ? x < y : b->op == DVS_BIN_LTE ? x <= y
                  : b->op == DVS_BIN_GT ? x > y : x >= y;
            }
            *out = (uint64_t)r;
            return 1;
        }
        case DVS_BIN_LSHIFT: case DVS_BIN_RSHIFT: case DVS_BIN_ASHR: {
            uint32_t lw = bb_self_width(S, b->lhs, 0);
            uint32_t ow = lw > w ? lw : w;
            uint32_t rw = bb_self_width(S, b->rhs, 0);
            if (rw == 0) rw = 64;
            if (ow > 64 || rw > 64) return 0;
            int sg = bb_signed(S, b->lhs, 0);
            if (!bb_eval(S, b->lhs, ow, val, &x)) return 0;
            if (!bb_eval(S, b->rhs, rw, val, &y)) return 0;
            uint64_t r;
            if (b->op == DVS_BIN_LSHIFT) r = y >= ow ? 0 : x << y;
            else if (b->op == DVS_BIN_ASHR && sg) {
                int neg = (x >> (ow - 1)) & 1;
                r = y >= ow ? (neg ? ~(uint64_t)0 : 0)
                            : bb_eval_ext(x >> y, ow - (uint32_t)y, ow, 1);
            } else r = y >= ow ? 0 : x >> y;
            *out = r & bb_eval_mask(w);
            return 1;
        }
        case DVS_BIN_ADD: case DVS_BIN_SUB: case DVS_BIN_MUL:
        case DVS_BIN_BAND: case DVS_BIN_BOR: case DVS_BIN_BXOR: {
            uint32_t lw = bb_self_width(S, b->lhs, 0), rw = bb_self_width(S, b->rhs, 0);
            uint32_t ow = lw > rw ? lw : rw;
            if (w > ow) ow = w;
            if (ow > 64) return 0;
            if (!bb_eval(S, b->lhs, ow, val, &x)) return 0;
            if (!bb_eval(S, b->rhs, ow, val, &y)) return 0;
            uint64_t r = b->op == DVS_BIN_ADD ? x + y : b->op == DVS_BIN_SUB ? x - y
                       : b->op == DVS_BIN_MUL ? x * y : b->op == DVS_BIN_BAND ? x & y
                       : b->op == DVS_BIN_BOR ? x | y : x ^ y;
            *out = r & bb_eval_mask(w);
            return 1;
        }
        case DVS_BIN_DIV: case DVS_BIN_MOD: {
            /* Unsigned only (signed decline). Zero divisor as the restoring
             * divider encodes it: quotient all-ones, remainder the dividend. */
            if (bb_signed(S, b->lhs, 0) && bb_signed(S, b->rhs, 0)) return 0;
            uint32_t lw = bb_self_width(S, b->lhs, 0), rw = bb_self_width(S, b->rhs, 0);
            uint32_t ow = lw > rw ? lw : rw;
            if (w > ow) ow = w;
            if (ow > 64) return 0;
            if (!bb_eval(S, b->lhs, ow, val, &x)) return 0;
            if (!bb_eval(S, b->rhs, ow, val, &y)) return 0;
            uint64_t r = b->op == DVS_BIN_DIV ? (y ? x / y : bb_eval_mask(ow))
                                               : (y ? x % y : x);
            *out = r & bb_eval_mask(w);
            return 1;
        }
        default:
            return 0; /* anything new */
        }
    }
    case EXPR_UNARY: {
        const ExprUnary *u = (const ExprUnary *)kp;
        uint64_t x;
        if (u->op == DVS_UN_NOT) {
            if (!bb_eval_pred(S, u->operand, val, &x)) return 0;
            *out = !x;
            return 1;
        }
        if (u->op != DVS_UN_NEG && u->op != DVS_UN_INVERT) return 0;
        uint32_t ow = bb_self_width(S, u->operand, 0);
        if (w > ow) ow = w;
        if (ow > 64) return 0;
        if (!bb_eval(S, u->operand, ow, val, &x)) return 0;
        *out = (u->op == DVS_UN_NEG ? (uint64_t)0 - x : ~x) & bb_eval_mask(w);
        return 1;
    }
    case EXPR_ITE: {
        const ExprITE *e = (const ExprITE *)kp;
        uint64_t c;
        if (!bb_eval_pred(S, e->cond, val, &c)) return 0;
        dvs_expr_t arm = c ? e->then_e : e->else_e;
        uint32_t tw = bb_self_width(S, e->then_e, 0), fw = bb_self_width(S, e->else_e, 0);
        uint32_t ow = tw > fw ? tw : fw;
        if (w > ow) ow = w;
        if (ow > 64) return 0;
        uint64_t x;
        if (!bb_eval(S, arm, ow, val, &x)) return 0;
        *out = x & bb_eval_mask(w);
        return 1;
    }
    case EXPR_IN_RANGE: {
        const ExprInRange *r = (const ExprInRange *)kp;
        int in;
        if (!bb_eval_in(S, r->value, r->lo, r->hi, val, &in)) return 0;
        *out = (uint64_t)in;
        return 1;
    }
    case EXPR_IN_SET: {
        const ExprInSet *n = (const ExprInSet *)kp;
        dvs_expr_t *el = expr_in_set_elems(S->problem, ref);
        int any = 0;
        for (uint32_t i = 0; i < n->n_elems; i++) {
            int in;
            if (!bb_eval_in(S, n->value, el[i], el[i], val, &in)) return 0;
            any |= in;
        }
        *out = (uint64_t)any; /* empty set -> false */
        return 1;
    }
    case EXPR_IN_RANGES: {
        const ExprInRanges *n = (const ExprInRanges *)kp;
        dvs_expr_t *los = expr_in_ranges_los(S->problem, ref);
        dvs_expr_t *his = expr_in_ranges_his(S->problem, ref);
        int any = 0;
        for (uint32_t i = 0; i < n->n_ranges; i++) {
            int in;
            if (!bb_eval_in(S, n->value, los[i], his[i], val, &in)) return 0;
            any |= in;
        }
        *out = (uint64_t)any;
        return 1;
    }
    case EXPR_EXTEND:
    case EXPR_SV_CAST: {
        /* Same layout (bb_extend); the cast's signedness is in bb_signed. */
        const ExprExtend *e = (const ExprExtend *)(const void *)kp;
        if (e->from_bits == 0 || e->from_bits > 64 || e->to_bits == 0
            || e->to_bits > 64) return 0;
        uint64_t x;
        if (!bb_eval(S, e->operand, e->from_bits, val, &x)) return 0;
        x = bb_eval_ext(x, e->from_bits, e->to_bits, e->sign_extend);
        *out = bb_eval_ext(x, e->to_bits, w, bb_signed(S, ref, 0));
        return 1;
    }
    case EXPR_EXTRACT: {
        /* Operand at its own width, zero-extended to hi_bit+1 (bb_extract). */
        const ExprExtract *e = (const ExprExtract *)kp;
        if (e->hi_bit >= 64 || e->lo_bit > e->hi_bit) return 0;
        uint32_t sw = bb_self_width(S, e->operand, 0);
        if (sw == 0 || sw > 64) return 0;
        uint64_t x;
        if (!bb_eval(S, e->operand, sw, val, &x)) return 0;
        x &= bb_eval_mask(sw);
        uint32_t rw = e->hi_bit - e->lo_bit + 1;
        x = (x >> e->lo_bit) & bb_eval_mask(rw);
        *out = bb_eval_ext(x, rw, w, bb_signed(S, ref, 0));
        return 1;
    }
    case EXPR_CONCAT: {
        /* lo sized to lo_width (zero-extended if narrower), hi at its own
         * width (bb_concat). */
        const ExprConcat *e = (const ExprConcat *)kp;
        uint32_t lw = e->lo_width, hw = bb_self_width(S, e->hi, 0);
        if (lw == 0 || hw == 0 || lw + hw > 64) return 0;
        uint32_t lsw = bb_self_width(S, e->lo, 0);
        if (lsw == 0) return 0;
        uint64_t l, h;
        if (lsw >= lw) {
            if (!bb_eval(S, e->lo, lw, val, &l)) return 0;
        } else {
            if (!bb_eval(S, e->lo, lsw, val, &l)) return 0;
            l = bb_eval_ext(l, lsw, lw, 0);
        }
        if (!bb_eval(S, e->hi, hw, val, &h)) return 0;
        uint64_t x = ((h & bb_eval_mask(hw)) << lw) | (l & bb_eval_mask(lw));
        *out = bb_eval_ext(x, lw + hw, w, bb_signed(S, ref, 0));
        return 1;
    }
    case EXPR_COUNTONES: {
        /* result == popcount(operand), compared at W as bb_countones does. */
        const ExprCountones *c = (const ExprCountones *)kp;
        uint32_t xs = bb_self_width(S, c->operand, 0);
        uint32_t rs = bb_self_width(S, c->result, 0);
        if (xs == 0 || xs > 64 || rs == 0) return 0;
        uint32_t xb = bb_bits_for(xs);
        uint32_t W = (rs > xb ? rs : xb) + 1;
        if (W > 64) return 0;
        uint64_t x, r, n = 0;
        if (!bb_eval(S, c->operand, xs, val, &x)
            || !bb_eval(S, c->result, W, val, &r)) return 0;
        for (x &= bb_eval_mask(xs); x; x &= x - 1) n++;
        *out = r == n;
        return 1;
    }
    default:
        return 0;
    }
}

/** Every kept constraint of `comp` can be evaluated by bb_eval: a dry run
 * with each var at `val` (its lo). No cons_comp map declines. */
static int bb_comp_evalable(dvs_bbsolver_t *S, uint32_t comp,
                            const uint64_t *val) {
    if (!S->cons_comp) return 0;
    uint32_t i = 0;
    for (dvs_expr_t cur = S->problem->constraints_head;
         cur != EXPR_NULL && i < S->n_constraints; i++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        cur = cs->next;
        if (S->cons_comp[i] != comp) continue;
        uint64_t t;
        if (!bb_eval_pred(S, cs->root, val, &t)) return 0;
    }
    return 1;
}

/** Small component: product of its vars' domains is <= 2^16, so it could be
 * enumerated and sampled by index (plan 9a). A var's domain is its tightened
 * [lo,hi] (bb_unary_bounds); an empty domain declines. */
#define BB_COMP_ENUM_LOG2 16
static int bb_comp_small_domain(dvs_bbsolver_t *S, uint32_t comp,
                                const uint64_t *lo, const uint64_t *hi) {
    double prod = 1.0;
    for (uint32_t i = 0; i < S->comps.n_vars; i++) {
        if (S->comps.var_comp[i] != comp) continue;
        /* A substituted var is a function of the others: no dimension. */
        if (S->subst && S->subst[i] != EXPR_NULL) continue;
        if (S->vars[i].width > 64 || lo[i] > hi[i]) return 0;
        prod *= (double)(hi[i] - lo[i]) + 1.0;
        if (prod > (double)(1u << BB_COMP_ENUM_LOG2)) return 0;
    }
    return bb_comp_evalable(S, comp, lo);
}

static const bb_comp_recognizer_t bb_comp_ladder[] = {
    bb_comp_small_domain, NULL };

/** Count the solutions of a claimed small-domain comp: every tuple of its
 * non-subst vars over [lo,hi] at which each of its constraints is true
 * (stats only; 5b4b slice 1). Returns UINT64_MAX if bb_eval declines. */
static uint64_t bb_comp_enum_count(dvs_bbsolver_t *S, uint32_t comp,
                                   const uint64_t *lo, const uint64_t *hi,
                                   uint64_t pick, uint64_t *out) {
    uint32_t nv = S->comps.n_vars, nd = 0;
    uint32_t *dim = (uint32_t *)malloc((nv ? nv : 1) * sizeof(uint32_t));
    uint64_t *val = (uint64_t *)calloc(nv ? nv : 1, sizeof(uint64_t));
    uint64_t n = 0;
    if (!dim || !val) { free(dim); free(val); return UINT64_MAX; }
    for (uint32_t i = 0; i < nv; i++) {
        val[i] = lo[i];
        if (S->comps.var_comp[i] != comp) continue;
        if (S->subst && S->subst[i] != EXPR_NULL) continue;
        dim[nd++] = i;
    }
    for (;;) {
        int ok = 1;
        uint32_t i = 0;
        for (dvs_expr_t cur = S->problem->constraints_head;
             ok && cur != EXPR_NULL && i < S->n_constraints; i++) {
            ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
            cur = cs->next;
            if (S->cons_comp[i] != comp) continue;
            uint64_t t;
            if (!bb_eval_pred(S, cs->root, val, &t)) { n = UINT64_MAX; goto out; }
            ok = t != 0;
        }
        if (ok) {
            if (out && n == pick) {   /* the pick-th solution: copy it out */
                for (uint32_t k = 0; k < nd; k++) out[dim[k]] = val[dim[k]];
                n++;
                goto out;
            }
            n++;
        }
        uint32_t d = 0;   /* odometer step */
        for (; d < nd; d++) {
            uint32_t v = dim[d];
            if (val[v] < hi[v]) { val[v]++; break; }
            val[v] = lo[v];
        }
        if (d == nd) break;
    }
out:
    free(dim); free(val);
    return n;
}

/** Pin each non-subst var of `comp` to the matching value of out[]
 * (DV_BB_LADDER, 5b4b slice 2). The comp's constraints stay encoded, so the
 * pins only select one of their models. */
static void bb_comp_pin(dvs_bbsolver_t *S, uint32_t comp, const uint64_t *out) {
    for (uint32_t i = 0; i < S->comps.n_vars; i++) {
        if (S->comps.var_comp[i] != comp) continue;
        if (S->subst && S->subst[i] != EXPR_NULL) continue;
        dvs_bv_t bv = bv_for_var(S, i);
        for (uint32_t b = 0; b < bv.size; b++) {   /* bits[0] is the MSB */
            uint32_t bit = bv.size - 1 - b;
            int one = bit < 64 && ((out[i] >> bit) & 1);
            assert_top(S, one ? bv.bits[b] : dvs_aig_not(bv.bits[b]));
        }
    }
}

/** Count components some ladder recognizer claims. With `pin` set
 * (DV_BB_LADDER), each claimed comp with solutions is pinned to one of them,
 * chosen by a per-comp stream of S->seed. */
static uint32_t bb_comp_ladder_claims(dvs_bbsolver_t *S, uint32_t *n_evalable,
                                      int pin) {
    uint32_t nv = S->comps.n_vars, claimed = 0;
    uint64_t *lo = (uint64_t *)malloc((nv ? nv : 1) * 2 * sizeof(uint64_t));
    if (!lo) return 0;
    uint64_t *hi = lo + (nv ? nv : 1);
    for (uint32_t i = 0; i < nv; i++) {
        const bb_var_t *v = &S->vars[i];
        uint64_t mask = v->width >= 64 ? UINT64_MAX
                        : ((uint64_t)1 << v->width) - 1;
        if (v->hi >= v->lo && v->lo >= 0) {
            lo[i] = (uint64_t)v->lo;
            hi[i] = (uint64_t)v->hi < mask ? (uint64_t)v->hi : mask;
        } else { lo[i] = 0; hi[i] = mask; }
    }
    if (S->problem) bb_unary_bounds(S, lo, hi);
    for (uint32_t c = 0; c < S->comps.n_comp; c++) {
        if (n_evalable && S->problem && bb_comp_evalable(S, c, lo))
            (*n_evalable)++;
        for (size_t r = 0; bb_comp_ladder[r]; r++)
            if (bb_comp_ladder[r](S, c, lo, hi)) {
                claimed++;
                uint64_t n = S->problem
                    ? bb_comp_enum_count(S, c, lo, hi, 0, NULL) : UINT64_MAX;
                if (n == 0) S->ladder_enum_zero++;
                if (pin && n != 0 && n != UINT64_MAX) {
                    uint64_t *out = (uint64_t *)malloc((nv ? nv : 1) * sizeof(uint64_t));
                    uint64_t k = _free_var_fill(S->seed ^ 0x9E3779B97F4A7C15ULL, c, 0) % n;
                    if (out && bb_comp_enum_count(S, c, lo, hi, k, out) == k + 1)
                        bb_comp_pin(S, c, out);
                    free(out);
                }
                break;
            }
    }
    free(lo);
    return claimed;
}

static int _bb_encode(dvs_bbsolver_t *S) {
    /* A-4 soundness guard: the bit-blaster does not encode Source groups.
     * pyvsc never emits these on the dv-solve path, but if one ever appears we
     * must NOT silently drop a hard constraint — defer (UNKNOWN) instead.
     * AllDifferent used to defer here too; it is encoded below. */
    if (S->problem->sources_head != EXPR_NULL) {
        S->last_result = DVS_BB_UNKNOWN;
        return DVS_BB_UNKNOWN;
    }

    int subst_enabled = getenv("DV_BB_NO_SUBST") == NULL;

    if (subst_enabled) run_subst_pass(S);
    count_fanout(S);
    /* Component census for the decomposition ladder; stats-only for now. */
    /* Pins ignore softs/dist/alldiff (bb_eval checks only constraints), so
     * the ladder stays off for any problem with them; and off in a session
     * frame, where later frames may constrain the pinned vars. */
    const char *ladder_env = getenv("DV_BB_LADDER");
    int ladder = ladder_env && *ladder_env && strcmp(ladder_env, "0") != 0
        && !S->assert_frame
        && S->problem->softs_head == EXPR_NULL
        && S->problem->dists_head == EXPR_NULL
        && S->problem->allDiff_head == EXPR_NULL;
    if (ladder && S->resolving) memset(S->resolving, 0, S->n_vars);
    if (getenv("DV_BB_STATS")) {
        compute_components(S);
        uint32_t n_evalable = 0;
        S->ladder_enum_zero = 0;
        uint32_t claimed = bb_comp_ladder_claims(S, &n_evalable, ladder);
        fprintf(stderr, "[bb-comp] n=%u largest=%u vars=%u claimed=%u evalable=%u enum_zero=%u\n",
                S->comps.n_comp, S->comps.largest, S->n_vars,
                claimed, n_evalable, S->ladder_enum_zero);
        /* Referenced vars: pinned (subst to a constant), derived (subst to
         * an expression), free (no subst). free=0 => no search needed. */
        uint32_t n_pin = 0, n_der = 0, n_free = 0;
        for (uint32_t i = 0; S->comps.var_comp && i < S->comps.n_vars; i++) {
            if (S->comps.var_comp[i] == UINT32_MAX) continue;
            dvs_expr_t e = S->subst ? S->subst[i] : EXPR_NULL;
            const ExprKind *k = e != EXPR_NULL
                ? (const ExprKind *)POOL_PTR(S->problem, e) : NULL;
            if (e == EXPR_NULL) n_free++;
            else if (k && *k == EXPR_CONST) n_pin++;
            else n_der++;
        }
        fprintf(stderr, "[bb-pin] pinned=%u derived=%u free=%u\n",
                n_pin, n_der, n_free);
    } else if (ladder) {
        compute_components(S);
        bb_comp_ladder_claims(S, NULL, 1);
    }
    /* bv_for_var uses S->resolving as its recursion guard; start clear. */
    if (S->resolving) memset(S->resolving, 0, S->n_vars);

    /* Encode each constraint as a top-level assertion. Skip constraints
     * fully consumed by the substitution pass. */
    uint32_t idx = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        if (S->constraint_skip && S->constraint_skip[idx]) {
            cur = cs->next;
            continue;
        }
        dvs_bv_t pred = bb_predicate(S, cs->root);
        if (S->had_error) { S->last_result = DVS_BB_ERROR; return DVS_BB_ERROR; }
        if (S->had_unsupported) { S->last_result = DVS_BB_UNKNOWN; return DVS_BB_UNKNOWN; }
        assert_top_r(S, pred.bits[0], cs->root);
        cur = cs->next;
    }

    /* DSE-2: encode the kept soft constraints as hard top-level assertions.
     * soft_keep[i] selects which softs (in softs_head walk order) to enforce;
     * the MaxSAT wrapper has already chosen the maximal priority-respecting set,
     * so here they are simply additional hard assertions. NULL → honor none. */
    if (S->soft_keep) {
        uint32_t si = 0;
        for (dvs_expr_t scur = S->problem->softs_head; scur != EXPR_NULL; si++) {
            SoftSpec *ss = (SoftSpec *)POOL_PTR(S->problem, scur);
            if (S->soft_keep[si]) {
                dvs_bv_t pred = bb_predicate(S, ss->root);
                if (S->had_error) { S->last_result = DVS_BB_ERROR; return DVS_BB_ERROR; }
                if (S->had_unsupported) { S->last_result = DVS_BB_UNKNOWN; return DVS_BB_UNKNOWN; }
                assert_top(S, pred.bits[0]);
            }
            scur = ss->next;
        }
    }

    if (S->problem->allDiff_head != EXPR_NULL) {
        if (!_bb_encode_alldiff(S)) {
            S->last_result = S->had_error ? DVS_BB_ERROR : DVS_BB_UNKNOWN;
            return S->last_result;
        }
    }

    /* Distribution constraints. Placed after the constraint and soft loops so a
     * dist on a substituted variable sees the substitution already resolved. */
    if (S->problem->dists_head != EXPR_NULL) {
        if (!_bb_encode_dists(S)) {
            S->last_result = S->had_error ? DVS_BB_ERROR : DVS_BB_UNKNOWN;
            return S->last_result;
        }
        if (S->had_error) { S->last_result = DVS_BB_ERROR; return DVS_BB_ERROR; }
        if (S->had_unsupported) { S->last_result = DVS_BB_UNKNOWN; return DVS_BB_UNKNOWN; }
    }

    /* Force-build any substituted variable that no surviving constraint
     * referenced. The subst pass consumed its defining constraint (e.g. the
     * sole `x == 42`), so without this its bv is never built and
     * dvs_bbsolver_value() would default it to 0 instead of the substituted
     * value. bv_for_var() ties var->bv to the (bit-blasted) RHS, so the model
     * — and thus readback — reflects the substitution. */
    if (S->subst) {
        for (uint32_t i = 0; i < S->n_vars; i++) {
            if (S->vars[i].defined && (S->subst[i] != EXPR_NULL ||
                (S->slice_head && S->slice_head[i] != BB_NO_SLICE))) {
                /* A substituted var's bv is a *computed* expression (AND/XOR
                 * gates), unlike a normal var whose bits are AIG inputs (always
                 * readable). Its gate bits are only recoverable if encoded into
                 * the CNF, and a constraint may reference only *some* of them
                 * (e.g. the sampler hashes just a few low bits via extract), or
                 * none (its defining `==` was consumed by the subst pass). Build
                 * the bv and encode *every* bit at non-top-level so the SAT model
                 * assigns all of them consistently with the substituted
                 * expression's inputs — otherwise dvs_aig_cnf_value() reads each
                 * unencoded bit as 0, a silent wrong model. Encoding is
                 * idempotent for already-encoded bits. */
                dvs_bv_t bv = bv_for_var(S, i);
                if (S->had_error) {
                    S->last_result = DVS_BB_ERROR;
                    return DVS_BB_ERROR;
                }
                for (uint32_t bit = 0; bit < bv.size; bit++) {
                    dvs_aig_cnf_encode(S->cnf, bv.bits[bit], /*top_level=*/0);
                }
            }
        }
    }

    /* A construct may have been bit-blasted only in the force-build pass above
     * (e.g. a substituted `y == <signed-div>` whose constraint was skipped),
     * so re-check the A-4 unsupported flag before committing to a verdict. */
    if (S->had_unsupported) { S->last_result = DVS_BB_UNKNOWN; return DVS_BB_UNKNOWN; }

    /* Variable bounds — only for variables that were actually referenced
     * (and therefore had a bv built). Vars never referenced are unconstrained. */
    for (uint32_t i = 0; i < S->n_vars; i++) {
        if (S->vars[i].defined && S->vars[i].bv_built && !S->vars[i].bounds_asserted) {
            assert_var_bounds(S, i);
            S->vars[i].bounds_asserted = 1;
        }
    }

    return DVS_BB_ENCODE_READY;
}

/* ------------------------------------------------------------------------- *
 * Diversified SAT portfolio (B1).
 *
 * The per-solve cost on hard single QF_BV instances is a SAT-search HEAVY TAIL,
 * not an encoding defect: identical-size CNFs solve 30x apart, and the runtime
 * tails of different backends are ANTI-CORRELATED (kissat wins some, CaDiCaL
 * others). So the lever is min-of-N over a diverse fleet: replay the recorded
 * clause DB into N independent solver instances — spread across backends first
 * (the strongest diversity axis), then seeds — run them concurrently on the
 * FULL instance, and take the first verdict. A full-instance solve carries no
 * assumptions, so ANY worker's SAT/UNSAT is authoritative for the whole problem;
 * the winner's model installs exactly like a cube worker's. Losers are aborted
 * mid-solve via the terminate hook the instant a peer finishes.
 *
 * Opt-in via DV_PORTFOLIO=N (N>=2). Unset/0/1 leaves the single-solve path (and
 * thus every existing caller) byte-for-byte unchanged.
 * ------------------------------------------------------------------------- */

/* Golden-ratio odd constant: spreads per-worker seeds so their low bits (hence
 * kissat/CaDiCaL initial phases) differ. */
#define BB_PORT_GOLDEN 0x9E3779B97F4A7C15ull

static uint32_t bb_portfolio_workers(void) {
    const char *e = getenv("DV_PORTFOLIO");
    if (!e || !*e) return 0;
    long n = atol(e);
    if (n < 2) return 0;         /* 0/1 => portfolio off (single solve) */
    if (n > 256) n = 256;        /* hard cap on the fleet */
    return (uint32_t)n;
}

typedef struct bb_port_arg_s {
    /* shared, read-only after spawn */
    const int32_t     *db;
    size_t             db_n;
    int32_t            max_var;
    dvs_sat_backend_t  backend;
    uint64_t           seed;
    int                verbose;
    uint32_t           idx;
    /* peer roster so the winner can interrupt the losers (see bb_port_worker) */
    struct bb_port_arg_s *all;
    uint32_t           n_all;
    /* shared, mutable (guarded by *mtx except *stop which is poll-only) */
    dvs_mutex_t       *mtx;
    volatile int      *stop;     /* 1 once any worker has a verdict            */
    int               *result;   /* DVS_BB_SAT/UNSAT written by the winner     */
    dvs_sat_t        **winner;   /* winning instance (model source), or NULL   */
    /* out: this worker's instance (freed by the driver after model install).
     * Published under *mtx so a winning peer sees it before interrupting.      */
    dvs_sat_t         *sat;
} bb_port_arg;

/* Terminate callback: the losing workers poll the shared stop flag and abort.
 * A benign lock-free read — the flag only ever transitions 0->1, and a slightly
 * stale read just means a worker grinds a few more conflicts before quitting. */
static int bb_port_terminate(void *state) {
    return *(volatile int *)state;
}

static void *bb_port_worker(void *arg) {
    bb_port_arg *w = (bb_port_arg *)arg;

    dvs_sat_t *sat = dvs_sat_new_backend(NULL, w->backend);
    if (!sat) return NULL;   /* w->sat stays NULL (calloc'd); peers skip us */
    /* Seed BEFORE any clause is added (CaDiCaL only honors seed/phase at init). */
    if (w->seed) dvs_sat_set_seed(sat, w->seed);
    dvs_sat_reserve(sat, (dvs_sat_var_t)w->max_var);
    for (size_t i = 0; i < w->db_n; i++)
        dvs_sat_add(sat, (dvs_sat_lit_t)w->db[i]);
    /* CaDiCaL polls this and aborts on the shared stop flag; kissat stores but
     * ignores it (interrupted instead via dvs_sat_interrupt below). */
    dvs_sat_set_terminate(sat, (void *)w->stop, bb_port_terminate);

    /* Publish our instance so a winning peer can interrupt us, and bail if a
     * peer already won while we were building. */
    dvs_mutex_lock(w->mtx);
    w->sat = sat;
    int already = *w->stop;
    dvs_mutex_unlock(w->mtx);
    if (already) return NULL;

    /* Full-instance solve (no assumptions, no conflict limit). */
    int rc = dvs_sat_solve(sat);

    dvs_mutex_lock(w->mtx);
    if (!*w->stop && (rc == DVS_BB_SAT || rc == DVS_BB_UNSAT)) {
        *w->result = rc;
        if (rc == DVS_BB_SAT) *w->winner = sat;
        *w->stop = 1;
        /* Stop the losers now: CaDiCaL peers see the flag via their callback,
         * kissat peers need the explicit poke. Safe under the mutex — every peer
         * that could be mid-solve has already published its instance. */
        for (uint32_t j = 0; j < w->n_all; j++)
            if (j != w->idx && w->all[j].sat)
                dvs_sat_interrupt(w->all[j].sat);
        if (w->verbose)
            fprintf(stderr, "[bb-port] worker %u (%s seed=%llu) won: %s\n",
                    w->idx,
                    w->backend == DVS_SAT_BACKEND_CADICAL ? "cadical" : "kissat",
                    (unsigned long long)w->seed,
                    rc == DVS_BB_SAT ? "sat" : "unsat");
    }
    dvs_mutex_unlock(w->mtx);
    return NULL;   /* keep `sat` alive; the driver reads the winner then frees. */
}

/* Assign (backend, seed) to worker `i`. Backend is the primary diversity axis:
 * worker 0 = kissat, worker 1 = CaDiCaL (the proven anti-correlated pair, both
 * at the caller's base seed = the field-measured fast configs). Extra workers
 * alternate backends and take a spread seed. When CaDiCaL is absent everything
 * runs on kissat, so seed diversity carries all the load (worker 0 keeps the
 * base seed; the rest are spread). */
static void bb_port_assign(uint32_t i, uint64_t base_seed, int have_cd,
                           dvs_sat_backend_t *bk, uint64_t *seed) {
    /* DV_PORT_BACKENDS overrides the backend mix (E2.3 calibration knob):
     *   "cadical" -> all workers CaDiCaL (seed diversity only); every worker gets
     *               a spread seed since there is no backend axis to diversify on.
     *   "kissat"  -> all workers kissat.
     *   unset/"mixed" -> the default anti-correlated kissat/CaDiCaL alternation. */
    const char *mix = getenv("DV_PORT_BACKENDS");
    if (mix && have_cd && strcmp(mix, "cadical") == 0) {
        *bk = DVS_SAT_BACKEND_CADICAL;
        *seed = (i == 0) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
        return;
    }
    if (mix && strcmp(mix, "kissat") == 0) {
        *bk = DVS_SAT_BACKEND_KISSAT;
        *seed = (i == 0) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
        return;
    }
    if (have_cd) {
        *bk = (i & 1u) ? DVS_SAT_BACKEND_CADICAL : DVS_SAT_BACKEND_KISSAT;
        *seed = (i < 2) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
    } else {
        *bk = DVS_SAT_BACKEND_KISSAT;
        *seed = (i == 0) ? base_seed : (base_seed ^ ((uint64_t)i * BB_PORT_GOLDEN));
    }
}

/* Run an n-way diversified portfolio over the already-encoded instance. Returns
 * a DVS_BB_* verdict, or sets *ran=0 (leaving the caller to fall back to the
 * single solve) if the DB could not be captured or no worker could start. */
static int bb_run_portfolio(dvs_bbsolver_t *S, uint32_t n_port, uint64_t seed,
                            int verbose, int *ran) {
    *ran = 0;

    size_t db_n = 0;
    int32_t max_var = 0;
    const int32_t *db = dvs_bbsolver_clause_db(S, &db_n, &max_var);
    if (!db || db_n == 0) return DVS_BB_UNKNOWN;   /* recording off/failed */

    dvs_mutex_t mtx;
    if (dvs_mutex_init(&mtx) != 0) return DVS_BB_UNKNOWN;

    volatile int stop = 0;
    int result = DVS_BB_UNKNOWN;
    dvs_sat_t *winner = NULL;
    int have_cd = dvs_sat_has_cadical();

    bb_port_arg  *wa = (bb_port_arg *)calloc(n_port, sizeof(*wa));
    dvs_thread_t *th = (dvs_thread_t *)calloc(n_port, sizeof(*th));
    if (!wa || !th) {
        free(wa); free(th); dvs_mutex_destroy(&mtx);
        return DVS_BB_UNKNOWN;
    }

    uint32_t spawned = 0;
    for (uint32_t i = 0; i < n_port; i++) {
        wa[i].db = db; wa[i].db_n = db_n; wa[i].max_var = max_var;
        bb_port_assign(i, seed, have_cd, &wa[i].backend, &wa[i].seed);
        wa[i].verbose = verbose; wa[i].idx = i;
        wa[i].all = wa; wa[i].n_all = n_port;
        wa[i].mtx = &mtx; wa[i].stop = &stop;
        wa[i].result = &result; wa[i].winner = &winner;
        if (dvs_thread_create(&th[i], bb_port_worker, &wa[i]) == 0) spawned++;
        else break;
    }

    if (spawned == 0) {
        free(wa); free(th); dvs_mutex_destroy(&mtx);
        return DVS_BB_UNKNOWN;   /* nothing started: caller falls back */
    }
    if (verbose)
        fprintf(stderr, "[bb-port] %u workers (cadical=%s), %zu db lits\n",
                spawned, have_cd ? "yes" : "no", db_n);

    for (uint32_t i = 0; i < spawned; i++)
        dvs_thread_join(&th[i], NULL);

    /* Materialize the winner's model into S before any instance is freed. */
    if (result == DVS_BB_SAT && winner) {
        if (dvs_bbsolver_install_worker_model(S, winner) != 0)
            result = DVS_BB_UNKNOWN;   /* couldn't build the model: stays sound */
    }

    for (uint32_t i = 0; i < spawned; i++)
        if (wa[i].sat) dvs_sat_free(wa[i].sat);
    free(wa); free(th); dvs_mutex_destroy(&mtx);

    *ran = 1;
    return result;
}

/* Bit-parallel simulation (docs/fw_hdl_implementation_plan.md, pieces 3.3,
 * 3.4; dvs_aigsim.h) around the SAT solve:
 *
 *  - an exhaustive sweep of the assertions' input cone decides the problem
 *    outright when it costs at most DV_SIM_MAX_WORK 64-lane gate evaluations
 *    (default 1.5e6, about 0.5 ms at ~3e9 a second; decision D6). Bigger
 *    sweeps race SAT instead: a 22-input BMC step swept in 2.5 ms where SAT
 *    needs 1 ms (tier2 fsm_onehot_bmc_d1);
 *  - otherwise a few hundred random assignments look for a cheap model;
 *  - otherwise, when the sweep costs at most DV_SIM_RACE_WORK (default 6e9,
 *    about 2 s), it runs on a second thread beside SAT and the first verdict
 *    wins. Parity-heavy proofs (CRC, affine maps) defeat CDCL but not a sweep.
 *
 * Every verdict is exact. DV_SIM_CHECK=1 confirms each simulation verdict with
 * SAT and aborts on a disagreement (the fuzz tests set it). */
typedef struct {
    dvs_aigsim_t *sim;
    dvs_sat_t    *sat;
    volatile int  stop;       /* SAT finished: the sweep gives up      */
    volatile int  sat_stop;   /* the sweep finished: SAT gives up      */
    int           verdict;
    uint64_t      seed;
} bb_sweep_arg;

static void *bb_sweep_worker(void *arg) {
    bb_sweep_arg *w = (bb_sweep_arg *)arg;
    w->verdict = dvs_aigsim_exhaustive(w->sim, &w->stop, w->seed);
    if (w->verdict != DVS_AIGSIM_UNDECIDED) {
        w->sat_stop = 1;            /* CaDiCaL polls the terminate callback */
        dvs_sat_interrupt(w->sat);  /* kissat needs the explicit poke       */
    }
    return NULL;
}

static int bb_sweep_terminate(void *state) {
    return *(volatile int *)state;
}

static uint64_t _env_u64(const char *name, uint64_t dflt) {
    const char *e = getenv(name);
    return e && *e ? strtoull(e, NULL, 10) : dflt;
}

static double _ms_since(const struct timespec *t0) {
    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    return (t1.tv_sec - t0->tv_sec) * 1000.0 + (t1.tv_nsec - t0->tv_nsec) / 1e6;
}

/* A simulation verdict as a bit-blast result; installs the model on SAT. */
static int _sim_result(dvs_bbsolver_t *S, dvs_aigsim_t *sim, int r) {
    if (r == DVS_AIGSIM_UNSAT) return DVS_BB_UNSAT;
    if (r != DVS_AIGSIM_SAT) return DVS_BB_UNKNOWN;
    int8_t *nv = dvs_aigsim_model(sim);
    if (!nv) return DVS_BB_UNKNOWN;
    free(S->node_val);
    S->node_val = nv;
    S->model_in_sat = 0;
    S->base_sim = 1;
    return DVS_BB_SAT;
}

/* Solve under the live session scopes' selectors (none outside a session). */
static int bb_sat_solve(dvs_bbsolver_t *S) {
    for (uint32_t i = 0; i < S->n_frames; i++)
        dvs_sat_assume(S->sat, (dvs_sat_lit_t)S->frames[i].sel);
    int rc = dvs_sat_solve(S->sat);
    S->model_in_sat = rc == DVS_BB_SAT;
    return rc;
}

static void _sim_check(dvs_bbsolver_t *S, int verdict) {
    if (!getenv("DV_SIM_CHECK") || verdict == DVS_BB_UNKNOWN) return;
    int sat = bb_sat_solve(S);
    if ((sat == DVS_BB_SAT || sat == DVS_BB_UNSAT) && sat != verdict) {
        fprintf(stderr, "bb-sim: MISMATCH simulation %s, SAT %s\n",
                verdict == DVS_BB_SAT ? "sat" : "unsat", sat == DVS_BB_SAT ? "sat" : "unsat");
        abort();
    }
}

/* The exhaustive sweep split into independent groups (DV_SIM_SPLIT=0 off).
 * DVS_BB_UNKNOWN when it does not apply; the whole-cone sweep then runs. */
static int _sim_split(dvs_bbsolver_t *S) {
    const char *e = getenv("DV_SIM_SPLIT");
    if (e && strcmp(e, "0") == 0) return DVS_BB_UNKNOWN;
    int8_t *nv = NULL;
    uint64_t work = 0;
    int r = dvs_aigsim_split_exhaustive(S->aig, S->assert_nodes, S->n_asserts,
                                        _env_u64("DV_SIM_MAX_WORK", 1500000ull), S->seed,
                                        &nv, &work);
    S->aux_work += work;
    if (r == DVS_AIGSIM_UNSAT) return DVS_BB_UNSAT;
    if (r != DVS_AIGSIM_SAT || !nv) { free(nv); return DVS_BB_UNKNOWN; }
    free(S->node_val);
    S->node_val = nv;
    S->model_in_sat = 0;
    S->base_sim = 1;
    return DVS_BB_SAT;
}

static int _popsamp(dvs_bbsolver_t *S);
static int _uniqsamp(dvs_bbsolver_t *S);
static int _sortsamp(dvs_bbsolver_t *S);
static int _divsamp(dvs_bbsolver_t *S);

/* Simulation, then SAT. Returns the DVS_BB_* verdict. */
static int _solve_with_sim(dvs_bbsolver_t *S) {
    if (!S->presim || S->asserts_lost || !S->n_asserts) return bb_sat_solve(S);
    int log = getenv("DV_LOG") != NULL;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const char *aff_env = getenv("DV_AFFINE");
    if (!aff_env || strcmp(aff_env, "0") != 0) {
        dvs_affine_stats_t ast;
        int8_t *nv = NULL;
        int ar = dvs_affine_check(S->aig, S->assert_nodes, S->n_asserts,
                                  _env_u64("DV_AFFINE_MAX_WORK", 0), S->seed, &nv, &ast);
        if (log)
            fprintf(stderr, "bb-affine: gates=%u tables=%u inputs=%u opaque=%u eqs=%u clauses=%u "
                    "rank=%u -> %s (%s) in %.2f ms\n", ast.xors, ast.tables, ast.inputs, ast.opaque,
                    ast.equations, ast.clauses, ast.rank,
                    ar == DVS_AFFINE_SAT ? "sat" : ar == DVS_AFFINE_UNSAT ? "unsat" : "undecided",
                    ast.why, _ms_since(&t0));
        if (ar != DVS_AFFINE_UNDECIDED) {
            int rc = ar == DVS_AFFINE_UNSAT ? DVS_BB_UNSAT : DVS_BB_SAT;
            if (nv) { free(S->node_val); S->node_val = nv; S->model_in_sat = 0; S->base_sim = 1; }
            _sim_check(S, rc);
            return rc;
        }
    }
    int src = _sim_split(S);
    if (src != DVS_BB_UNKNOWN) {
        if (log)
            fprintf(stderr, "bb-sim: split -> %s in %.2f ms\n",
                    src == DVS_BB_SAT ? "sat" : "unsat", _ms_since(&t0));
        _sim_check(S, src);
        return src;
    }
    dvs_aigsim_t *sim = dvs_aigsim_new(S->aig, S->assert_nodes, S->n_asserts);
    if (!sim) return bb_sat_solve(S);
    uint64_t work = dvs_aigsim_exhaustive_work(sim);
    uint64_t per_pass = (uint64_t)dvs_aigsim_gates(sim) + S->n_asserts;
    const char *how;
    int r = DVS_AIGSIM_UNDECIDED, rc;

    if (work <= _env_u64("DV_SIM_MAX_WORK", 1500000ull)) {
        how = "exhaustive";
        r = dvs_aigsim_exhaustive(sim, NULL, S->seed);
    } else {
        how = "random";
        uint64_t blocks = 1000000ull / (per_pass ? per_pass : 1);
        r = dvs_aigsim_random(sim, (uint32_t)(blocks < 8 ? blocks : 8), S->seed);
    }
    /* Gate evaluations, 64 vectors per word. */
    S->aux_work += dvs_aigsim_vectors(sim) / 64u * per_pass;
    rc = _sim_result(S, sim, r);
    if (rc != DVS_BB_UNKNOWN) {
        if (log)
            fprintf(stderr, "bb-sim: %s inputs=%u gates=%u vectors=%llu -> %s in %.2f ms\n",
                    how, dvs_aigsim_inputs(sim), dvs_aigsim_gates(sim),
                    (unsigned long long)dvs_aigsim_vectors(sim),
                    rc == DVS_BB_SAT ? "sat" : "unsat", _ms_since(&t0));
        dvs_aigsim_free(sim);
        _sim_check(S, rc);
        return rc;
    }

    /* Race the sweep against SAT. */
    dvs_thread_t th;
    bb_sweep_arg arg = { sim, S->sat, 0, 0, DVS_AIGSIM_UNDECIDED, S->seed };
    dvs_sat_materialize(S->sat);   /* the sweep thread may interrupt it */
    int raced = work <= _env_u64("DV_SIM_RACE_WORK", 6000000000ull)
             && dvs_thread_create(&th, bb_sweep_worker, &arg) == 0;
    if (raced) dvs_sat_set_terminate(S->sat, (void *)&arg.sat_stop, bb_sweep_terminate);
    rc = bb_sat_solve(S);
    const char *winner = "sat-solver";
    if (raced) {
        arg.stop = 1;
        dvs_thread_join(&th, NULL);
        dvs_sat_set_terminate(S->sat, NULL, NULL);
        if (arg.verdict != DVS_AIGSIM_UNDECIDED) {
            int sr = arg.verdict == DVS_AIGSIM_SAT ? DVS_BB_SAT : DVS_BB_UNSAT;
            if (rc == DVS_BB_SAT || rc == DVS_BB_UNSAT) {
                /* Both finished. They must agree; SAT's model is installed. */
                if (sr != rc) {
                    fprintf(stderr, "bb-sim: MISMATCH sweep %s, SAT %s\n",
                            sr == DVS_BB_SAT ? "sat" : "unsat", rc == DVS_BB_SAT ? "sat" : "unsat");
                    if (getenv("DV_SIM_CHECK")) abort();
                    rc = DVS_BB_UNKNOWN;
                }
            } else {
                winner = "sweep";
                rc = _sim_result(S, sim, arg.verdict);
            }
        }
    }
    if (log)
        fprintf(stderr, "bb-sim: %s inputs=%u gates=%u work=%llu%s -> %s by %s in %.2f ms\n",
                how, dvs_aigsim_inputs(sim), dvs_aigsim_gates(sim), (unsigned long long)work,
                raced ? " raced" : "",
                rc == DVS_BB_SAT ? "sat" : rc == DVS_BB_UNSAT ? "unsat" : "unknown",
                winner, _ms_since(&t0));
    dvs_aigsim_free(sim);
    return rc;
}

void dvs_bbsolver_set_presim(dvs_bbsolver_t *S, int on) {
    if (S) S->presim = on;
}

void dvs_bbsolver_set_resample(dvs_bbsolver_t *S, int on) {
    if (S) S->resample = on;
}

/** Stats only (DV_BB_STATS): evaluate every kept constraint with bb_eval at
 * the SAT model. A constraint that evaluates false is a bb_eval defect
 * (mismatch); one bb_eval declines is counted apart. */
static void bb_eval_model_check(dvs_bbsolver_t *S) {
    if (!S->cons_comp || !S->problem) return;
    uint32_t nv = S->n_vars;
    uint64_t *val = (uint64_t *)calloc(nv ? nv : 1, sizeof(uint64_t));
    if (!val) return;
    for (uint32_t i = 0; i < nv; i++) {
        int64_t x;
        if (!S->vars[i].defined || dvs_bbsolver_value(S, i, &x) != 0) continue;
        uint32_t w = S->vars[i].width;
        val[i] = w >= 64 ? (uint64_t)x : (uint64_t)x & (((uint64_t)1 << w) - 1);
    }
    uint32_t i = 0, checked = 0, mismatch = 0, declined = 0;
    for (dvs_expr_t cur = S->problem->constraints_head;
         cur != EXPR_NULL && i < S->n_constraints; i++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        cur = cs->next;
        if (S->cons_comp[i] == UINT32_MAX) continue;
        uint64_t t;
        if (!bb_eval_pred(S, cs->root, val, &t)) { declined++; continue; }
        checked++;
        if (!t) mismatch++;
    }
    free(val);
    /* A claimed comp with 0 enumerated solutions contradicts this SAT. */
    fprintf(stderr, "[bb-eval] mismatch=%u checked=%u declined=%u enum_zero_sat=%u\n",
            mismatch, checked, declined, S->ladder_enum_zero);
}

int dvs_bbsolver_check(dvs_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->problem) return DVS_BB_ERROR;

    S->seed = seed;
    /* Seed kissat's randomness so repeated checks can return different models
     * (the completeness-fallback's only source of stimulus diversity). */
    dvs_sat_set_seed(S->sat, seed);

    /* Diversified portfolio (opt-in, DV_PORTFOLIO>=2). Enable clause recording
     * BEFORE encoding so the exact literal stream can be replayed into the
     * worker instances; the recorder is a no-op otherwise. */
    uint32_t n_port = bb_portfolio_workers();
    if (n_port >= 2 || S->resample) dvs_bbsolver_record_clauses(S);
    S->base_sim = 0;

    int stats_enabled = getenv("DV_BB_STATS") != NULL;
    struct timespec t0, t1, t2;
    if (stats_enabled) clock_gettime(CLOCK_MONOTONIC, &t0);

    int enc = _bb_encode(S);
    if (enc != DVS_BB_ENCODE_READY) return enc;   /* UNKNOWN/ERROR: S->last_result already set */

    if (stats_enabled) clock_gettime(CLOCK_MONOTONIC, &t1);

    /* Phase-0 instrumentation: emit the CNF size BEFORE solving, so it is
     * observable even when the solve runs long or never returns (the plain
     * bitblast path is otherwise uncapped — see DV_BB_MAX_CONFLICTS below). */
    if (stats_enabled) {
        double enc_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                      + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        fprintf(stderr,
                "[bb-encode] bb=%.2fms ands=%llu vars=%llu clauses=%llu substs=%llu"
                " slices=%u sums=%llu/%llu word=%llu/%llu/%llu\n",
                enc_ms,
                (unsigned long long)dvs_aig_num_ands(S->aig),
                (unsigned long long)dvs_aig_cnf_num_vars(S->cnf),
                (unsigned long long)dvs_aig_cnf_num_clauses(S->cnf),
                (unsigned long long)S->n_substs, S->n_slices,
                (unsigned long long)dvs_bbsum_num_flattened(S->sum),
                (unsigned long long)dvs_bbsum_num_adds(S->sum),
                (unsigned long long)dvs_bbword_num_cancelled(S->word),
                (unsigned long long)dvs_bbword_num_peeled(S->word),
                (unsigned long long)dvs_bbword_num_decided(S->word));
        fflush(stderr);
    }

    /* Diversified portfolio: race N backend/seed-diverse solvers on the full
     * instance, first verdict wins. On success the winner's model is already
     * installed in S; return straight away. On a setup miss (ran==0) fall
     * through to the single solve so the verdict is never worse than bitblast. */
    if (n_port >= 2) {
        int ran = 0;
        int prc = bb_run_portfolio(S, n_port, seed, stats_enabled, &ran);
        if (ran) {
            S->last_result = prc;
            return prc;
        }
    }

    /* Optional conflict cap on the bitblast solve (unset/0 = unlimited, the
     * default so normal runs are unaffected). The CDCL engine's DV_MAX_CONFLICTS
     * does NOT reach this path, so bounding a bitblast experiment needs its own
     * knob. On hit, the backend returns DVS_BB_UNKNOWN (sound: never a verdict). */
    {
        const char *mc = getenv("DV_BB_MAX_CONFLICTS");
        if (mc && *mc) dvs_sat_set_conflict_limit(S->sat, (uint32_t)atoi(mc));
    }

    /* Size-gated light search: kissat's failed-literal probing is pure overhead
     * on almost everything the one-shot bitblast path sees — the array/BMC class
     * (refuted by plain CDCL almost immediately) AND the hard QF_BV UNSAT band.
     * Measured (docs/perf_sweep_medium_band_2026-07-21.md): probe-OFF wins on
     * deep array-BMC (2-3x), vlsat3_a57 @181k cl (1.46x) and vlsat3_a80 @1.47M cl
     * (3.8x); it only *loses* on the single 11.6M-clause monster vlsat3_a67
     * (~1.45x), which genuinely needs the deep implication chains probing finds.
     * So the gate is a HIGH clause threshold (2M): everything below goes light,
     * only the >>1M-clause hardest instances keep probing. Toggling probe never
     * changes sat/unsat — this is a pure schedule/latency lever. */
    {
        const char *mcl = getenv("DV_KISSAT_LIGHT_MAXCLAUSES");
        uint64_t thresh = mcl && *mcl ? strtoull(mcl, NULL, 10) : 2000000;
        uint64_t ncl = dvs_aig_cnf_num_clauses(S->cnf);
        dvs_sat_set_light_search(S->sat, ncl < thresh);
    }

    /* A model the simulation finds is installed as node_val. */
    free(S->node_val);
    S->node_val = NULL;
    int rc = _popsamp(S);
    if (rc == DVS_BB_UNKNOWN) rc = _uniqsamp(S);
    if (rc == DVS_BB_UNKNOWN) rc = _sortsamp(S);
    if (rc == DVS_BB_UNKNOWN) rc = _divsamp(S);
    int sampled = rc != DVS_BB_UNKNOWN;
    if (sampled) _sim_check(S, rc);
    else rc = _solve_with_sim(S);
    if (stats_enabled) {
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double bb_ms = (t1.tv_sec - t0.tv_sec) * 1000.0
                     + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        double sat_ms = (t2.tv_sec - t1.tv_sec) * 1000.0
                      + (t2.tv_nsec - t1.tv_nsec) / 1e6;
        fprintf(stderr,
                "[bb-stats] bb=%.2fms sat=%.2fms ands=%llu vars=%llu clauses=%llu substs=%llu\n",
                bb_ms, sat_ms,
                (unsigned long long)dvs_aig_num_ands(S->aig),
                (unsigned long long)dvs_aig_cnf_num_vars(S->cnf),
                (unsigned long long)dvs_aig_cnf_num_clauses(S->cnf),
                (unsigned long long)S->n_substs);
    }
    S->last_result = rc;
    /* On SAT, diversify the don't-care bits toward seeded-random targets (no-op
     * for seed 0). Sound: only flips that keep every assertion true are kept.
     * It starts from the simulation's model when that is the one found. */
    if (rc == DVS_BB_SAT && !sampled) _diversify(S);
    if (stats_enabled && rc == DVS_BB_SAT) bb_eval_model_check(S);
    return rc;
}

/* ---- cube-and-conquer support (see dvs_cube.c, docs/cube_and_conquer_design.md) ---- */

int dvs_bbsolver_prepare(dvs_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->problem) return DVS_BB_ERROR;
    S->seed = seed;
    dvs_sat_set_seed(S->sat, seed);
    return _bb_encode(S);   /* DVS_BB_ENCODE_READY = ready, else DVS_BB_UNKNOWN/ERROR */
}

int dvs_bbsolver_is_incremental(const dvs_bbsolver_t *S) {
    return (S && S->sat) ? dvs_sat_is_incremental(S->sat) : 0;
}

const char *dvs_bbsolver_unknown_reason(const dvs_bbsolver_t *S) {
    if (!S) return "incomplete: the bit-blast engine could not start (out of memory?)";
    if (S->too_deep) return "incomplete: an expression nests deeper than the stack allows";
    if (S->had_unsupported) return "unsupported: a construct the bit-blast engine does not encode";
    if (S->had_error) return "incomplete: the bit-blast encoding failed (out of memory?)";
    return "incomplete: the SAT search stopped without a verdict (conflict limit)";
}

uint32_t dvs_bbsolver_split_lits(dvs_bbsolver_t *S, int32_t *out, uint32_t cap) {
    if (!S || !out || cap == 0) return 0;
    uint32_t n = 0;
    /* Candidate split literals are the SAT variables backing bits of
     * referenced (bv_built) problem variables. A var's bit is a genuine AIG
     * input node (id == SAT var id, 1:1); constant bits are not splittable.
     * The caller forms the two exhaustive cubes {+v} and {-v} per literal. */
    for (uint32_t i = 0; i < S->n_vars && n < cap; i++) {
        bb_var_t *v = &S->vars[i];
        if (!v->defined || !v->bv_built) continue;
        for (uint32_t b = 0; b < v->bv.size && n < cap; b++) {
            dvs_aig_node_t node = v->bv.bits[b];
            if (node == DVS_AIG_TRUE || node == DVS_AIG_FALSE) continue;
            if (!dvs_aig_is_input(S->aig, node)) continue;
            int32_t var = node < 0 ? -node : node;
            out[n++] = var;   /* positive literal; assumes bit = 1 */
        }
    }
    return n;
}

/* Flatten a DVS_BIN_OR chain rooted at `ref` into out[] (up to `cap`), counting
 * ALL leaves even past `cap` so the caller can detect an over-wide disjunction
 * that it cannot represent exhaustively. `n` is the running count. */
/* A disjunct count no caller accepts: the or nests deeper than the stack. */
#define BB_OR_TOO_DEEP 0x7FFFFFFFu

static uint32_t bb_flatten_or(dvs_bbsolver_t *S, dvs_expr_t ref,
                              dvs_expr_t *out, uint32_t cap, uint32_t n) {
    if (n >= BB_OR_TOO_DEEP || dvs_stack_low(&S->sg)) return BB_OR_TOO_DEEP;
    ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, ref);
    if (kp && *kp == EXPR_BINARY && ((ExprBinary *)kp)->op == DVS_BIN_OR) {
        ExprBinary *b = (ExprBinary *)kp;
        n = bb_flatten_or(S, b->lhs, out, cap, n);
        n = bb_flatten_or(S, b->rhs, out, cap, n);
        return n;
    }
    if (out && n < cap) out[n] = ref;
    return n + 1;
}

uint32_t dvs_bbsolver_or_split_lits(dvs_bbsolver_t *S, int32_t *out, uint32_t cap) {
    if (!S || !out || cap == 0) return 0;

    /* Pass 1: find the surviving (non-substituted) top-level constraint that is
     * the WIDEST disjunction. Its `or` is asserted true, so splitting on which
     * disjunct holds is an exhaustive k-way cover — the most SAT-directed cut. */
    dvs_expr_t best = EXPR_NULL;
    uint32_t best_k = 0, idx = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        dvs_expr_t root = cs->root;
        cur = cs->next;
        if (S->constraint_skip && S->constraint_skip[idx]) continue;
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, root);
        if (!kp || *kp != EXPR_BINARY || ((ExprBinary *)kp)->op != DVS_BIN_OR) continue;
        uint32_t k = bb_flatten_or(S, root, NULL, 0, 0);   /* count only */
        if (k > best_k) { best_k = k; best = root; }
    }
    /* Reject a disjunction wider than the caller's buffer: a partial split
     * would be non-exhaustive and break the UNSAT soundness contract. */
    if (best == EXPR_NULL || best_k < 2 || best_k > cap) return 0;

    /* Pass 2: materialize one assumption literal per disjunct. Each disjunct is
     * already in the AIG (the parent `or` was bit-blasted at prepare), so
     * bb_predicate hits the memo cache; encoding non-top-level guarantees the
     * disjunct node has a SAT variable to assume on. */
    dvs_expr_t *ds = (dvs_expr_t *)xalloc(S->alloc, best_k * sizeof(dvs_expr_t));
    if (!ds) return 0;
    bb_flatten_or(S, best, ds, best_k, 0);
    uint32_t n = 0;
    for (uint32_t i = 0; i < best_k; i++) {
        dvs_bv_t p = bb_predicate(S, ds[i]);
        if (S->had_error || S->had_unsupported || p.size == 0) { n = 0; break; }
        dvs_aig_node_t node = p.bits[0];
        dvs_aig_cnf_encode(S->cnf, node, /*top_level=*/0);
        out[n++] = (int32_t)node;   /* literal true ⟺ disjunct i holds */
    }
    xfree(S->alloc, ds, best_k * sizeof(dvs_expr_t));
    return n;
}

uint32_t dvs_bbsolver_or_groups(dvs_bbsolver_t *S,
                                int32_t *lits, uint32_t lits_cap,
                                uint32_t *sizes, uint32_t max_groups,
                                uint32_t per_group_cap) {
    if (!S || !lits || !sizes || lits_cap == 0 || max_groups == 0) return 0;

    /* Enumerate EVERY surviving top-level `(or ...)` constraint as a group of
     * disjunct assumption literals (same per-disjunct encoding as
     * dvs_bbsolver_or_split_lits, but for all ors, not just the widest). Because
     * each such `or` is asserted true, every group is individually exhaustive;
     * the CARTESIAN PRODUCT across groups (built by the cube driver) therefore
     * remains an exhaustive partition of the search space — all-cubes-UNSAT
     * still soundly implies UNSAT. We only encode groups whose arity fits
     * [2, per_group_cap] and that fit the flat `lits` / `max_groups` budgets;
     * skipped groups just aren't folded into the product (still sound: fewer
     * splits, never a lost model). Groups are returned in constraint order; the
     * driver sorts and selects which to fold under its cube-count cap. */
    uint32_t ngroups = 0, nlits = 0, idx = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; idx++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        dvs_expr_t root = cs->root;
        cur = cs->next;
        if (S->constraint_skip && S->constraint_skip[idx]) continue;
        ExprKind *kp = (ExprKind *)POOL_PTR(S->problem, root);
        if (!kp || *kp != EXPR_BINARY || ((ExprBinary *)kp)->op != DVS_BIN_OR) continue;
        uint32_t k = bb_flatten_or(S, root, NULL, 0, 0);   /* count only */
        if (k < 2 || k > per_group_cap) continue;          /* too small / too wide */
        if (nlits > lits_cap - k) continue;                /* wouldn't fit flat buf */

        dvs_expr_t *ds = (dvs_expr_t *)xalloc(S->alloc, k * sizeof(dvs_expr_t));
        if (!ds) break;
        bb_flatten_or(S, root, ds, k, 0);
        uint32_t got = 0;
        for (uint32_t i = 0; i < k; i++) {
            dvs_bv_t p = bb_predicate(S, ds[i]);
            if (S->had_error || S->had_unsupported || p.size == 0) { got = 0; break; }
            dvs_aig_node_t node = p.bits[0];
            dvs_aig_cnf_encode(S->cnf, node, /*top_level=*/0);
            lits[nlits + i] = (int32_t)node;   /* literal true ⟺ disjunct i holds */
            got++;
        }
        xfree(S->alloc, ds, k * sizeof(dvs_expr_t));
        if (got != k) continue;                /* encoding failed: skip this group */

        sizes[ngroups++] = k;
        nlits += k;
        if (ngroups >= max_groups) break;
    }
    return ngroups;
}

int dvs_bbsolver_solve_assuming(dvs_bbsolver_t *S, const int32_t *lits,
                                uint32_t n, uint32_t conflict_limit) {
    if (!S || !S->sat) return DVS_BB_ERROR;
    /* Per-solve assumptions are retracted after each solve on the incremental
     * backend, so each call solves original ∧ cube in isolation. On a
     * non-incremental backend only n==0 (a single plain solve) is valid; the
     * cube driver guards this via dvs_bbsolver_is_incremental. */
    dvs_sat_set_conflict_limit(S->sat, conflict_limit);
    for (uint32_t i = 0; i < n; i++)
        dvs_sat_assume(S->sat, (dvs_sat_lit_t)lits[i]);
    int rc = dvs_sat_solve(S->sat);
    S->model_in_sat = rc == DVS_BB_SAT;
    S->last_result = rc;
    if (rc == DVS_BB_SAT) {
        /* Fresh model for this cube: drop any stale flip-check valuation and
         * re-diversify so model read-back reflects THIS solve's assignment. */
        free(S->node_val);
        S->node_val = NULL;
        _diversify(S);
    }
    return rc;
}

/* ---- near-uniform sampling (dvs_bbsolver_sample) ------------------------ */

/* The sampler's settings. A feasible set over 2^BB_SAMPLE_M_CAP solutions
 * cannot be covered by the draws a randomize loop makes, so more planes only
 * cost solves. A CDCL solver handles a long parity badly, so each variable
 * contributes at most BB_SAMPLE_BITS_PER_VAR of its low bits to the planes;
 * the bits above vary through _diversify. */
#define BB_SAMPLE_M_CAP          16u
#define BB_SAMPLE_BITS_PER_VAR   16u

static uint64_t bb_splitmix64(uint64_t *st) {
    uint64_t z = (*st += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* The bits of `v` a plane may hash, LSB first: an unsigned domain anchored at
 * 0 is carried by its low bit_length(hi) bits; a signed or offset domain may
 * use them all. Capped at BB_SAMPLE_BITS_PER_VAR. Bounds are int64 bit
 * patterns: an unsigned 64-bit hi above INT64_MAX is negative. */
static uint32_t bb_sample_bits_of(const bb_var_t *v) {
    uint32_t eff = v->width;
    if (!v->is_signed && v->lo == 0) {
        uint32_t bl = 0;
        for (uint64_t h = (uint64_t)v->hi; h; h >>= 1) bl++;
        if (bl == 0) bl = 1;
        if (bl < eff) eff = bl;
    }
    return eff < BB_SAMPLE_BITS_PER_VAR ? eff : BB_SAMPLE_BITS_PER_VAR;
}

/* floor(log2) of the declared domains' product, which over-bounds
 * log2(#solutions) (constraints only shrink the set); capped. */
static uint32_t bb_sample_m_estimate(dvs_bbsolver_t *S, const uint32_t *vars,
                                     uint32_t n_vars) {
    const uint64_t cap = (uint64_t)1 << (BB_SAMPLE_M_CAP + 2);
    uint64_t n = 1;
    for (uint32_t i = 0; i < n_vars && n < cap; i++) {
        if (vars[i] >= S->n_vars || !S->vars[vars[i]].defined) continue;
        const bb_var_t *v = &S->vars[vars[i]];
        int empty = v->is_signed ? v->hi < v->lo
                                 : (uint64_t)v->hi < (uint64_t)v->lo;
        uint64_t span = empty ? 1 : (uint64_t)v->hi - (uint64_t)v->lo + 1;
        if (span == 0 || span >= cap) { n = cap; break; }   /* 0: the full 2^64 */
        n = n > cap / span ? cap : n * span;
    }
    uint32_t m = 0;
    while ((n >> 1) >= 1 && m < 63) { n >>= 1; m++; }
    return m;
}

/* One random parity plane over `bits`: an assumption literal that holds
 * when the XOR of a random half of the bits equals a random bit. 0 when
 * eight draws gave only constant planes. */
static int32_t bb_sample_plane(dvs_bbsolver_t *S, const dvs_aig_node_t *bits,
                               uint32_t n_bits, uint64_t *rng) {
    for (int attempt = 0; attempt < 8; attempt++) {
        dvs_aig_node_t x = DVS_AIG_FALSE;
        uint64_t word = 0;
        for (uint32_t i = 0; i < n_bits; i++) {
            if ((i & 63u) == 0) word = bb_splitmix64(rng);
            if ((word >> (i & 63u)) & 1u) x = dvs_aig_mk_xor(S->aig, x, bits[i]);
        }
        if (bb_splitmix64(rng) & 1u) x = dvs_aig_not(x);
        if (x == DVS_AIG_TRUE || x == DVS_AIG_FALSE) continue;
        dvs_aig_cnf_encode(S->cnf, x, /*top_level=*/0);
        return (int32_t)x;
    }
    return 0;
}

static int bb_sample_solve(dvs_bbsolver_t *S, const int32_t *planes, uint32_t k) {
    for (uint32_t i = 0; i < S->n_frames; i++)
        dvs_sat_assume(S->sat, (dvs_sat_lit_t)S->frames[i].sel);
    for (uint32_t i = 0; i < k; i++)
        dvs_sat_assume(S->sat, (dvs_sat_lit_t)planes[i]);
    return dvs_sat_solve(S->sat);
}

void dvs_bbsolver_work(const dvs_bbsolver_t *S, uint64_t *clauses,
                       uint64_t *sat_work, int *cadical, uint64_t *aux) {
    *aux      = S ? S->aux_work : 0;
    *clauses  = S && S->sat ? dvs_sat_num_clauses(S->sat) : 0;
    *sat_work = S && S->sat ? dvs_sat_work(S->sat) : 0;
    *cadical  = S && S->sat && dvs_sat_backend(S->sat) == DVS_SAT_BACKEND_CADICAL;
}

int dvs_bbsolver_sample_stale(const dvs_bbsolver_t *S) {
    if (!S || !S->sample_ready) return 0;
    uint64_t planes = dvs_aig_num_ands(S->aig) - S->sample_base_ands;
    return planes > S->sample_base_ands + 8192u;
}

int dvs_bbsolver_sample(dvs_bbsolver_t *S, const uint32_t *vars,
                        uint32_t n_vars, uint64_t seed) {
    if (!S || !S->problem || !S->sat) return DVS_BB_ERROR;
    if (!dvs_sat_is_incremental(S->sat)) return DVS_BB_ERROR;

    if (!S->sample_ready) {
        S->seed = seed;
        dvs_sat_set_seed(S->sat, seed);
        int enc = _bb_encode(S);
        if (enc != DVS_BB_ENCODE_READY) return enc;
        S->sample_ready = 1;
        S->sample_base_ands = dvs_aig_num_ands(S->aig);
        const char *mcl = getenv("DV_KISSAT_LIGHT_MAXCLAUSES");
        uint64_t thresh = mcl && *mcl ? strtoull(mcl, NULL, 10) : 2000000;
        dvs_sat_set_light_search(S->sat, dvs_aig_cnf_num_clauses(S->cnf) < thresh);
    }
    {
        const char *mc = getenv("DV_BB_MAX_CONFLICTS");
        dvs_sat_set_conflict_limit(S->sat, mc && *mc ? (uint32_t)atoi(mc) : 0);
    }

    /* The hashed bits: AIG nodes of the variables' low bits (inputs, or the
     * gates of a substituted variable), constants left out. A variable no
     * constraint names has no bits; it is free, and _diversify fills it. */
    uint32_t n_bits = 0;
    for (uint32_t i = 0; i < n_vars; i++)
        if (vars[i] < S->n_vars && S->vars[vars[i]].defined && S->vars[vars[i]].bv_built)
            n_bits += bb_sample_bits_of(&S->vars[vars[i]]);
    dvs_aig_node_t *bits = n_bits
        ? (dvs_aig_node_t *)malloc(n_bits * sizeof(dvs_aig_node_t)) : NULL;
    if (n_bits && !bits) return DVS_BB_ERROR;
    n_bits = 0;
    for (uint32_t i = 0; i < n_vars; i++) {
        if (vars[i] >= S->n_vars || !S->vars[vars[i]].defined) continue;
        bb_var_t *v = &S->vars[vars[i]];
        if (!v->bv_built) continue;
        uint32_t eff = bb_sample_bits_of(v);
        for (uint32_t b = 0; b < eff && b < v->bv.size; b++) {
            dvs_aig_node_t nd = v->bv.bits[v->bv.size - 1 - b];   /* MSB first */
            if (nd != DVS_AIG_TRUE && nd != DVS_AIG_FALSE) bits[n_bits++] = nd;
        }
    }

    uint32_t m_max = bb_sample_m_estimate(S, vars, n_vars) + 2;
    if (m_max > n_bits) m_max = n_bits;
    if (m_max > BB_SAMPLE_M_CAP) m_max = BB_SAMPLE_M_CAP;

    /* A random first-try phase for every bit of the sampled variables: the
     * planes hash only low bits, and a reused instance would otherwise decide
     * the rest as it did last draw (its saved phases). */
    {
        uint64_t prng = seed ^ 0xA0761D6478BD642Full;
        uint64_t word = 0;
        uint32_t used = 64;
        for (uint32_t i = 0; i < n_vars; i++) {
            if (vars[i] >= S->n_vars || !S->vars[vars[i]].defined) continue;
            bb_var_t *v = &S->vars[vars[i]];
            if (!v->bv_built) continue;
            for (uint32_t b = 0; b < v->bv.size; b++) {
                dvs_aig_node_t nd = v->bv.bits[b];
                if (nd == DVS_AIG_TRUE || nd == DVS_AIG_FALSE) continue;
                if (used == 64) { word = bb_splitmix64(&prng); used = 0; }
                int32_t var = nd < 0 ? -nd : nd;
                dvs_sat_phase(S->sat, (dvs_sat_lit_t)(((word >> used++) & 1u) ? var : -var));
            }
        }
    }

    /* Nested cells: plane k+1 only shrinks the cell of planes 1..k, so the
     * satisfiable levels are 0..k* for some k*. The cell at k* is the smallest
     * non-empty one, near log2(#solutions), where one model is a near-uniform
     * draw. Find k* from the top: a feasible set too big to empty (the common
     * case) takes one solve; otherwise binary search, ~log2(m_max) solves. */
    uint64_t rng = seed;
    int32_t planes[BB_SAMPLE_M_CAP];
    uint32_t n_planes = 0;
    while (n_planes < m_max) {
        int32_t p = bb_sample_plane(S, bits, n_bits, &rng);
        if (!p) break;
        planes[n_planes++] = p;
    }
    free(bits);

    int rc = bb_sample_solve(S, planes, n_planes);
    int best = rc == DVS_BB_SAT ? (int)n_planes : -1, solved_at = best;
    if (best < 0) {
        int lo = 0, hi = (int)n_planes - 1, undecided = rc != DVS_BB_UNSAT;
        while (lo <= hi) {
            int mid = lo + (hi - lo) / 2;
            rc = bb_sample_solve(S, planes, (uint32_t)mid);
            if (rc == DVS_BB_SAT) { best = solved_at = mid; lo = mid + 1; continue; }
            if (rc != DVS_BB_UNSAT) undecided = 1;   /* conflict limit: no model here */
            hi = mid - 1;
        }
        if (best < 0) {
            S->model_in_sat = 0;
            S->last_result = undecided ? DVS_BB_UNKNOWN : DVS_BB_UNSAT;
            return S->last_result;
        }
        if (solved_at != best || rc != DVS_BB_SAT) {
            rc = bb_sample_solve(S, planes, (uint32_t)best);
            if (rc != DVS_BB_SAT) {
                S->model_in_sat = 0;
                S->last_result = rc;
                return rc;
            }
        }
    }
    S->model_in_sat = 1;
    S->last_result = DVS_BB_SAT;
    free(S->node_val);
    S->node_val = NULL;
    S->seed = seed ? seed : 1;
    _diversify_sweeps(S, DVS_DIVERSIFY_SWEEPS);  /* the uniformity promise */
    return DVS_BB_SAT;
}

/* --- Parallel cube-and-conquer support (P2; see dvs_bbsolver.h, dvs_cube.c). */

void dvs_bbsolver_record_clauses(dvs_bbsolver_t *S) {
    if (S && S->sat) dvs_sat_record_start(S->sat);
}

const int32_t *dvs_bbsolver_clause_db(dvs_bbsolver_t *S, size_t *n_lits,
                                      int32_t *max_var) {
    if (n_lits) *n_lits = 0;
    if (max_var) *max_var = 0;
    if (!S || !S->sat) return NULL;
    if (max_var) *max_var = (int32_t)dvs_sat_max_var(S->sat);
    return dvs_sat_recorded(S->sat, n_lits);
}

int dvs_bbsolver_install_worker_model(dvs_bbsolver_t *S, dvs_sat_t *wsat) {
    if (!S || !wsat || !S->aig) return -1;
    uint32_t N = (uint32_t)dvs_aig_num_nodes(S->aig);
    /* node_val is indexed by AIG node id (0..N); readback prefers it over the
     * live SAT instance, so installing it here makes THIS solver report the
     * worker's model without ever solving on its own instance. AIG node id ==
     * SAT var id (identity), so each node's truth is read straight from the
     * worker's assignment; the TRUE constant (id 1) is forced, and free/undriven
     * nodes take whatever the worker assigned (a sound don't-care — they lie
     * outside every asserted cone). */
    int8_t *nv = (int8_t *)malloc((size_t)(N + 1));
    if (!nv) return -1;
    nv[0] = 0;
    if (N >= 1) nv[1] = 1;                 /* DVS_AIG_TRUE */
    for (uint32_t id = 2; id <= N; id++)
        nv[id] = (int8_t)(dvs_sat_value(wsat, (dvs_sat_var_t)id) > 0 ? 1 : 0);
    free(S->node_val);
    S->node_val = nv;
    S->model_in_sat = 0;
    S->last_result = DVS_BB_SAT;
    return 0;
}

static int _rediversify_same_base(dvs_bbsolver_t *S, uint64_t seed);
static int _fresh_base(dvs_bbsolver_t *S, uint64_t seed);

/* A count, never time: a fresh base every 8 calls bounds how long samples stay
 * flips of one model. */
#define DVS_REDIV_EPOCH 8u

int dvs_bbsolver_rediversify(dvs_bbsolver_t *S, uint64_t seed) {
    if (!S || S->last_result != DVS_BB_SAT) return -1;
    if (++S->redivs_since_base >= DVS_REDIV_EPOCH && !S->n_frames && seed
            && (S->resample || (S->base_sim && !S->asserts_lost))
            && _fresh_base(S, seed) == 0)
        return 0;
    return _rediversify_same_base(S, seed);
}

static int _rediversify_same_base(dvs_bbsolver_t *S, uint64_t seed) {
    /* Drop the prior flip-check result and re-run the diversity pass over the
     * still-valid SAT model with the new seed. _diversify re-reads the base
     * model via dvs_aig_cnf_value() (kissat's assignment persists until
     * dvs_bbsolver_free) and only depends on S->seed for the flip targets, so a
     * new seed yields a different sound model with no re-solve. When the model
     * did not come from S->sat (simulation, the affine check, a cube or
     * portfolio worker), S->sat's assignment is stale or absent: diversify from
     * node_val, the current model, instead. */
    if (S->model_in_sat) {
        free(S->node_val);
        S->node_val = NULL;
    } else if (!S->node_val) {
        return -1;
    }
    S->seed = seed;
    _diversify(S);
    return 0;
}

/* The seeded steps _solve_with_sim runs before SAT, without the SAT race: the
 * affine check, then one simulation pass (exhaustive when small enough, else
 * budgeted random). */
static int _sim_once(dvs_bbsolver_t *S) {
    const char *aff_env = getenv("DV_AFFINE");
    if (!aff_env || strcmp(aff_env, "0") != 0) {
        dvs_affine_stats_t ast;
        int8_t *nv = NULL;
        int ar = dvs_affine_check(S->aig, S->assert_nodes, S->n_asserts,
                                  _env_u64("DV_AFFINE_MAX_WORK", 0), S->seed, &nv, &ast);
        if (ar == DVS_AFFINE_SAT && nv) {
            free(S->node_val);
            S->node_val = nv;
            S->model_in_sat = 0;
            return DVS_BB_SAT;
        }
        free(nv);
        if (ar == DVS_AFFINE_UNSAT) return DVS_BB_UNKNOWN;   /* never trust it over the last verdict */
    }
    int src = _sim_split(S);
    if (src == DVS_BB_SAT) return src;
    if (src == DVS_BB_UNSAT) return DVS_BB_UNKNOWN;   /* never trust it over the last verdict */
    dvs_aigsim_t *sim = dvs_aigsim_new(S->aig, S->assert_nodes, S->n_asserts);
    if (!sim) return DVS_BB_UNKNOWN;
    uint64_t work = dvs_aigsim_exhaustive_work(sim);
    uint64_t per_pass = (uint64_t)dvs_aigsim_gates(sim) + S->n_asserts;
    int r;
    if (work <= _env_u64("DV_SIM_MAX_WORK", 1500000ull)) {
        r = dvs_aigsim_exhaustive(sim, NULL, S->seed);
    } else {
        uint64_t blocks = 1000000ull / (per_pass ? per_pass : 1);
        r = dvs_aigsim_random(sim, (uint32_t)(blocks < 8 ? blocks : 8), S->seed);
    }
    S->aux_work += dvs_aigsim_vectors(sim) / 64u * per_pass;
    int rc = _sim_result(S, sim, r);
    dvs_aigsim_free(sim);
    return rc;
}

/* A new seeded SAT search over the recorded clauses, on a fresh instance of
 * this instance's backend (seeded before any clause: CaDiCaL honours the seed
 * only then). Its model is installed as node_val. */
static int _sat_fresh(dvs_bbsolver_t *S) {
    size_t n = 0;
    int32_t max_var = 0;
    const int32_t *db = dvs_bbsolver_clause_db(S, &n, &max_var);
    if (!db || !n) return DVS_BB_UNKNOWN;
    dvs_sat_t *w = dvs_sat_new_backend(NULL, dvs_sat_backend(S->sat));
    if (!w) return DVS_BB_UNKNOWN;
    dvs_sat_set_seed(w, S->seed);
    dvs_sat_reserve(w, (dvs_sat_var_t)max_var);
    for (size_t i = 0; i < n; i++) dvs_sat_add(w, (dvs_sat_lit_t)db[i]);
    const char *mcl = getenv("DV_KISSAT_LIGHT_MAXCLAUSES");
    uint64_t thresh = mcl && *mcl ? strtoull(mcl, NULL, 10) : 2000000;
    dvs_sat_set_light_search(w, dvs_aig_cnf_num_clauses(S->cnf) < thresh);
    int rc = dvs_sat_solve(w);
    if (rc == DVS_BB_SAT && dvs_bbsolver_install_worker_model(S, w) != 0) rc = DVS_BB_UNKNOWN;
    dvs_sat_free(w);
    return rc;
}

int dvs_bbsolver_resample(dvs_bbsolver_t *S, uint64_t seed) {
    if (!S || S->last_result != DVS_BB_SAT) return -1;
    if (S->n_frames || !seed) return dvs_bbsolver_rediversify(S, seed);
    if (_fresh_base(S, seed) == 0) return 0;
    /* UNSAT here would contradict the last verdict: never trust it over
     * the cached model, which is still valid. */
    return _rediversify_same_base(S, seed);
}

/* Draw a new base model (simulation, else a seeded fresh SAT search) and
 * diversify it. Returns 0 on success, -1 leaving the cached model in place. */
static int _fresh_base(dvs_bbsolver_t *S, uint64_t seed) {
    S->seed = seed;
    /* The popcount sampler draws a model that needs no diversify; a miss
     * leaves the cached model in place. (No _sim_check: the verdict is
     * already SAT, and a second kissat solve is not supported.) */
    int rc = _popsamp(S);
    if (rc != DVS_BB_SAT) rc = _uniqsamp(S);
    if (rc != DVS_BB_SAT) rc = _sortsamp(S);
    if (rc != DVS_BB_SAT) rc = _divsamp(S);
    if (rc == DVS_BB_SAT) {
        S->last_result = DVS_BB_SAT;
        S->redivs_since_base = 0;
        return 0;
    }
    rc = DVS_BB_UNKNOWN;
    if (S->base_sim && !S->asserts_lost) rc = _sim_once(S);
    if (rc != DVS_BB_SAT && S->resample) rc = _sat_fresh(S);
    if (rc != DVS_BB_SAT) return -1;
    S->last_result = DVS_BB_SAT;
    S->redivs_since_base = 0;
    _diversify(S);
    return 0;
}

/* ------------------------------------------------------------------------- *
 * Incremental extension (Phase 5a/5b foundation).
 *
 * Keep ONE live bbsolver instance across successive solves: add constraints and
 * re-solve without tearing down the AIG/SAT/CNF. On the CaDiCaL backend this
 * retains learned clauses across solves (true incrementality); on kissat it
 * still re-solves the accumulated DB correctly, just without cross-solve
 * learning. Monotonic add ONLY — there is no scoped retraction here. Scoped
 * push/pop over incrementally added clauses needs selector variables minted in
 * the AIG id space (AIG node id == SAT var id), which is deliberately out of
 * this layer; see docs/phase5_incremental_bitblast_scope.md (Gap B, sub-phase
 * 5c, held). The frontend delta plumbing that would drive these is 5d (held).
 * ------------------------------------------------------------------------- */

/* Bit-blast one additional predicate (an dvs_expr_t into the live problem's pool)
 * and assert it as a hard top-level constraint on the running instance,
 * building and bounding any newly referenced variables. Does not solve — call
 * dvs_bbsolver_resolve afterward. The predicate is bit-blasted directly (no
 * substitution pass), so it composes with whatever a prior check already
 * asserted. Returns 0 on success, DVS_BB_UNKNOWN for an unsupported construct,
 * DVS_BB_ERROR on a hard error. */
/* Grow S->vars/subst/resolving to cover variables added to S->problem after
 * construction (e.g. read vars the lazy array loop mints between solves). Safe
 * to call before any assert; a no-op when no new vars appeared. bv_for_var and
 * assert's bounds loop both index by var_id < S->n_vars, so this keeps them in
 * range. The memo cache bounds-checks out-of-range refs itself, so it needs no
 * growth (new lemma nodes are simply re-blasted without memoization). */
/* Fill each table entry from `from` up that is not yet defined from the
 * first `walk` entries of the problem's variable list. */
static void bb_fill_vars(dvs_bbsolver_t *S, uint32_t from, uint32_t walk) {
    uint32_t k = 0;
    for (dvs_expr_t cur = S->problem->vars_head; cur != EXPR_NULL && k++ < walk; ) {
        VarSpec *vs = (VarSpec *)POOL_PTR(S->problem, cur);
        if (vs->var_id >= from && vs->var_id < S->n_vars && !S->vars[vs->var_id].defined) {
            bb_var_t *v = &S->vars[vs->var_id];
            v->width = vs->width; v->is_signed = vs->is_signed;
            v->defined = 1; v->lo = vs->lo; v->hi = vs->hi;
        }
        cur = vs->next;
    }
}

static void bb_grow_vars(dvs_bbsolver_t *S) {
    /* A session walks only the variables added since its last sync (the
     * list is newest first and only grows between pops); after a pop, all
     * of them, to refill the entries the pop cleared (ids are reused). */
    uint32_t walk = UINT32_MAX;
    if (S->sess && !S->refill && S->problem->n_vars >= S->seen_vars)
        walk = S->problem->n_vars - S->seen_vars;
    uint32_t max_id = 0, k = 0; int any = 0;
    for (dvs_expr_t cur = S->problem->vars_head; cur != EXPR_NULL && k < walk; k++) {
        VarSpec *vs = (VarSpec *)POOL_PTR(S->problem, cur);
        if (!any || vs->var_id > max_id) { max_id = vs->var_id; any = 1; }
        cur = vs->next;
    }
    uint32_t need = any ? max_id + 1 : 0;
    if (need <= S->n_vars) {
        if (S->sess) {
            bb_fill_vars(S, 0, walk);
            S->seen_vars = S->problem->n_vars;
            S->refill = 0;
        }
        return;
    }
    uint32_t old = S->n_vars;
    bb_var_t *nv = (bb_var_t *)xalloc(S->alloc, need * sizeof(bb_var_t));
    if (!nv) return;   /* OOM: leave as-is; assert's var_id<n_vars guard holds */
    memset(nv, 0, need * sizeof(bb_var_t));
    if (S->vars) {
        memcpy(nv, S->vars, old * sizeof(bb_var_t));
        xfree(S->alloc, S->vars, old * sizeof(bb_var_t));
    }
    S->vars = nv;
    if (S->subst) {
        dvs_expr_t *ns = (dvs_expr_t *)xalloc(S->alloc, need * sizeof(dvs_expr_t));
        if (ns) {
            memcpy(ns, S->subst, old * sizeof(dvs_expr_t));
            for (uint32_t i = old; i < need; i++) ns[i] = EXPR_NULL;
            xfree(S->alloc, S->subst, old * sizeof(dvs_expr_t));
            S->subst = ns;
        } else {
            return;
        }
    }
    if (S->slice_head) {
        uint32_t *nh = (uint32_t *)realloc(S->slice_head, need * sizeof(uint32_t));
        if (nh) {
            for (uint32_t i = old; i < need; i++) nh[i] = BB_NO_SLICE;
            S->slice_head = nh;
        } else {
            return;   /* OOM: leave n_vars as-is so every index stays in range */
        }
    }
    if (S->resolving) {
        uint8_t *nr = (uint8_t *)xalloc(S->alloc, need * sizeof(uint8_t));
        if (nr) {
            memcpy(nr, S->resolving, old * sizeof(uint8_t));
            memset(nr + old, 0, need - old);
            xfree(S->alloc, S->resolving, old * sizeof(uint8_t));
            S->resolving = nr;
        } else {
            return;
        }
    }
    if (S->rhs_var) {
        uint8_t *nr = (uint8_t *)realloc(S->rhs_var, need);
        if (nr) {
            memset(nr + old, 0, need - old);
            S->rhs_var = nr;
        } else {
            free(S->rhs_var);       /* exact fallback: always walk */
            S->rhs_var = NULL;
        }
    }
    S->n_vars = need;
    bb_fill_vars(S, S->sess ? 0 : old, walk);
    if (S->sess) {
        S->seen_vars = S->problem->n_vars;
        S->refill = 0;
    }
}

void dvs_bbsolver_set_diversify(dvs_bbsolver_t *S, int on) {
    if (S) S->no_diversify = !on;
}

int dvs_bbsolver_rebase(dvs_bbsolver_t *S, dvs_problem_t *problem) {
    if (!S || !problem || S->orig_problem) return -1;
    S->problem = problem;
    /* Extend the memo cache over the new pool range, so shared subterms of the
     * added constraints are blasted once. */
    uint32_t cap = dvs_pool_used(&problem->pool);
    if (cap > S->cache_cap) {
        if (cap < 2 * S->cache_cap) cap = 2 * S->cache_cap;   /* amortized growth */
        bb_cache_entry_t *nc = (bb_cache_entry_t *)xalloc(S->alloc,
                                                          cap * sizeof(bb_cache_entry_t));
        if (nc) {
            if (S->cache) memcpy(nc, S->cache, S->cache_cap * sizeof(bb_cache_entry_t));
            memset(nc + S->cache_cap, 0, (cap - S->cache_cap) * sizeof(bb_cache_entry_t));
            xfree(S->alloc, S->cache, S->cache_cap * sizeof(bb_cache_entry_t));
            S->cache = nc;
            S->cache_cap = cap;
        }
    }
    bb_grow_vars(S);
    return 0;
}

int dvs_bbsolver_forget_from(dvs_bbsolver_t *S, uint32_t pool_off) {
    if (!S) return -1;
    if (S->cache && pool_off < S->cache_cap)
        memset(S->cache + pool_off, 0,
               (S->cache_cap - pool_off) * sizeof(bb_cache_entry_t));
    return 0;
}

int dvs_bbsolver_assert(dvs_bbsolver_t *S, dvs_expr_t pred_ref) {
    if (!S || !S->problem || pred_ref == EXPR_NULL) return DVS_BB_ERROR;
    /* Blasting an SV-elaborated private copy: bring it up to date with the
     * caller's problem and elaborate the new predicate into it. */
    if (S->orig_problem) {
        dvs_expr_t er;
        if (dvs_sv_elaborate_more(&S->problem, S->orig_problem,
                                  &S->orig_synced, pred_ref, &er) != 0)
            return DVS_BB_ERROR;
        pred_ref = er;
    }
    /* Adding clauses after a solve is only legal on an incremental backend;
     * kissat aborts on add-after-solve. Non-incremental callers must free +
     * rebuild instead (the current frontend behavior). */
    if (!dvs_sat_is_incremental(S->sat)) return DVS_BB_ERROR;
    /* Cover any vars minted into the problem since construction (lazy arrays). */
    bb_grow_vars(S);

    /* bv_for_var uses S->resolving as its recursion guard; clear it (the base
     * check left it as run_subst_pass scratch). */
    if (S->resolving) memset(S->resolving, 0, S->n_vars);

    dvs_bv_t pred = bb_predicate(S, pred_ref);
    if (S->had_error)       { S->last_result = DVS_BB_ERROR;   return DVS_BB_ERROR; }
    if (S->had_unsupported) { S->last_result = DVS_BB_UNKNOWN; return DVS_BB_UNKNOWN; }
    assert_top_r(S, pred.bits[0], pred_ref);

    /* Bound any variable this predicate caused to be built for the first time.
     * bounds_asserted keeps this idempotent across calls and vs. the base
     * check's bounds pass. */
    for (uint32_t i = 0; i < S->n_vars; i++) {
        if (S->vars[i].defined && S->vars[i].bv_built && !S->vars[i].bounds_asserted) {
            assert_var_bounds(S, i);
            S->vars[i].bounds_asserted = 1;
        }
    }
    if (S->had_unsupported) { S->last_result = DVS_BB_UNKNOWN; return DVS_BB_UNKNOWN; }
    if (S->had_error)       { S->last_result = DVS_BB_ERROR;   return DVS_BB_ERROR; }
    return 0;
}

/* Re-solve the live instance, reusing the accumulated clause DB (and, on
 * CaDiCaL, the retained learned clauses). Mirrors the solve tail of
 * dvs_bbsolver_check. Returns DVS_BB_SAT/UNSAT/UNKNOWN/ERROR. */
int dvs_bbsolver_resolve(dvs_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->problem) return DVS_BB_ERROR;
    /* Re-solving a live instance requires an incremental backend; kissat is
     * one-shot per lifetime. */
    if (!dvs_sat_is_incremental(S->sat)) return DVS_BB_ERROR;
    S->seed = seed;
    dvs_sat_set_seed(S->sat, seed);
    /* Drop the prior solve's flip-check model; the next readback re-derives. */
    free(S->node_val);
    S->node_val = NULL;
    int rc = dvs_sat_solve(S->sat);
    S->model_in_sat = rc == DVS_BB_SAT;
    S->last_result = rc;
    if (rc == DVS_BB_SAT) _diversify(S);
    return rc;
}

/* Re-solve WITHOUT the don't-care diversification, reading back the solver's
 * raw assignment (seed 0 => dvs_bbsolver_value returns the actual SAT model, not
 * a seeded-random fill of free bits). The lazy array refinement loop must see the
 * true model: a lazily-unconstrained read var is a don't-care bit that diversify
 * would randomize, corrupting the array-consistency check into a spurious SAT. */
int dvs_bbsolver_resolve_raw(dvs_bbsolver_t *S) {
    if (!S || !S->problem) return DVS_BB_ERROR;
    if (!dvs_sat_is_incremental(S->sat)) return DVS_BB_ERROR;
    S->seed = 0;
    dvs_sat_set_seed(S->sat, 0);
    free(S->node_val);
    S->node_val = NULL;   /* value readback -> raw cnf model (seed 0) */
    int rc = dvs_sat_solve(S->sat);
    S->model_in_sat = rc == DVS_BB_SAT;
    S->last_result = rc;
    return rc;
}

/* ------------------------------------------------------------------------- *
 * Session mode (docs/fw_hdl_implementation_plan.md, piece 6.1).
 *
 * One instance serves every check-sat of an SMT-LIB session. Constraints are
 * added in batches; a batch added inside a scope (sess_push) is asserted as
 * (-sel | c) and solved under the assumption sel, and the scope's pop adds
 * -sel for good. Selectors are AIG inputs, so they share the AIG's id space
 * with every other SAT variable.
 *
 * Substitution (`v == e` defines v) works batch by batch: v must not be
 * built yet (an earlier batch did not read it) and, inside a scope, must
 * belong to the innermost scope, whose pop forgets it. The caller reuses
 * variable ids after a pop, so pop clears those entries; expression offsets
 * are never reused (the builder does not reclaim its pool), so the memo cache
 * stays valid.
 * ------------------------------------------------------------------------- */

int dvs_bbsolver_sess_begin(dvs_bbsolver_t *S) {
    if (!S || S->sess || S->orig_problem || S->subst || S->n_asserts
            || !dvs_sat_is_incremental(S->sat))
        return -1;
    uint32_t n = S->n_vars ? S->n_vars : 1;
    S->subst = (dvs_expr_t *)xalloc(S->alloc, n * sizeof(dvs_expr_t));
    S->resolving = (uint8_t *)xalloc(S->alloc, n);
    S->rhs_var = (uint8_t *)calloc(n, 1);
    if (!S->subst || !S->resolving || !S->rhs_var) return -1;
    for (uint32_t i = 0; i < n; i++) S->subst[i] = EXPR_NULL;
    memset(S->resolving, 0, n);
    if (getenv("DV_BB_NO_SLICE") == NULL) {
        S->slice_head = (uint32_t *)malloc(n * sizeof(uint32_t));
        if (!S->slice_head) return -1;
        for (uint32_t i = 0; i < n; i++) S->slice_head[i] = BB_NO_SLICE;
    }
    S->sess = 1;
    return 0;
}

uint32_t dvs_bbsolver_sess_depth(const dvs_bbsolver_t *S) {
    return S ? S->n_frames : 0;
}

int dvs_bbsolver_sess_push(dvs_bbsolver_t *S, uint32_t var_mark) {
    if (!S || !S->sess) return -1;
    if (S->n_frames && var_mark < S->frames[S->n_frames - 1].var_mark) return -1;
    if (S->n_frames == S->cap_frames) {
        uint32_t c = S->cap_frames ? 2 * S->cap_frames : 8;
        bb_frame_t *f = (bb_frame_t *)realloc(S->frames, c * sizeof(*f));
        if (!f) return -1;
        S->frames = f; S->cap_frames = c;
    }
    bb_frame_t *f = &S->frames[S->n_frames];
    f->sel = dvs_aig_mk_input(S->aig);
    dvs_aig_cnf_encode(S->cnf, f->sel, /*top_level=*/0);
    f->var_mark  = var_mark;
    f->n_asserts = S->n_asserts;
    f->n_slices  = S->n_slices;
    S->n_frames++;
    S->sub_floor = var_mark;
    return 0;
}

int dvs_bbsolver_sess_pop(dvs_bbsolver_t *S) {
    if (!S || !S->sess || !S->n_frames) return -1;
    bb_frame_t *f = &S->frames[--S->n_frames];
    dvs_sat_add_unit(S->sat, (dvs_sat_lit_t)-f->sel);
    if (!S->asserts_lost) {
        /* Drop the scope's entries; keep the permanent ones made inside it
         * (bounds of a variable from outside the scope). */
        uint32_t k = f->n_asserts, depth = S->n_frames + 1;
        for (uint32_t i = f->n_asserts; i < S->n_asserts; i++)
            if (S->assert_tags[i] < depth) {
                S->assert_nodes[k] = S->assert_nodes[i];
                if (S->assert_roots) S->assert_roots[k] = S->assert_roots[i];
                S->assert_tags[k++] = S->assert_tags[i];
            }
        S->n_asserts = k;
    }
    for (uint32_t v = f->var_mark; v < S->n_vars; v++) {
        memset(&S->vars[v], 0, sizeof(S->vars[v]));
        S->subst[v] = EXPR_NULL;
        S->resolving[v] = 0;
        if (S->rhs_var) S->rhs_var[v] = 0;
        if (S->slice_head) S->slice_head[v] = BB_NO_SLICE;
    }
    S->n_slices  = f->n_slices;
    S->sub_floor = S->n_frames ? S->frames[S->n_frames - 1].var_mark : 0;
    S->refill = 1;
    free(S->node_val);
    S->node_val = NULL;
    S->last_result = DVS_BB_UNKNOWN;
    return 0;
}

/* The innermost scope owning variable `v`, plus one (0: none). */
static uint32_t sess_owner(const dvs_bbsolver_t *S, uint32_t v) {
    for (uint32_t i = S->n_frames; i-- > 0; )
        if (S->frames[i].var_mark <= v) return i + 1;
    return 0;
}

int dvs_bbsolver_sess_add(dvs_bbsolver_t *S, const dvs_expr_t *roots, uint32_t n) {
    if (!S || !S->sess) return DVS_BB_ERROR;
    if (S->had_error)       return DVS_BB_ERROR;
    if (S->had_unsupported) return DVS_BB_UNKNOWN;
    if (S->problem->sources_head != EXPR_NULL) return DVS_BB_UNKNOWN;
    uint8_t *skip = (uint8_t *)calloc(n ? n : 1, 1);
    if (!skip) return DVS_BB_ERROR;
    S->n_new_subst = 0;
    /* Newest first, as run_subst_pass walks the constraint list: a
     * transition's `(= w15 w16)` then comes before the bindings of w15 and
     * w16, so slices go from the next state to the current one and the
     * transition folds away. Oldest first left 2048 of crc32x16's
     * equalities to the affine pass at every check. (subst_reaches_var
     * stops at built variables, so the walk stays inside the batch.) */
    if (getenv("DV_BB_NO_SUBST") == NULL)
        for (uint32_t i = n; i-- > 0; ) skip[i] = (uint8_t)collect_substs_from(S, roots[i]);
    count_fanout_from(S, roots, skip, n);
    memset(S->resolving, 0, S->n_vars);

    S->assert_frame = S->n_frames;
    for (uint32_t i = 0; i < n && !S->had_error && !S->had_unsupported; i++) {
        if (skip[i]) continue;
        dvs_bv_t pred = bb_predicate(S, roots[i]);
        if (S->had_error || S->had_unsupported) break;
        assert_top_r(S, pred.bits[0], roots[i]);
    }
    free(skip);
    /* A substituted variable's bits are gates: encode them so the model
     * reads them (see the same pass in _bb_encode). */
    for (uint32_t i = 0; i < S->n_new_subst && !S->had_error; i++) {
        dvs_bv_t bv = bv_for_var(S, S->new_subst[i]);
        if (S->had_error) break;
        for (uint32_t bit = 0; bit < bv.size; bit++)
            dvs_aig_cnf_encode(S->cnf, bv.bits[bit], /*top_level=*/0);
    }
    for (uint32_t v = 0; v < S->n_vars && !S->had_error; v++) {
        if (S->vars[v].defined && S->vars[v].bv_built && !S->vars[v].bounds_asserted) {
            S->assert_frame = sess_owner(S, v);
            assert_var_bounds(S, v);
            S->vars[v].bounds_asserted = 1;
        }
    }
    S->assert_frame = 0;
    if (S->had_error)       { S->last_result = DVS_BB_ERROR;   return DVS_BB_ERROR; }
    if (S->had_unsupported) { S->last_result = DVS_BB_UNKNOWN; return DVS_BB_UNKNOWN; }
    return 0;
}

/* The input bits the live asserts force through their top-level conjunctions:
 * tv[id] = 0 / 1, or 2 when not forced (tv has N + 1 entries, seen is zeroed,
 * wl has N + 2). Returns 1, DVS_BB_UNSAT on a contradiction, or 0 (give up). */
static int _forced_inputs(dvs_bbsolver_t *S, uint32_t N, int8_t *tv, uint8_t *seen,
                          dvs_aig_node_t *wl, uint32_t *n_forced) {
    dvs_aig_t *m = S->aig;
    memset(tv, 2, (size_t)N + 1);
    tv[0] = 0;
    tv[1] = 1;
    for (uint32_t a = 0; a < S->n_asserts; a++) {
        uint32_t top = 0;
        wl[top++] = S->assert_nodes[a];
        while (top) {
            dvs_aig_node_t lit = wl[--top];
            if (lit == DVS_AIG_TRUE) continue;
            if (lit == DVS_AIG_FALSE) return DVS_BB_UNSAT;
            if (lit == 0) return 0;
            uint32_t id = (uint32_t)(lit < 0 ? -lit : lit);
            if (id > N) return 0;
            if (dvs_aig_is_input(m, (dvs_aig_node_t)id)) {
                int8_t want = lit > 0 ? 1 : 0;
                if (tv[id] == 2) { tv[id] = want; (*n_forced)++; }
                else if (tv[id] != want) return DVS_BB_UNSAT;
            } else if (lit > 0 && !seen[id] && dvs_aig_is_and(m, lit)) {
                dvs_aig_node_t l, r;
                seen[id] = 1;   /* each AND pushes once: top <= N + 1 */
                dvs_aig_get_children(m, lit, &l, &r);
                wl[top++] = l;
                wl[top++] = r;
            }
        }
    }
    return 1;
}

/* Popcount sampler (DV_POPSAMP=0 off). A component is a variable R that
 * try_record_popcount defined as popcount(X), X a plain variable of n <= 63
 * bits whose bits are AIG inputs or constants. K is the set of k at which
 * every assert reading only R's bits holds. A draw takes X uniformly from
 * {x : x has the forced input bits, popcount(x) in K} (k weighted by the
 * number of such x, then a uniform subset of the free bits) and every other
 * input a uniform bit unless forced. Every assert is then evaluated on the
 * AIG: the first draw that satisfies them all is the model, else after
 * DVS_PS_DRAWS draws the normal path runs. The proposal is uniform over a
 * superset of the models, so an accepted draw is uniform over the models,
 * and it satisfies every assert, so it is sound. */
#define DVS_PS_MAX_RS    4

struct bb_psk {                 /* one assert's R-only mask, cached */
    dvs_aig_node_t lit;
    uint32_t       rvar;        /* R, or UINT32_MAX: the assert is not R-only */
    uint64_t       rsig;        /* _ps_sig of R's component the mask was computed for */
    uint64_t       kmask;       /* bit k: the assert holds at R == k */
};

typedef struct {
    uint32_t       rvar, n;     /* rvar = rvars[0]; n = width of X */
    uint32_t       xvar, nr;
    uint32_t       rvars[DVS_PS_MAX_RS];   /* every R defined as popcount(X) */
    dvs_aig_node_t xb[64];      /* X bit p (p = 0 the LSB) */
    uint64_t       kmask;
    uint32_t       free_pos[64], nfree, ones;
    uint64_t       w[65], total;
} bb_ps_comp;

#define DVS_PS_MAX_COMPS 8
#define DVS_PS_DRAWS     16

static uint64_t _ps_next(uint64_t *st) {
    uint64_t x = (*st += 0x9E3779B97F4A7C15ULL);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

/* Uniform in [0, n), n >= 1. */
static uint64_t _ps_below(uint64_t *st, uint64_t n) {
    uint64_t m = n - 1;
    m |= m >> 1; m |= m >> 2; m |= m >> 4; m |= m >> 8; m |= m >> 16; m |= m >> 32;
    for (;;) {
        uint64_t r = _ps_next(st) & m;
        if (r < n) return r;
    }
}

static inline uint64_t _lit_word(const uint64_t *wv, dvs_aig_node_t lit) {
    uint64_t w = wv[lit < 0 ? -lit : lit];
    return lit < 0 ? ~w : w;
}

/* The R vars of component C and their LSB literals, as one cache key. */
static uint64_t _ps_sig(const dvs_bbsolver_t *S, const bb_ps_comp *C) {
    uint64_t h = C->nr;
    for (uint32_t i = 0; i < C->nr; i++) {
        const bb_var_t *rv = &S->vars[C->rvars[i]];
        uint64_t b = rv->bv_built ? (uint64_t)(uint32_t)rv->bv.bits[rv->bv.size - 1] : 0;
        h = (h ^ C->rvars[i] ^ (b << 32)) * 0x9E3779B97F4A7C15ULL;
    }
    return h;
}

/* The component whose R vars are the only variables expression `root` reads
 * (through the operators listed; substitutions are not followed), or
 * UINT32_MAX. */
static uint32_t _ps_expr_comp(const dvs_bbsolver_t *S, dvs_expr_t root,
                              const bb_ps_comp *comp, uint32_t nc) {
    enum { CAP = 1024 };
    dvs_expr_t stk[CAP];
    uint32_t n = 0, ci = UINT32_MAX, budget = 4096;
#define PS_PUSH(e) do { if (n == CAP) return UINT32_MAX; stk[n++] = (e); } while (0)
    if (root == EXPR_NULL) return UINT32_MAX;
    PS_PUSH(root);
    while (n) {
        dvs_expr_t r = stk[--n];
        if (!budget--) return UINT32_MAX;
        const ExprKind *kp = (const ExprKind *)POOL_PTR(S->problem, r);
        if (!kp) return UINT32_MAX;
        switch (*kp) {
        case EXPR_CONST:
        case EXPR_CONST_WIDE: break;
        case EXPR_VAR: {
            uint32_t v = ((const ExprVar *)kp)->var_id, hit = UINT32_MAX;
            for (uint32_t c = 0; c < nc && hit == UINT32_MAX; c++)
                for (uint32_t i = 0; i < comp[c].nr; i++)
                    if (comp[c].rvars[i] == v) { hit = c; break; }
            if (hit == UINT32_MAX && v < S->n_vars && S->subst[v] != EXPR_NULL
                    && !popcount_def((dvs_bbsolver_t *)S, v)) {
                PS_PUSH(S->subst[v]);   /* v is defined by an expression: read it */
                break;
            }
            if (hit == UINT32_MAX || (ci != UINT32_MAX && ci != hit)) return UINT32_MAX;
            ci = hit;
            break;
        }
        case EXPR_BINARY:
            PS_PUSH(((const ExprBinary *)kp)->lhs);
            PS_PUSH(((const ExprBinary *)kp)->rhs);
            break;
        case EXPR_UNARY:   PS_PUSH(((const ExprUnary *)kp)->operand); break;
        case EXPR_EXTEND:  PS_PUSH(((const ExprExtend *)kp)->operand); break;
        case EXPR_SV_CAST: PS_PUSH(((const ExprSvCast *)kp)->operand); break;
        case EXPR_EXTRACT: PS_PUSH(((const ExprExtract *)kp)->operand); break;
        case EXPR_CONCAT:
            PS_PUSH(((const ExprConcat *)kp)->hi);
            PS_PUSH(((const ExprConcat *)kp)->lo);
            break;
        case EXPR_ITE:
            PS_PUSH(((const ExprITE *)kp)->cond);
            PS_PUSH(((const ExprITE *)kp)->then_e);
            PS_PUSH(((const ExprITE *)kp)->else_e);
            break;
        case EXPR_IN_RANGE:
            PS_PUSH(((const ExprInRange *)kp)->value);
            PS_PUSH(((const ExprInRange *)kp)->lo);
            PS_PUSH(((const ExprInRange *)kp)->hi);
            break;
        case EXPR_IN_SET: {
            const ExprInSet *iset = (const ExprInSet *)kp;
            PS_PUSH(iset->value);
            dvs_expr_t *el = expr_in_set_elems(S->problem, r);
            for (uint32_t i = 0; i < iset->n_elems; i++) PS_PUSH(el[i]);
            break;
        }
        case EXPR_IN_RANGES: {
            const ExprInRanges *irs = (const ExprInRanges *)kp;
            PS_PUSH(irs->value);
            dvs_expr_t *los = expr_in_ranges_los(S->problem, r);
            dvs_expr_t *his = expr_in_ranges_his(S->problem, r);
            for (uint32_t i = 0; i < irs->n_ranges; i++) { PS_PUSH(los[i]); PS_PUSH(his[i]); }
            break;
        }
        default:
            return UINT32_MAX;
        }
    }
#undef PS_PUSH
    return ci;
}

/* Classify assert `lit` (of source expression `root`) into *e: R-only for a
 * component, with its k mask, or not. R-only means `root` reads only that
 * component's R vars, so `lit` is a function of popcount(X); its AIG cone
 * (which the AND rewrites may route around R's bits) must then read only X's
 * bits, and it is evaluated at one X of each weight k. val, mark (zeroed,
 * left zeroed), wl and cone have N + 2 entries. */
static void _ps_classify(dvs_bbsolver_t *S, uint32_t N, dvs_aig_node_t lit,
                         dvs_expr_t root, const bb_ps_comp *comp, uint32_t nc,
                         uint64_t *wv, uint8_t *mark, dvs_aig_node_t *wl,
                         uint32_t *cone, struct bb_psk *e) {
    dvs_aig_t *m = S->aig;
    uint32_t top = 0, ncone = 0, n_and = 0;
    int ok = 1;
    e->lit = lit;
    e->rvar = UINT32_MAX;
    e->rsig = 0;
    e->kmask = 0;
    uint32_t ci = _ps_expr_comp(S, root, comp, nc);
    if (ci == UINT32_MAX) return;
    const bb_ps_comp *C = &comp[ci];
    wl[top++] = lit;
    while (top && ok) {
        dvs_aig_node_t l = wl[--top];
        if (l == DVS_AIG_TRUE || l == DVS_AIG_FALSE) continue;
        uint32_t id = (uint32_t)(l < 0 ? -l : l);
        if (id < 2 || id > N) { ok = 0; break; }
        if (mark[id]) continue;
        mark[id] = 1;
        cone[ncone++] = id;
        if (dvs_aig_is_input(m, (dvs_aig_node_t)id)) {
            int isx = 0;
            for (uint32_t p = 0; p < C->n && !isx; p++)
                if (C->xb[p] == (dvs_aig_node_t)id) isx = 1;
            if (!isx) { ok = 0; break; }
            mark[id] = 2;
            continue;
        }
        if (!dvs_aig_is_and(m, (dvs_aig_node_t)id)) { ok = 0; break; }
        dvs_aig_node_t a, b;
        dvs_aig_get_children(m, (dvs_aig_node_t)id, &a, &b);
        wl[top++] = a;
        wl[top++] = b;
    }
    /* The cone's ANDs in ascending id (topological) order: scan the id range
     * the cone spans; it is no wider than the cone's top id. */
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t i = 0; i < ncone; i++) {
        if (cone[i] < lo) lo = cone[i];
        if (cone[i] > hi) hi = cone[i];
    }
    for (uint32_t id = lo; ncone && id <= hi; id++) {
        if (mark[id] == 1 && ok) cone[n_and++] = id;
        mark[id] = 0;
    }
    if (!ok) return;
    uint32_t ones = 0, nvar = 0;
    for (uint32_t p = 0; p < C->n; p++) {
        if (C->xb[p] == DVS_AIG_TRUE) ones++;
        else if (C->xb[p] != DVS_AIG_FALSE) nvar++;
    }
    /* All weights at once: bit j of a node's word is its value at the X whose
     * lowest j variable bits are 1 (k = ones + j; nvar <= 63). */
    wv[1] = ~0ull;
    uint32_t set = 0;
    for (uint32_t p = 0; p < C->n; p++) {
        dvs_aig_node_t b = C->xb[p];
        if (b == DVS_AIG_TRUE || b == DVS_AIG_FALSE) continue;
        wv[b] = set < 63 ? ~0ull << (set + 1) : 0;   /* 1 where j > set */
        set++;
    }
    for (uint32_t i = 0; i < n_and; i++) {
        dvs_aig_node_t a, b;
        dvs_aig_get_children(m, (dvs_aig_node_t)cone[i], &a, &b);
        wv[cone[i]] = _lit_word(wv, a) & _lit_word(wv, b);
    }
    uint64_t js = nvar >= 63 ? ~0ull : (1ull << (nvar + 1)) - 1;
    e->kmask = (_lit_word(wv, lit) & js) << ones;
    e->rvar = C->rvar;
    e->rsig = _ps_sig(S, C);
}

static int _popsamp(dvs_bbsolver_t *S) {
    if (!S->presim || S->asserts_lost || !S->n_asserts || !S->aig || !S->subst)
        return DVS_BB_UNKNOWN;
    const char *env = getenv("DV_POPSAMP");
    if (env && strcmp(env, "0") == 0) return DVS_BB_UNKNOWN;
    if (S->ps_tries >= 8 && S->ps_hits * 4 < S->ps_tries) return DVS_BB_UNKNOWN;
    dvs_aig_t *m = S->aig;
    bb_ps_comp comp[DVS_PS_MAX_COMPS];
    uint32_t nc = 0;
    for (uint32_t r = 0; r < S->n_vars; r++) {
        if (S->subst[r] == EXPR_NULL) continue;
        const ExprCountones *pc = popcount_def(S, r);
        if (!pc) continue;
        const ExprKind *xk = (const ExprKind *)POOL_PTR(S->problem, pc->operand);
        if (!xk || *xk != EXPR_VAR) return DVS_BB_UNKNOWN;
        uint32_t x = ((const ExprVar *)xk)->var_id;
        if (x >= S->n_vars || !S->vars[x].bv_built) return DVS_BB_UNKNOWN;
        uint32_t same = UINT32_MAX;          /* another R over the same X */
        for (uint32_t o = 0; o < nc; o++)
            if (comp[o].xvar == x) same = o;
        if (same != UINT32_MAX) {
            if (comp[same].nr == DVS_PS_MAX_RS) return DVS_BB_UNKNOWN;
            comp[same].rvars[comp[same].nr++] = r;
            continue;
        }
        const dvs_bv_t *xb = &S->vars[x].bv;
        if (xb->size == 0 || xb->size > 63 || nc == DVS_PS_MAX_COMPS) return DVS_BB_UNKNOWN;
        bb_ps_comp *c = &comp[nc];
        c->rvar = r;
        c->rvars[0] = r;
        c->nr = 1;
        c->xvar = x;
        c->n = xb->size;
        for (uint32_t p = 0; p < c->n; p++) {
            dvs_aig_node_t b = xb->bits[c->n - 1 - p];
            if (b != DVS_AIG_TRUE && b != DVS_AIG_FALSE
                    && (b < 0 || !dvs_aig_is_input(m, b)))
                return DVS_BB_UNKNOWN;
            for (uint32_t o = 0; o < nc; o++)       /* X bits in two components */
                for (uint32_t q = 0; q < comp[o].n; q++)
                    if (b != DVS_AIG_TRUE && b != DVS_AIG_FALSE && comp[o].xb[q] == b)
                        return DVS_BB_UNKNOWN;
            c->xb[p] = b;
        }
        c->kmask = (c->n >= 63) ? ~0ull : ((1ull << (c->n + 1)) - 1);
        nc++;
    }
    if (!nc) return DVS_BB_UNKNOWN;
    uint32_t N = (uint32_t)dvs_aig_num_nodes(m);
    if (N < 2) return DVS_BB_UNKNOWN;
    S->ps_tries++;
    int8_t *fv = (int8_t *)malloc((size_t)N + 2);
    int8_t *val = (int8_t *)malloc((size_t)N + 2);
    uint8_t *mark = (uint8_t *)calloc((size_t)N + 2, 1);
    dvs_aig_node_t *wl = (dvs_aig_node_t *)malloc(((size_t)N + 2) * sizeof(dvs_aig_node_t));
    uint32_t *cone = (uint32_t *)malloc(((size_t)N + 2) * sizeof(uint32_t));
    uint64_t *wv = (uint64_t *)calloc((size_t)N + 2, sizeof(uint64_t));
    int rc = DVS_BB_UNKNOWN;
    uint32_t forced = 0, draws = 0;
    if (!fv || !val || !mark || !wl || !cone || !wv) goto out;
    if (_forced_inputs(S, N, fv, mark, wl, &forced) != 1) goto out;
    memset(mark, 0, (size_t)N + 2);

    /* K: the R-only asserts' masks (cached per assert literal). */
    for (uint32_t a = 0; a < S->n_asserts; a++) {
        dvs_aig_node_t lit = S->assert_nodes[a];
        struct bb_psk *e = NULL;
        for (uint32_t i = 0; i < S->n_psk; i++)
            if (S->psk[i].lit == lit) { e = &S->psk[i]; break; }
        int fresh = !e;
        if (e && e->rvar != UINT32_MAX) {
            fresh = 1;   /* valid only while R is the same component */
            for (uint32_t c = 0; c < nc; c++)
                if (comp[c].rvar == e->rvar && _ps_sig(S, &comp[c]) == e->rsig)
                    fresh = 0;
        }
        if (fresh) {
            if (!e) {
                if (S->n_psk == S->cap_psk) {
                    uint32_t cap = S->cap_psk ? 2 * S->cap_psk : 16;
                    struct bb_psk *t = (struct bb_psk *)realloc(S->psk, cap * sizeof(*t));
                    if (!t) goto out;
                    S->psk = t;
                    S->cap_psk = cap;
                }
                e = &S->psk[S->n_psk++];
            }
            _ps_classify(S, N, lit, S->assert_roots ? S->assert_roots[a] : EXPR_NULL,
                         comp, nc, wv, mark, wl, cone, e);
        }
        if (e->rvar == UINT32_MAX) continue;
        for (uint32_t c = 0; c < nc; c++)
            if (comp[c].rvar == e->rvar) comp[c].kmask &= e->kmask;
    }

    /* No R-only assert narrowed any K: the draws are plain uniform guesses,
     * which rarely pass, and a fresh instance per randomize never learns
     * that from ps_hits. */
    {
        int narrowed = 0;
        for (uint32_t c = 0; c < nc; c++) {
            uint64_t full = (comp[c].n >= 63) ? ~0ull : ((1ull << (comp[c].n + 1)) - 1);
            if (comp[c].kmask != full) narrowed = 1;
        }
        if (!narrowed) goto out;
    }

    /* Per component: fixed ones, free positions, the k weights. */
    {
        uint64_t binom[64][64];   /* C(f, j), f <= 63: C(63, 31) < 2^63 */
        for (uint32_t f = 0; f < 64; f++) {
            binom[f][0] = 1;
            for (uint32_t j = 1; j < 64; j++)
                binom[f][j] = j > f ? 0 : (j == f ? 1 : binom[f - 1][j - 1] + binom[f - 1][j]);
        }
        for (uint32_t c = 0; c < nc; c++) {
            bb_ps_comp *C = &comp[c];
            C->nfree = C->ones = 0;
            for (uint32_t p = 0; p < C->n; p++) {
                dvs_aig_node_t b = C->xb[p];
                if (b == DVS_AIG_TRUE) C->ones++;
                else if (b == DVS_AIG_FALSE) continue;
                else if (fv[b] == 1) C->ones++;
                else if (fv[b] == 2) C->free_pos[C->nfree++] = p;
            }
            C->total = 0;
            for (uint32_t k = 0; k <= C->n; k++) {
                C->w[k] = 0;
                if (!((C->kmask >> k) & 1) || k < C->ones || k - C->ones > C->nfree) continue;
                C->w[k] = binom[C->nfree][k - C->ones];
                C->total += C->w[k];
            }
            if (C->total == 0) goto out;   /* K empty here: the normal path decides */
        }
    }

    uint64_t st = S->seed ^ 0x504F5053414D50ull;
    for (draws = 1; draws <= DVS_PS_DRAWS; draws++) {
        uint64_t bits = 0;
        uint32_t nb = 0;
        val[0] = 0;
        val[1] = 1;
        for (uint32_t id = 2; id <= N; id++) {
            if (!dvs_aig_is_input(m, (dvs_aig_node_t)id)) continue;
            if (fv[id] != 2) { val[id] = fv[id]; continue; }
            if (!nb) { bits = _ps_next(&st); nb = 64; }
            val[id] = (int8_t)(bits & 1);
            bits >>= 1;
            nb--;
        }
        for (uint32_t c = 0; c < nc; c++) {
            bb_ps_comp *C = &comp[c];
            uint64_t r = _ps_below(&st, C->total);
            uint32_t k = 0;
            while (r >= C->w[k]) r -= C->w[k++];
            uint32_t j = k - C->ones;
            uint8_t chosen[64];
            memset(chosen, 0, sizeof chosen);
            for (uint32_t i = C->nfree - j; i < C->nfree; i++) {   /* Floyd */
                uint32_t t = (uint32_t)_ps_below(&st, (uint64_t)i + 1);
                if (chosen[t]) chosen[i] = 1; else chosen[t] = 1;
            }
            for (uint32_t q = 0; q < C->nfree; q++)
                val[C->xb[C->free_pos[q]]] = (int8_t)chosen[q];
        }
        for (uint32_t id = 2; id <= N; id++) {
            if (!dvs_aig_is_and(m, (dvs_aig_node_t)id)) continue;
            dvs_aig_node_t l, r;
            dvs_aig_get_children(m, (dvs_aig_node_t)id, &l, &r);
            val[id] = (int8_t)(_lit_val(val, l) & _lit_val(val, r));
        }
        S->aux_work += N;
        uint32_t a = 0;
        while (a < S->n_asserts && _lit_val(val, S->assert_nodes[a])) a++;
        if (a == S->n_asserts) {
            free(S->node_val);
            S->node_val = val;
            val = NULL;
            S->model_in_sat = 0;
            S->base_sim = 1;
            S->ps_hits++;
            rc = DVS_BB_SAT;
            break;
        }
    }
out:
    if (getenv("DV_BB_STATS"))
        fprintf(stderr, "[bb-popsamp] comps=%u forced=%u draws=%u -> %s\n", nc, forced,
                draws, rc == DVS_BB_SAT ? "sat" : "miss");
    free(fv); free(val); free(mark); free(wl); free(cone); free(wv);
    return rc;
}

/* Unique sampler (DV_UNIQSAMP=0 off): an exact draw for problems made only of
 * all-different groups and unary bounds. Every kept conjunct must be `x != y`
 * of two bare unsigned vars (AllDiffSpec groups give the same pairs) or a
 * unary `x cmp const`, `x inside {consts}`, `x inside [ranges]` (or its
 * negation). The != graph's components must be cliques whose members (<= 16
 * bits) share one domain D, the values their unary conjuncts allow; a partial
 * Fisher-Yates draw over D is then uniform over the injective tuples, and a
 * var with no != is uniform over its D. Other inputs get a seeded fill. The
 * draw is checked against every assert before it is installed as node_val.
 * Returns DVS_BB_SAT or DVS_BB_UNKNOWN (take the normal path). */
#define DVS_US_MAXW 16
#define DVS_US_WORK (1u << 20)

/* The var of a unary conjunct (const op var, var op const, var inside
 * consts, a bare var), else -1. */
static int64_t _us_unary_var(dvs_bbsolver_t *S, dvs_expr_t r) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, r);
    uint64_t a, b;
    int64_t v;
    if (!k) return -1;
    if (*k == EXPR_VAR) return bb_unsigned_var(S, r);
    if (*k == EXPR_BINARY) {
        const ExprBinary *e = (const ExprBinary *)k;
        switch (e->op) {
        case DVS_BIN_EQ: case DVS_BIN_NEQ: case DVS_BIN_LT:
        case DVS_BIN_LTE: case DVS_BIN_GT: case DVS_BIN_GTE: break;
        default: return -1;
        }
        v = bb_unsigned_var(S, e->lhs);
        if (v >= 0 && bb_const_u(S, e->rhs, &a)) return v;
        v = bb_unsigned_var(S, e->rhs);
        if (v >= 0 && bb_const_u(S, e->lhs, &a)) return v;
        return -1;
    }
    if (*k == EXPR_IN_RANGE) {
        const ExprInRange *e = (const ExprInRange *)k;
        v = bb_unsigned_var(S, e->value);
        return v >= 0 && bb_const_u(S, e->lo, &a) && bb_const_u(S, e->hi, &b) ? v : -1;
    }
    if (*k == EXPR_IN_SET) {
        const ExprInSet *e = (const ExprInSet *)k;
        v = bb_unsigned_var(S, e->value);
        const dvs_expr_t *el = expr_in_set_elems(S->problem, r);
        for (uint32_t i = 0; v >= 0 && i < e->n_elems; i++)
            if (!bb_const_u(S, el[i], &a)) v = -1;
        return v;
    }
    if (*k == EXPR_IN_RANGES) {
        const ExprInRanges *e = (const ExprInRanges *)k;
        v = bb_unsigned_var(S, e->value);
        const dvs_expr_t *ls = expr_in_ranges_los(S->problem, r);
        const dvs_expr_t *hs = expr_in_ranges_his(S->problem, r);
        for (uint32_t i = 0; v >= 0 && i < e->n_ranges; i++)
            if (!bb_const_u(S, ls[i], &a) || !bb_const_u(S, hs[i], &b)) v = -1;
        return v;
    }
    if (*k == EXPR_UNARY && ((const ExprUnary *)k)->op == DVS_UN_NOT)
        return _us_unary_var(S, ((const ExprUnary *)k)->operand);
    return -1;
}

/* Both ends of `x != y` / `!(x == y)` over two bare unsigned vars. */
static int _us_edge(dvs_bbsolver_t *S, dvs_expr_t r, int64_t *x, int64_t *y) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, r);
    dvs_binop_t want = DVS_BIN_NEQ;
    if (k && *k == EXPR_UNARY && ((const ExprUnary *)k)->op == DVS_UN_NOT) {
        k = (const ExprKind *)POOL_PTR(S->problem, ((const ExprUnary *)k)->operand);
        want = DVS_BIN_EQ;
    }
    if (!k || *k != EXPR_BINARY || ((const ExprBinary *)k)->op != want) return 0;
    *x = bb_unsigned_var(S, ((const ExprBinary *)k)->lhs);
    *y = bb_unsigned_var(S, ((const ExprBinary *)k)->rhs);
    return *x >= 0 && *y >= 0;
}

static int _us_grow(void **p, size_t *cap, size_t n, size_t esz) {
    if (n < *cap) return 1;
    size_t c = *cap ? 2 * *cap : 64;
    void *t = realloc(*p, c * esz);
    if (!t) return 0;
    *p = t;
    *cap = c;
    return 1;
}

static int _us_edge_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t _us_find(uint32_t *uf, uint32_t v) {
    while (uf[v] != v) { uf[v] = uf[uf[v]]; v = uf[v]; }
    return v;
}

/* The values in [lo[v], hi[v]] that v's fixed bits and unary conjuncts allow,
 * into dom; -1 if a conjunct cannot be evaluated. */
static int64_t _us_domain(dvs_bbsolver_t *S, uint32_t v, const uint64_t *lo,
                          const uint64_t *hi, const uint64_t *fmask,
                          const uint64_t *fval, const uint32_t *uhead,
                          const uint32_t *unext, const dvs_expr_t *un,
                          uint64_t *vval, uint32_t *dom) {
    uint32_t d = 0;
    for (uint64_t x = lo[v]; x <= hi[v]; x++) {
        if ((x & fmask[v]) == fval[v]) {
            vval[v] = x;
            int ok = 1;
            for (uint32_t u = uhead[v]; ok && u != UINT32_MAX; u = unext[u]) {
                uint64_t t;
                if (!bb_eval_pred(S, un[u], vval, &t)) return -1;
                ok = t != 0;
            }
            if (ok) dom[d++] = (uint32_t)x;
        }
        if (x == hi[v]) break;
    }
    vval[v] = 0;
    return d;
}

/* The leaf conjuncts of the kept constraints, in the order _uniqsamp has
 * always classified them: the walk peels &&, a both-bool-ite &, `1 == p` of a
 * 1-bit p, ite(c,1,0) and `t == ite(c,1,0)` with t substituted by 1. Returns 0
 * on an allocation failure or a missing node. */
static int _us_leaves(dvs_bbsolver_t *S, dvs_expr_t **out, size_t *n_out,
                      size_t *cap_out) {
    uint32_t nv = S->n_vars, me = 0;
    size_t cap_s = 0;
    dvs_expr_t *stk = NULL;
    int ok = 0;
    *n_out = 0;
    for (dvs_expr_t cur = S->problem->constraints_head; cur != EXPR_NULL; me++) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(S->problem, cur);
        cur = cs->next;
        if (S->constraint_skip && S->constraint_skip[me]) continue;
        size_t n = 0;
        if (!_us_grow((void **)&stk, &cap_s, n, sizeof(*stk))) goto out;
        stk[n++] = cs->root;
        while (n > 0) {
            dvs_expr_t r = stk[--n];
            const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, r);
            if (!k) goto out;
            if (!_us_grow((void **)&stk, &cap_s, n + 2, sizeof(*stk))) goto out;
            dvs_expr_t cond;
            uint64_t a;
            if (bb_is_bool_ite(S, r, &cond)) { stk[n++] = cond; continue; }
            if (*k == EXPR_BINARY) {
                const ExprBinary *e = (const ExprBinary *)k;
                if (e->op == DVS_BIN_AND ||
                    (e->op == DVS_BIN_BAND && bb_is_bool_ite(S, e->lhs, NULL) &&
                     bb_is_bool_ite(S, e->rhs, NULL))) {
                    stk[n++] = e->lhs;
                    stk[n++] = e->rhs;
                    continue;
                }
                if (e->op == DVS_BIN_EQ && bb_const_u(S, e->lhs, &a) && a == 1 &&
                    bb_unsigned_var(S, e->rhs) < 0 && bb_self_width(S, e->rhs, 0) == 1) {
                    stk[n++] = e->rhs;
                    continue;
                }
                if (e->op == DVS_BIN_EQ && S->subst) {
                    const ExprKind *lk = (const ExprKind *)POOL_PTR(S->problem, e->lhs);
                    if (lk && *lk == EXPR_VAR &&
                        ((const ExprVar *)lk)->var_id < nv &&
                        S->subst[((const ExprVar *)lk)->var_id] != EXPR_NULL &&
                        bb_const_u(S, S->subst[((const ExprVar *)lk)->var_id], &a) &&
                        a == 1 && bb_is_bool_ite(S, e->rhs, &cond)) {
                        stk[n++] = cond;
                        continue;
                    }
                }
            }
            if (!_us_grow((void **)out, cap_out, *n_out, sizeof(**out))) goto out;
            (*out)[(*n_out)++] = r;
        }
    }
    ok = 1;
out:
    free(stk);
    return ok;
}

static int _uniqsamp(dvs_bbsolver_t *S) {
    if (!S->presim || S->asserts_lost || !S->n_asserts || !S->aig) return DVS_BB_UNKNOWN;
    const char *env = getenv("DV_UNIQSAMP");
    if (env && strcmp(env, "0") == 0) return DVS_BB_UNKNOWN;
    if (S->us_tries >= 8 && S->us_hits * 4 < S->us_tries) return DVS_BB_UNKNOWN;
    if (S->problem->softs_head != EXPR_NULL || S->problem->dists_head != EXPR_NULL)
        return DVS_BB_UNKNOWN;
    dvs_aig_t *m = S->aig;
    uint32_t N = (uint32_t)dvs_aig_num_nodes(m), nv = S->n_vars;
    if (N < 2 || nv == 0) return DVS_BB_UNKNOWN;
    int rc = DVS_BB_UNKNOWN;
    const char *why = "shape";
    size_t n_e = 0, cap_e = 0, n_u = 0, cap_u = 0, cap_uv = 0, n_lf = 0, cap_lf = 0;
    uint32_t ncomp = 0, nused = 0;
    uint64_t *edge = NULL;
    dvs_expr_t *un = NULL, *lf = NULL;
    uint32_t *unv = NULL, *unext = NULL, *dom = NULL, *dom2 = NULL;
    uint32_t *uhead = (uint32_t *)malloc(nv * sizeof(uint32_t));
    uint32_t *uf = (uint32_t *)malloc(nv * sizeof(uint32_t));
    uint32_t *cnt = (uint32_t *)calloc((size_t)nv + 1, sizeof(uint32_t));
    uint32_t *memb = (uint32_t *)malloc(nv * sizeof(uint32_t));
    uint64_t *lo = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *hi = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *fmask = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *fval = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *vval = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *ecnt = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint8_t *used = (uint8_t *)calloc(nv, 1);
    uint8_t *nmark = (uint8_t *)calloc((size_t)N + 2, 1);
    int8_t *val = (int8_t *)malloc((size_t)N + 2);
    if (!uhead || !uf || !cnt || !memb || !lo || !hi || !fmask || !fval || !vval
        || !ecnt || !used || !nmark || !val) goto out;
    for (uint32_t v = 0; v < nv; v++) {
        uhead[v] = UINT32_MAX;
        uf[v] = v;
        lo[v] = 0;
        hi[v] = bb_eval_mask(S->vars[v].width);
    }

    /* Classify the top-level conjuncts (bb_unary_bounds' peel). */
    if (!_us_leaves(S, &lf, &n_lf, &cap_lf)) goto out;
    for (size_t li = 0; li < n_lf; li++) {
        dvs_expr_t r = lf[li];
        int64_t x, y;
        if (_us_edge(S, r, &x, &y)) {
            if (x == y) goto out;
            if (!_us_grow((void **)&edge, &cap_e, n_e, sizeof(*edge))) goto out;
            edge[n_e++] = x < y ? ((uint64_t)x << 32) | (uint64_t)y
                                : ((uint64_t)y << 32) | (uint64_t)x;
            used[x] = used[y] = 1;
            continue;
        }
        x = _us_unary_var(S, r);
        if (x < 0) goto out;
        if (!_us_grow((void **)&un, &cap_u, n_u, sizeof(*un)) ||
            !_us_grow((void **)&unv, &cap_uv, n_u, sizeof(*unv))) goto out;
        un[n_u] = r;
        unv[n_u++] = (uint32_t)x;
        used[x] = 1;
    }
    for (dvs_expr_t aref = S->problem->allDiff_head; aref != EXPR_NULL; ) {
        AllDiffSpec *as = (AllDiffSpec *)POOL_PTR(S->problem, aref);
        const uint32_t *ids = (const uint32_t *)(as + 1);
        for (uint32_t i = 0; i < as->n_vars; i++) {
            if (ids[i] >= nv || S->vars[ids[i]].is_signed) goto out;
            for (uint32_t j = i + 1; j < as->n_vars; j++) {
                uint32_t x = ids[i], y = ids[j];
                if (x == y || y >= nv) goto out;
                if (!_us_grow((void **)&edge, &cap_e, n_e, sizeof(*edge))) goto out;
                edge[n_e++] = x < y ? ((uint64_t)x << 32) | y : ((uint64_t)y << 32) | x;
            }
            used[ids[i]] = 1;
        }
        aref = as->next;
    }
    unext = (uint32_t *)malloc((n_u + 1) * sizeof(uint32_t));
    if (!unext) goto out;
    for (size_t u = n_u; u-- > 0; ) { unext[u] = uhead[unv[u]]; uhead[unv[u]] = (uint32_t)u; }

    /* Each used var: narrow, unsigned, own AIG input bits (constants kept as
     * fixed bits). */
    why = "var";
    for (uint32_t v = 0; v < nv; v++) {
        if (!used[v]) continue;
        nused++;
        const bb_var_t *bv = &S->vars[v];
        uint32_t w = bv->width;
        if (bv->is_signed || w == 0 || w > DVS_US_MAXW || !bv->bv_built || bv->bv.size != w
            || (S->subst && S->subst[v] != EXPR_NULL)) goto out;
        for (uint32_t i = 0; i < w; i++) {
            dvs_aig_node_t b = bv->bv.bits[w - 1 - i];
            if (b == DVS_AIG_TRUE || b == DVS_AIG_FALSE) {
                fmask[v] |= 1ull << i;
                if (b == DVS_AIG_TRUE) fval[v] |= 1ull << i;
                continue;
            }
            if (b < 0 || (uint32_t)b > N || !dvs_aig_is_input(m, b) || nmark[b]) goto out;
            nmark[b] = 1;
        }
    }
    if (!nused) goto out;
    S->us_tries++;

    /* Components of the != graph must be cliques. */
    why = "clique";
    qsort(edge, n_e, sizeof(*edge), _us_edge_cmp);
    {
        size_t k = 0;
        for (size_t i = 0; i < n_e; i++)
            if (!k || edge[i] != edge[k - 1]) edge[k++] = edge[i];
        n_e = k;
    }
    for (size_t i = 0; i < n_e; i++) {
        uint32_t a = _us_find(uf, (uint32_t)(edge[i] >> 32));
        uint32_t b = _us_find(uf, (uint32_t)edge[i]);
        if (a != b) uf[a] = b;
    }
    for (size_t i = 0; i < n_e; i++) ecnt[_us_find(uf, (uint32_t)edge[i])]++;
    for (uint32_t v = 0; v < nv; v++) if (used[v]) cnt[_us_find(uf, v) + 1]++;
    for (uint32_t v = 0; v < nv; v++) {
        uint64_t k = cnt[v + 1];
        if (k && ecnt[v] != k * (k - 1) / 2) goto out;
        if (k) ncomp++;
    }
    for (uint32_t v = 0; v < nv; v++) cnt[v + 1] += cnt[v];
    {
        uint32_t *pos = (uint32_t *)malloc(nv * sizeof(uint32_t));
        if (!pos) goto out;
        memcpy(pos, cnt, nv * sizeof(uint32_t));
        for (uint32_t v = 0; v < nv; v++) if (used[v]) memb[pos[_us_find(uf, v)]++] = v;
        free(pos);
    }

    /* Domains, bounded work. */
    why = "domain";
    bb_unary_bounds(S, lo, hi);
    {
        uint64_t work = 0, maxd = 1;
        for (uint32_t v = 0; v < nv; v++) {
            if (!used[v]) continue;
            if (lo[v] > hi[v]) goto out;          /* empty: the normal path decides */
            uint64_t span = hi[v] - lo[v] + 1, nuv = 1;
            for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u]) nuv++;
            work += span * nuv;
            if (span > maxd) maxd = span;
        }
        if (work > DVS_US_WORK) goto out;
        dom = (uint32_t *)malloc(maxd * sizeof(uint32_t));
        dom2 = (uint32_t *)malloc(maxd * sizeof(uint32_t));
        if (!dom || !dom2) goto out;
    }

    /* Draw: seeded fill, then each component. */
    val[0] = 0;
    val[1] = 1;
    {
        uint64_t st = S->seed ^ 0x554E495153414D50ull, bits = 0;
        uint32_t nb = 0;
        for (uint32_t id = 2; id <= N; id++) {
            if (!dvs_aig_is_input(m, (dvs_aig_node_t)id)) continue;
            if (!nb) { bits = _ps_next(&st); nb = 64; }
            val[id] = (int8_t)(bits & 1);
            bits >>= 1;
            nb--;
        }
        for (uint32_t r = 0; r < nv; r++) {
            uint32_t b0 = cnt[r], k = cnt[r + 1] - cnt[r];
            if (!k) continue;
            int64_t d = _us_domain(S, memb[b0], lo, hi, fmask, fval, uhead, unext, un,
                                   vval, dom);
            if (d < 0) goto out;
            if ((uint64_t)d < k) { why = "small"; goto out; }
            for (uint32_t i = 1; i < k; i++) {
                int64_t d2 = _us_domain(S, memb[b0 + i], lo, hi, fmask, fval, uhead,
                                        unext, un, vval, dom2);
                if (d2 != d || memcmp(dom, dom2, (size_t)d * sizeof(uint32_t))) goto out;
            }
            for (uint32_t i = 0; i < k; i++) {     /* partial Fisher-Yates */
                uint64_t j = i + _ps_below(&st, (uint64_t)d - i);
                uint32_t t = dom[i]; dom[i] = dom[j]; dom[j] = t;
                const bb_var_t *bv = &S->vars[memb[b0 + i]];
                for (uint32_t q = 0; q < bv->width; q++) {
                    dvs_aig_node_t b = bv->bv.bits[bv->width - 1 - q];
                    if (b > 1) val[b] = (int8_t)((dom[i] >> q) & 1);
                }
            }
        }
    }
    why = "assert";
    for (uint32_t id = 2; id <= N; id++) {
        if (!dvs_aig_is_and(m, (dvs_aig_node_t)id)) continue;
        dvs_aig_node_t l, r;
        dvs_aig_get_children(m, (dvs_aig_node_t)id, &l, &r);
        val[id] = (int8_t)(_lit_val(val, l) & _lit_val(val, r));
    }
    S->aux_work += N;
    {
        uint32_t a = 0;
        while (a < S->n_asserts && _lit_val(val, S->assert_nodes[a])) a++;
        if (a == S->n_asserts) {
            free(S->node_val);
            S->node_val = val;
            val = NULL;
            S->model_in_sat = 0;
            S->base_sim = 1;
            S->us_hits++;
            rc = DVS_BB_SAT;
        }
    }
out:
    if (getenv("DV_BB_STATS"))
        fprintf(stderr, "[bb-uniqsamp] vars=%u edges=%zu unary=%zu comps=%u -> %s\n",
                nused, n_e, n_u, ncomp, rc == DVS_BB_SAT ? "sat" : why);
    free(edge); free(un); free(lf); free(unv); free(unext); free(dom); free(dom2);
    free(uhead); free(uf); free(cnt); free(memb); free(lo); free(hi); free(fmask);
    free(fval); free(vval); free(ecnt); free(used); free(nmark); free(val);
    return rc;
}

/* Sorted sampler (DV_SORTSAMP=0 off): an exact draw for problems made of
 * ordered chains and one-var conjuncts. A two-var conjunct must be an edge
 * `x >= y + c` (or >, <=, <; c a const >= 0; `y + c` may be an aux var
 * substituted by it) over bare unsigned vars of one width. The edges must form
 * simple paths whose members share one interval [L,H] from pure interval
 * conjuncts. With D_0 = 0, D_i = D_{i-1} + c_i - 1 and M = H - L - D_{n-1} + 1,
 * x_i = L + s_i + D_i maps the n-subsets s of [0,M) one-to-one onto the
 * chain's solutions, so a Floyd subset, sorted, is uniform. A var in no edge
 * is uniform over the values its conjuncts allow, enumerated over a hull of at
 * most 2^16 values (in `x ^ bias` space for the signed-compare form). A
 * conjunct with no var must be true. Other inputs get a seeded fill; the draw
 * is checked against every assert before it is installed as node_val.
 * Returns DVS_BB_SAT or DVS_BB_UNKNOWN (take the normal path). */
#define DVS_SS_MAXN 512
#define DVS_SS_HULL (1u << 16)
#define DVS_SS_WORK (1u << 20)

static int64_t _ss_join(int64_t a, int64_t b) {
    if (a == -1 || b == -1) return -1;
    if (a == -2) return b;
    if (b == -2) return a;
    return a == b ? a : -1;
}

/* The one var `e` reads through substitutions; -2 for none, -1 for two or
 * more or a construct outside the walk. */
static int64_t _ss_base(dvs_bbsolver_t *S, dvs_expr_t e, int depth) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, e);
    if (!k || depth > 32) return -1;
    switch (*k) {
    case EXPR_CONST: return -2;
    case EXPR_VAR: {
        uint32_t id = ((const ExprVar *)k)->var_id;
        if (id >= S->n_vars) return -1;
        if (S->subst && S->subst[id] != EXPR_NULL)
            return _ss_base(S, S->subst[id], depth + 1);
        return id;
    }
    case EXPR_BINARY: {
        const ExprBinary *b = (const ExprBinary *)k;
        return _ss_join(_ss_base(S, b->lhs, depth + 1), _ss_base(S, b->rhs, depth + 1));
    }
    case EXPR_UNARY: return _ss_base(S, ((const ExprUnary *)k)->operand, depth + 1);
    case EXPR_EXTEND: return _ss_base(S, ((const ExprExtend *)k)->operand, depth + 1);
    case EXPR_ITE: {
        const ExprITE *i = (const ExprITE *)k;
        return _ss_join(_ss_base(S, i->cond, depth + 1),
                        _ss_join(_ss_base(S, i->then_e, depth + 1),
                                 _ss_base(S, i->else_e, depth + 1)));
    }
    case EXPR_IN_RANGE: {
        const ExprInRange *r = (const ExprInRange *)k;
        return _ss_join(_ss_base(S, r->value, depth + 1),
                        _ss_join(_ss_base(S, r->lo, depth + 1),
                                 _ss_base(S, r->hi, depth + 1)));
    }
    default: return -1;
    }
}

/* `e` through var substitutions down to its first non-var node or bare var. */
static dvs_expr_t _ss_strip(dvs_bbsolver_t *S, dvs_expr_t e) {
    for (int hops = 0; hops < 32; hops++) {
        const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, e);
        if (!k || *k != EXPR_VAR) break;
        uint32_t id = ((const ExprVar *)k)->var_id;
        if (id >= S->n_vars || !S->subst || S->subst[id] == EXPR_NULL) break;
        e = S->subst[id];
    }
    return e;
}

/* `e` through substitutions as v + c: v an unsubstituted unsigned var of
 * width w, every add w wide, c a sum of consts below 2^32. */
static int _ss_aff(dvs_bbsolver_t *S, dvs_expr_t e, uint32_t w, int64_t *v,
                   uint64_t *c, int depth) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, e);
    uint64_t a;
    if (!k || depth > 32) return 0;
    if (*k == EXPR_VAR) {
        uint32_t id = ((const ExprVar *)k)->var_id;
        if (id >= S->n_vars || S->vars[id].width != w) return 0;
        if (S->subst && S->subst[id] != EXPR_NULL)
            return _ss_aff(S, S->subst[id], w, v, c, depth + 1);
        if (S->vars[id].is_signed) return 0;
        *v = id;
        return 1;
    }
    if (*k != EXPR_BINARY || ((const ExprBinary *)k)->op != DVS_BIN_ADD ||
        bb_self_width(S, e, 0) != w) return 0;
    const ExprBinary *b = (const ExprBinary *)k;
    dvs_expr_t t;
    if (bb_const_u(S, b->rhs, &a)) t = b->lhs;
    else if (bb_const_u(S, b->lhs, &a)) t = b->rhs;
    else return 0;
    if (a >= (1ull << 32) || !_ss_aff(S, t, w, v, c, depth + 1)) return 0;
    *c += a;
    return 1;
}

/* An edge x >= y + c (c counts the strict +1); *g is the add's const, which
 * must not wrap. */
static int _ss_edge(dvs_bbsolver_t *S, dvs_expr_t r, int64_t *x, int64_t *y,
                    uint64_t *c, uint64_t *g, uint32_t *w) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, r);
    if (!k || *k != EXPR_BINARY) return 0;
    const ExprBinary *e = (const ExprBinary *)k;
    dvs_expr_t big, small;
    switch (e->op) {
    case DVS_BIN_GT: case DVS_BIN_GTE: big = e->lhs; small = e->rhs; break;
    case DVS_BIN_LT: case DVS_BIN_LTE: big = e->rhs; small = e->lhs; break;
    default: return 0;
    }
    uint32_t bw = bb_self_width(S, big, 0);
    uint64_t cb = 0, cs = 0;
    if (bw == 0 || bw > 62 || bb_self_width(S, small, 0) != bw) return 0;
    if (!_ss_aff(S, big, bw, x, &cb, 0) || cb != 0) return 0;
    if (!_ss_aff(S, small, bw, y, &cs, 0)) return 0;
    *g = cs;
    *c = cs + (e->op == DVS_BIN_GT || e->op == DVS_BIN_LT);
    *w = bw;
    return 1;
}

/* `x cmp const` or `x inside [const:const]` of the bare var v. */
static int _ss_pure(dvs_bbsolver_t *S, dvs_expr_t r, uint32_t v) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, r);
    uint64_t a, b;
    if (!k) return 0;
    if (*k == EXPR_BINARY) {
        const ExprBinary *e = (const ExprBinary *)k;
        switch (e->op) {
        case DVS_BIN_EQ: case DVS_BIN_LT: case DVS_BIN_LTE:
        case DVS_BIN_GT: case DVS_BIN_GTE: break;
        default: return 0;
        }
        return (bb_unsigned_var(S, e->lhs) == v && bb_const_u(S, e->rhs, &a)) ||
               (bb_unsigned_var(S, e->rhs) == v && bb_const_u(S, e->lhs, &a));
    }
    if (*k == EXPR_IN_RANGE) {
        const ExprInRange *e = (const ExprInRange *)k;
        return bb_unsigned_var(S, e->value) == v && bb_const_u(S, e->lo, &a) &&
               bb_const_u(S, e->hi, &b);
    }
    return 0;
}

/* Narrow the key hull [*kl,*kh] of v (key = v ^ *bias) by `t cmp const` where
 * t is v or v ^ bias through substitutions; the first such conjunct fixes the
 * bias. Other conjuncts leave the hull as is (the enumeration checks them). */
static void _ss_hull(dvs_bbsolver_t *S, dvs_expr_t r, uint32_t v, uint64_t *bias,
                     int *have_bias, uint64_t *kl, uint64_t *kh) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, r);
    if (!k || *k != EXPR_BINARY) return;
    const ExprBinary *e = (const ExprBinary *)k;
    dvs_binop_t op = e->op;
    dvs_expr_t t;
    uint64_t c, b = 0;
    if (bb_const_u(S, e->rhs, &c)) t = e->lhs;
    else if (bb_const_u(S, e->lhs, &c)) {
        t = e->rhs;
        if (op == DVS_BIN_LT) op = DVS_BIN_GT;
        else if (op == DVS_BIN_LTE) op = DVS_BIN_GTE;
        else if (op == DVS_BIN_GT) op = DVS_BIN_LT;
        else if (op == DVS_BIN_GTE) op = DVS_BIN_LTE;
    } else return;
    uint32_t w = S->vars[v].width;
    if (bb_self_width(S, t, 0) != w || c > bb_eval_mask(w)) return;
    t = _ss_strip(S, t);
    const ExprKind *tk = (const ExprKind *)POOL_PTR(S->problem, t);
    if (!tk) return;
    if (*tk == EXPR_BINARY && ((const ExprBinary *)tk)->op == DVS_BIN_BXOR) {
        const ExprBinary *x = (const ExprBinary *)tk;
        dvs_expr_t o;
        if (bb_const_u(S, x->rhs, &b)) o = x->lhs;
        else if (bb_const_u(S, x->lhs, &b)) o = x->rhs;
        else return;
        if (bb_self_width(S, t, 0) != w) return;
        t = _ss_strip(S, o);
        tk = (const ExprKind *)POOL_PTR(S->problem, t);
        if (!tk) return;
    }
    if (*tk != EXPR_VAR || ((const ExprVar *)tk)->var_id != v) return;
    if (!*have_bias) { *bias = b; *have_bias = 1; }
    if (b != *bias) return;
    switch (op) {
    case DVS_BIN_EQ:  if (c > *kl) *kl = c; if (c < *kh) *kh = c; break;
    case DVS_BIN_LT:  if (c == 0) { *kl = 1; *kh = 0; } else if (c - 1 < *kh) *kh = c - 1; break;
    case DVS_BIN_LTE: if (c < *kh) *kh = c; break;
    case DVS_BIN_GT:  if (c == bb_eval_mask(w)) { *kl = 1; *kh = 0; }
                      else if (c + 1 > *kl) *kl = c + 1; break;
    case DVS_BIN_GTE: if (c > *kl) *kl = c; break;
    default: break;
    }
}

static int _ss_u64_cmp(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static void _ss_write(dvs_bbsolver_t *S, int8_t *val, uint32_t v, uint64_t x) {
    const bb_var_t *bv = &S->vars[v];
    for (uint32_t q = 0; q < bv->width; q++) {
        dvs_aig_node_t b = bv->bv.bits[bv->width - 1 - q];
        if (b > 1) val[b] = (int8_t)((x >> q) & 1);
    }
}

static int _sortsamp(dvs_bbsolver_t *S) {
    if (!S->presim || S->asserts_lost || !S->n_asserts || !S->aig) return DVS_BB_UNKNOWN;
    const char *env = getenv("DV_SORTSAMP");
    if (env && strcmp(env, "0") == 0) return DVS_BB_UNKNOWN;
    if (S->ss_tries >= 8 && S->ss_hits * 4 < S->ss_tries) return DVS_BB_UNKNOWN;
    if (S->problem->softs_head != EXPR_NULL || S->problem->dists_head != EXPR_NULL ||
        S->problem->allDiff_head != EXPR_NULL)
        return DVS_BB_UNKNOWN;
    dvs_aig_t *m = S->aig;
    uint32_t N = (uint32_t)dvs_aig_num_nodes(m), nv = S->n_vars;
    if (N < 2 || nv == 0) return DVS_BB_UNKNOWN;
    int rc = DVS_BB_UNKNOWN;
    const char *why = "shape";
    size_t n_lf = 0, cap_lf = 0, n_u = 0, cap_u = 0, cap_uv = 0, n_e = 0;
    uint32_t nused = 0, nchain = 0;
    dvs_expr_t *lf = NULL, *un = NULL;
    uint32_t *unv = NULL, *unext = NULL, *dom = NULL, *memb = NULL;
    uint64_t *sub = NULL;
    int64_t *pred = (int64_t *)malloc(nv * sizeof(int64_t));
    int64_t *succ = (int64_t *)malloc(nv * sizeof(int64_t));
    uint64_t *gap = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *grd = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint32_t *uhead = (uint32_t *)malloc(nv * sizeof(uint32_t));
    uint64_t *lo = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *hi = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *fmask = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *fval = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *vval = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint8_t *used = (uint8_t *)calloc(nv, 1);
    uint8_t *nmark = (uint8_t *)calloc((size_t)N + 2, 1);
    int8_t *val = (int8_t *)malloc((size_t)N + 2);
    if (!pred || !succ || !gap || !grd || !uhead || !lo || !hi || !fmask || !fval ||
        !vval || !used || !nmark || !val) goto out;
    for (uint32_t v = 0; v < nv; v++) {
        pred[v] = succ[v] = -1;
        uhead[v] = UINT32_MAX;
        lo[v] = 0;
        hi[v] = bb_eval_mask(S->vars[v].width);
    }

    /* Classify the leaves: no var (must hold), one var, or an edge. */
    if (!_us_leaves(S, &lf, &n_lf, &cap_lf)) goto out;
    for (size_t li = 0; li < n_lf; li++) {
        dvs_expr_t r = lf[li];
        int64_t b = _ss_base(S, r, 0), x, y;
        uint64_t c, g, t;
        uint32_t w;
        if (b == -2) {
            if (!bb_eval_pred(S, r, vval, &t) || !t) { why = "const"; goto out; }
            continue;
        }
        if (b >= 0) {
            if (!_us_grow((void **)&un, &cap_u, n_u, sizeof(*un)) ||
                !_us_grow((void **)&unv, &cap_uv, n_u, sizeof(*unv))) goto out;
            un[n_u] = r;
            unv[n_u++] = (uint32_t)b;
            if (!used[b]) used[b] = 1;
            continue;
        }
        if (!_ss_edge(S, r, &x, &y, &c, &g, &w)) goto out;
        why = "path";
        if (x == y || pred[x] >= 0 || succ[y] >= 0) goto out;
        why = "shape";
        pred[x] = y;
        succ[y] = x;
        gap[x] = c;
        grd[x] = g;
        used[x] = used[y] = 2;
        n_e++;
    }
    why = "edges";
    if (!n_e) goto out;
    unext = (uint32_t *)malloc((n_u + 1) * sizeof(uint32_t));
    if (!unext) goto out;
    for (size_t u = n_u; u-- > 0; ) { unext[u] = uhead[unv[u]]; uhead[unv[u]] = (uint32_t)u; }

    /* Each used var: unsigned, own AIG input bits; a chain var none fixed. */
    why = "var";
    for (uint32_t v = 0; v < nv; v++) {
        if (!used[v]) continue;
        nused++;
        const bb_var_t *bv = &S->vars[v];
        uint32_t w = bv->width;
        if (bv->is_signed || w == 0 || w > 64 || !bv->bv_built || bv->bv.size != w
            || (S->subst && S->subst[v] != EXPR_NULL)) goto out;
        for (uint32_t i = 0; i < w; i++) {
            dvs_aig_node_t b = bv->bv.bits[w - 1 - i];
            if (b == DVS_AIG_TRUE || b == DVS_AIG_FALSE) {
                if (used[v] == 2) goto out;
                fmask[v] |= 1ull << i;
                if (b == DVS_AIG_TRUE) fval[v] |= 1ull << i;
                continue;
            }
            if (b < 0 || (uint32_t)b > N || !dvs_aig_is_input(m, b) || nmark[b]) goto out;
            nmark[b] = 1;
        }
        if (used[v] == 2)
            for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u])
                if (!_ss_pure(S, un[u], v)) { why = "domain"; goto out; }
    }
    S->ss_tries++;
    bb_unary_bounds(S, lo, hi);

    /* Draw: seeded fill, each chain, then each single var. */
    val[0] = 0;
    val[1] = 1;
    memb = (uint32_t *)malloc(DVS_SS_MAXN * sizeof(uint32_t));
    sub = (uint64_t *)malloc(DVS_SS_MAXN * sizeof(uint64_t));
    if (!memb || !sub) goto out;
    {
        uint64_t st = S->seed ^ 0x534F525453414D50ull, bits = 0, work = 0;
        uint32_t nb = 0, seen = 0;
        for (uint32_t id = 2; id <= N; id++) {
            if (!dvs_aig_is_input(m, (dvs_aig_node_t)id)) continue;
            if (!nb) { bits = _ps_next(&st); nb = 64; }
            val[id] = (int8_t)(bits & 1);
            bits >>= 1;
            nb--;
        }
        for (uint32_t h = 0; h < nv; h++) {
            if (used[h] != 2 || pred[h] >= 0) continue;
            uint32_t n = 0;
            why = "path";
            for (int64_t v = h; v >= 0; v = succ[v]) {
                if (n == DVS_SS_MAXN) goto out;
                memb[n++] = (uint32_t)v;
            }
            seen += n;
            nchain++;
            why = "domain";
            uint64_t L = lo[h], H = hi[h], mask = bb_eval_mask(S->vars[h].width);
            if (L > H) goto out;               /* empty: the normal path decides */
            int64_t D = 0;
            for (uint32_t i = 0; i < n; i++) {
                uint32_t v = memb[i];
                if (lo[v] != L || hi[v] != H || S->vars[v].width != S->vars[h].width)
                    goto out;
                if (i) {
                    if (grd[v] > mask - H) { why = "wrap"; goto out; }
                    D += (int64_t)gap[v] - 1;
                }
            }
            int64_t M = (int64_t)(H - L) - D + 1;
            if (M < (int64_t)n) { why = "small"; goto out; }
            work += (uint64_t)n * n;
            if (work > DVS_SS_WORK) goto out;
            uint32_t k = 0;                    /* Floyd n-subset of [0,M) */
            for (uint64_t j = (uint64_t)M - n; j < (uint64_t)M; j++) {
                uint64_t t = _ps_below(&st, j + 1);
                uint32_t q = 0;
                while (q < k && sub[q] != t) q++;
                sub[k] = q < k ? j : t;
                k++;
            }
            qsort(sub, n, sizeof(*sub), _ss_u64_cmp);
            D = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (i) D += (int64_t)gap[memb[i]] - 1;
                _ss_write(S, val, memb[i], (uint64_t)((int64_t)(L + sub[i]) + D));
            }
        }
        why = "path";                              /* a cycle has no head */
        for (uint32_t v = 0; v < nv; v++) if (used[v] == 2) seen--;
        if (seen) goto out;
        why = "domain";
        for (uint32_t v = 0; v < nv; v++) {
            if (used[v] != 1) continue;
            uint64_t kl = 0, kh = bb_eval_mask(S->vars[v].width), bias = 0, t;
            int have = 0;
            for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u])
                _ss_hull(S, un[u], v, &bias, &have, &kl, &kh);
            if (kl > kh) goto out;
            if (kh - kl >= DVS_SS_HULL) goto out;
            uint32_t nuv = 1;
            for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u]) nuv++;
            work += (kh - kl + 1) * nuv;
            if (work > DVS_SS_WORK) goto out;
            if (!dom) dom = (uint32_t *)malloc(DVS_SS_HULL * sizeof(uint32_t));
            if (!dom) goto out;
            uint32_t d = 0;
            for (uint64_t key = kl; ; key++) {
                uint64_t x = key ^ bias;
                if ((x & fmask[v]) == fval[v]) {
                    int ok = 1;
                    vval[v] = x;
                    for (uint32_t u = uhead[v]; ok && u != UINT32_MAX; u = unext[u]) {
                        if (!bb_eval_pred(S, un[u], vval, &t)) goto out;
                        ok = t != 0;
                    }
                    if (ok) dom[d++] = (uint32_t)(key - kl);
                }
                if (key == kh) break;
            }
            vval[v] = 0;
            if (!d) { why = "small"; goto out; }
            _ss_write(S, val, v, (kl + dom[_ps_below(&st, d)]) ^ bias);
        }
    }
    why = "assert";
    for (uint32_t id = 2; id <= N; id++) {
        if (!dvs_aig_is_and(m, (dvs_aig_node_t)id)) continue;
        dvs_aig_node_t l, r;
        dvs_aig_get_children(m, (dvs_aig_node_t)id, &l, &r);
        val[id] = (int8_t)(_lit_val(val, l) & _lit_val(val, r));
    }
    S->aux_work += N;
    {
        uint32_t a = 0;
        while (a < S->n_asserts && _lit_val(val, S->assert_nodes[a])) a++;
        if (a == S->n_asserts) {
            free(S->node_val);
            S->node_val = val;
            val = NULL;
            S->model_in_sat = 0;
            S->base_sim = 1;
            S->ss_hits++;
            rc = DVS_BB_SAT;
        }
    }
out:
    if (getenv("DV_BB_STATS"))
        fprintf(stderr, "[bb-sortsamp] vars=%u edges=%zu unary=%zu chains=%u -> %s\n",
                nused, n_e, n_u, nchain, rc == DVS_BB_SAT ? "sat" : why);
    free(lf); free(un); free(unv); free(unext); free(dom); free(memb); free(sub);
    free(pred); free(succ); free(gap); free(grd); free(uhead); free(lo); free(hi);
    free(fmask); free(fval); free(vval); free(used); free(nmark); free(val);
    return rc;
}

/* Append to (*vs)[s0..] the unsubstituted vars `e` reads through
 * substitutions, each once; 0 for a construct outside the walk or an
 * allocation failure. */
static int _ds_vars(dvs_bbsolver_t *S, dvs_expr_t e, uint32_t **vs, size_t s0,
                    size_t *n, size_t *cap, int depth) {
    const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, e);
    if (!k || depth > 64) return 0;
    switch (*k) {
    case EXPR_CONST: return 1;
    case EXPR_VAR: {
        uint32_t id = ((const ExprVar *)k)->var_id;
        if (id >= S->n_vars) return 0;
        if (S->subst && S->subst[id] != EXPR_NULL)
            return _ds_vars(S, S->subst[id], vs, s0, n, cap, depth + 1);
        for (size_t i = s0; i < *n; i++) if ((*vs)[i] == id) return 1;
        if (!_us_grow((void **)vs, cap, *n, sizeof(**vs))) return 0;
        (*vs)[(*n)++] = id;
        return 1;
    }
    case EXPR_BINARY: {
        const ExprBinary *b = (const ExprBinary *)k;
        return _ds_vars(S, b->lhs, vs, s0, n, cap, depth + 1) &&
               _ds_vars(S, b->rhs, vs, s0, n, cap, depth + 1);
    }
    case EXPR_UNARY:
        return _ds_vars(S, ((const ExprUnary *)k)->operand, vs, s0, n, cap, depth + 1);
    case EXPR_EXTEND: case EXPR_SV_CAST:
        return _ds_vars(S, ((const ExprExtend *)k)->operand, vs, s0, n, cap, depth + 1);
    case EXPR_EXTRACT:
        return _ds_vars(S, ((const ExprExtract *)k)->operand, vs, s0, n, cap, depth + 1);
    case EXPR_CONCAT: {
        const ExprConcat *c = (const ExprConcat *)k;
        return _ds_vars(S, c->lo, vs, s0, n, cap, depth + 1) &&
               _ds_vars(S, c->hi, vs, s0, n, cap, depth + 1);
    }
    case EXPR_ITE: {
        const ExprITE *i = (const ExprITE *)k;
        return _ds_vars(S, i->cond, vs, s0, n, cap, depth + 1) &&
               _ds_vars(S, i->then_e, vs, s0, n, cap, depth + 1) &&
               _ds_vars(S, i->else_e, vs, s0, n, cap, depth + 1);
    }
    case EXPR_IN_RANGE: {
        const ExprInRange *r = (const ExprInRange *)k;
        return _ds_vars(S, r->value, vs, s0, n, cap, depth + 1) &&
               _ds_vars(S, r->lo, vs, s0, n, cap, depth + 1) &&
               _ds_vars(S, r->hi, vs, s0, n, cap, depth + 1);
    }
    case EXPR_IN_SET: {
        const ExprInSet *r = (const ExprInSet *)k;
        dvs_expr_t *el = expr_in_set_elems(S->problem, e);
        if (!_ds_vars(S, r->value, vs, s0, n, cap, depth + 1)) return 0;
        for (uint32_t i = 0; i < r->n_elems; i++)
            if (!_ds_vars(S, el[i], vs, s0, n, cap, depth + 1)) return 0;
        return 1;
    }
    case EXPR_IN_RANGES: {
        const ExprInRanges *r = (const ExprInRanges *)k;
        dvs_expr_t *los = expr_in_ranges_los(S->problem, e);
        dvs_expr_t *his = expr_in_ranges_his(S->problem, e);
        if (!_ds_vars(S, r->value, vs, s0, n, cap, depth + 1)) return 0;
        for (uint32_t i = 0; i < r->n_ranges; i++)
            if (!_ds_vars(S, los[i], vs, s0, n, cap, depth + 1) ||
                !_ds_vars(S, his[i], vs, s0, n, cap, depth + 1)) return 0;
        return 1;
    }
    default: return 0;
    }
}

/* Whether a var of vs[s0..n2) reaches v through the definitions (a cycle
 * if `v` were defined by them). */
static int _ds_reaches(dvs_bbsolver_t *S, const uint32_t *vs, size_t s0, size_t n2,
                       const dvs_expr_t *def, const size_t *dvoff, const uint32_t *dvn,
                       uint32_t v, uint32_t *stk, uint8_t *seen) {
    uint32_t sp = 0;
    memset(seen, 0, S->n_vars);
    for (size_t j = s0; j < n2; j++) {
        if (vs[j] == v) return 1;
        if (!seen[vs[j]]) { seen[vs[j]] = 1; stk[sp++] = vs[j]; }
    }
    while (sp) {
        uint32_t u = stk[--sp];
        if (def[u] == EXPR_NULL) continue;
        for (size_t j = dvoff[u]; j < dvoff[u] + dvn[u]; j++) {
            if (vs[j] == v) return 1;
            if (!seen[vs[j]]) { seen[vs[j]] = 1; stk[sp++] = vs[j]; }
        }
    }
    return 0;
}

#define DVS_DS_TRIES 64

/* Div/mul rejection sampler (DV_DIVSAMP=0 off). A var a leaf `v == e`
 * defines (v a bare unsigned var not in e, the definitions acyclic) is
 * DEFINED; every other var a leaf reads is FREE. Each free var is drawn
 * uniform over its unary-leaf hull, or over its domain enumerated when the
 * hull is small; the defined vars are evaluated in dependency order and every
 * leaf the draw does not already satisfy is checked. The first draw that
 * passes is the model. A free assignment has at most one extension, so
 * rejection keeps the draw uniform over the models. Returns DVS_BB_SAT or
 * DVS_BB_UNKNOWN (take the normal path). */
static int _divsamp(dvs_bbsolver_t *S) {
    if (!S->presim || S->asserts_lost || !S->n_asserts || !S->aig) return DVS_BB_UNKNOWN;
    const char *env = getenv("DV_DIVSAMP");
    if (env && strcmp(env, "0") == 0) return DVS_BB_UNKNOWN;
    if (S->ds_tries >= 8 && S->ds_hits * 4 < S->ds_tries) return DVS_BB_UNKNOWN;
    if (S->problem->softs_head != EXPR_NULL || S->problem->dists_head != EXPR_NULL ||
        S->problem->allDiff_head != EXPR_NULL)
        return DVS_BB_UNKNOWN;
    dvs_aig_t *m = S->aig;
    uint32_t N = (uint32_t)dvs_aig_num_nodes(m), nv = S->n_vars;
    if (N < 2 || nv == 0) return DVS_BB_UNKNOWN;
    int rc = DVS_BB_UNKNOWN;
    const char *why = "shape";
    size_t n_lf = 0, cap_lf = 0, n_vs = 0, cap_vs = 0, n_ck = 0;
    uint32_t n_free = 0, n_def = 0, tries = 0;
    dvs_expr_t *lf = NULL;
    uint32_t *vs = NULL, *ck = NULL, *dom_all = NULL;
    size_t *loff = NULL;
    dvs_expr_t *def = (dvs_expr_t *)malloc(nv * sizeof(dvs_expr_t));
    uint32_t *defl = (uint32_t *)malloc(nv * sizeof(uint32_t));
    uint32_t *uhead = (uint32_t *)malloc(nv * sizeof(uint32_t));
    uint32_t *order = (uint32_t *)malloc(nv * sizeof(uint32_t));
    uint32_t *dn = (uint32_t *)calloc(nv, sizeof(uint32_t));
    size_t *doff = (size_t *)calloc(nv, sizeof(size_t));
    uint64_t *kl = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *kh = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *bias = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *lo = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *hi = (uint64_t *)malloc(nv * sizeof(uint64_t));
    uint64_t *fmask = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *fval = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint64_t *vval = (uint64_t *)calloc(nv, sizeof(uint64_t));
    uint8_t *role = (uint8_t *)calloc(nv, 1);      /* 1 read, 2 free, 3 defined */
    uint8_t *nmark = (uint8_t *)calloc((size_t)N + 2, 1);
    int8_t *val = (int8_t *)malloc((size_t)N + 2);
    size_t *dvoff = (size_t *)calloc(nv, sizeof(size_t));
    uint32_t *dvn = (uint32_t *)calloc(nv, sizeof(uint32_t));
    uint32_t *stk = (uint32_t *)malloc(((size_t)nv + 1) * sizeof(uint32_t));
    uint8_t *seen = (uint8_t *)malloc(nv);
    uint32_t *unext = NULL;
    if (!dvoff || !dvn || !stk || !seen || !def || !defl || !uhead || !order || !dn || !doff || !kl || !kh || !bias ||
        !lo || !hi || !fmask || !fval || !vval || !role || !nmark || !val) goto out;
    for (uint32_t v = 0; v < nv; v++) {
        def[v] = EXPR_NULL;
        uhead[v] = UINT32_MAX;
        lo[v] = 0;
        hi[v] = bb_eval_mask(S->vars[v].width);
    }

    /* The vars of each leaf; a no-var leaf must hold. */
    if (!_us_leaves(S, &lf, &n_lf, &cap_lf)) goto out;
    loff = (size_t *)malloc((n_lf + 1) * sizeof(size_t));
    unext = (uint32_t *)malloc((n_lf + 1) * sizeof(uint32_t));
    ck = (uint32_t *)malloc((n_lf + 1) * sizeof(uint32_t));
    if (!loff || !unext || !ck) goto out;
    for (size_t li = 0; li < n_lf; li++) {
        uint64_t t;
        loff[li] = n_vs;
        if (!_ds_vars(S, lf[li], &vs, n_vs, &n_vs, &cap_vs, 0)) goto out;
        for (size_t i = loff[li]; i < n_vs; i++) role[vs[i]] = 1;
        if (n_vs == loff[li]) {
            if (!bb_eval_pred(S, lf[li], vval, &t) || !t) { why = "const"; goto out; }
        } else if (n_vs - loff[li] == 1) {
            uint32_t v = vs[loff[li]];
            unext[li] = uhead[v];
            uhead[v] = (uint32_t)li;
        }
    }
    loff[n_lf] = n_vs;

    /* Definitions: `v == e` of two or more vars, v bare and not in e. */
    for (size_t li = 0; li < n_lf; li++) {
        if (loff[li + 1] - loff[li] < 2) continue;
        const ExprKind *k = (const ExprKind *)POOL_PTR(S->problem, lf[li]);
        if (!k || *k != EXPR_BINARY || ((const ExprBinary *)k)->op != DVS_BIN_EQ) continue;
        const ExprBinary *b = (const ExprBinary *)k;
        for (int side = 0; side < 2; side++) {
            dvs_expr_t s = _ss_strip(S, side ? b->rhs : b->lhs);
            dvs_expr_t o = side ? b->lhs : b->rhs;
            const ExprKind *sk = (const ExprKind *)POOL_PTR(S->problem, s);
            if (!sk || *sk != EXPR_VAR) continue;
            uint32_t v = ((const ExprVar *)sk)->var_id;
            if (v >= nv || def[v] != EXPR_NULL) continue;
            size_t s0 = n_vs, n2 = n_vs, j;
            if (!_ds_vars(S, o, &vs, s0, &n2, &cap_vs, 0)) goto out;
            if (_ds_reaches(S, vs, s0, n2, def, dvoff, dvn, v, stk, seen)) continue;
            def[v] = o;
            dvoff[v] = s0;
            dvn[v] = (uint32_t)(n2 - s0);
            n_vs = n2;
            defl[v] = (uint32_t)li;
            role[v] = 3;
            break;
        }
    }

    /* Dependency order of the definitions; a cycle declines. */
    {
        uint32_t done = 0, total = 0;
        for (uint32_t v = 0; v < nv; v++) {
            if (role[v] == 1) { role[v] = 2; n_free++; }
            else if (role[v] == 3) total++;
        }
        uint8_t *rdy = (uint8_t *)calloc(nv, 1);
        if (!rdy) goto out;
        while (done < total) {
            uint32_t was = done;
            for (uint32_t v = 0; v < nv; v++) {
                if (role[v] != 3 || rdy[v]) continue;
                size_t s0 = dvoff[v], n2 = s0 + dvn[v], j;
                for (j = s0; j < n2; j++)
                    if (role[vs[j]] == 3 && !rdy[vs[j]]) break;
                if (j < n2) continue;
                rdy[v] = 1;
                order[done++] = v;
            }
            if (done == was) { free(rdy); why = "cycle"; goto out; }
        }
        free(rdy);
        n_def = total;
    }

    /* Each read var: unsigned, own AIG input bits. */
    why = "var";
    for (uint32_t v = 0; v < nv; v++) {
        if (!role[v]) continue;
        const bb_var_t *bv = &S->vars[v];
        uint32_t w = bv->width;
        if (bv->is_signed || w == 0 || w > 64 || !bv->bv_built || bv->bv.size != w
            || (S->subst && S->subst[v] != EXPR_NULL)) goto out;
        for (uint32_t i = 0; i < w; i++) {
            dvs_aig_node_t b = bv->bv.bits[w - 1 - i];
            if (b == DVS_AIG_TRUE || b == DVS_AIG_FALSE) {
                fmask[v] |= 1ull << i;
                if (b == DVS_AIG_TRUE) fval[v] |= 1ull << i;
                continue;
            }
            if (b < 0 || (uint32_t)b > N || !dvs_aig_is_input(m, b) || nmark[b]) goto out;
            nmark[b] = 1;
        }
    }
    bb_unary_bounds(S, lo, hi);

    /* Free-var proposals: the domain when the hull is small, else the box. */
    why = "domain";
    {
        uint64_t work = 0;
        size_t cap_d = 0, n_d = 0;
        for (uint32_t v = 0; v < nv; v++) {
            if (role[v] != 2) continue;
            int have = 0;
            uint64_t t;
            kl[v] = 0;
            kh[v] = bb_eval_mask(S->vars[v].width);
            for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u])
                _ss_hull(S, lf[u], v, &bias[v], &have, &kl[v], &kh[v]);
            if (bias[v] == 0) {
                if (lo[v] > kl[v]) kl[v] = lo[v];
                if (hi[v] < kh[v]) kh[v] = hi[v];
            }
            if (kl[v] > kh[v]) goto out;       /* empty: the normal path decides */
            if (kh[v] - kl[v] >= DVS_SS_HULL) {
                for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u]) ck[n_ck++] = u;
                continue;
            }
            uint32_t nuv = 1;
            for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u]) nuv++;
            work += (kh[v] - kl[v] + 1) * nuv;
            if (work > DVS_SS_WORK) goto out;
            doff[v] = n_d;
            for (uint64_t key = kl[v]; ; key++) {
                uint64_t x = key ^ bias[v];
                if ((x & fmask[v]) == fval[v]) {
                    int ok = 1;
                    vval[v] = x;
                    for (uint32_t u = uhead[v]; ok && u != UINT32_MAX; u = unext[u]) {
                        if (!bb_eval_pred(S, lf[u], vval, &t)) goto out;
                        ok = t != 0;
                    }
                    if (ok) {
                        if (!_us_grow((void **)&dom_all, &cap_d, n_d, sizeof(*dom_all)))
                            goto out;
                        dom_all[n_d++] = (uint32_t)(key - kl[v]);
                    }
                }
                if (key == kh[v]) break;
            }
            vval[v] = 0;
            dn[v] = (uint32_t)(n_d - doff[v]);
            if (!dn[v]) { why = "small"; goto out; }
        }
    }
    /* Checks: every leaf of two or more vars, and the unary leaves of the
     * defined vars (the box vars' were added above). */
    for (size_t li = 0; li < n_lf; li++)
        if (loff[li + 1] - loff[li] >= 2) ck[n_ck++] = (uint32_t)li;
    for (uint32_t v = 0; v < nv; v++)
        if (role[v] == 3)
            for (uint32_t u = uhead[v]; u != UINT32_MAX; u = unext[u]) ck[n_ck++] = u;
    S->ds_tries++;

    /* Draw: seeded fill, then up to DVS_DS_TRIES proposals. */
    why = "reject";
    val[0] = 0;
    val[1] = 1;
    {
        uint64_t st = S->seed ^ 0x44495653414D504Cull, bits = 0;
        uint32_t nb = 0;
        int hit = 0;
        for (uint32_t id = 2; id <= N; id++) {
            if (!dvs_aig_is_input(m, (dvs_aig_node_t)id)) continue;
            if (!nb) { bits = _ps_next(&st); nb = 64; }
            val[id] = (int8_t)(bits & 1);
            bits >>= 1;
            nb--;
        }
        for (; tries < DVS_DS_TRIES && !hit; tries++) {
            uint32_t v;
            uint64_t x, t;
            int ok = 1;
            for (v = 0; ok && v < nv; v++) {
                if (role[v] != 2) continue;
                if (dn[v]) x = (kl[v] + dom_all[doff[v] + _ps_below(&st, dn[v])]) ^ bias[v];
                else {
                    uint64_t span = kh[v] - kl[v];
                    x = (kl[v] + (span == UINT64_MAX ? _ps_next(&st)
                                                     : _ps_below(&st, span + 1))) ^ bias[v];
                    ok = (x & fmask[v]) == fval[v];
                }
                vval[v] = x;
            }
            if (!ok) continue;
            for (uint32_t i = 0; i < n_def; i++) {
                v = order[i];
                uint32_t w = S->vars[v].width;
                if (!bb_eval(S, def[v], w, vval, &x)) { why = "eval"; goto out; }
                vval[v] = x & bb_eval_mask(w);
            }
            for (size_t c = 0; ok && c < n_ck; c++) {
                if (!bb_eval_pred(S, lf[ck[c]], vval, &t)) { why = "eval"; goto out; }
                ok = t != 0;
            }
            hit = ok;
        }
        if (!hit) goto out;
        for (uint32_t v = 0; v < nv; v++)
            if (role[v] >= 2) _ss_write(S, val, v, vval[v]);
    }
    why = "assert";
    for (uint32_t id = 2; id <= N; id++) {
        if (!dvs_aig_is_and(m, (dvs_aig_node_t)id)) continue;
        dvs_aig_node_t l, r;
        dvs_aig_get_children(m, (dvs_aig_node_t)id, &l, &r);
        val[id] = (int8_t)(_lit_val(val, l) & _lit_val(val, r));
    }
    S->aux_work += N;
    {
        uint32_t a = 0;
        while (a < S->n_asserts && _lit_val(val, S->assert_nodes[a])) a++;
        if (a == S->n_asserts) {
            free(S->node_val);
            S->node_val = val;
            val = NULL;
            S->model_in_sat = 0;
            S->base_sim = 1;
            S->ds_hits++;
            rc = DVS_BB_SAT;
        }
    }
out:
    if (getenv("DV_BB_STATS"))
        fprintf(stderr, "[bb-divsamp] free=%u def=%u checks=%zu tries=%u -> %s\n",
                n_free, n_def, n_ck, tries, rc == DVS_BB_SAT ? "sat" : why);
    free(lf); free(vs); free(ck); free(dom_all); free(loff); free(unext);
    free(def); free(defl); free(uhead); free(order); free(dn); free(doff);
    free(kl); free(kh); free(bias); free(lo); free(hi); free(fmask); free(fval);
    free(vval); free(role); free(nmark); free(val);
    free(dvoff); free(dvn); free(stk); free(seen);
    return rc;
}

/* Pinned-constant fast path (DV_PINFAST=0 off). The input bits a live assert
 * forces through its top-level conjunction are fixed; the rest are unknown.
 * If a three-valued evaluation makes every live assert true whatever the
 * unknown bits are, any fill is a model: the unknown inputs get a seeded fill
 * and the model is installed as node_val, with no simulation, SAT or
 * diversify. The forced bits hold in every model, so a forced input wanted
 * both ways, a constant-false conjunct or an assert that evaluates false is
 * unsat. Returns DVS_BB_SAT or DVS_BB_UNSAT, else DVS_BB_UNKNOWN (take the
 * normal path). */
static int _pinfast(dvs_bbsolver_t *S) {
    if (!S->presim || S->asserts_lost || !S->n_asserts || !S->aig) return DVS_BB_UNKNOWN;
    const char *pf = getenv("DV_PINFAST");
    if (pf && strcmp(pf, "0") == 0) return DVS_BB_UNKNOWN;
    dvs_aig_t *m = S->aig;
    uint32_t N = (uint32_t)dvs_aig_num_nodes(m);
    if (N == 0) return DVS_BB_UNKNOWN;
    S->aux_work += N;
    /* tv: 0 / 1 / 2 = unknown. seen: the positive literal was walked. */
    int8_t *tv = (int8_t *)malloc((size_t)N + 1);
    uint8_t *seen = (uint8_t *)calloc((size_t)N + 1, 1);
    dvs_aig_node_t *wl = (dvs_aig_node_t *)malloc(((size_t)N + 2) * sizeof(dvs_aig_node_t));
    int rc = DVS_BB_UNKNOWN;
    uint32_t forced = 0;
    if (!tv || !seen || !wl) goto out;
    {
        int fr = _forced_inputs(S, N, tv, seen, wl, &forced);
        if (fr != 1) { rc = fr == DVS_BB_UNSAT ? DVS_BB_UNSAT : DVS_BB_UNKNOWN; goto out; }
    }
    /* Three-valued pass in topological (ascending id) order. */
    for (uint32_t id = 2; id <= N; id++) {
        if (!dvs_aig_is_and(m, (dvs_aig_node_t)id)) continue;
        dvs_aig_node_t l, r;
        dvs_aig_get_children(m, (dvs_aig_node_t)id, &l, &r);
        int8_t lv = tv[l < 0 ? -l : l], rv = tv[r < 0 ? -r : r];
        if (lv != 2 && l < 0) lv = !lv;
        if (rv != 2 && r < 0) rv = !rv;
        tv[id] = (lv == 0 || rv == 0) ? 0 : (lv == 1 && rv == 1) ? 1 : 2;
    }
    for (uint32_t a = 0; a < S->n_asserts; a++) {
        dvs_aig_node_t lit = S->assert_nodes[a];
        int8_t v = tv[lit < 0 ? -lit : lit];
        if (v != 2 && (lit < 0 ? !v : v) == 0) { rc = DVS_BB_UNSAT; goto out; }
    }
    for (uint32_t a = 0; a < S->n_asserts; a++) {
        dvs_aig_node_t lit = S->assert_nodes[a];
        int8_t v = tv[lit < 0 ? -lit : lit];
        if (v == 2) goto out;
    }
    /* Every assert holds for any fill of the unknown inputs: fill and evaluate. */
    for (uint32_t id = 2; id <= N; id++) {
        if (dvs_aig_is_input(m, (dvs_aig_node_t)id)) {
            if (tv[id] == 2)
                tv[id] = S->seed ? (int8_t)(_free_var_fill(S->seed, id, 0x50494e46u) & 1) : 0;
        } else if (dvs_aig_is_and(m, (dvs_aig_node_t)id)) {
            dvs_aig_node_t l, r;
            dvs_aig_get_children(m, (dvs_aig_node_t)id, &l, &r);
            tv[id] = (int8_t)(_lit_val(tv, l) & _lit_val(tv, r));
        }
    }
    free(S->node_val);
    S->node_val = tv;
    tv = NULL;
    S->model_in_sat = 0;
    S->base_sim = 1;
    rc = DVS_BB_SAT;
out:
    if (getenv("DV_BB_STATS"))
        fprintf(stderr, "[bb-pinfast] pinfast=%d forced=%u nodes=%u asserts=%u\n",
                rc == DVS_BB_SAT ? 1 : rc == DVS_BB_UNSAT ? 2 : 0, forced, N, S->n_asserts);
    free(tv); free(seen); free(wl);
    return rc;
}

int dvs_bbsolver_sess_solve(dvs_bbsolver_t *S, uint64_t seed) {
    if (!S || !S->sess) return DVS_BB_ERROR;
    if (S->had_error)       return DVS_BB_ERROR;
    if (S->had_unsupported) return DVS_BB_UNKNOWN;
    S->seed = seed;
    free(S->node_val);
    S->node_val = NULL;
    int pf = _popsamp(S);
    if (pf == DVS_BB_UNKNOWN) pf = _uniqsamp(S);
    if (pf == DVS_BB_UNKNOWN) pf = _sortsamp(S);
    if (pf == DVS_BB_UNKNOWN) pf = _pinfast(S);
    if (pf == DVS_BB_UNKNOWN) pf = _divsamp(S);
    if (pf != DVS_BB_UNKNOWN) {
        _sim_check(S, pf);
        S->last_result = pf;
        return pf;
    }
    dvs_sat_set_seed(S->sat, seed);
    {
        const char *mcl = getenv("DV_KISSAT_LIGHT_MAXCLAUSES");
        uint64_t thresh = mcl && *mcl ? strtoull(mcl, NULL, 10) : 2000000;
        dvs_sat_set_light_search(S->sat, dvs_aig_cnf_num_clauses(S->cnf) < thresh);
    }
    free(S->node_val);
    S->node_val = NULL;
    int rc = _solve_with_sim(S);
    S->last_result = rc;
    if (rc == DVS_BB_SAT) _diversify(S);
    return rc;
}

/* ------------------------------------------------------------------------- *
 * DSE-2: soft-aware MaxSAT serve.
 *
 * The BV-SAT serve path is otherwise soft-less.  This wrapper keeps the
 * maximal priority-respecting set of soft constraints, mirroring the primary
 * engine's relaxation policy (dvs_solver_solve in dvs_search.c): start with all
 * softs kept; on UNSAT, drop the single kept soft with the highest priority
 * *value* (= lowest preference; ties broken toward the last in walk order, as
 * dvs_solver_solve does with `>=`) and retry.  Each attempt is a fresh bbsolver
 * because the SAT layer is non-incremental — cheap in the common case (all
 * softs satisfiable → one solve), and bounded by the number that must be
 * dropped otherwise.
 *
 * On DVS_BB_SAT, *out_bb receives the solved bbsolver (caller frees it and
 * reads the model via dvs_bbsolver_value); the kept-set is reflected in its
 * model.  On UNSAT/UNKNOWN/ERROR, *out_bb is NULL.  A hard-UNSAT problem
 * (UNSAT even with every soft dropped) returns DVS_BB_UNSAT.
 * ------------------------------------------------------------------------- */
int dvs_bbsolver_check_maxsat(dvs_alloc_t *alloc, dvs_problem_t *problem,
                              uint64_t seed, dvs_bbsolver_t **out_bb,
                              uint8_t *out_keep, uint32_t keep_cap) {
    if (!problem || !out_bb) return DVS_BB_ERROR;
    *out_bb = NULL;

    uint32_t n = problem->n_softs;
    if (n == 0) {
        /* No softs — a plain check. */
        dvs_bbsolver_t *bb = dvs_bbsolver_new(alloc, problem);
        if (!bb) return DVS_BB_ERROR;
        int rc = dvs_bbsolver_check(bb, seed);
        if (rc == DVS_BB_SAT) { *out_bb = bb; return rc; }
        dvs_bbsolver_free(bb);
        return rc;
    }

    /* Gather soft priorities in softs_head walk order (the soft_keep index). */
    uint32_t *pri = (uint32_t *)xalloc(alloc, n * sizeof(uint32_t));
    uint8_t  *keep = (uint8_t *)xalloc(alloc, n * sizeof(uint8_t));
    if (!pri || !keep) {
        if (pri) xfree(alloc, pri, n * sizeof(uint32_t));
        if (keep) xfree(alloc, keep, n * sizeof(uint8_t));
        return DVS_BB_ERROR;
    }
    {
        uint32_t i = 0;
        for (dvs_expr_t cur = problem->softs_head; cur != EXPR_NULL && i < n; i++) {
            SoftSpec *ss = (SoftSpec *)POOL_PTR(problem, cur);
            pri[i] = ss->priority;
            keep[i] = 1;
            cur = ss->next;
        }
    }

    /* Fast path: try ALL softs kept. The common case is no conflicting softs, so
     * one solve settles it. */
    int rc;
    {
        dvs_bbsolver_t *bb = dvs_bbsolver_new(alloc, problem);
        if (!bb) { rc = DVS_BB_ERROR; goto done; }
        bb->soft_keep = keep;   /* keep[] is all-1 from the init above */
        bb->n_softs   = n;
        rc = dvs_bbsolver_check(bb, seed);
        if (rc == DVS_BB_SAT) {
            *out_bb = bb;
            if (out_keep) {
                uint32_t m = (keep_cap < n) ? keep_cap : n;
                for (uint32_t i = 0; i < m; i++) out_keep[i] = keep[i];
            }
            goto done;
        }
        dvs_bbsolver_free(bb);
        if (rc != DVS_BB_UNSAT) goto done;   /* UNKNOWN / ERROR — defer */
    }

    /* Some soft conflicts. Use ADDITIVE greedy (Boolector-parity): start from the
     * hard core only and add softs one at a time in DESCENDING preference
     * (ascending priority value; ties by declaration / walk order), keeping each
     * soft that is still SAT with the hard core + the already-kept softs. This
     * keeps EVERY individually-compatible soft. A subtractive "drop the worst on
     * UNSAT" greedy is wrong here: working down to a conflicting higher-preference
     * soft, it sheds satisfiable lower-preference softs as collateral (e.g. it
     * dropped a satisfiable `d==40` while a conflicting sibling forced relaxation).
     * Cost is one solve per soft only on the (rare) conflicting path; the all-kept
     * fast path above covers the common no-conflict case in a single solve. */

    /* Preference order: indices sorted by priority value ascending (0 = keep
     * hardest), ties broken by walk-order index. n is small (<= a few dozen). */
    uint32_t *order = (uint32_t *)xalloc(alloc, n * sizeof(uint32_t));
    if (!order) { rc = DVS_BB_ERROR; goto done; }
    for (uint32_t i = 0; i < n; i++) order[i] = i;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t best_j = i;
        for (uint32_t j = i + 1; j < n; j++) {
            if (pri[order[j]] < pri[order[best_j]] ||
                (pri[order[j]] == pri[order[best_j]] && order[j] < order[best_j]))
                best_j = j;
        }
        uint32_t tmp = order[i]; order[i] = order[best_j]; order[best_j] = tmp;
    }

    /* Base model: hard core only (all softs relaxed). */
    for (uint32_t i = 0; i < n; i++) keep[i] = 0;
    dvs_bbsolver_t *best = dvs_bbsolver_new(alloc, problem);
    if (!best) { xfree(alloc, order, n * sizeof(uint32_t)); rc = DVS_BB_ERROR; goto done; }
    best->soft_keep = keep;
    best->n_softs   = n;
    rc = dvs_bbsolver_check(best, seed);
    if (rc != DVS_BB_SAT) {
        /* Hard core itself UNSAT (or undecided) — softs can't help. */
        dvs_bbsolver_free(best);
        xfree(alloc, order, n * sizeof(uint32_t));
        goto done;
    }

    /* Additively try each soft, most-preferred first. */
    for (uint32_t k = 0; k < n; k++) {
        uint32_t idx = order[k];
        keep[idx] = 1;                                  /* tentatively add */
        dvs_bbsolver_t *cand = dvs_bbsolver_new(alloc, problem);
        if (!cand) { keep[idx] = 0; continue; }         /* OOM: skip this soft */
        cand->soft_keep = keep;
        cand->n_softs   = n;
        int crc = dvs_bbsolver_check(cand, seed);
        if (crc == DVS_BB_SAT) {
            dvs_bbsolver_free(best);                    /* this soft fits — keep it */
            best = cand;
        } else {
            dvs_bbsolver_free(cand);                    /* UNSAT/undecided — drop it */
            keep[idx] = 0;
        }
    }

    *out_bb = best;
    rc = DVS_BB_SAT;
    if (out_keep) {
        uint32_t m = (keep_cap < n) ? keep_cap : n;
        for (uint32_t i = 0; i < m; i++) out_keep[i] = keep[i];
    }
    xfree(alloc, order, n * sizeof(uint32_t));

done:
    xfree(alloc, pri, n * sizeof(uint32_t));
    xfree(alloc, keep, n * sizeof(uint8_t));
    return rc;
}

void dvs_bbsolver_set_soft_keep(dvs_bbsolver_t *S, const uint8_t *keep,
                                uint32_t n) {
    if (!S) return;
    S->soft_keep = keep;   /* caller owns the array; must outlive the check */
    S->n_softs   = n;
}

/* A variable that appears in no constraint is never bit-blasted, so the SAT
 * model says nothing about it. For a design-verification (randomization) solve
 * every rand variable should still get a value, and an unconstrained one should
 * be uniformly random rather than a fixed 0. Derive a value from (seed, var_id,
 * limb): deterministic per seed (reproducible), varying across vars/seeds.
 * Only used when a nonzero (diversity) seed was supplied; seed 0 keeps the
 * legacy deterministic 0 so BMC/decision flows are unchanged. */
static uint64_t _free_var_fill(uint64_t seed, uint32_t var_id, uint32_t limb) {
    uint64_t x = seed + 0x9E3779B97F4A7C15ULL * ((uint64_t)var_id + 1)
                      + 0xD1B54A32D192ED03ULL * ((uint64_t)limb + 1);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

int dvs_bbsolver_value(dvs_bbsolver_t *S, uint32_t var_id, int64_t *out_value) {
    if (!S || !out_value) return -1;
    if (S->last_result != DVS_BB_SAT) return -1;
    if (var_id >= S->n_vars) return -1;
    bb_var_t *v = &S->vars[var_id];
    if (!v->defined) return -1;
    if (!v->bv_built) {
        /* Unreferenced variable: seeded-random for a diversity solve, else 0. */
        uint64_t r = S->seed ? _free_var_fill(S->seed, var_id, 0) : 0;
        if (v->width < 64) r &= ((uint64_t)1 << v->width) - 1;
        *out_value = (int64_t)r;
        return 0;
    }
    /* bits[] are stored MSB-first: bits[0] is the high bit (logical position
     * size-1), bits[size-1] is the LSB. Read only the low 64 bits here; the
     * shift stays < 64 so there is no UB even for a wide var (the high bits are
     * simply dropped — callers needing them use dvs_bbsolver_value_wide). */
    uint64_t val = 0;
    uint32_t n = v->bv.size < 64 ? v->bv.size : 64;
    /* Fallback seeded fill for don't-care bits when the flip-check pass did not
     * run (e.g. it was skipped or out of memory). */
    uint64_t rnd = S->seed ? _free_var_fill(S->seed, var_id, 0) : 0;
    for (uint32_t p = 0; p < n; p++) {
        /* logical bit p (0 = LSB) lives at bits[size-1-p] */
        dvs_aig_node_t bit = v->bv.bits[v->bv.size - 1 - p];
        int set;
        if (S->node_val)                                 /* flip-check result */
            set = _lit_val(S->node_val, bit);
        else if (S->seed && dvs_aig_cnf_is_free(S->cnf, bit))
            set = (int)((rnd >> p) & 1);
        else
            set = (dvs_aig_cnf_value(S->cnf, bit) == 1) ? 1 : 0;
        if (set) val |= (uint64_t)1 << p;
    }
    if (v->is_signed && v->width < 64) {
        /* sign-extend from `width` to 64 */
        uint64_t sign_bit = (uint64_t)1 << (v->width - 1);
        if (val & sign_bit) {
            val |= ~(((uint64_t)1 << v->width) - 1);
        }
    }
    *out_value = (int64_t)val;
    return 0;
}

int dvs_bbsolver_value_wide(dvs_bbsolver_t *S, uint32_t var_id,
                            uint64_t *limbs, uint32_t n_limbs) {
    if (!S || !limbs || n_limbs == 0) return -1;
    if (S->last_result != DVS_BB_SAT) return -1;
    if (var_id >= S->n_vars) return -1;
    bb_var_t *v = &S->vars[var_id];
    if (!v->defined) return -1;
    for (uint32_t i = 0; i < n_limbs; i++) limbs[i] = 0;
    if (!v->bv_built) {
        /* Unreferenced variable: seeded-random for a diversity solve, else 0. */
        if (S->seed) {
            uint32_t full = v->width / 64;      /* fully-used limbs */
            for (uint32_t i = 0; i < n_limbs; i++) {
                uint64_t r = _free_var_fill(S->seed, var_id, i);
                if (i < full) limbs[i] = r;
                else if (i == full && (v->width & 63))
                    limbs[i] = r & (((uint64_t)1 << (v->width & 63)) - 1);
            }
        }
        return 0;
    }
    /* Little-endian limbs: logical bit p (0 = LSB) → limbs[p/64] bit (p%64).
     * bits[] is MSB-first, so logical bit p is at bits[size-1-p]. The shift is
     * always < 64, so no UB regardless of width. */
    uint32_t cap = n_limbs * 64;
    uint32_t n = v->bv.size < cap ? v->bv.size : cap;
    for (uint32_t p = 0; p < n; p++) {
        dvs_aig_node_t bit = v->bv.bits[v->bv.size - 1 - p];
        int set;
        if (S->node_val) {                               /* flip-check result */
            set = _lit_val(S->node_val, bit);
        } else if (S->seed && dvs_aig_cnf_is_free(S->cnf, bit)) {
            /* don't-care bit: seeded-random (see dvs_bbsolver_value) */
            uint64_t r = _free_var_fill(S->seed, var_id, p >> 6);
            set = (int)((r >> (p & 63)) & 1);
        } else {
            set = (dvs_aig_cnf_value(S->cnf, bit) == 1) ? 1 : 0;
        }
        if (set) limbs[p >> 6] |= (uint64_t)1 << (p & 63);
    }
    return 0;
}

uint64_t dvs_bbsolver_num_aig_ands(const dvs_bbsolver_t *S) {
    return S ? dvs_aig_num_ands(S->aig) : 0;
}
uint64_t dvs_bbsolver_num_sat_clauses(const dvs_bbsolver_t *S) {
    return S ? dvs_aig_cnf_num_clauses(S->cnf) : 0;
}
uint64_t dvs_bbsolver_num_sat_vars(const dvs_bbsolver_t *S) {
    return S ? dvs_aig_cnf_num_vars(S->cnf) : 0;
}
