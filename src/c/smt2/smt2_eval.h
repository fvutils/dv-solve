#ifndef SMT2_EVAL_H
#define SMT2_EVAL_H

/* An evaluator for SMT-LIB terms over a model (fw-hdl R1).
 *
 * It interprets the term as written -- let, define-fun application, every
 * QF_BV operator, Bool connectives, select/store and constant arrays -- and
 * reads only the values of the declared symbols from the model. It shares no
 * code with the translator, so it can check that translator's work: a model
 * that satisfies the compiled problem but falsifies an assertion as written
 * is a translation bug.
 *
 * Bit-vectors are of any width (limb vectors). A term the evaluator cannot
 * evaluate -- an unknown symbol, an unsupported operator, an array equality it
 * cannot decide -- fails with a message; it never produces a guessed value. */

#include <stdint.h>
#include <stdio.h>
#include "smt2/smt2_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Value kinds. */
#define SMT2_VAL_BOOL   0
#define SMT2_VAL_BV     1
#define SMT2_VAL_ARRAY  2
#define SMT2_VAL_OPAQUE 3   /* an element of a declare-sort sort: its name */

typedef struct Smt2Value Smt2Value;

#define SMT2_EVAL_MAX_PARAMS 64

/* A define-fun, as the host knows it. */
typedef struct {
    uint32_t     n_params;
    const char  *param_names[SMT2_EVAL_MAX_PARAMS];  /* NUL-terminated; must
                                                      * outlive the evaluator */
    const Sexpr *body;
} Smt2EvalFun;

/* What the evaluator needs from the model and the symbol tables. */
typedef struct {
    void *ctx;
    /* A define-fun named `name`: fills *f and returns 1, else 0. */
    int (*fun)(void *ctx, const char *name, uint32_t len, Smt2EvalFun *f);
    /* A declared BitVec or Bool constant -- or an uninterpreted function
     * applied to sort constants, named "f@a@b" -- and its model value, in
     * ceil(width/64) little-endian limbs. Returns 1 if found. */
    int (*scalar)(void *ctx, const char *name, uint32_t len,
                  uint32_t *width, int *is_bool, uint64_t *limbs, uint32_t max_limbs);
    /* A declared array: a handle, its flat index width (I + J for a nested
     * array whose inner index has J bits; inner_w = J, else 0) and element
     * width. *total is 1 when the model holds a value for every index (so the
     * evaluator may compare it element by element). NULL if none. */
    const void *(*array)(void *ctx, const char *name, uint32_t len,
                         uint32_t *index_w, uint32_t *inner_w, uint32_t *elem_w,
                         int *total);
    /* Element `key` (index_w bits, little-endian limbs) of an array handle.
     * Returns 1, or 2 when no model value backs the element (no constraint
     * reads it, so the value is arbitrary), or 0 on failure. */
    int (*array_elem)(void *ctx, const void *arr, const uint64_t *key,
                      uint64_t *limbs, uint32_t elem_w);
    /* Print a declared array's model value (a store chain). 1 on success. */
    int (*print_array)(void *ctx, FILE *out, const void *arr);
    /* The arity of an uninterpreted function over declare-sort sorts, else 0. */
    uint32_t (*uf_arity)(void *ctx, const char *name, uint32_t len);
    /* 1 if `name` is a constant of a declare-sort sort. */
    int (*opaque_const)(void *ctx, const char *name, uint32_t len);
    /* The definition of a define-sort alias, or NULL. */
    const Sexpr *(*sort_alias)(void *ctx, const char *name, uint32_t len);
    /* Deepest term nesting to evaluate; deeper fails rather than overflow. */
    uint32_t max_depth;
} Smt2EvalHost;

typedef struct Smt2Eval Smt2Eval;

/* An evaluator over one model. Values and memoized function results live until
 * smt2_eval_free, so evaluate every term of one query with one evaluator. */
Smt2Eval *smt2_eval_new(const Smt2EvalHost *host);
void      smt2_eval_free(Smt2Eval *ev);

/* Evaluate `term`. NULL on failure; smt2_eval_error() then says why. */
const Smt2Value *smt2_eval(Smt2Eval *ev, const Sexpr *term);
const char      *smt2_eval_error(const Smt2Eval *ev);

int      smt2_value_kind(const Smt2Value *v);
uint32_t smt2_value_width(const Smt2Value *v);       /* BV width, else 0 */
/* Bool: 0 or 1. BV: the low 64 bits. */
uint64_t smt2_value_u64(const Smt2Value *v);
/* 1 if the value depends on an array element no constraint reads, whose model
 * value is therefore arbitrary. */
int      smt2_value_arbitrary(const Smt2Value *v);
/* Print as SMT-LIB: true/false, #b..., or a store chain for an array. Returns
 * 0 when the value cannot be printed (a slice of a declared nested array). */
int      smt2_value_print(FILE *out, const Smt2Value *v, const Smt2EvalHost *host);

#ifdef __cplusplus
}
#endif

#endif /* SMT2_EVAL_H */
