/*
 * dvs_writer.h -- building a dv-solve problem.
 *
 * The problem-building half of the public API: a dvs_builder_t is filled in
 * and finalized into a dvs_problem_t, a self-contained byte image that the
 * runtime solves (dvs_solver_compile(), dvs_handle_new()). It is installed as
 * <dv_solve/dvs_writer.h> and included by dv_solve.h.
 *
 * Used alone, it is the API of the writer library (dv-solve-writer), which
 * needs nothing but libc: a program that only writes problems, such as a
 * compiler embedding them in generated code, links that and no solver.
 */
#ifndef DVS_WRITER_H
#define DVS_WRITER_H

#include <stddef.h>
#include <stdint.h>

#include "dvs_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The version of the problem image format. A runtime solves only images of
 *  its own version: dvs_handle_new() refuses any other. */
#define DVS_PROBLEM_FORMAT_VERSION 5u

/** DVS_PROBLEM_FORMAT_VERSION of the linked library. */
DVS_API uint32_t dvs_problem_format_version(void);

/** Builds a problem; see dvs_builder_create(). */
typedef struct dvs_builder_s dvs_builder_t;

/** A finalized problem, ready to compile; see dvs_builder_finalize(). */
typedef struct dvs_problem_s dvs_problem_t;

/** A memory allocator. Every function that takes one accepts NULL, meaning
 *  malloc and free. Its members are in dvs_alloc.h: alloc, release, and an
 *  optional resize (leave it NULL and the solver copies to grow a block). */
typedef struct dvs_alloc_s dvs_alloc_t;
/** A reference to an expression or a constraint inside a builder. */
typedef uint32_t dvs_expr_t;

/** The dvs_expr_t a builder function returns when it cannot allocate memory. */
#define DVS_EXPR_NULL ((dvs_expr_t)0xFFFFFFFFu)

/** The widest variable, constant or expression the engine solves, in bits.
 *  Widths are 32-bit in the API; this is the engine's own ceiling, set so
 *  bit-blasting stays within 32-bit node and buffer arithmetic. A wider
 *  variable or constant is refused when it is built (DVS_EXPR_NULL), and a
 *  wider intermediate expression fails the solve. Variables of 1 to 64 bits
 *  go to the CDCL engine; wider ones are bit-blasted (BV-SAT). */
#define DVS_MAX_WIDTH (1u << 24)

/** Binary operators, for dvs_builder_expr_binary(). */
typedef enum {
    DVS_BIN_ADD = 0,    /* +   */
    DVS_BIN_SUB,        /* -   */
    DVS_BIN_MUL,        /* *   */
    DVS_BIN_DIV,        /* /   */
    DVS_BIN_MOD,        /* %   */
    DVS_BIN_BAND,       /* &   */
    DVS_BIN_BOR,        /* |   */
    DVS_BIN_BXOR,       /* ^   */
    DVS_BIN_LSHIFT,     /* <<  */
    DVS_BIN_RSHIFT,     /* >>  (logical)                                  */
    DVS_BIN_EQ,         /* ==  */
    DVS_BIN_NEQ,        /* !=  */
    DVS_BIN_LT,         /* <   */
    DVS_BIN_LTE,        /* <=  */
    DVS_BIN_GT,         /* >   */
    DVS_BIN_GTE,        /* >=  */
    DVS_BIN_AND,        /* &&  */
    DVS_BIN_OR,         /* ||  */
    DVS_BIN_ASHR,       /* >>> (arithmetic in a signed expression, else >>) */
    /* Typed operators: the operator, not the operand types, says how the
     * operands are read. Each operand is extended to the wider operand's
     * width by the operator's signedness (sign for S*, zero for U* and
     * LSHR), and the operator works at that width: the node is
     * self-determined, as an SMT-LIB term is. Division by zero is defined as
     * in SMT-LIB (no SV x): x UDIV 0 is all ones, x UREM 0 is x, x SDIV 0 is
     * -1 for x >= 0 and 1 for x < 0, x SREM 0 and x SMOD 0 are x. */
    DVS_BIN_ULT,        /* <  unsigned (bvult)                            */
    DVS_BIN_ULE,        /* <= unsigned (bvule)                            */
    DVS_BIN_UGT,        /* >  unsigned (bvugt)                            */
    DVS_BIN_UGE,        /* >= unsigned (bvuge)                            */
    DVS_BIN_SLT,        /* <  signed (bvslt)                              */
    DVS_BIN_SLE,        /* <= signed (bvsle)                              */
    DVS_BIN_SGT,        /* >  signed (bvsgt)                              */
    DVS_BIN_SGE,        /* >= signed (bvsge)                              */
    DVS_BIN_UDIV,       /* /  unsigned (bvudiv)                           */
    DVS_BIN_UREM,       /* %  unsigned (bvurem)                           */
    DVS_BIN_SDIV,       /* /  signed, truncating (bvsdiv)                 */
    DVS_BIN_SREM,       /* %  signed, the dividend's sign (bvsrem)        */
    DVS_BIN_SMOD,       /* signed modulo, the divisor's sign (bvsmod)     */
    DVS_BIN_LSHR,       /* >> logical, whatever the operand (bvlshr)      */
} dvs_binop_t;

/** Unary operators, for dvs_builder_expr_unary(). */
typedef enum {
    DVS_UN_NEG    = 0,  /* -  */
    DVS_UN_NOT    = 1,  /* !  */
    DVS_UN_INVERT = 2,  /* ~  */
} dvs_unop_t;

/** One entry of a weighted distribution; see dvs_builder_add_dist(). */
typedef struct {
    int64_t  lo;           /* lowest value of the range                     */
    int64_t  hi;           /* highest value of the range (== lo for a value) */
    uint32_t weight;
    uint8_t  is_per_value; /* 1: each value gets `weight` (SV :=);
                            * 0: the range shares `weight` (SV :/)          */
    uint8_t  _dpad[3];
} dvs_dist_entry_t;

/* ------------------------------------------------------------------ */
/* Building a problem                                                  */
/* ------------------------------------------------------------------ */

/**
 * Create a builder.
 *
 * @param block_size  Size of each memory block the builder grows by; 0 for
 *                    the default (4096 bytes).
 * @param alloc       Allocator, or NULL for malloc.
 * @return  The builder, or NULL if memory could not be allocated.
 */
DVS_API dvs_builder_t *dvs_builder_create(uint32_t block_size, dvs_alloc_t *alloc);

/** Free the builder. Problems it finalized are not freed. */
DVS_API void dvs_builder_destroy(dvs_builder_t *b);

/** Empty the builder so it can build another problem, keeping its memory. */
DVS_API void dvs_builder_reset(dvs_builder_t *b);

/**
 * Declare variable `var_id`.
 *
 * @param var_id     The variable's id, used in dvs_builder_expr_var() and
 *                   dvs_solver_get_value(). A problem with n variables uses
 *                   the ids 0 to n-1, each declared once.
 * @param width      Width in bits, 1 to DVS_MAX_WIDTH. Over 64 bits the
 *                   variable is solved by bit-blasting (BV-SAT), and lo/hi
 *                   do not bound it. DVS_EXPR_NULL for a width of 0 or over
 *                   DVS_MAX_WIDTH.
 * @param is_signed  1 if the variable is signed.
 * @param lo, hi     The variable's value must lie in [lo, hi].
 */
DVS_API dvs_expr_t dvs_builder_add_var(dvs_builder_t *b, uint32_t var_id,
                                       uint32_t width, uint8_t is_signed,
                                       int64_t lo, int64_t hi);

/** Require the Boolean expression `root` to hold. */
DVS_API dvs_expr_t dvs_builder_add_constraint(dvs_builder_t *b, dvs_expr_t root);

/** Require the variables to take pairwise different values (SV `unique`).
 *  A member may be a whole array: DVS_ARRAY_MEMBER(array_id). Not in a
 *  foreach body (EXPR_NULL). */
DVS_API dvs_expr_t dvs_builder_add_all_different(dvs_builder_t *b, uint32_t n_vars,
                                                 const uint32_t *var_ids);

/**
 * Ask for `root` to hold where possible. Priority 0 is the most important:
 * when soft constraints conflict, those with the highest priority number are
 * dropped first.
 */
DVS_API dvs_expr_t dvs_builder_add_soft_constraint(dvs_builder_t *b, dvs_expr_t root,
                                                   uint32_t priority);

/**
 * Put variable `var_id` in solve-order stage `stage` (SV `solve ... before`):
 * the variables of stage 1 are chosen first, then those of stage 2, and so
 * on; variables in stage 0 (the default) come last. Each stage is chosen
 * among the values that leave the constraints satisfiable, and in a seeded
 * fair_pick solve the last stage is uniform over the solutions that remain.
 * Declare the variable first.
 *
 * @return 0, or -1 if no variable `var_id` has been declared.
 */
DVS_API int dvs_builder_set_var_order(dvs_builder_t *b, uint32_t var_id,
                                      uint8_t stage);

/**
 * What a variable is to the solve (SV terms). The value is stored in the
 * problem, so the numbers are fixed.
 */
typedef enum {
    DVS_VAR_RAND  = 0,  /* rand: a search decision (the default)          */
    DVS_VAR_AUX   = 1,  /* compiler-generated: never a search decision; its
                         * value follows from the constraints that define it */
    DVS_VAR_RANDC = 2,  /* randc: solved as RAND; dvs_randomize() keeps the
                         * per-var history                                */
    DVS_VAR_STATE = 3,  /* state: read per call and pinned, never sampled;
                         * solved as RAND when nothing pins it            */
    DVS_VAR_IMPORT = 4  /* another object's variable: set by
                         * dvs_builder_add_import(), not by
                         * dvs_builder_set_var_kind()                     */
} dvs_var_kind_t;

/**
 * Set the kind of variable `var_id`. Declare the variable first.
 *
 * @return 0, or -1 if no variable `var_id` has been declared or `kind` is
 *         not a dvs_var_kind_t.
 */
DVS_API int dvs_builder_set_var_kind(dvs_builder_t *b, uint32_t var_id,
                                     dvs_var_kind_t kind);

/** Give variable `var_id` a weighted distribution (SV `dist`). Not in a
 *  foreach body (EXPR_NULL): see dvs_builder_add_dist_elem(). */
DVS_API dvs_expr_t dvs_builder_add_dist(dvs_builder_t *b, uint32_t var_id,
                                        uint32_t n_entries,
                                        const dvs_dist_entry_t *entries);

/** A constant. Like an unsized SystemVerilog literal it is 32 bits wide,
 *  unless the value doesn't fit in 32 bits. */
DVS_API dvs_expr_t dvs_builder_expr_const(dvs_builder_t *b, int64_t value,
                                          uint8_t is_signed);

/** A sized constant: the low `width` bits (1..64) of `value`, signed when
 *  `is_signed`, like the SystemVerilog literal 8'd3. A width of 0 is an
 *  unsized constant (dvs_builder_expr_const()). Wider constants are spelled
 *  with dvs_builder_expr_const_wide(). */
DVS_API dvs_expr_t dvs_builder_expr_const_sized(dvs_builder_t *b, int64_t value,
                                                uint8_t is_signed, uint32_t width);

/** A sized constant of `width` bits (1..DVS_MAX_WIDTH), signed when `is_signed`,
 *  given as `n_limbs` = ceil(width / 64) 64-bit limbs, least significant
 *  first: limbs[0] holds bits 63:0. Bits at and above `width` are ignored.
 *  It is one node whatever its width, so it is how to spell a constant wider
 *  than 64 bits. Returns EXPR_NULL for a bad width or limb count. */
DVS_API dvs_expr_t dvs_builder_expr_const_wide(dvs_builder_t *b, uint32_t n_limbs,
                                               const uint64_t *limbs, uint32_t width,
                                               uint8_t is_signed);

/** A reference to variable `var_id`. */
DVS_API dvs_expr_t dvs_builder_expr_var(dvs_builder_t *b, uint32_t var_id);

/** `lhs op rhs`. */
DVS_API dvs_expr_t dvs_builder_expr_binary(dvs_builder_t *b, dvs_binop_t op,
                                           dvs_expr_t lhs, dvs_expr_t rhs);

/** `op operand`. */
DVS_API dvs_expr_t dvs_builder_expr_unary(dvs_builder_t *b, dvs_unop_t op,
                                          dvs_expr_t operand);

/** `cond ? then_e : else_e`. With Boolean arms, an if/else constraint. */
DVS_API dvs_expr_t dvs_builder_expr_ite(dvs_builder_t *b, dvs_expr_t cond,
                                        dvs_expr_t then_e, dvs_expr_t else_e);

/** `value inside {[lo:hi]}`. */
DVS_API dvs_expr_t dvs_builder_expr_in_range(dvs_builder_t *b, dvs_expr_t value,
                                             dvs_expr_t lo, dvs_expr_t hi);

/** `value inside {elems[0], elems[1], ...}`. */
DVS_API dvs_expr_t dvs_builder_expr_in_set(dvs_builder_t *b, dvs_expr_t value,
                                           uint32_t n_elems, const dvs_expr_t *elems);

/** `value inside {[los[0]:his[0]], [los[1]:his[1]], ...}`. */
DVS_API dvs_expr_t dvs_builder_expr_in_ranges(dvs_builder_t *b, dvs_expr_t value,
                                              uint32_t n_ranges, const dvs_expr_t *los,
                                              const dvs_expr_t *his);

/** Zero-extend (sign_extend 0) or sign-extend `operand` from `from_bits`
 *  to `to_bits`. */
DVS_API dvs_expr_t dvs_builder_expr_extend(dvs_builder_t *b, dvs_expr_t operand,
                                           uint32_t from_bits, uint32_t to_bits,
                                           uint8_t sign_extend);

/** A cast of `operand` to a `to_bits`-bit integer, signed when `to_signed`:
 *  SystemVerilog's `T'(operand)`. When `to_bits` is wider than the operand,
 *  the operand is evaluated at `to_bits` (an assignment-like context), so
 *  `cast(a * b, 64, 0)` of two 32-bit variables does not wrap at 32 bits.
 *  The value is truncated to `to_bits`, or extended by the operand's own
 *  signedness, and then read as signed or unsigned. Not for an EXPLICIT
 *  problem: see dvs_builder_expr_sv_cast(). */
DVS_API dvs_expr_t dvs_builder_expr_cast(dvs_builder_t *b, dvs_expr_t operand,
                                         uint32_t to_bits, uint8_t to_signed);

/** The conversion of an EXPLICIT problem (dvs_builder_set_explicit()), in
 *  which no operator converts its operands, so dvs_builder_expr_cast() does
 *  not apply: the low `from_bits` bits of `operand`, zero-extended
 *  (sign_extend 0) or sign-extended (1) to `to_bits`, read as signed when
 *  `dst_signed`, else unsigned. With `from_bits` equal to `to_bits` it only
 *  changes how the value reads, e.g. an unsigned bit pattern as signed. */
DVS_API dvs_expr_t dvs_builder_expr_sv_cast(dvs_builder_t *b, dvs_expr_t operand,
                                            uint32_t from_bits, uint32_t to_bits,
                                            uint8_t sign_extend, uint8_t dst_signed);

/** `operand[hi_bit:lo_bit]`. */
DVS_API dvs_expr_t dvs_builder_expr_extract(dvs_builder_t *b, dvs_expr_t operand,
                                            uint32_t hi_bit, uint32_t lo_bit);

/** `{hi, lo}`, where `lo` is `lo_width` bits wide. */
DVS_API dvs_expr_t dvs_builder_expr_concat(dvs_builder_t *b, dvs_expr_t hi,
                                           dvs_expr_t lo, uint32_t lo_width);

/** The constraint `result == var_refs[0] + var_refs[1] + ...`. Pass it to
 *  dvs_builder_add_constraint(). */
DVS_API dvs_expr_t dvs_builder_expr_sum(dvs_builder_t *b, dvs_expr_t result,
                                        uint32_t n_vars, const dvs_expr_t *var_refs);

/** The constraint `result == $countones(operand)`. */
DVS_API dvs_expr_t dvs_builder_expr_countones(dvs_builder_t *b, dvs_expr_t result,
                                              dvs_expr_t operand);

/** The constraint `result == $clog2(operand)`. */
DVS_API dvs_expr_t dvs_builder_expr_clog2(dvs_builder_t *b, dvs_expr_t result,
                                          dvs_expr_t operand);

/* ------------------------------------------------------------------ */
/* Arrays whose size is given on each call (SV dynamic arrays, queues) */
/*                                                                     */
/* A problem with arrays is a template: dvs_randomize_arrays() takes   */
/* each array's size and elements, and dv-solve instantiates the       */
/* constraints for those sizes. Array ids are their own space, 0..n-1, */
/* apart from variable ids. Only a handle solves such a problem        */
/* (dvs_solver_compile() refuses it).                                  */
/* ------------------------------------------------------------------ */

/** Most nested foreach and reduction levels one expression may bind. */
#define DVS_MAX_LOOP_LEVELS 8u

/**
 * Declare array `array_id` with elements of `elem_width` bits (1 to 64).
 * `kind` is DVS_VAR_RAND (the elements are solved) or DVS_VAR_STATE (they
 * are read).
 *
 * @return 0, or -1 if the width or kind is not one of these.
 */
DVS_API int dvs_builder_add_array(dvs_builder_t *b, uint32_t array_id,
                                  uint32_t elem_width, uint8_t elem_signed,
                                  dvs_var_kind_t kind);

/** Put every element of array `array_id` in solve-order stage `stage`
 *  (dvs_builder_set_var_order). @return 0, or -1 if it is not declared. */
DVS_API int dvs_builder_set_array_order(dvs_builder_t *b, uint32_t array_id,
                                        uint8_t stage);

/** Make the size of RAND array (or slot array) `array_id` random:
 *  dvs_randomize_sizes() solves it, with the constraints that read no
 *  element, before the elements are solved at that size (IEEE 1800-2023
 *  18.5.8.1). A random size solved above 65535 fails the call
 *  (DVS_RANDOMIZE_TOO_LARGE). @return 0, or -1 if the array is not declared,
 *  or neither RAND nor a slot array. */
DVS_API int dvs_builder_set_array_size_random(dvs_builder_t *b, uint32_t array_id);

/** The size of array `array_id`: a 32-bit signed int. */
DVS_API dvs_expr_t dvs_builder_expr_array_size(dvs_builder_t *b, uint32_t array_id);

/** The index bound at `level` by the enclosing dvs_builder_foreach_begin()
 *  or dvs_builder_expr_reduce(): a 32-bit signed int. */
DVS_API dvs_expr_t dvs_builder_expr_loop_index(dvs_builder_t *b, uint32_t level);

/** Element `index` of array `array_id`, of its element type; 0 when the
 *  index is out of range. A random index selects among the elements. */
DVS_API dvs_expr_t dvs_builder_expr_elem(dvs_builder_t *b, uint32_t array_id,
                                         dvs_expr_t index);

/**
 * A reduction over array `array_id` (SV `sum()`, `product()`, `and()`,
 * `or()`, `xor()`, with or without `with`): `body`, built with
 * dvs_builder_expr_loop_index(level) as the element's index, folded with
 * `op` (DVS_BIN_ADD, MUL, BAND, BOR or BXOR). Of the type of `body`; over an
 * empty array, the identity of `op`.
 */
DVS_API dvs_expr_t dvs_builder_expr_reduce(dvs_builder_t *b, dvs_binop_t op,
                                           uint32_t array_id, uint32_t level,
                                           dvs_expr_t body);

/**
 * Open a foreach over array `array_id` (SV `foreach (a[i])`): until the
 * matching dvs_builder_foreach_end(), dvs_builder_add_constraint(),
 * dvs_builder_add_soft_constraint() and dvs_builder_add_dist_elem() add to
 * its body, which holds once per element with
 * dvs_builder_expr_loop_index(level) its index. Foreachs nest; `level`
 * must differ from those of the enclosing ones.
 *
 * @return 0, or -1 if `level` is DVS_MAX_LOOP_LEVELS or more, or is bound.
 */
DVS_API int dvs_builder_foreach_begin(dvs_builder_t *b, uint32_t array_id,
                                      uint32_t level);

/** Close the innermost open foreach. @return 0, or -1 if none is open. */
DVS_API int dvs_builder_foreach_end(dvs_builder_t *b);

/** Give element `index` of array `array_id` a weighted distribution, in the
 *  body of an open foreach. @return EXPR_NULL if no foreach is open. */
DVS_API dvs_expr_t dvs_builder_add_dist_elem(dvs_builder_t *b, uint32_t array_id,
                                             dvs_expr_t index, uint32_t n_entries,
                                             const dvs_dist_entry_t *entries);

/* ------------------------------------------------------------------ */
/* Parts: constraints across objects (IEEE 1800-2023 18.5.8)          */
/*                                                                     */
/* A part is the problem of one class: its own constraints, its        */
/* exports (the variables other objects' constraints may name), and    */
/* its slots (its handles to other objects, whose parts are given on   */
/* each call). A constraint that names a variable of another object    */
/* reads an import: a variable bound to an export of the object at a   */
/* slot path. dvs_randomize_parts() solves the parts of the objects    */
/* reached from one object together.                                   */
/* ------------------------------------------------------------------ */

/** Give the part `n_slots` slots, numbered 0 to n_slots-1. */
DVS_API void dvs_builder_set_n_slots(dvs_builder_t *b, uint32_t n_slots);

/**
 * Export variable `var_id`: the next export index, in the order of the
 * calls. A derived class's part declares its base class's exports first, in
 * the same order, so that a part imports through a slot without knowing the
 * class of the object in it.
 *
 * @return  the export index, or -1 if `var_id` is not declared or is an
 *          import.
 */
DVS_API int dvs_builder_add_export(dvs_builder_t *b, uint32_t var_id);

/**
 * Declare array `array_id` as a slot array taking slot `slot`: the handles
 * of an SV dynamic array or queue of class handles. Its size is the number
 * of handles (random with dvs_builder_set_array_size_random), a foreach over
 * it iterates them, and its element `i` is 1 if handle `i` is not null.
 * dvs_randomize_parts() instantiates a foreach over it once per element.
 *
 * @return 0, or -1 if the array could not be added.
 */
DVS_API int dvs_builder_add_slot_array(dvs_builder_t *b, uint32_t array_id, uint32_t slot);

/** ArraySpec.slot of an array that is not a slot array. */
#define DVS_NO_SLOT  0xFFFFFFFFu
/** In an import path, after a slot that is a slot array: the element at
 *  the index bound to loop `level` (a foreach over a slot array). Any other
 *  word there is a constant index. */
#define DVS_PATH_LOOP(level)  (0x80000000u | (uint32_t)(level))

/**
 * Make variable `var_id` an import (DVS_VAR_IMPORT): it stands for export
 * `export_index` of the object reached through slot path[0] of this part,
 * then slot path[1] of that object's part, and so on. A slot that is a
 * slot array is followed by the word selecting its element (a constant
 * index or DVS_PATH_LOOP); at most one DVS_PATH_LOOP per path. Declare the
 * variable first, with the export's width and signedness.
 *
 * @return  0, or -1 if `var_id` is not declared or already an import, or
 *          `n_path` is 0.
 */
DVS_API int dvs_builder_add_import(dvs_builder_t *b, uint32_t var_id, uint32_t n_path,
                                   const uint32_t *path, uint32_t export_index);

/** In dvs_builder_add_all_different(): all the elements of array `id`. */
#define DVS_ARRAY_MEMBER(id)  (0x80000000u | (uint32_t)(id))

/**
 * Produce the finished problem. The builder is unchanged and can go on to be
 * reset or destroyed.
 *
 * @param size  If not NULL, receives the problem's size in bytes, which
 *              dvs_builder_free_problem() needs.
 * @return  The problem, or NULL if memory could not be allocated. Free it
 *          with dvs_builder_free_problem(), and not before every context
 *          compiled from it is destroyed.
 */
DVS_API dvs_problem_t *dvs_builder_finalize(dvs_builder_t *b, size_t *size);

/** Free a problem returned by dvs_builder_finalize(). */
DVS_API void dvs_builder_free_problem(dvs_builder_t *b, dvs_problem_t *p, size_t size);

/**
 * Mark the problem EXPLICIT: every constant is sized, and every operator's
 * operands already have the width and signedness it works at (SMT-LIB
 * bit-vector semantics), so the runtime skips SystemVerilog width
 * elaboration. Applies to the problems finalized after the call.
 */
DVS_API void dvs_builder_set_explicit(dvs_builder_t *b);

/**
 * Check a finalized problem against what the runtime solves, so that a
 * program writing problems can refuse one when it writes it rather than have
 * it fail at run time. It checks that the image is well formed, and makes
 * the checks dvs_solver_compile() makes: variable ids 0..n-1, each declared
 * once, 1 to 64 bits wide; all-different over at most 16 variables of at
 * most 32 bits; every variable named is declared; no width over
 * DVS_MAX_WIDTH, no reversed slice, no expression nested over 20000 deep.
 *
 * @param why     If not NULL, receives the reason for a refusal, starting
 *                with the dvs_solver_compile() code it stands for
 *                ("BAD_VAR: ...", "UNSUPPORTED_WIDTH: ...").
 * @param whylen  Size of `why`.
 * @return 0 if the problem passes, -1 if not.
 */
DVS_API int dvs_problem_validate(const dvs_problem_t *p, char *why, size_t whylen);

#ifdef __cplusplus
}
#endif

#endif /* DVS_WRITER_H */
