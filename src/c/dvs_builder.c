#include <stdlib.h>
#include <string.h>
#include "dvs_builder.h"

#define DEFAULT_BLOCK_SIZE  4096u
#define POOL_HEADER_SZ      ((uint32_t)sizeof(dvs_pool_t))

/* ------------------------------------------------------------------ */
/* Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static void *_alloc(dvs_builder_t *b, size_t sz) {
    if (b->alloc)
        return DVS_ALLOC(b->alloc, sz);
    return malloc(sz);
}

static void _release(dvs_builder_t *b, void *ptr, size_t sz) {
    if (b->alloc) {
        DVS_RELEASE(b->alloc, ptr, sz);
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
static BuilderBlock *_new_block(dvs_builder_t *b, uint32_t capacity) {
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
static void _free_blocks(dvs_builder_t *b, BuilderBlock *blk) {
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

dvs_builder_t *dvs_builder_create(uint32_t block_size, dvs_alloc_t *alloc) {
    if (block_size == 0) block_size = DEFAULT_BLOCK_SIZE;

    dvs_builder_t *b;
    if (alloc)
        b = (dvs_builder_t *)DVS_ALLOC(alloc, sizeof(*b));
    else
        b = (dvs_builder_t *)malloc(sizeof(*b));
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

void dvs_builder_reset(dvs_builder_t *b) {
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

void dvs_builder_destroy(dvs_builder_t *b) {
    if (!b) return;
    _free_blocks(b, b->first);
    _release(b, b, sizeof(*b));
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

dvs_expr_t dvs_builder_alloc(dvs_builder_t *b, uint32_t bytes, uint32_t align) {
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

uint32_t dvs_builder_virtual_used(const dvs_builder_t *b) {
    return b->virtual_used;
}

void *dvs_builder_ref_ptr(const dvs_builder_t *b, dvs_expr_t ref) {
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

dvs_problem_t *dvs_builder_finalize(dvs_builder_t *b, size_t *out_size) {
    uint32_t pool_data_size = b->virtual_used;
    size_t total = sizeof(dvs_problem_t) + pool_data_size;

    void *buf = _alloc(b, total);
    if (!buf) return NULL;
    memset(buf, 0, total);

    dvs_problem_t *sp = (dvs_problem_t *)buf;

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
    uint8_t *pool_base = (uint8_t *)&sp->pool + sizeof(dvs_pool_t);
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

dvs_problem_t *dvs_builder_finalize_reserve(dvs_builder_t *b, size_t *out_size,
                                       uint32_t extra_bytes) {
    uint32_t pool_data_size = b->virtual_used;
    size_t total = sizeof(dvs_problem_t) + pool_data_size + extra_bytes;

    void *buf = _alloc(b, total);
    if (!buf) return NULL;
    memset(buf, 0, total);

    dvs_problem_t *sp = (dvs_problem_t *)buf;
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

    /* Leave headroom: capacity > used, so the in-place expr_ / problem_add_var
     * API can append lemma nodes + new vars into the slack after finalize
     * (the lazy array refinement loop relies on this). */
    sp->pool.capacity = pool_data_size + extra_bytes;
    sp->pool.used     = pool_data_size;
    sp->pool.overflow = 0;
    sp->pool._pad     = 0;

    uint8_t *pool_base = (uint8_t *)&sp->pool + sizeof(dvs_pool_t);
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

dvs_builder_mark_t dvs_builder_mark(const dvs_builder_t *b) {
    dvs_builder_mark_t m;
    m.n_vars        = b->n_vars;
    m.n_constraints = b->n_constraints;
    m.n_sources     = b->n_sources;
    m.n_alldiffs    = b->n_alldiffs;
    m.n_softs       = b->n_softs;
    m.n_dists       = b->n_dists;
    return m;
}

/* Keep only the first `keep` nodes of a newest-first list inside the copied
 * problem `sp`, by ending the list after node `keep`. Every spec struct starts
 * with its `next` dvs_expr_t, so the cut is the same for all six lists. */
static int _cut_list(dvs_problem_t *sp, dvs_expr_t *head, uint32_t *count,
                     uint32_t keep) {
    if (keep > *count) return -1;
    *count = keep;
    if (keep == 0) { *head = EXPR_NULL; return 0; }
    dvs_expr_t cur = *head;
    for (uint32_t i = 1; i < keep; i++) {
        if (cur == EXPR_NULL) return -1;
        cur = *(dvs_expr_t *)POOL_PTR(sp, cur);
    }
    if (cur == EXPR_NULL) return -1;
    *(dvs_expr_t *)POOL_PTR(sp, cur) = EXPR_NULL;
    return 0;
}

dvs_problem_t *dvs_builder_finalize_since(dvs_builder_t *b,
                                     const dvs_builder_mark_t *m, size_t *out_size) {
    size_t sz = 0;
    dvs_problem_t *sp = dvs_builder_finalize(b, &sz);
    if (!sp) return NULL;
    /* Lists are newest-first, so the items added since the mark are exactly
     * the first (current - marked) nodes of each list. */
    if (m->n_vars > sp->n_vars || m->n_constraints > sp->n_constraints ||
        m->n_sources > sp->n_sources || m->n_alldiffs > sp->n_alldiffs ||
        m->n_softs > sp->n_softs || m->n_dists > sp->n_dists ||
        _cut_list(sp, &sp->vars_head, &sp->n_vars, sp->n_vars - m->n_vars) ||
        _cut_list(sp, &sp->constraints_head, &sp->n_constraints,
                  sp->n_constraints - m->n_constraints) ||
        _cut_list(sp, &sp->sources_head, &sp->n_sources,
                  sp->n_sources - m->n_sources) ||
        _cut_list(sp, &sp->allDiff_head, &sp->n_alldiffs,
                  sp->n_alldiffs - m->n_alldiffs) ||
        _cut_list(sp, &sp->softs_head, &sp->n_softs, sp->n_softs - m->n_softs) ||
        _cut_list(sp, &sp->dists_head, &sp->n_dists, sp->n_dists - m->n_dists)) {
        dvs_builder_free_problem(b, sp, sz);
        return NULL;
    }
    if (out_size) *out_size = sz;
    return sp;
}

void dvs_builder_free_problem(dvs_builder_t *b, dvs_problem_t *sp, size_t size) {
    if (!sp) return;
    _release(b, sp, size);
}

/* ------------------------------------------------------------------ */
/* Expression builders                                                 */
/* ------------------------------------------------------------------ */

dvs_expr_t dvs_builder_expr_const_sized(dvs_builder_t *b, int64_t value,
                                 uint8_t is_signed, uint8_t width) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprConst),
                                (uint32_t)_Alignof(ExprConst));
    if (ref == EXPR_NULL) return EXPR_NULL;

    /* Locate the allocation within the current block */
    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprConst *n = (ExprConst *)_block_ptr_at(b->current, local);
    n->kind      = EXPR_CONST;
    n->is_signed = is_signed;
    n->width     = width;
    n->_pad[0] = n->_pad[1] = 0;
    n->value     = value;
    return ref;
}

dvs_expr_t dvs_builder_expr_const(dvs_builder_t *b, int64_t value,
                           uint8_t is_signed) {
    return dvs_builder_expr_const_sized(b, value, is_signed, 0);
}

dvs_expr_t dvs_builder_expr_var(dvs_builder_t *b, uint32_t var_id) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprVar),
                                (uint32_t)_Alignof(ExprVar));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprVar *n = (ExprVar *)_block_ptr_at(b->current, local);
    n->kind   = EXPR_VAR;
    n->var_id = var_id;
    return ref;
}

dvs_expr_t dvs_builder_expr_binary(dvs_builder_t *b, dvs_binop_t op,
                            dvs_expr_t lhs, dvs_expr_t rhs) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprBinary),
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

dvs_expr_t dvs_builder_expr_unary(dvs_builder_t *b, dvs_unop_t op,
                           dvs_expr_t operand) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprUnary),
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

dvs_expr_t dvs_builder_expr_ite(dvs_builder_t *b,
                         dvs_expr_t cond, dvs_expr_t then_e, dvs_expr_t else_e) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprITE),
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

dvs_expr_t dvs_builder_expr_in_range(dvs_builder_t *b,
                              dvs_expr_t value, dvs_expr_t lo, dvs_expr_t hi) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprInRange),
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

dvs_expr_t dvs_builder_expr_in_set(dvs_builder_t *b, dvs_expr_t value,
                            uint32_t n_elems, const dvs_expr_t *elems) {
    uint32_t total = (uint32_t)sizeof(ExprInSet) +
                     n_elems * (uint32_t)sizeof(dvs_expr_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, (uint32_t)_Alignof(ExprInSet));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprInSet *n = (ExprInSet *)_block_ptr_at(b->current, local);
    n->kind    = EXPR_IN_SET;
    n->value   = value;
    n->n_elems = n_elems;
    dvs_expr_t *dst = (dvs_expr_t *)(n + 1);
    for (uint32_t i = 0; i < n_elems; i++)
        dst[i] = elems[i];
    return ref;
}

dvs_expr_t dvs_builder_expr_in_ranges(dvs_builder_t *b, dvs_expr_t value,
                               uint32_t n_ranges, const dvs_expr_t *los,
                               const dvs_expr_t *his) {
    uint32_t total = (uint32_t)sizeof(ExprInRanges) +
                     2u * n_ranges * (uint32_t)sizeof(dvs_expr_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, (uint32_t)_Alignof(ExprInRanges));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprInRanges *n = (ExprInRanges *)_block_ptr_at(b->current, local);
    n->kind     = EXPR_IN_RANGES;
    n->value    = value;
    n->n_ranges = n_ranges;
    dvs_expr_t *lo_dst = (dvs_expr_t *)(n + 1);
    dvs_expr_t *hi_dst = lo_dst + n_ranges;
    for (uint32_t i = 0; i < n_ranges; i++) {
        lo_dst[i] = los[i];
        hi_dst[i] = his[i];
    }
    return ref;
}

dvs_expr_t dvs_builder_expr_extend(dvs_builder_t *b, dvs_expr_t operand,
                            uint8_t from_bits, uint8_t to_bits,
                            uint8_t sign_extend) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprExtend),
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

dvs_expr_t dvs_builder_expr_sv_cast(dvs_builder_t *b, dvs_expr_t operand,
                             uint8_t from_bits, uint8_t to_bits,
                             uint8_t sign_extend, uint8_t dst_signed) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprSvCast),
                                (uint32_t)_Alignof(ExprSvCast));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprSvCast *n = (ExprSvCast *)_block_ptr_at(b->current, local);
    n->kind        = EXPR_SV_CAST;
    n->sign_extend = sign_extend;
    n->from_bits   = from_bits;
    n->to_bits     = to_bits;
    n->dst_signed  = dst_signed;
    n->operand     = operand;
    return ref;
}

dvs_expr_t dvs_builder_expr_extract(dvs_builder_t *b, dvs_expr_t operand,
                             uint8_t hi_bit, uint8_t lo_bit) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprExtract),
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

dvs_expr_t dvs_builder_expr_concat(dvs_builder_t *b, dvs_expr_t hi,
                            dvs_expr_t lo, uint8_t lo_width) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprConcat),
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

dvs_expr_t dvs_builder_expr_array_select(dvs_builder_t *b, uint32_t base_var_id,
                                   uint32_t n_elems, dvs_expr_t result, dvs_expr_t index) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprArraySelect),
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

dvs_expr_t dvs_builder_expr_sum(dvs_builder_t *b, dvs_expr_t result,
                         uint32_t n_vars, const dvs_expr_t *var_refs) {
    uint32_t total = (uint32_t)sizeof(ExprSum) + n_vars * (uint32_t)sizeof(dvs_expr_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, (uint32_t)_Alignof(ExprSum));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprSum *n = (ExprSum *)_block_ptr_at(b->current, local);
    n->kind   = EXPR_SUM;
    n->result = result;
    n->n_vars = n_vars;
    dvs_expr_t *dst = (dvs_expr_t *)((char *)n + sizeof(ExprSum));
    for (uint32_t i = 0; i < n_vars; i++)
        dst[i] = var_refs[i];
    return ref;
}

dvs_expr_t dvs_builder_expr_countones(dvs_builder_t *b, dvs_expr_t result,
                                dvs_expr_t operand) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprCountones),
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

dvs_expr_t dvs_builder_expr_clog2(dvs_builder_t *b, dvs_expr_t result,
                            dvs_expr_t operand) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprClog2),
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

dvs_expr_t dvs_builder_add_var(dvs_builder_t *b, uint32_t var_id,
                        uint8_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(VarSpec),
                                (uint32_t)_Alignof(VarSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    VarSpec *v = (VarSpec *)_block_ptr_at(b->current, local);
    v->next       = b->vars_head;
    v->var_id     = var_id;
    v->width      = width;
    v->is_signed  = is_signed;
    v->is_aux     = 0;
    v->_pad       = 0;
    v->lo         = lo;
    v->hi         = hi;
    b->vars_head  = ref;
    b->n_vars++;
    return ref;
}

void dvs_builder_mark_var_aux(dvs_builder_t *b, dvs_expr_t var_ref) {
    if (var_ref == EXPR_NULL) return;
    VarSpec *v = (VarSpec *)dvs_builder_ref_ptr(b, var_ref);
    if (v) v->is_aux = 1;
}

dvs_expr_t dvs_builder_add_constraint(dvs_builder_t *b, dvs_expr_t root) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ConstraintSpec),
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

dvs_expr_t dvs_builder_add_source(dvs_builder_t *b,
                           uint32_t n_vars, const uint32_t *var_ids) {
    uint32_t total = (uint32_t)sizeof(SourceSpec) +
                     n_vars * (uint32_t)sizeof(uint32_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, (uint32_t)_Alignof(SourceSpec));
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

dvs_expr_t dvs_builder_add_all_different(dvs_builder_t *b,
                                  uint32_t n_vars, const uint32_t *var_ids) {
    uint32_t total = (uint32_t)sizeof(AllDiffSpec) +
                     n_vars * (uint32_t)sizeof(uint32_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, (uint32_t)_Alignof(AllDiffSpec));
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

dvs_expr_t dvs_builder_add_soft_constraint(dvs_builder_t *b, dvs_expr_t root,
                                    uint32_t priority) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(SoftSpec),
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

dvs_expr_t dvs_builder_add_dist(dvs_builder_t *b, uint32_t var_id,
                         uint32_t n_entries, const dvs_dist_entry_t *entries) {
    uint32_t total = (uint32_t)sizeof(DistSpec) +
                     n_entries * (uint32_t)sizeof(dvs_dist_entry_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, (uint32_t)_Alignof(DistSpec));
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    DistSpec *ds = (DistSpec *)_block_ptr_at(b->current, local);
    ds->next      = b->dists_head;
    ds->var_id    = var_id;
    ds->n_entries = n_entries;
    dvs_dist_entry_t *dst = (dvs_dist_entry_t *)(ds + 1);
    for (uint32_t i = 0; i < n_entries; i++)
        dst[i] = entries[i];
    b->dists_head = ref;
    b->n_dists++;
    return ref;
}
