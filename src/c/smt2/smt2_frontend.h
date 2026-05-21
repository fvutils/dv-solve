#ifndef SMT2_FRONTEND_H
#define SMT2_FRONTEND_H

#include <stdio.h>
#include <stdint.h>
#include "zsp_builder.h"
#include "zsp_ctx.h"
#include "zsp_search.h"
#include "zsp_block_alloc.h"
#include "smt2/smt2_parser.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Variable table entry                                                */
/* ------------------------------------------------------------------ */

#define SMT2_MAX_NAME  64

typedef struct {
    char     name[SMT2_MAX_NAME];
    uint32_t var_id;
    uint8_t  width;
    uint8_t  is_signed;
    uint8_t  _pad[2];
} Smt2Var;

/* ------------------------------------------------------------------ */
/* TypedExpr -- expression reference with width tracking               */
/* ------------------------------------------------------------------ */

typedef struct {
    ExprRef  ref;
    uint16_t width;  /* bit width (up to 65535) */
} TypedExpr;

/* ------------------------------------------------------------------ */
/* Frontend state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    SolveProblemBuilder *builder;
    SolveCtx            *ctx;          /* NULL until check-sat */
    SolveProblem        *problem;      /* finalized; NULL until check-sat */
    size_t               problem_size;
    zsp_block_alloc_t   *block_alloc;
    void                *ctx_buf;      /* raw buffer for SolveCtx */
    size_t               ctx_buf_size;

    /* Symbol table */
    Smt2Var             *vars;
    uint32_t             n_vars;
    uint32_t             vars_cap;

    /* Configuration */
    int                  produce_models;
    uint64_t             seed;

    /* Result of last check-sat */
    SolveResult          last_result;
    int                  has_result;   /* 1 after check-sat */

    /* Output */
    FILE                *out;
    FILE                *err;

    /* Statistics */
    int                  print_stats;
} Smt2Frontend;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * Initialise a frontend.  Caller owns *fe and must call smt2_frontend_destroy().
 */
void smt2_frontend_init(Smt2Frontend *fe, FILE *out, FILE *err);

/**
 * Destroy frontend, freeing all internal allocations.
 */
void smt2_frontend_destroy(Smt2Frontend *fe);

/**
 * Dispatch one parsed S-expression (a top-level SMT-LIB2 command).
 *
 * @return  0 to continue, 1 on (exit), -1 on fatal error.
 */
int smt2_frontend_dispatch(Smt2Frontend *fe, const Sexpr *cmd);

#ifdef __cplusplus
}
#endif

#endif /* SMT2_FRONTEND_H */
