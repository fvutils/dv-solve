#include <stdlib.h>
#include <string.h>
#include "zsp_builder.h"

#define DEFAULT_BLOCK_SIZE  4096u
#define POOL_HEADER_SZ      ((uint32_t)sizeof(zsp_pool_t))

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void *_alloc(SolveProblemBuilder *b, size_t sz) {
    if (b->alloc)
        return ZSP_ALLOC(b->alloc, sz);
    return malloc(sz);
}

static void _release(SolveProblemBuilder *b, void *ptr, size_t sz) {
    if (b->alloc) {
        ZSP_RELEASE(b->alloc, ptr, sz);
    } else {
        free(ptr);
    }
}

static uint32_t _align_up(uint32_t val, uint32_t align) {
    if (align <= 1) return val;
    uint32_t mask = align - 1;
    return (val + mask) & ~mask;
}

/** Allocate a new BuilderBlock with the given data capacity. */
static BuilderBlock *_new_block(SolveProblemBuilder *b, uint32_t capacity) {
    size_t total = sizeof(BuilderBlock) + capacity;
    BuilderBlock *blk = (BuilderBlock *)_alloc(b, total);
    if (!blk) return NULL;
    blk->next        = NULL;
    blk->base_offset = 0;
    blk->capacity    = capacity;
    blk->used        = 0;
    blk->_pad        = 0;
    return blk;
}

/** Free all blocks starting from blk. */
static void _free_blocks(SolveProblemBuilder *b, BuilderBlock *blk) {
    while (blk) {
        BuilderBlock *next = blk->next;
        size_t total = sizeof(BuilderBlock) + blk->capacity;
        _release(b, blk, total);
        blk = next;
    }
}

/**
 * Write data into the current block at the given offset within the block.
 * Caller must ensure the space is available.
 */
static void *_block_ptr_at(BuilderBlock *blk, uint32_t local_off) {
    return BUILDER_BLOCK_DATA(blk) + local_off;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

SolveProblemBuilder *builder_create(uint32_t block_size, zsp_alloc_t *alloc) {
    if (block_size == 0) block_size = DEFAULT_BLOCK_SIZE;

    SolveProblemBuilder *b;
    if (alloc)
        b = (SolveProblemBuilder *)ZSP_ALLOC(alloc, sizeof(*b));
    else
        b = (SolveProblemBuilder *)malloc(sizeof(*b));
    if (!b) return NULL;

    memset(b, 0, sizeof(*b));
    b->block_size       = block_size;
    b->alloc            = alloc;
    b->vars_head        = EXPR_NULL;
    b->constraints_head = EXPR_NULL;
    b->sources_head     = EXPR_NULL;
    b->n_alldiffs       = 0;
    b->allDiff_head     = EXPR_NULL;
    b->n_softs          = 0;
    b->softs_head       = EXPR_NULL;
    b->n_dists          = 0;
    b->dists_head       = EXPR_NULL;

    /* Allocate the first block */
    b->first = _new_block(b, block_size);
    if (!b->first) {
        _release(b, b, sizeof(*b));
        return NULL;
    }
    b->current = b->first;
    return b;
}

void builder_reset(SolveProblemBuilder *b) {
    if (!b) return;

    /* Keep the first block, free the rest */
    if (b->first) {
        _free_blocks(b, b->first->next);
        b->first->next = NULL;
        b->first->base_offset = 0;
        b->first->used = 0;
    }
    b->current          = b->first;
    b->virtual_used     = 0;
    b->n_vars           = 0;
    b->n_constraints    = 0;
    b->n_sources        = 0;
    b->vars_head        = EXPR_NULL;
    b->constraints_head = EXPR_NULL;
    b->sources_head     = EXPR_NULL;
    b->n_alldiffs       = 0;
    b->allDiff_head     = EXPR_NULL;
    b->n_softs          = 0;
    b->softs_head       = EXPR_NULL;
    b->n_dists          = 0;
    b->dists_head       = EXPR_NULL;
}

void builder_destroy(SolveProblemBuilder *b) {
    if (!b) return;
    _free_blocks(b, b->first);
    _release(b, b, sizeof(*b));
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

ExprRef builder_alloc(SolveProblemBuilder *b, uint32_t bytes, uint32_t align) {
    if (align < 1) align = 1;

    uint32_t aligned = _align_up(b->virtual_used, align);
    uint32_t padding = aligned - b->virtual_used;

    if (bytes == 0) {
        b->virtual_used = aligned;
        return POOL_HEADER_SZ + aligned;
    }

    uint32_t needed = padding + bytes;

    /* Check if current block has room */
    if (!b->current || b->current->used + needed > b->current->capacity) {
        /* Need a new block.  Ensure it can hold the full allocation. */
        uint32_t cap = b->block_size;
        if (bytes > cap) cap = bytes;

        BuilderBlock *blk = _new_block(b, cap);
        if (!blk) return EXPR_NULL;

        blk->base_offset = aligned;
        if (b->current)
            b->current->next = blk;
        else
            b->first = blk;
        b->current = blk;

        /* No padding needed in new block (base_offset is already aligned) */
        void *ptr = _block_ptr_at(blk, 0);
        blk->used = bytes;
        b->virtual_used = aligned + bytes;
        memset(ptr, 0, bytes);
        return POOL_HEADER_SZ + aligned;
    }

    /* Fit in current block: zero-fill padding gap, then write */
    if (padding > 0) {
        memset(_block_ptr_at(b->current, b->current->used), 0, padding);
        b->current->used += padding;
    }
    void *ptr = _block_ptr_at(b->current, b->current->used);
    b->current->used += bytes;
    b->virtual_used = aligned + bytes;
    memset(ptr, 0, bytes);
    return POOL_HEADER_SZ + aligned;
}

uint32_t builder_virtual_used(const SolveProblemBuilder *b) {
    return b->virtual_used;
}

void *builder_ref_ptr(const SolveProblemBuilder *b, ExprRef ref) {
    if (ref == EXPR_NULL) return NULL;
    uint32_t voff = ref - POOL_HEADER_SZ;
    for (BuilderBlock *blk = b->first; blk; blk = blk->next) {
        if (voff >= blk->base_offset &&
            voff < blk->base_offset + blk->used) {
            return _block_ptr_at(blk, voff - blk->base_offset);
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Finalize                                                            */
/* ------------------------------------------------------------------ */

SolveProblem *builder_finalize(SolveProblemBuilder *b, size_t *out_size) {
    uint32_t pool_data_size = b->virtual_used;
    size_t total = sizeof(SolveProblem) + pool_data_size;

    void *buf = _alloc(b, total);
    if (!buf) return NULL;
    memset(buf, 0, total);

    SolveProblem *sp = (SolveProblem *)buf;

    /* Copy header fields */
    sp->n_vars           = b->n_vars;
    sp->n_constraints    = b->n_constraints;
    sp->n_sources        = b->n_sources;
    sp->vars_head        = b->vars_head;
    sp->constraints_head = b->constraints_head;
    sp->sources_head     = b->sources_head;
    sp->n_alldiffs       = b->n_alldiffs;
    sp->allDiff_head     = b->allDiff_head;
    sp->n_softs          = b->n_softs;
    sp->softs_head       = b->softs_head;
    sp->n_dists          = b->n_dists;
    sp->dists_head       = b->dists_head;

    /* Init the embedded pool header: mark it as fully used */
    sp->pool.capacity = pool_data_size;
    sp->pool.used     = pool_data_size;
    sp->pool.overflow = 0;
    sp->pool._pad     = 0;

    /* Copy block data into the contiguous pool region */
    uint8_t *pool_base = (uint8_t *)&sp->pool + sizeof(zsp_pool_t);
    for (BuilderBlock *blk = b->first; blk; blk = blk->next) {
        if (blk->used > 0) {
            memcpy(pool_base + blk->base_offset,
                   BUILDER_BLOCK_DATA(blk),
                   blk->used);
        }
    }

    if (out_size) *out_size = total;
    return sp;
}

void builder_free_problem(SolveProblemBuilder *b, SolveProblem *sp, size_t size) {
    if (!sp) return;
    _release(b, sp, size);
}

/* ------------------------------------------------------------------ */
/* Expression builders                                                 */
/* ------------------------------------------------------------------ */

ExprRef builder_expr_const(SolveProblemBuilder *b, int64_t value,
                           uint8_t is_signed) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprConst),
                                (uint32_t)_Alignof(ExprConst));
    if (ref == EXPR_NULL) return EXPR_NULL;

    /* Locate the allocation within the current block */
    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprConst *n = (ExprConst *)_block_ptr_at(b->current, local);
    n->kind      = EXPR_CONST;
    n->is_signed = is_signed;
    n->_pad[0] = n->_pad[1] = n->_pad[2] = 0;
    n->value     = value;
    return ref;
}

ExprRef builder_expr_var(SolveProblemBuilder *b, uint32_t var_id) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprVar),
                                (uint32_t)_Alignof(ExprVar));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprVar *n = (ExprVar *)_block_ptr_at(b->current, local);
    n->kind   = EXPR_VAR;
    n->var_id = var_id;
    return ref;
}

ExprRef builder_expr_binary(SolveProblemBuilder *b, BinOp op,
                            ExprRef lhs, ExprRef rhs) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprBinary),
                                (uint32_t)_Alignof(ExprBinary));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprBinary *n = (ExprBinary *)_block_ptr_at(b->current, local);
    n->kind = EXPR_BINARY;
    n->op   = op;
    n->lhs  = lhs;
    n->rhs  = rhs;
    return ref;
}

ExprRef builder_expr_unary(SolveProblemBuilder *b, UnaryOp op,
                           ExprRef operand) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprUnary),
                                (uint32_t)_Alignof(ExprUnary));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprUnary *n = (ExprUnary *)_block_ptr_at(b->current, local);
    n->kind    = EXPR_UNARY;
    n->op      = op;
    n->operand = operand;
    return ref;
}

ExprRef builder_expr_ite(SolveProblemBuilder *b,
                         ExprRef cond, ExprRef then_e, ExprRef else_e) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprITE),
                                (uint32_t)_Alignof(ExprITE));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprITE *n = (ExprITE *)_block_ptr_at(b->current, local);
    n->kind   = EXPR_ITE;
    n->cond   = cond;
    n->then_e = then_e;
    n->else_e = else_e;
    return ref;
}

ExprRef builder_expr_in_range(SolveProblemBuilder *b,
                              ExprRef value, ExprRef lo, ExprRef hi) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprInRange),
                                (uint32_t)_Alignof(ExprInRange));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprInRange *n = (ExprInRange *)_block_ptr_at(b->current, local);
    n->kind  = EXPR_IN_RANGE;
    n->value = value;
    n->lo    = lo;
    n->hi    = hi;
    return ref;
}

ExprRef builder_expr_in_set(SolveProblemBuilder *b, ExprRef value,
                            uint32_t n_elems, const ExprRef *elems) {
    uint32_t total = (uint32_t)sizeof(ExprInSet) +
                     n_elems * (uint32_t)sizeof(ExprRef);
    ExprRef ref = builder_alloc(b, total, (uint32_t)_Alignof(ExprInSet));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprInSet *n = (ExprInSet *)_block_ptr_at(b->current, local);
    n->kind    = EXPR_IN_SET;
    n->value   = value;
    n->n_elems = n_elems;
    ExprRef *dst = (ExprRef *)(n + 1);
    for (uint32_t i = 0; i < n_elems; i++)
        dst[i] = elems[i];
    return ref;
}

ExprRef builder_expr_extend(SolveProblemBuilder *b, ExprRef operand,
                            uint8_t from_bits, uint8_t to_bits,
                            uint8_t sign_extend) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprExtend),
                                (uint32_t)_Alignof(ExprExtend));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprExtend *n = (ExprExtend *)_block_ptr_at(b->current, local);
    n->kind        = EXPR_EXTEND;
    n->sign_extend = sign_extend;
    n->from_bits   = from_bits;
    n->to_bits     = to_bits;
    n->_pad        = 0;
    n->operand     = operand;
    return ref;
}

ExprRef builder_expr_extract(SolveProblemBuilder *b, ExprRef operand,
                             uint8_t hi_bit, uint8_t lo_bit) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprExtract),
                                (uint32_t)_Alignof(ExprExtract));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprExtract *n = (ExprExtract *)_block_ptr_at(b->current, local);
    n->kind    = EXPR_EXTRACT;
    n->hi_bit  = hi_bit;
    n->lo_bit  = lo_bit;
    n->_pad[0] = n->_pad[1] = 0;
    n->operand = operand;
    return ref;
}

ExprRef builder_expr_concat(SolveProblemBuilder *b, ExprRef hi,
                            ExprRef lo, uint8_t lo_width) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprConcat),
                                (uint32_t)_Alignof(ExprConcat));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprConcat *n = (ExprConcat *)_block_ptr_at(b->current, local);
    n->kind     = EXPR_CONCAT;
    n->lo_width = lo_width;
    n->_pad[0] = n->_pad[1] = n->_pad[2] = 0;
    n->hi       = hi;
    n->lo       = lo;
    return ref;
}

ExprRef builder_expr_array_select(SolveProblemBuilder *b, uint32_t base_var_id,
                                   uint32_t n_elems, ExprRef result, ExprRef index) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprArraySelect),
                                (uint32_t)_Alignof(ExprArraySelect));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprArraySelect *n = (ExprArraySelect *)_block_ptr_at(b->current, local);
    n->kind        = EXPR_ARRAY_SELECT;
    n->base_var_id = base_var_id;
    n->n_elems     = n_elems;
    n->result      = result;
    n->index       = index;
    return ref;
}

ExprRef builder_expr_sum(SolveProblemBuilder *b, ExprRef result,
                         uint32_t n_vars, const ExprRef *var_refs) {
    uint32_t total = (uint32_t)sizeof(ExprSum) + n_vars * (uint32_t)sizeof(ExprRef);
    ExprRef ref = builder_alloc(b, total, (uint32_t)_Alignof(ExprSum));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprSum *n = (ExprSum *)_block_ptr_at(b->current, local);
    n->kind   = EXPR_SUM;
    n->result = result;
    n->n_vars = n_vars;
    ExprRef *dst = (ExprRef *)((char *)n + sizeof(ExprSum));
    for (uint32_t i = 0; i < n_vars; i++)
        dst[i] = var_refs[i];
    return ref;
}

ExprRef builder_expr_countones(SolveProblemBuilder *b, ExprRef result,
                                ExprRef operand) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprCountones),
                                (uint32_t)_Alignof(ExprCountones));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprCountones *n = (ExprCountones *)_block_ptr_at(b->current, local);
    n->kind    = EXPR_COUNTONES;
    n->result  = result;
    n->operand = operand;
    return ref;
}

ExprRef builder_expr_clog2(SolveProblemBuilder *b, ExprRef result,
                            ExprRef operand) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ExprClog2),
                                (uint32_t)_Alignof(ExprClog2));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprClog2 *n = (ExprClog2 *)_block_ptr_at(b->current, local);
    n->kind    = EXPR_CLOG2;
    n->result  = result;
    n->operand = operand;
    return ref;
}

/* ------------------------------------------------------------------ */
/* Problem builders                                                    */
/* ------------------------------------------------------------------ */

ExprRef builder_add_var(SolveProblemBuilder *b, uint32_t var_id,
                        uint8_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(VarSpec),
                                (uint32_t)_Alignof(VarSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    VarSpec *v = (VarSpec *)_block_ptr_at(b->current, local);
    v->next       = b->vars_head;
    v->var_id     = var_id;
    v->width      = width;
    v->is_signed  = is_signed;
    v->_pad[0]    = v->_pad[1] = 0;
    v->lo         = lo;
    v->hi         = hi;
    b->vars_head  = ref;
    b->n_vars++;
    return ref;
}

ExprRef builder_add_constraint(SolveProblemBuilder *b, ExprRef root) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(ConstraintSpec),
                                (uint32_t)_Alignof(ConstraintSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ConstraintSpec *c = (ConstraintSpec *)_block_ptr_at(b->current, local);
    c->next              = b->constraints_head;
    c->root              = root;
    b->constraints_head  = ref;
    b->n_constraints++;
    return ref;
}

ExprRef builder_add_source(SolveProblemBuilder *b,
                           uint32_t n_vars, const uint32_t *var_ids) {
    uint32_t total = (uint32_t)sizeof(SourceSpec) +
                     n_vars * (uint32_t)sizeof(uint32_t);
    ExprRef ref = builder_alloc(b, total, (uint32_t)_Alignof(SourceSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    SourceSpec *s = (SourceSpec *)_block_ptr_at(b->current, local);
    s->next         = b->sources_head;
    s->n_vars       = n_vars;
    uint32_t *dst   = (uint32_t *)(s + 1);
    for (uint32_t i = 0; i < n_vars; i++)
        dst[i] = var_ids[i];
    b->sources_head = ref;
    b->n_sources++;
    return ref;
}

ExprRef builder_add_all_different(SolveProblemBuilder *b,
                                  uint32_t n_vars, const uint32_t *var_ids) {
    uint32_t total = (uint32_t)sizeof(AllDiffSpec) +
                     n_vars * (uint32_t)sizeof(uint32_t);
    ExprRef ref = builder_alloc(b, total, (uint32_t)_Alignof(AllDiffSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    AllDiffSpec *ad = (AllDiffSpec *)_block_ptr_at(b->current, local);
    ad->next         = b->allDiff_head;
    ad->n_vars       = n_vars;
    uint32_t *dst    = (uint32_t *)(ad + 1);
    for (uint32_t i = 0; i < n_vars; i++)
        dst[i] = var_ids[i];
    b->allDiff_head  = ref;
    b->n_alldiffs++;
    return ref;
}

ExprRef builder_add_soft_constraint(SolveProblemBuilder *b, ExprRef root,
                                    uint32_t priority) {
    ExprRef ref = builder_alloc(b, (uint32_t)sizeof(SoftSpec),
                                (uint32_t)_Alignof(SoftSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    SoftSpec *s = (SoftSpec *)_block_ptr_at(b->current, local);
    s->next       = b->softs_head;
    s->root       = root;
    s->priority   = priority;
    b->softs_head = ref;
    b->n_softs++;
    return ref;
}

ExprRef builder_add_dist(SolveProblemBuilder *b, uint32_t var_id,
                         uint32_t n_entries, const DistEntry *entries) {
    uint32_t total = (uint32_t)sizeof(DistSpec) +
                     n_entries * (uint32_t)sizeof(DistEntry);
    ExprRef ref = builder_alloc(b, total, (uint32_t)_Alignof(DistSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    DistSpec *ds = (DistSpec *)_block_ptr_at(b->current, local);
    ds->next      = b->dists_head;
    ds->var_id    = var_id;
    ds->n_entries = n_entries;
    DistEntry *dst = (DistEntry *)(ds + 1);
    for (uint32_t i = 0; i < n_entries; i++)
        dst[i] = entries[i];
    b->dists_head = ref;
    b->n_dists++;
    return ref;
}
