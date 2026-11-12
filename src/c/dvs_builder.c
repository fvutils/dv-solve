#include <assert.h>
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

/** Append blk to the block index (blocks are added in offset order). */
static int _index_block(dvs_builder_t *b, BuilderBlock *blk) {
    if (b->n_blocks == b->cap_blocks) {
        uint32_t cap = b->cap_blocks ? 2 * b->cap_blocks : 16;
        BuilderBlock **nb = (BuilderBlock **)realloc(b->blocks, cap * sizeof(*nb));
        if (!nb) return -1;
        b->blocks = nb;
        b->cap_blocks = cap;
    }
    b->blocks[b->n_blocks++] = blk;
    return 0;
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
    b->arrays_head      = EXPR_NULL;
    b->exports_head     = EXPR_NULL;
    b->imports_head     = EXPR_NULL;
    b->foreachs_head    = EXPR_NULL;
    b->foreach_open     = EXPR_NULL;

    /* Allocate the first block */
    b->first = _new_block(b, block_size);
    if (!b->first || _index_block(b, b->first)) {
        if (b->first) _free_blocks(b, b->first);
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
    b->n_blocks         = b->first ? 1 : 0;
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
    b->n_arrays         = 0;
    b->arrays_head      = EXPR_NULL;
    b->n_slots          = 0;
    b->n_exports        = 0;
    b->exports_head     = EXPR_NULL;
    b->n_imports        = 0;
    b->imports_head     = EXPR_NULL;
    b->n_foreachs       = 0;
    b->foreachs_head    = EXPR_NULL;
    b->foreach_open     = EXPR_NULL;
    b->levels_bound     = 0;
}

void dvs_builder_destroy(dvs_builder_t *b) {
    if (!b) return;
    _free_blocks(b, b->first);
    free(b->blocks);
    _release(b, b, sizeof(*b));
}

/* ------------------------------------------------------------------ */
/* Allocation                                                          */
/* ------------------------------------------------------------------ */

dvs_expr_t dvs_builder_alloc(dvs_builder_t *b, uint32_t bytes, uint32_t align) {
    if (align < 1) align = 1;

    /* A reference is a 32-bit pool offset (EXPR_NULL is its top value). Refuse
     * an allocation past that rather than let the offset wrap: blocks would
     * then overlap, and finalize copies past the problem it sized. */
    uint64_t end = ((uint64_t)b->virtual_used + align - 1) / align * align + bytes;
    if (end + POOL_HEADER_SZ >= (uint64_t)EXPR_NULL) return EXPR_NULL;

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
        if (_index_block(b, blk)) {
            _free_blocks(b, blk);
            return EXPR_NULL;
        }

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
    /* Last block whose base_offset <= voff. */
    uint32_t lo = 0, hi = b->n_blocks;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (b->blocks[mid]->base_offset <= voff) lo = mid;
        else hi = mid;
    }
    if (lo < b->n_blocks) {
        BuilderBlock *blk = b->blocks[lo];
        if (voff >= blk->base_offset && voff < blk->base_offset + blk->used)
            return _block_ptr_at(blk, voff - blk->base_offset);
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
    sp->magic            = DVS_PROBLEM_MAGIC;
    sp->format_version   = DVS_PROBLEM_FORMAT_VERSION;
    sp->flags            = b->flags;
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
    sp->n_arrays         = b->n_arrays;
    sp->arrays_head      = b->arrays_head;
    sp->n_foreachs       = b->n_foreachs;
    sp->foreachs_head    = b->foreachs_head;
    sp->n_slots          = b->n_slots;
    sp->n_exports        = b->n_exports;
    sp->exports_head     = b->exports_head;
    sp->n_imports        = b->n_imports;
    sp->imports_head     = b->imports_head;

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

/* Copy pool bytes [from, to) from the blocks into the problem's pool region,
 * zeroing what no block covers. */
static void _copy_range(dvs_builder_t *b, dvs_problem_t *sp, uint32_t from, uint32_t to) {
    uint8_t *pool_base = (uint8_t *)&sp->pool + sizeof(dvs_pool_t);
    memset(pool_base + from, 0, to - from);
    /* The last block starting at or below `from` (blocks are in offset order). */
    uint32_t lo = 0, hi = b->n_blocks;
    while (hi - lo > 1) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (b->blocks[mid]->base_offset <= from) lo = mid; else hi = mid;
    }
    for (uint32_t i = lo; i < b->n_blocks; i++) {
        BuilderBlock *blk = b->blocks[i];
        if (blk->base_offset >= to) break;
        uint32_t s = blk->base_offset > from ? blk->base_offset : from;
        uint32_t e = blk->base_offset + blk->used;
        if (e > to) e = to;
        if (e > s)
            memcpy(pool_base + s, BUILDER_BLOCK_DATA(blk) + (s - blk->base_offset), e - s);
    }
}

dvs_problem_t *dvs_builder_finalize_append(dvs_builder_t *b, dvs_problem_t *prev,
                                           size_t *size) {
    uint32_t used = b->virtual_used;
    uint32_t old = prev ? prev->pool.used : 0;
    if (!prev || b->alloc || old > used || !b->n_blocks) {
        /* First time (or a custom allocator): a full copy with headroom. */
        uint32_t extra = used / 2 > 4096 ? used / 2 : 4096;
        if (b->alloc || !b->n_blocks) return dvs_builder_finalize(b, size);
        dvs_problem_t *sp = dvs_builder_finalize_reserve(b, size, extra);
        if (sp) {
            sp->pool.used = used;
            if (size) *size = sizeof(dvs_problem_t) + sp->pool.capacity;
        }
        return sp;
    }
    dvs_problem_t *sp = prev;
    if (used > prev->pool.capacity) {
        /* Growing moves the problem (a context reads one only during
         * compile and add). */
        uint32_t cap = used + (used / 2 > 4096 ? used / 2 : 4096);
        sp = (dvs_problem_t *)realloc(prev, sizeof(dvs_problem_t) + cap);
        if (!sp) return NULL;                 /* prev is still valid */
        sp->pool.capacity = cap;
    }
    _copy_range(b, sp, old, used);
    sp->pool.used        = used;
    sp->flags           |= b->flags;
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
    sp->n_arrays         = b->n_arrays;
    sp->arrays_head      = b->arrays_head;
    sp->n_foreachs       = b->n_foreachs;
    sp->foreachs_head    = b->foreachs_head;
    sp->n_slots          = b->n_slots;
    sp->n_exports        = b->n_exports;
    sp->exports_head     = b->exports_head;
    sp->n_imports        = b->n_imports;
    sp->imports_head     = b->imports_head;
    if (size) *size = sizeof(dvs_problem_t) + sp->pool.capacity;
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
    sp->magic            = DVS_PROBLEM_MAGIC;
    sp->format_version   = DVS_PROBLEM_FORMAT_VERSION;
    sp->flags            = b->flags;
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
    sp->n_arrays         = b->n_arrays;
    sp->arrays_head      = b->arrays_head;
    sp->n_foreachs       = b->n_foreachs;
    sp->foreachs_head    = b->foreachs_head;
    sp->n_slots          = b->n_slots;
    sp->n_exports        = b->n_exports;
    sp->exports_head     = b->exports_head;
    sp->n_imports        = b->n_imports;
    sp->imports_head     = b->imports_head;

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

/* The six item lists of a problem, as (head, count) pairs. */
static void _lists(dvs_problem_t *sp, dvs_expr_t *head[6], uint32_t *count[6]) {
    head[0] = &sp->vars_head;        count[0] = &sp->n_vars;
    head[1] = &sp->constraints_head; count[1] = &sp->n_constraints;
    head[2] = &sp->sources_head;     count[2] = &sp->n_sources;
    head[3] = &sp->allDiff_head;     count[3] = &sp->n_alldiffs;
    head[4] = &sp->softs_head;       count[4] = &sp->n_softs;
    head[5] = &sp->dists_head;       count[5] = &sp->n_dists;
}

int dvs_problem_cut_since(dvs_problem_t *sp, const dvs_builder_mark_t *m,
                          dvs_problem_cut_t *save) {
    dvs_expr_t *head[6];
    uint32_t *count[6];
    _lists(sp, head, count);
    const uint32_t at[6] = { m->n_vars, m->n_constraints, m->n_sources,
                             m->n_alldiffs, m->n_softs, m->n_dists };
    for (int i = 0; i < 6; i++)
        if (at[i] > *count[i]) return -1;
    for (int i = 0; i < 6; i++) {
        save->head[i] = *head[i];
        save->count[i] = *count[i];
        save->link_at[i] = EXPR_NULL;
        save->link_was[i] = EXPR_NULL;
        uint32_t keep = *count[i] - at[i];
        *count[i] = keep;
        if (keep == 0) { *head[i] = EXPR_NULL; continue; }
        /* Lists are newest-first: the kept items are the first `keep`. Every
         * spec type starts with its `next` link. */
        dvs_expr_t cur = *head[i];
        for (uint32_t k = 1; k < keep; k++)
            cur = *(dvs_expr_t *)POOL_PTR(sp, cur);
        dvs_expr_t *link = (dvs_expr_t *)POOL_PTR(sp, cur);
        save->link_at[i] = cur;
        save->link_was[i] = *link;
        *link = EXPR_NULL;
    }
    return 0;
}

void dvs_problem_uncut(dvs_problem_t *sp, const dvs_problem_cut_t *save) {
    dvs_expr_t *head[6];
    uint32_t *count[6];
    _lists(sp, head, count);
    for (int i = 0; i < 6; i++) {
        if (save->link_at[i] != EXPR_NULL)
            *(dvs_expr_t *)POOL_PTR(sp, save->link_at[i]) = save->link_was[i];
        *head[i] = save->head[i];
        *count[i] = save->count[i];
    }
}

/* Advance a newest-first list head past its items beyond `keep`. */
static int _drop_newest(dvs_builder_t *b, dvs_expr_t *head, uint32_t *count,
                        uint32_t keep) {
    if (keep > *count) return -1;
    while (*count > keep) {
        if (*head == EXPR_NULL) return -1;
        *head = *(const dvs_expr_t *)dvs_builder_ref_ptr(b, *head);
        (*count)--;
    }
    return 0;
}

int dvs_builder_rewind(dvs_builder_t *b, const dvs_builder_mark_t *m) {
    if (m->n_vars > b->n_vars || m->n_constraints > b->n_constraints ||
        m->n_sources > b->n_sources || m->n_alldiffs > b->n_alldiffs ||
        m->n_softs > b->n_softs || m->n_dists > b->n_dists)
        return -1;
    return (_drop_newest(b, &b->vars_head, &b->n_vars, m->n_vars) ||
            _drop_newest(b, &b->constraints_head, &b->n_constraints, m->n_constraints) ||
            _drop_newest(b, &b->sources_head, &b->n_sources, m->n_sources) ||
            _drop_newest(b, &b->allDiff_head, &b->n_alldiffs, m->n_alldiffs) ||
            _drop_newest(b, &b->softs_head, &b->n_softs, m->n_softs) ||
            _drop_newest(b, &b->dists_head, &b->n_dists, m->n_dists)) ? -1 : 0;
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

void dvs_builder_set_explicit(dvs_builder_t *b) {
    b->flags |= DVS_PROBLEM_F_EXPLICIT;
}

uint32_t dvs_problem_format_version(void) {
    return DVS_PROBLEM_FORMAT_VERSION;
}

/* ------------------------------------------------------------------ */
/* Expression builders                                                 */
/* ------------------------------------------------------------------ */

dvs_expr_t dvs_builder_expr_const_sized(dvs_builder_t *b, int64_t value,
                                 uint8_t is_signed, uint32_t width) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprConst),
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    /* Locate the allocation within the current block */
    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprConst *n = (ExprConst *)_block_ptr_at(b->current, local);
    n->kind      = EXPR_CONST;
    n->is_signed = is_signed;
    n->width     = width;
    n->_p8       = 0;
    n->value     = dvs_const_wrap(value, width, is_signed);
    return ref;
}

dvs_expr_t dvs_builder_expr_const_wide(dvs_builder_t *b, uint32_t n_limbs,
                                       const uint64_t *limbs, uint32_t width,
                                       uint8_t is_signed) {
    if (width == 0 || width > DVS_MAX_WIDTH || !limbs
        || n_limbs != (width + 63u) / 64u)
        return EXPR_NULL;
    if (width <= 64)
        return dvs_builder_expr_const_sized(b, (int64_t)limbs[0],
                                            is_signed ? 1 : 0, (uint32_t)width);
    dvs_expr_t ref = dvs_builder_alloc(b,
        (uint32_t)(sizeof(ExprConstWide) + n_limbs * sizeof(uint64_t)),
        DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprConstWide *n = (ExprConstWide *)_block_ptr_at(b->current, local);
    dvs_const_wide_init(n, n_limbs, limbs, width, is_signed);
    return ref;
}

dvs_expr_t dvs_builder_expr_const(dvs_builder_t *b, int64_t value,
                           uint8_t is_signed) {
    return dvs_builder_expr_const_sized(b, value, is_signed, 0);
}

dvs_expr_t dvs_builder_expr_var(dvs_builder_t *b, uint32_t var_id) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprVar),
                                DVS_IMAGE_ALIGN);
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
                                DVS_IMAGE_ALIGN);
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
                                DVS_IMAGE_ALIGN);
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
                                DVS_IMAGE_ALIGN);
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
                                DVS_IMAGE_ALIGN);
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
    dvs_expr_t ref = dvs_builder_alloc(b, total, DVS_IMAGE_ALIGN);
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
    dvs_expr_t ref = dvs_builder_alloc(b, total, DVS_IMAGE_ALIGN);
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
                            uint32_t from_bits, uint32_t to_bits,
                            uint8_t sign_extend) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprExtend),
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprExtend *n = (ExprExtend *)_block_ptr_at(b->current, local);
    n->kind        = EXPR_EXTEND;
    n->sign_extend = sign_extend;
    n->from_bits   = from_bits;
    n->to_bits     = to_bits;
    n->_p8 = 0;
    n->operand     = operand;
    return ref;
}

dvs_expr_t dvs_builder_expr_cast(dvs_builder_t *b, dvs_expr_t operand,
                                 uint32_t to_bits, uint8_t to_signed) {
    if (to_bits == 0) return EXPR_NULL;
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprCast),
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprCast *n = (ExprCast *)_block_ptr_at(b->current, local);
    n->kind      = EXPR_CAST;
    n->to_bits   = to_bits;
    n->to_signed = to_signed ? 1 : 0;
    n->_p8 = 0;
    n->operand   = operand;
    return ref;
}

dvs_expr_t dvs_builder_expr_sv_cast(dvs_builder_t *b, dvs_expr_t operand,
                             uint32_t from_bits, uint32_t to_bits,
                             uint8_t sign_extend, uint8_t dst_signed) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprSvCast),
                                DVS_IMAGE_ALIGN);
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
                             uint32_t hi_bit, uint32_t lo_bit) {
    /* A slice of a sized constant is a constant: Verilator reads a signed
     * literal as unsigned with a full-width extract, and the compile and the
     * samplers match only bare constants. */
    const ExprConst *c = (const ExprConst *)dvs_builder_ref_ptr(b, operand);
    if (c && c->kind == EXPR_CONST && lo_bit <= hi_bit && hi_bit < c->width)
        return dvs_builder_expr_const_sized(b, (int64_t)((uint64_t)c->value >> lo_bit), 0,
                                            hi_bit - lo_bit + 1);

    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprExtract),
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprExtract *n = (ExprExtract *)_block_ptr_at(b->current, local);
    n->kind    = EXPR_EXTRACT;
    n->hi_bit  = hi_bit;
    n->lo_bit  = lo_bit;
    n->operand = operand;
    return ref;
}

dvs_expr_t dvs_builder_expr_concat(dvs_builder_t *b, dvs_expr_t hi,
                            dvs_expr_t lo, uint32_t lo_width) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprConcat),
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ExprConcat *n = (ExprConcat *)_block_ptr_at(b->current, local);
    n->kind     = EXPR_CONCAT;
    n->lo_width = lo_width;
   
    n->hi       = hi;
    n->lo       = lo;
    return ref;
}

dvs_expr_t dvs_builder_expr_array_select(dvs_builder_t *b, uint32_t base_var_id,
                                   uint32_t n_elems, dvs_expr_t result, dvs_expr_t index) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprArraySelect),
                                DVS_IMAGE_ALIGN);
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
    dvs_expr_t ref = dvs_builder_alloc(b, total, DVS_IMAGE_ALIGN);
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
                                DVS_IMAGE_ALIGN);
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
                                DVS_IMAGE_ALIGN);
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
                        uint32_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi) {
    if (width == 0 || width > DVS_MAX_WIDTH) return EXPR_NULL;
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(VarSpec),
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    VarSpec *v = (VarSpec *)_block_ptr_at(b->current, local);
    v->next       = b->vars_head;
    v->var_id     = var_id;
    v->width      = width;
    v->is_signed  = is_signed;
    v->kind       = DVS_VAR_RAND;
    v->order      = 0;
    v->lo         = lo;
    v->hi         = hi;
    b->vars_head  = ref;
    b->n_vars++;
    return ref;
}

/* The VarSpec of `var_id`, or NULL if it is not declared. */
static VarSpec *_find_var(dvs_builder_t *b, uint32_t var_id) {
    for (dvs_expr_t r = b->vars_head; r != EXPR_NULL; ) {
        VarSpec *v = (VarSpec *)dvs_builder_ref_ptr(b, r);
        if (!v || v->var_id == var_id) return v;
        r = v->next;
    }
    return NULL;
}

int dvs_builder_set_var_order(dvs_builder_t *b, uint32_t var_id,
                              uint8_t stage) {
    VarSpec *v = _find_var(b, var_id);
    if (!v) return -1;
    v->order = stage;
    return 0;
}

int dvs_builder_set_var_kind(dvs_builder_t *b, uint32_t var_id,
                             dvs_var_kind_t kind) {
    VarSpec *v = _find_var(b, var_id);
    if ((unsigned)kind > DVS_VAR_STATE || !v) return -1;
    v->kind = (uint8_t)kind;
    return 0;
}

/* ------------------------------------------------------------------ */
/* Parts (dvs_randomize_parts)                                         */
/* ------------------------------------------------------------------ */

void dvs_builder_set_n_slots(dvs_builder_t *b, uint32_t n_slots) {
    b->n_slots = n_slots;
}

int dvs_builder_add_export(dvs_builder_t *b, uint32_t var_id) {
    VarSpec *v = _find_var(b, var_id);
    if (!v || v->kind == DVS_VAR_IMPORT) return -1;
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExportSpec), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return -1;
    ExportSpec *e = (ExportSpec *)dvs_builder_ref_ptr(b, ref);
    e->next = b->exports_head;
    e->var_id = var_id;
    b->exports_head = ref;
    return (int)b->n_exports++;
}

int dvs_builder_add_import(dvs_builder_t *b, uint32_t var_id, uint32_t n_path,
                           const uint32_t *path, uint32_t export_index) {
    VarSpec *v = _find_var(b, var_id);
    if (!v || v->kind == DVS_VAR_IMPORT || n_path == 0) return -1;
    dvs_expr_t ref = dvs_builder_alloc(
        b, (uint32_t)(sizeof(ImportSpec) + n_path * sizeof(uint32_t)), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return -1;
    v->kind = DVS_VAR_IMPORT;
    ImportSpec *im = (ImportSpec *)dvs_builder_ref_ptr(b, ref);
    im->next = b->imports_head;
    im->var_id = var_id;
    im->export_index = export_index;
    im->n_path = n_path;
    memcpy(im + 1, path, n_path * sizeof(uint32_t));
    b->imports_head = ref;
    b->n_imports++;
    return 0;
}

void dvs_builder_mark_var_aux(dvs_builder_t *b, dvs_expr_t var_ref) {
    if (var_ref == EXPR_NULL) return;
    VarSpec *v = (VarSpec *)dvs_builder_ref_ptr(b, var_ref);
    if (v) v->kind = DVS_VAR_AUX;
}

dvs_expr_t dvs_builder_add_constraint(dvs_builder_t *b, dvs_expr_t root) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ConstraintSpec),
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    ConstraintSpec *c = (ConstraintSpec *)_block_ptr_at(b->current, local);
    c->root              = root;
    if (b->foreach_open != EXPR_NULL) {
        ForeachSpec *fe = (ForeachSpec *)dvs_builder_ref_ptr(b, b->foreach_open);
        c->next = fe->constraints_head;
        fe->constraints_head = ref;
        fe->n_constraints++;
        return ref;
    }
    c->next              = b->constraints_head;
    b->constraints_head  = ref;
    b->n_constraints++;
    return ref;
}

dvs_expr_t dvs_builder_add_source(dvs_builder_t *b,
                           uint32_t n_vars, const uint32_t *var_ids) {
    uint32_t total = (uint32_t)sizeof(SourceSpec) +
                     n_vars * (uint32_t)sizeof(uint32_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, DVS_IMAGE_ALIGN);
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
    if (b->foreach_open != EXPR_NULL) return EXPR_NULL;
    uint32_t total = (uint32_t)sizeof(AllDiffSpec) +
                     n_vars * (uint32_t)sizeof(uint32_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, DVS_IMAGE_ALIGN);
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
                                DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    SoftSpec *s = (SoftSpec *)_block_ptr_at(b->current, local);
    s->root       = root;
    s->priority   = priority;
    if (b->foreach_open != EXPR_NULL) {
        ForeachSpec *fe = (ForeachSpec *)dvs_builder_ref_ptr(b, b->foreach_open);
        s->next = fe->softs_head;
        fe->softs_head = ref;
        fe->n_softs++;
        return ref;
    }
    s->next       = b->softs_head;
    b->softs_head = ref;
    b->n_softs++;
    return ref;
}

static dvs_expr_t _add_dist(dvs_builder_t *b, uint32_t var_id, dvs_expr_t index,
                            uint32_t n_entries, const dvs_dist_entry_t *entries) {
    uint32_t total = (uint32_t)sizeof(DistSpec) +
                     n_entries * (uint32_t)sizeof(dvs_dist_entry_t);
    dvs_expr_t ref = dvs_builder_alloc(b, total, DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;

    uint32_t voff = ref - POOL_HEADER_SZ;
    uint32_t local = voff - b->current->base_offset;
    DistSpec *ds = (DistSpec *)_block_ptr_at(b->current, local);
    ds->var_id    = var_id;
    ds->n_entries = n_entries;
    ds->index     = index;
    dvs_dist_entry_t *dst = (dvs_dist_entry_t *)(ds + 1);
    for (uint32_t i = 0; i < n_entries; i++)
        dst[i] = entries[i];
    if (index != EXPR_NULL) {
        ForeachSpec *fe = (ForeachSpec *)dvs_builder_ref_ptr(b, b->foreach_open);
        ds->next = fe->dists_head;
        fe->dists_head = ref;
        fe->n_dists++;
        return ref;
    }
    ds->next      = b->dists_head;
    b->dists_head = ref;
    b->n_dists++;
    return ref;
}

dvs_expr_t dvs_builder_add_dist(dvs_builder_t *b, uint32_t var_id,
                         uint32_t n_entries, const dvs_dist_entry_t *entries) {
    if (b->foreach_open != EXPR_NULL) return EXPR_NULL;
    return _add_dist(b, var_id, EXPR_NULL, n_entries, entries);
}

dvs_expr_t dvs_builder_add_dist_elem(dvs_builder_t *b, uint32_t array_id,
                                     dvs_expr_t index, uint32_t n_entries,
                                     const dvs_dist_entry_t *entries) {
    if (b->foreach_open == EXPR_NULL || index == EXPR_NULL) return EXPR_NULL;
    return _add_dist(b, array_id, index, n_entries, entries);
}

/* ------------------------------------------------------------------ */
/* Arrays                                                              */
/* ------------------------------------------------------------------ */

int dvs_builder_add_array(dvs_builder_t *b, uint32_t array_id, uint32_t elem_width,
                          uint8_t elem_signed, dvs_var_kind_t kind) {
    if (elem_width == 0 || elem_width > 64
            || (kind != DVS_VAR_RAND && kind != DVS_VAR_STATE))
        return -1;
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ArraySpec), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return -1;
    ArraySpec *a = (ArraySpec *)dvs_builder_ref_ptr(b, ref);
    a->next        = b->arrays_head;
    a->array_id    = array_id;
    a->elem_width  = elem_width;
    a->elem_signed = elem_signed ? 1 : 0;
    a->kind        = (uint8_t)kind;
    a->slot        = DVS_NO_SLOT;
    b->arrays_head = ref;
    b->n_arrays++;
    return 0;
}

int dvs_builder_add_slot_array(dvs_builder_t *b, uint32_t array_id, uint32_t slot) {
    if (dvs_builder_add_array(b, array_id, 1, 0, DVS_VAR_STATE)) return -1;
    ((ArraySpec *)dvs_builder_ref_ptr(b, b->arrays_head))->slot = slot;
    return 0;
}

int dvs_builder_set_array_order(dvs_builder_t *b, uint32_t array_id, uint8_t stage) {
    for (dvs_expr_t r = b->arrays_head; r != EXPR_NULL;) {
        ArraySpec *a = (ArraySpec *)dvs_builder_ref_ptr(b, r);
        if (a->array_id == array_id) { a->order = stage; return 0; }
        r = a->next;
    }
    return -1;
}

int dvs_builder_set_array_size_random(dvs_builder_t *b, uint32_t array_id) {
    for (dvs_expr_t r = b->arrays_head; r != EXPR_NULL;) {
        ArraySpec *a = (ArraySpec *)dvs_builder_ref_ptr(b, r);
        if (a->array_id == array_id) {
            if (a->kind != DVS_VAR_RAND && a->slot == DVS_NO_SLOT) return -1;
            a->size_random = 1;
            return 0;
        }
        r = a->next;
    }
    return -1;
}

dvs_expr_t dvs_builder_expr_array_size(dvs_builder_t *b, uint32_t array_id) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprArraySize), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprArraySize *n = (ExprArraySize *)dvs_builder_ref_ptr(b, ref);
    n->kind     = EXPR_ARRAY_SIZE;
    n->array_id = array_id;
    return ref;
}

dvs_expr_t dvs_builder_expr_loop_index(dvs_builder_t *b, uint32_t level) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprLoopIndex), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprLoopIndex *n = (ExprLoopIndex *)dvs_builder_ref_ptr(b, ref);
    n->kind  = EXPR_LOOP_INDEX;
    n->level = level;
    return ref;
}

dvs_expr_t dvs_builder_expr_elem(dvs_builder_t *b, uint32_t array_id, dvs_expr_t index) {
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprElem), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprElem *n = (ExprElem *)dvs_builder_ref_ptr(b, ref);
    n->kind     = EXPR_ELEM;
    n->array_id = array_id;
    n->index    = index;
    return ref;
}

dvs_expr_t dvs_builder_expr_reduce(dvs_builder_t *b, dvs_binop_t op, uint32_t array_id,
                                   uint32_t level, dvs_expr_t body) {
    if (op != DVS_BIN_ADD && op != DVS_BIN_MUL && op != DVS_BIN_BAND && op != DVS_BIN_BOR
            && op != DVS_BIN_BXOR)
        return EXPR_NULL;
    if (level >= DVS_MAX_LOOP_LEVELS) return EXPR_NULL;
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ExprReduce), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return EXPR_NULL;
    ExprReduce *n = (ExprReduce *)dvs_builder_ref_ptr(b, ref);
    n->kind     = EXPR_REDUCE;
    n->op       = (uint32_t)op;
    n->array_id = array_id;
    n->level    = level;
    n->body     = body;
    return ref;
}

int dvs_builder_foreach_begin(dvs_builder_t *b, uint32_t array_id, uint32_t level) {
    if (level >= DVS_MAX_LOOP_LEVELS || (b->levels_bound >> level) & 1u) return -1;
    dvs_expr_t ref = dvs_builder_alloc(b, (uint32_t)sizeof(ForeachSpec), DVS_IMAGE_ALIGN);
    if (ref == EXPR_NULL) return -1;
    ForeachSpec *fe = (ForeachSpec *)dvs_builder_ref_ptr(b, ref);
    fe->parent           = b->foreach_open;
    fe->array_id         = array_id;
    fe->level            = level;
    fe->constraints_head = EXPR_NULL;
    fe->softs_head       = EXPR_NULL;
    fe->dists_head       = EXPR_NULL;
    b->foreach_open = ref;
    b->levels_bound |= 1u << level;
    return 0;
}

int dvs_builder_foreach_end(dvs_builder_t *b) {
    if (b->foreach_open == EXPR_NULL) return -1;
    dvs_expr_t ref = b->foreach_open;
    ForeachSpec *fe = (ForeachSpec *)dvs_builder_ref_ptr(b, ref);
    b->foreach_open = fe->parent;
    b->levels_bound &= ~(1u << fe->level);
    fe->next = b->foreachs_head;
    b->foreachs_head = ref;
    b->n_foreachs++;
    return 0;
}

uint32_t dvs_max_width(void) { return DVS_MAX_WIDTH; }
