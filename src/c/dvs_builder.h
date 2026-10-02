#ifndef DVS_BUILDER_H
#define DVS_BUILDER_H

#include <stddef.h>
#include <stdint.h>
#include "dvs_alloc.h"
#include "dvs_pool.h"     /* dvs_expr_t, EXPR_NULL */
#include "dvs_problem.h"  /* ExprKind, dvs_binop_t, dvs_unop_t, dvs_problem_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* BuilderBlock -- one segment of the virtual linear address space     */
/* ------------------------------------------------------------------ */
typedef struct BuilderBlock {
    struct BuilderBlock *next;        /* next block (or NULL)           */
    uint32_t             base_offset; /* virtual offset of data start   */
    uint32_t             capacity;    /* size of data[]                 */
    uint32_t             used;        /* bytes consumed in data[]       */
    uint32_t             _pad;
    /* uint8_t data[] follows (flexible array member) */
} BuilderBlock;

/** Access the data region of a block. */
#define BUILDER_BLOCK_DATA(blk) ((uint8_t *)((blk) + 1))

/* ------------------------------------------------------------------ */
/* dvs_builder_t                                                 */
/*                                                                     */
/* Growable problem builder that produces an exact-sized, contiguous   */
/* dvs_problem_t buffer on finalize().  dvs_expr_t values match exactly    */
/* what the fixed-buffer dvs_problem_t API would produce for the same   */
/* allocation sequence: sizeof(dvs_pool_t) + virtual_offset.           */
/* ------------------------------------------------------------------ */
struct dvs_builder_s {
    BuilderBlock *first;            /* head of block list              */
    BuilderBlock *current;          /* tail (active block)             */
    uint32_t      virtual_used;     /* running total logical offset    */
    uint32_t      block_size;       /* capacity for new blocks         */
    dvs_alloc_t  *alloc;            /* backing allocator (NULL=malloc) */

    /* Problem metadata (mirrors dvs_problem_t header) */
    uint32_t      n_vars;
    uint32_t      n_constraints;
    uint32_t      n_sources;
    dvs_expr_t       vars_head;
    dvs_expr_t       constraints_head;
    dvs_expr_t       sources_head;
    uint32_t      n_alldiffs;
    dvs_expr_t       allDiff_head;
    uint32_t      n_softs;
    dvs_expr_t       softs_head;
    uint32_t      n_dists;
    dvs_expr_t       dists_head;
};

/* The documented builder functions are declared in dv_solve.h; this
 * header adds the internal ones. */

/* ------------------------------------------------------------------ */
/* Finalize -- produce a contiguous dvs_problem_t buffer                */
/* ------------------------------------------------------------------ */

/**
 * Like dvs_builder_finalize, but leaves `extra_bytes` of unused pool capacity
 * (pool.capacity > pool.used). The in-place expr_ / problem_add_var API can then
 * append nodes + variables into the slack after finalize -- used by the lazy
 * array refinement loop to inject read-over-write / congruence lemmas and new
 * read variables into the live problem. Returns NULL on allocation failure.
 */
dvs_problem_t *dvs_builder_finalize_reserve(dvs_builder_t *b, size_t *size,
                                       uint32_t extra_bytes);

/**
 * A position in the builder's item lists (vars, constraints, sources,
 * all-different groups, softs, dists), taken with dvs_builder_mark().
 */
typedef struct {
    uint32_t n_vars, n_constraints, n_sources, n_alldiffs, n_softs, n_dists;
} dvs_builder_mark_t;

/** Record the current end of every item list. */
dvs_builder_mark_t dvs_builder_mark(const dvs_builder_t *b);

/**
 * Like dvs_builder_finalize, but the returned problem lists ONLY the items added
 * since `mark`. The whole pool is still copied, so every dvs_expr_t stays valid
 * even though earlier items are no longer reachable from the list heads.
 *
 * This lets a caller keep one builder holding the complete constraint set (to
 * finalize in full later) while handing an incremental consumer just the new
 * items. Returns NULL on allocation failure or if `mark` is ahead of `b`.
 */
dvs_problem_t *dvs_builder_finalize_since(dvs_builder_t *b,
                                     const dvs_builder_mark_t *mark, size_t *size);

/**
 * Drop every item added since `mark` (the builder then finalizes as it would
 * have at the mark). Pool storage is not reclaimed. Returns -1 if `mark` is
 * ahead of `b`. Used by the SMT-LIB2 frontend's (pop) when no CDCL context
 * holds the scope.
 */
int dvs_builder_rewind(dvs_builder_t *b, const dvs_builder_mark_t *mark);

/* ------------------------------------------------------------------ */
/* Low-level allocation                                                */
/* ------------------------------------------------------------------ */

/**
 * Allocate bytes from the builder's virtual address space.
 *
 * @param b      The builder.
 * @param bytes  Number of bytes to allocate.
 * @param align  Alignment requirement (power of 2, 0 or 1 for none).
 * @return  dvs_expr_t (sizeof(dvs_pool_t) + virtual_offset), matching the
 *          pool offset convention.  Returns EXPR_NULL only on malloc
 *          failure (not on capacity overflow -- the builder grows).
 */
dvs_expr_t dvs_builder_alloc(dvs_builder_t *b, uint32_t bytes, uint32_t align);

/**
 * Return the current virtual offset (bytes allocated so far).
 */
uint32_t dvs_builder_virtual_used(const dvs_builder_t *b);

/**
 * Return a pointer to the data at the given dvs_expr_t within the builder's
 * virtual address space.  Used to read back nodes (e.g. ExprVar.var_id)
 * that were allocated earlier.  Returns NULL if ref is EXPR_NULL.
 */
void *dvs_builder_ref_ptr(const dvs_builder_t *b, dvs_expr_t ref);

/* ------------------------------------------------------------------ */
/* Expression builders (mirror dvs_problem.h API)                      */
/* ------------------------------------------------------------------ */

/** A sized constant of `width` bits (see ExprConst in dvs_problem.h).
 *  width 0 is the same as dvs_builder_expr_const (an unsized literal). */
dvs_expr_t dvs_builder_expr_const_sized(dvs_builder_t *b, int64_t value,
                                 uint8_t is_signed, uint8_t width);
/** INTERNAL (not part of the documented surface): an explicit
 *  width/signedness conversion node, EXPR_SV_CAST (see dvs_problem.h). Used
 *  by front ends that build explicit problems, e.g. the SMT-LIB2 front end
 *  reading an unsigned bit pattern as signed for `bvashr`. */
dvs_expr_t dvs_builder_expr_sv_cast(dvs_builder_t *b, dvs_expr_t operand,
                             uint8_t from_bits, uint8_t to_bits,
                             uint8_t sign_extend, uint8_t dst_signed);

/** Build an array-select expression: result = base[index]. */
dvs_expr_t dvs_builder_expr_array_select(dvs_builder_t *b, uint32_t base_var_id,
                                   uint32_t n_elems, dvs_expr_t result, dvs_expr_t index);

/* ------------------------------------------------------------------ */
/* Problem builders (mirror dvs_problem.h API)                         */
/* ------------------------------------------------------------------ */

/**
 * Mark a previously-added variable as a compiler-generated auxiliary.
 * Aux variables are never picked as a search decision; their value is
 * determined entirely by propagation from the constraints that define
 * them. Mis-marking a user variable as aux can lead to unknown results
 * when search would have been needed.
 */
void dvs_builder_mark_var_aux(dvs_builder_t *b, dvs_expr_t var_ref);

/**
 * Add a source group (set of variables to randomize together).
 * @return dvs_expr_t to the SourceSpec, or EXPR_NULL on alloc failure.
 */
dvs_expr_t dvs_builder_add_source(dvs_builder_t *b,
                           uint32_t n_vars, const uint32_t *var_ids);


/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

/** Return the number of variables added so far. */
static inline uint32_t dvs_builder_n_vars(const dvs_builder_t *b) {
    return b->n_vars;
}

/** Return the number of constraints added so far. */
static inline uint32_t dvs_builder_n_constraints(const dvs_builder_t *b) {
    return b->n_constraints;
}

/** Return the number of source groups added so far. */
static inline uint32_t dvs_builder_n_sources(const dvs_builder_t *b) {
    return b->n_sources;
}

#ifdef __cplusplus
}
#endif

#endif /* DVS_BUILDER_H */
