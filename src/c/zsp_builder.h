#ifndef ZSP_BUILDER_H
#define ZSP_BUILDER_H

#include <stddef.h>
#include <stdint.h>
#include "zsp_alloc.h"
#include "zsp_pool.h"     /* ExprRef, EXPR_NULL */
#include "zsp_problem.h"  /* ExprKind, BinOp, UnaryOp, SolveProblem */

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
/* SolveProblemBuilder                                                 */
/*                                                                     */
/* Growable problem builder that produces an exact-sized, contiguous   */
/* SolveProblem buffer on finalize().  ExprRef values match exactly    */
/* what the fixed-buffer SolveProblem API would produce for the same   */
/* allocation sequence: sizeof(zsp_pool_t) + virtual_offset.           */
/* ------------------------------------------------------------------ */
typedef struct {
    BuilderBlock *first;            /* head of block list              */
    BuilderBlock *current;          /* tail (active block)             */
    uint32_t      virtual_used;     /* running total logical offset    */
    uint32_t      block_size;       /* capacity for new blocks         */
    zsp_alloc_t  *alloc;            /* backing allocator (NULL=malloc) */

    /* Problem metadata (mirrors SolveProblem header) */
    uint32_t      n_vars;
    uint32_t      n_constraints;
    uint32_t      n_sources;
    ExprRef       vars_head;
    ExprRef       constraints_head;
    ExprRef       sources_head;
    uint32_t      n_alldiffs;
    ExprRef       allDiff_head;
    uint32_t      n_softs;
    ExprRef       softs_head;
    uint32_t      n_dists;
    ExprRef       dists_head;
} SolveProblemBuilder;

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

/**
 * Create a new problem builder.
 *
 * @param block_size  Capacity for each allocation block (default 4096).
 *                    Pass 0 for the default.
 * @param alloc       Backing allocator, or NULL for malloc/free.
 * @return  New builder, or NULL on allocation failure.
 */
SolveProblemBuilder *builder_create(uint32_t block_size, zsp_alloc_t *alloc);

/**
 * Reset the builder to empty state, reusing existing block memory.
 */
void builder_reset(SolveProblemBuilder *b);

/**
 * Destroy the builder and free all blocks.
 */
void builder_destroy(SolveProblemBuilder *b);

/* ------------------------------------------------------------------ */
/* Finalize -- produce a contiguous SolveProblem buffer                */
/* ------------------------------------------------------------------ */

/**
 * Finalize the builder into an exact-sized, contiguous SolveProblem.
 *
 * The returned buffer is allocated via calloc (or the builder's alloc).
 * The caller owns the buffer and must free it with builder_free_problem()
 * or free() (if using the default allocator).
 *
 * @param b     The builder.
 * @param size  If non-NULL, receives the total buffer size in bytes.
 * @return  Pointer to a valid SolveProblem, or NULL on failure.
 */
SolveProblem *builder_finalize(SolveProblemBuilder *b, size_t *size);

/**
 * Free a SolveProblem buffer returned by builder_finalize().
 */
void builder_free_problem(SolveProblemBuilder *b, SolveProblem *sp, size_t size);

/* ------------------------------------------------------------------ */
/* Low-level allocation                                                */
/* ------------------------------------------------------------------ */

/**
 * Allocate bytes from the builder's virtual address space.
 *
 * @param b      The builder.
 * @param bytes  Number of bytes to allocate.
 * @param align  Alignment requirement (power of 2, 0 or 1 for none).
 * @return  ExprRef (sizeof(zsp_pool_t) + virtual_offset), matching the
 *          pool offset convention.  Returns EXPR_NULL only on malloc
 *          failure (not on capacity overflow -- the builder grows).
 */
ExprRef builder_alloc(SolveProblemBuilder *b, uint32_t bytes, uint32_t align);

/**
 * Return the current virtual offset (bytes allocated so far).
 */
uint32_t builder_virtual_used(const SolveProblemBuilder *b);

/**
 * Return a pointer to the data at the given ExprRef within the builder's
 * virtual address space.  Used to read back nodes (e.g. ExprVar.var_id)
 * that were allocated earlier.  Returns NULL if ref is EXPR_NULL.
 */
void *builder_ref_ptr(const SolveProblemBuilder *b, ExprRef ref);

/* ------------------------------------------------------------------ */
/* Expression builders (mirror zsp_problem.h API)                      */
/* ------------------------------------------------------------------ */

ExprRef builder_expr_const(SolveProblemBuilder *b, int64_t value,
                           uint8_t is_signed);
ExprRef builder_expr_var(SolveProblemBuilder *b, uint32_t var_id);
ExprRef builder_expr_binary(SolveProblemBuilder *b, BinOp op,
                            ExprRef lhs, ExprRef rhs);
ExprRef builder_expr_unary(SolveProblemBuilder *b, UnaryOp op,
                           ExprRef operand);
ExprRef builder_expr_ite(SolveProblemBuilder *b,
                         ExprRef cond, ExprRef then_e, ExprRef else_e);
ExprRef builder_expr_in_range(SolveProblemBuilder *b,
                              ExprRef value, ExprRef lo, ExprRef hi);
ExprRef builder_expr_in_set(SolveProblemBuilder *b, ExprRef value,
                            uint32_t n_elems, const ExprRef *elems);
ExprRef builder_expr_extend(SolveProblemBuilder *b, ExprRef operand,
                            uint8_t from_bits, uint8_t to_bits,
                            uint8_t sign_extend);
ExprRef builder_expr_extract(SolveProblemBuilder *b, ExprRef operand,
                             uint8_t hi_bit, uint8_t lo_bit);
ExprRef builder_expr_concat(SolveProblemBuilder *b, ExprRef hi,
                           ExprRef lo, uint8_t lo_width);

/** Build an array-select expression: result = base[index]. */
ExprRef builder_expr_array_select(SolveProblemBuilder *b, uint32_t base_var_id,
                                   uint32_t n_elems, ExprRef result, ExprRef index);

/** Build an N-ary sum expression. var_refs[] are ExprRef for summand vars. */
ExprRef builder_expr_sum(SolveProblemBuilder *b, ExprRef result,
                         uint32_t n_vars, const ExprRef *var_refs);

/** Build a countones (popcount) expression. */
ExprRef builder_expr_countones(SolveProblemBuilder *b, ExprRef result,
                                ExprRef operand);

/** Build a clog2 expression. */
ExprRef builder_expr_clog2(SolveProblemBuilder *b, ExprRef result,
                            ExprRef operand);

/* ------------------------------------------------------------------ */
/* Problem builders (mirror zsp_problem.h API)                         */
/* ------------------------------------------------------------------ */

/**
 * Add a variable declaration to the problem.
 * @return ExprRef to the VarSpec, or EXPR_NULL on alloc failure.
 */
ExprRef builder_add_var(SolveProblemBuilder *b, uint32_t var_id,
                        uint8_t width, uint8_t is_signed,
                        int64_t lo, int64_t hi);

/**
 * Mark a previously-added variable as a compiler-generated auxiliary.
 * Aux variables are never picked as a search decision; their value is
 * determined entirely by propagation from the constraints that define
 * them. Mis-marking a user variable as aux can lead to unknown results
 * when search would have been needed.
 */
void builder_mark_var_aux(SolveProblemBuilder *b, ExprRef var_ref);

/**
 * Add a constraint to the problem.
 * @param root  ExprRef of the constraint expression root.
 * @return ExprRef to the ConstraintSpec, or EXPR_NULL on alloc failure.
 */
ExprRef builder_add_constraint(SolveProblemBuilder *b, ExprRef root);

/**
 * Add a source group (set of variables to randomize together).
 * @return ExprRef to the SourceSpec, or EXPR_NULL on alloc failure.
 */
ExprRef builder_add_source(SolveProblemBuilder *b,
                           uint32_t n_vars, const uint32_t *var_ids);


/**
 * Add an AllDifferent constraint.
 * @return ExprRef to the AllDiffSpec, or EXPR_NULL on alloc failure.
 */
ExprRef builder_add_all_different(SolveProblemBuilder *b,
                                  uint32_t n_vars, const uint32_t *var_ids);

/**
 * Add a soft (relaxable) constraint.
 * @param root     ExprRef of the constraint expression root.
 * @param priority Priority (0 = highest, larger = relaxed first).
 * @return ExprRef to the SoftSpec, or EXPR_NULL on alloc failure.
 */
ExprRef builder_add_soft_constraint(SolveProblemBuilder *b, ExprRef root,
                                    uint32_t priority);

/**
 * Add a distribution constraint on a variable.
 * @param var_id     Variable ID this distribution applies to.
 * @param n_entries  Number of DistEntry items.
 * @param entries    Array of DistEntry values (copied into pool).
 * @return ExprRef to the DistSpec, or EXPR_NULL on alloc failure.
 */
ExprRef builder_add_dist(SolveProblemBuilder *b, uint32_t var_id,
                         uint32_t n_entries, const DistEntry *entries);

/* ------------------------------------------------------------------ */
/* Query                                                               */
/* ------------------------------------------------------------------ */

/** Return the number of variables added so far. */
static inline uint32_t builder_n_vars(const SolveProblemBuilder *b) {
    return b->n_vars;
}

/** Return the number of constraints added so far. */
static inline uint32_t builder_n_constraints(const SolveProblemBuilder *b) {
    return b->n_constraints;
}

/** Return the number of source groups added so far. */
static inline uint32_t builder_n_sources(const SolveProblemBuilder *b) {
    return b->n_sources;
}

#ifdef __cplusplus
}
#endif

#endif /* ZSP_BUILDER_H */
