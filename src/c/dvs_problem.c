#include <stddef.h>
#include <string.h>
#include "dvs_problem.h"

/* ------------------------------------------------------------------ */
/* Internal allocation helper                                          */
/* ------------------------------------------------------------------ */

static dvs_expr_t _pool_alloc(dvs_problem_t *sp, uint32_t bytes, uint32_t align) {
    return dvs_pool_alloc(&sp->pool, bytes, align);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

dvs_problem_t *solve_problem_init(void *buf, size_t buf_size) {
    if (!buf) return NULL;

    /* Minimum: the dvs_problem_t header + at least 1 byte of pool data */
    size_t pool_offset = offsetof(dvs_problem_t, pool);
    if (buf_size <= pool_offset + sizeof(dvs_pool_t))
        return NULL;

    dvs_problem_t *sp = (dvs_problem_t *)buf;
    sp->n_vars           = 0;
    sp->n_constraints    = 0;
    sp->n_sources        = 0;
    sp->vars_head        = EXPR_NULL;
    sp->constraints_head = EXPR_NULL;
    sp->sources_head     = EXPR_NULL;
    sp->n_alldiffs       = 0;
    sp->allDiff_head     = EXPR_NULL;
    sp->n_softs          = 0;
    sp->softs_head       = EXPR_NULL;
    sp->n_dists          = 0;
    sp->dists_head       = EXPR_NULL;
    sp->next_constraint_id = 0;
    sp->flags            = 0;

    size_t pool_buf_size = buf_size - pool_offset;
    if (!dvs_pool_init(&sp->pool, pool_buf_size))
        return NULL;

    return sp;
}

dvs_problem_t *solve_problem_init_sized(void *buf, size_t buf_size,
                                       uint32_t n_vars,
                                       uint32_t n_constraints,
                                       uint32_t n_sources) {
    (void)n_vars; (void)n_constraints; (void)n_sources; /* hints unused in Phase 3 */
    return solve_problem_init(buf, buf_size);
}

void solve_problem_reset(dvs_problem_t *sp) {
    sp->n_vars           = 0;
    sp->n_constraints    = 0;
    sp->n_sources        = 0;
    sp->vars_head        = EXPR_NULL;
    sp->constraints_head = EXPR_NULL;
    sp->sources_head     = EXPR_NULL;
    sp->n_alldiffs       = 0;
    sp->allDiff_head     = EXPR_NULL;
    sp->n_softs          = 0;
    sp->softs_head       = EXPR_NULL;
    sp->n_dists          = 0;
    sp->dists_head       = EXPR_NULL;
    sp->next_constraint_id = 0;
    sp->flags            = 0;
    dvs_pool_reset(&sp->pool);
}

void solve_problem_destroy(dvs_problem_t *sp) {
    (void)sp; /* caller manages the buffer */
}

/* ------------------------------------------------------------------ */
/* Expression builders                                                 */
/* ------------------------------------------------------------------ */

dvs_expr_t expr_const_sized(dvs_problem_t *sp, int64_t value, uint8_t is_signed,
                         uint8_t width) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprConst),
                              (uint32_t)_Alignof(ExprConst));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprConst *n = (ExprConst *)POOL_PTR(sp, ref);
    n->kind      = EXPR_CONST;
    n->is_signed = is_signed;
    n->width     = width;
    n->_pad[0]   = n->_pad[1] = 0;
    n->value     = value;
    return ref;
}

dvs_expr_t expr_const(dvs_problem_t *sp, int64_t value, uint8_t is_signed) {
    return expr_const_sized(sp, value, is_signed, 0);
}

dvs_expr_t expr_var(dvs_problem_t *sp, uint32_t var_id) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprVar),
                              (uint32_t)_Alignof(ExprVar));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprVar *n = (ExprVar *)POOL_PTR(sp, ref);
    n->kind   = EXPR_VAR;
    n->var_id = var_id;
    return ref;
}

dvs_expr_t expr_binary(dvs_problem_t *sp, dvs_binop_t op, dvs_expr_t lhs, dvs_expr_t rhs) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprBinary),
                              (uint32_t)_Alignof(ExprBinary));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprBinary *n = (ExprBinary *)POOL_PTR(sp, ref);
    n->kind = EXPR_BINARY;
    n->op   = op;
    n->lhs  = lhs;
    n->rhs  = rhs;
    return ref;
}

dvs_expr_t expr_unary(dvs_problem_t *sp, dvs_unop_t op, dvs_expr_t operand) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprUnary),
                              (uint32_t)_Alignof(ExprUnary));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprUnary *n = (ExprUnary *)POOL_PTR(sp, ref);
    n->kind    = EXPR_UNARY;
    n->op      = op;
    n->operand = operand;
    return ref;
}

dvs_expr_t expr_ite(dvs_problem_t *sp, dvs_expr_t cond, dvs_expr_t then_e, dvs_expr_t else_e) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprITE),
                              (uint32_t)_Alignof(ExprITE));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprITE *n = (ExprITE *)POOL_PTR(sp, ref);
    n->kind   = EXPR_ITE;
    n->cond   = cond;
    n->then_e = then_e;
    n->else_e = else_e;
    return ref;
}

dvs_expr_t expr_in_range(dvs_problem_t *sp, dvs_expr_t value, dvs_expr_t lo, dvs_expr_t hi) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprInRange),
                              (uint32_t)_Alignof(ExprInRange));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprInRange *n = (ExprInRange *)POOL_PTR(sp, ref);
    n->kind  = EXPR_IN_RANGE;
    n->value = value;
    n->lo    = lo;
    n->hi    = hi;
    return ref;
}

dvs_expr_t expr_in_set(dvs_problem_t *sp, dvs_expr_t value,
                    uint32_t n_elems, const dvs_expr_t *elems) {
    /* Allocate struct + trailing element array in one shot */
    uint32_t total = (uint32_t)sizeof(ExprInSet) + n_elems * (uint32_t)sizeof(dvs_expr_t);
    dvs_expr_t ref = _pool_alloc(sp, total, (uint32_t)_Alignof(ExprInSet));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprInSet *n = (ExprInSet *)POOL_PTR(sp, ref);
    n->kind    = EXPR_IN_SET;
    n->value   = value;
    n->n_elems = n_elems;
    dvs_expr_t *dst = (dvs_expr_t *)(n + 1);
    for (uint32_t i = 0; i < n_elems; i++)
        dst[i] = elems[i];
    return ref;
}

dvs_expr_t expr_extend(dvs_problem_t *sp, dvs_expr_t operand,
                    uint8_t from_bits, uint8_t to_bits, uint8_t sign_extend) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprExtend),
                              (uint32_t)_Alignof(ExprExtend));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprExtend *n = (ExprExtend *)POOL_PTR(sp, ref);
    n->kind         = EXPR_EXTEND;
    n->sign_extend  = sign_extend;
    n->from_bits    = from_bits;
    n->to_bits      = to_bits;
    n->_pad         = 0;
    n->operand      = operand;
    return ref;
}

dvs_expr_t expr_cast(dvs_problem_t *sp, dvs_expr_t operand,
                  uint8_t to_bits, uint8_t to_signed) {
    if (to_bits == 0) return EXPR_NULL;
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprCast),
                              (uint32_t)_Alignof(ExprCast));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprCast *n = (ExprCast *)POOL_PTR(sp, ref);
    n->kind      = EXPR_CAST;
    n->to_bits   = to_bits;
    n->to_signed = to_signed ? 1 : 0;
    n->_pad[0]   = n->_pad[1] = 0;
    n->operand   = operand;
    return ref;
}

dvs_expr_t expr_extract(dvs_problem_t *sp, dvs_expr_t operand,
                     uint8_t hi_bit, uint8_t lo_bit) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprExtract),
                              (uint32_t)_Alignof(ExprExtract));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprExtract *n = (ExprExtract *)POOL_PTR(sp, ref);
    n->kind    = EXPR_EXTRACT;
    n->hi_bit  = hi_bit;
    n->lo_bit  = lo_bit;
    n->_pad[0] = n->_pad[1] = 0;
    n->operand = operand;
    return ref;
}

dvs_expr_t expr_concat(dvs_problem_t *sp, dvs_expr_t hi, dvs_expr_t lo,
                    uint8_t lo_width) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprConcat),
                              (uint32_t)_Alignof(ExprConcat));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprConcat *n = (ExprConcat *)POOL_PTR(sp, ref);
    n->kind     = EXPR_CONCAT;
    n->lo_width = lo_width;
    n->_pad[0] = n->_pad[1] = n->_pad[2] = 0;
    n->hi       = hi;
    n->lo       = lo;
    return ref;
}

dvs_expr_t expr_array_select(dvs_problem_t *sp, uint32_t base_var_id,
                          uint32_t n_elems, dvs_expr_t result, dvs_expr_t index) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprArraySelect),
                              (uint32_t)_Alignof(ExprArraySelect));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprArraySelect *n = (ExprArraySelect *)POOL_PTR(sp, ref);
    n->kind        = EXPR_ARRAY_SELECT;
    n->base_var_id = base_var_id;
    n->n_elems     = n_elems;
    n->result      = result;
    n->index       = index;
    return ref;
}

dvs_expr_t expr_sum(dvs_problem_t *sp, dvs_expr_t result,
                 uint32_t n_vars, const dvs_expr_t *var_refs) {
    uint32_t total = (uint32_t)sizeof(ExprSum) + n_vars * (uint32_t)sizeof(dvs_expr_t);
    dvs_expr_t ref = _pool_alloc(sp, total, (uint32_t)_Alignof(ExprSum));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprSum *n = (ExprSum *)POOL_PTR(sp, ref);
    n->kind    = EXPR_SUM;
    n->result  = result;
    n->n_vars  = n_vars;
    dvs_expr_t *dst = (dvs_expr_t *)(n + 1);
    for (uint32_t i = 0; i < n_vars; i++)
        dst[i] = var_refs[i];
    return ref;
}

dvs_expr_t expr_countones(dvs_problem_t *sp, dvs_expr_t result, dvs_expr_t operand) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprCountones),
                              (uint32_t)_Alignof(ExprCountones));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprCountones *n = (ExprCountones *)POOL_PTR(sp, ref);
    n->kind    = EXPR_COUNTONES;
    n->result  = result;
    n->operand = operand;
    return ref;
}

dvs_expr_t expr_clog2(dvs_problem_t *sp, dvs_expr_t result, dvs_expr_t operand) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ExprClog2),
                              (uint32_t)_Alignof(ExprClog2));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprClog2 *n = (ExprClog2 *)POOL_PTR(sp, ref);
    n->kind    = EXPR_CLOG2;
    n->result  = result;
    n->operand = operand;
    return ref;
}

/* ------------------------------------------------------------------ */
/* Problem builders                                                    */
/* ------------------------------------------------------------------ */

dvs_expr_t problem_add_var(dvs_problem_t *sp, uint32_t var_id,
                        uint8_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(VarSpec),
                              (uint32_t)_Alignof(VarSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;
    VarSpec *v    = (VarSpec *)POOL_PTR(sp, ref);
    v->next       = sp->vars_head;
    v->var_id     = var_id;
    v->width      = width;
    v->is_signed  = is_signed;
    v->is_aux     = 0;
    v->_pad       = 0;
    v->lo         = lo;
    v->hi         = hi;
    sp->vars_head = ref;
    sp->n_vars++;
    return ref;
}

dvs_expr_t problem_add_constraint(dvs_problem_t *sp, dvs_expr_t root) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(ConstraintSpec),
                              (uint32_t)_Alignof(ConstraintSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;
    ConstraintSpec *c     = (ConstraintSpec *)POOL_PTR(sp, ref);
    c->next               = sp->constraints_head;
    c->root               = root;
    c->constraint_id      = ++sp->next_constraint_id;
    sp->constraints_head  = ref;
    sp->n_constraints++;
    return ref;
}

dvs_expr_t problem_add_source(dvs_problem_t *sp,
                           uint32_t n_vars, const uint32_t *var_ids) {
    uint32_t total = (uint32_t)sizeof(SourceSpec) + n_vars * (uint32_t)sizeof(uint32_t);
    dvs_expr_t ref = _pool_alloc(sp, total, (uint32_t)_Alignof(SourceSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;
    SourceSpec *s    = (SourceSpec *)POOL_PTR(sp, ref);
    s->next          = sp->sources_head;
    s->n_vars        = n_vars;
    uint32_t *dst    = (uint32_t *)(s + 1);
    for (uint32_t i = 0; i < n_vars; i++)
        dst[i] = var_ids[i];
    sp->sources_head = ref;
    sp->n_sources++;
    return ref;
}

/* ------------------------------------------------------------------ */

dvs_expr_t problem_add_all_different(dvs_problem_t *sp,
                                  uint32_t n_vars, const uint32_t *var_ids) {
    uint32_t total = (uint32_t)sizeof(AllDiffSpec) + n_vars * (uint32_t)sizeof(uint32_t);
    dvs_expr_t ref = _pool_alloc(sp, total, (uint32_t)_Alignof(AllDiffSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;
    AllDiffSpec *ad = (AllDiffSpec *)POOL_PTR(sp, ref);
    ad->next         = sp->allDiff_head;
    ad->n_vars       = n_vars;
    uint32_t *dst    = (uint32_t *)(ad + 1);
    for (uint32_t i = 0; i < n_vars; i++)
        dst[i] = var_ids[i];
    sp->allDiff_head = ref;
    sp->n_alldiffs++;
    return ref;
}


dvs_expr_t problem_add_soft_constraint(dvs_problem_t *sp, dvs_expr_t root,
                                    uint32_t priority) {
    dvs_expr_t ref = _pool_alloc(sp, (uint32_t)sizeof(SoftSpec),
                              (uint32_t)_Alignof(SoftSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;
    SoftSpec *s       = (SoftSpec *)POOL_PTR(sp, ref);
    s->next           = sp->softs_head;
    s->root           = root;
    s->priority       = priority;
    sp->softs_head    = ref;
    sp->n_softs++;
    return ref;
}

dvs_expr_t problem_add_dist(dvs_problem_t *sp, uint32_t var_id,
                        uint32_t n_entries, const dvs_dist_entry_t *entries) {
    uint32_t total = (uint32_t)sizeof(DistSpec) +
                     n_entries * (uint32_t)sizeof(dvs_dist_entry_t);
    dvs_expr_t ref = _pool_alloc(sp, total, (uint32_t)_Alignof(DistSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;
    DistSpec *ds      = (DistSpec *)POOL_PTR(sp, ref);
    ds->next          = sp->dists_head;
    ds->var_id        = var_id;
    ds->n_entries     = n_entries;
    dvs_dist_entry_t *dst    = (dvs_dist_entry_t *)(ds + 1);
    for (uint32_t i = 0; i < n_entries; i++)
        dst[i] = entries[i];
    sp->dists_head    = ref;
    sp->n_dists++;
    return ref;
}

dvs_dist_entry_t *dist_spec_entries(dvs_problem_t *sp, dvs_expr_t dist_ref) {
    if (dist_ref == EXPR_NULL) return NULL;
    DistSpec *ds = (DistSpec *)POOL_PTR(sp, dist_ref);
    return (dvs_dist_entry_t *)(ds + 1);
}

/* Access helpers                                                      */
/* ------------------------------------------------------------------ */

dvs_expr_t *expr_in_set_elems(dvs_problem_t *sp, dvs_expr_t set_ref) {
    if (set_ref == EXPR_NULL) return NULL;
    ExprInSet *n = (ExprInSet *)POOL_PTR(sp, set_ref);
    return (dvs_expr_t *)(n + 1);
}

dvs_expr_t *expr_in_ranges_los(dvs_problem_t *sp, dvs_expr_t ref) {
    if (ref == EXPR_NULL) return NULL;
    ExprInRanges *n = (ExprInRanges *)POOL_PTR(sp, ref);
    return (dvs_expr_t *)(n + 1);
}

dvs_expr_t *expr_in_ranges_his(dvs_problem_t *sp, dvs_expr_t ref) {
    if (ref == EXPR_NULL) return NULL;
    ExprInRanges *n = (ExprInRanges *)POOL_PTR(sp, ref);
    return (dvs_expr_t *)(n + 1) + n->n_ranges;
}

uint32_t *source_spec_vars(dvs_problem_t *sp, dvs_expr_t src_ref) {
    if (src_ref == EXPR_NULL) return NULL;
    SourceSpec *s = (SourceSpec *)POOL_PTR(sp, src_ref);
    return (uint32_t *)(s + 1);
}

void *solve_problem_pool_base(dvs_problem_t *sp) {
    return &sp->pool;
}
