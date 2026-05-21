#ifndef ZSP_LCG_H
#define ZSP_LCG_H

/*
 * Lazy Clause Generation (LCG) for zuspec-solver.
 *
 * Maps integer variable domain changes to Boolean literals and supports
 * conflict-driven clause learning (CDCL) with 1UIP analysis.
 *
 * Literal encoding:
 *   A literal encodes "x >= v" or "x <= v" for integer variable x.
 *   Literal = (var_id << 2) | (is_ub << 1) | sign
 *     var_id: the integer variable
 *     is_ub:  0 = lower-bound literal (x >= v), 1 = upper-bound literal (x <= v)
 *     sign:   0 = positive, 1 = negated
 *
 *   Positive literal (x >= v): true when var_lo(x) >= v
 *   Negated literal !(x >= v): true when var_hi(x) < v, i.e., x <= v-1
 *
 * Simplified encoding for this implementation:
 *   We use a compact representation where a literal is a (var_id, bound_kind, value)
 *   triple stored as a 16-byte struct. Clauses are arrays of literals.
 */

#include <stdint.h>
#include "zsp_propagator.h"  /* PropResult */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SolveCtx SolveCtx;
typedef struct Propagator Propagator;

/* ================================================================== */
/* Literal: an integer bound assertion                                 */
/* ================================================================== */

typedef struct {
    uint32_t var_id;    /* integer variable ID */
    int32_t  bound;     /* bound value         */
    uint8_t  is_lb;     /* 1 = x >= bound, 0 = x <= bound */
    uint8_t  _pad[3];
} Literal;              /* 12 bytes */

/* A literal is true under the current domains iff:
 *   is_lb && var_lo(x) >= bound, or
 *  !is_lb && var_hi(x) <= bound
 * A literal is false iff:
 *   is_lb && var_hi(x) < bound, or
 *  !is_lb && var_lo(x) > bound
 */
static inline int literal_is_true(const SolveCtx *ctx, Literal lit);
static inline int literal_is_false(const SolveCtx *ctx, Literal lit);

/* Negate a literal: (x >= v) becomes (x <= v-1), (x <= v) becomes (x >= v+1) */
static inline Literal literal_negate(Literal lit) {
    Literal neg;
    neg.var_id = lit.var_id;
    neg.is_lb  = !lit.is_lb;
    neg.bound  = lit.is_lb ? lit.bound - 1 : lit.bound + 1;
    neg._pad[0] = neg._pad[1] = neg._pad[2] = 0;
    return neg;
}

/* ================================================================== */
/* Clause: disjunction of literals                                     */
/* ================================================================== */

typedef struct {
    uint32_t n_lits;    /* number of literals */
    uint32_t lbd;       /* literal block distance (for clause quality) */
    uint32_t watch0;    /* index of first watched literal */
    uint32_t watch1;    /* index of second watched literal */
    /* Literal lits[n_lits] follow immediately */
} Clause;

/* ================================================================== */
/* ClauseDB: clause database with 2-watched literal scheme             */
/* ================================================================== */

#define CLAUSE_DB_INIT_CAP  4096u
#define MAX_CLAUSE_LITS     64u

/* Watch list entry: points to a clause that watches a given variable */
typedef struct WatchEntry {
    uint32_t           clause_idx;  /* index into clauses array */
    struct WatchEntry *next;        /* linked list              */
} WatchEntry;

typedef struct {
    /* Clause storage: array of pointers to variable-length Clause structs */
    Clause  **clauses;
    uint32_t  n_clauses;
    uint32_t  clauses_cap;

    /* Per-variable watch lists (one for LB events, one for UB events) */
    WatchEntry **watch_lb;   /* watch_lb[var_id] -> linked list */
    WatchEntry **watch_ub;   /* watch_ub[var_id] -> linked list */
    uint32_t     n_watch_vars;

    /* Allocation arena for clauses and watch entries */
    uint8_t  *arena;
    uint32_t  arena_used;
    uint32_t  arena_cap;

    /* Statistics */
    uint64_t  n_propagations;
    uint64_t  n_conflicts;
} ClauseDB;

/** Initialize a clause database.
 *  @param n_vars  Number of integer variables (for watch list sizing). */
int clause_db_init(ClauseDB *db, uint32_t n_vars);

/** Destroy the clause database and free all memory. */
void clause_db_destroy(ClauseDB *db);

/** Add a learned clause. Returns clause index, or UINT32_MAX on failure.
 *  The clause is copied into the arena. */
uint32_t clause_db_add(ClauseDB *db, uint32_t n_lits, const Literal *lits,
                        uint32_t lbd);

/** Remove clauses with LBD > threshold (garbage collection). */
void clause_db_gc(ClauseDB *db, uint32_t lbd_threshold);

/* ================================================================== */
/* Explanation callback                                                */
/*                                                                     */
/* Each propagator that supports LCG must provide an explain callback. */
/* Given a trail entry, it produces a set of literals that imply the   */
/* bound change recorded in the entry.                                 */
/*                                                                     */
/* The explanation is: if all literals in the clause are true, then    */
/* the bound change must hold.  Formally:                             */
/*   (lit1 AND lit2 AND ... AND litN) => (var_id >= new_bound)        */
/* which is equivalent to the clause:                                  */
/*   NOT(lit1) OR NOT(lit2) OR ... OR NOT(litN) OR (var_id >= new_bound) */
/* ================================================================== */

/* Maximum number of literals in a single explanation */
#define MAX_EXPLAIN_LITS 32u

typedef struct Explanation {
    uint32_t n_lits;
    Literal  lits[MAX_EXPLAIN_LITS];
} Explanation;

/**
 * Explanation callback type.
 *
 * @param self       The propagator being asked to explain.
 * @param ctx        Solver context.
 * @param var_id     Variable whose bound was tightened.
 * @param is_lb      1 if explaining a lower-bound tighten, 0 for upper-bound.
 * @param new_bound  The bound value that was inferred.
 * @param out        Output: the literals that imply this bound change.
 * @return 0 on success, -1 if the propagator cannot explain.
 */
typedef int (*ExplainFunc)(Propagator *self, SolveCtx *ctx,
                            uint32_t var_id, uint8_t is_lb,
                            int64_t new_bound, Explanation *out);

/* ================================================================== */
/* VSIDS: Variable State Independent Decaying Sum                     */
/* ================================================================== */

typedef struct {
    double  *activity;    /* per-variable activity score */
    uint32_t n_vars;
    double   var_inc;     /* current increment (grows with decay) */
    double   var_decay;   /* decay factor (e.g. 0.95) */
} VSIDS;

int  vsids_init(VSIDS *vs, uint32_t n_vars);
void vsids_destroy(VSIDS *vs);
void vsids_bump(VSIDS *vs, uint32_t var_id);
void vsids_decay(VSIDS *vs);
uint32_t vsids_pick(const VSIDS *vs, const SolveCtx *ctx);

/* ================================================================== */
/* LCG context: aggregates clause DB, VSIDS, and conflict analysis    */
/* ================================================================== */

typedef struct {
    ClauseDB  clause_db;
    VSIDS     vsids;
    int       enabled;       /* 1 if LCG is active */

    /* Conflict analysis working storage */
    uint8_t  *seen;          /* per-variable: seen during analysis */
    Literal  *seen_lit;      /* the explanation literal for each seen variable */
    Literal  *learnt_buf;    /* buffer for building learnt clause  */
    uint32_t  learnt_cap;

    /* Statistics */
    uint64_t  n_learnt;
    uint64_t  n_analyses;
} LCGCtx;

int  lcg_init(LCGCtx *lcg, uint32_t n_vars);
void lcg_destroy(LCGCtx *lcg);

/**
 * Perform 1UIP conflict analysis.
 *
 * Traces back from the conflict through trail entries, calling
 * propagator explain() callbacks, to derive a learnt clause.
 *
 * @param lcg        LCG context.
 * @param ctx        Solver context.
 * @param out_lits   Output: learned clause literals.
 * @param out_n      Output: number of literals in learned clause.
 * @param out_bt     Output: backtrack level.
 * @return 0 on success, -1 if analysis fails (e.g., no explanations).
 */
int lcg_analyze_conflict(LCGCtx *lcg, SolveCtx *ctx,
                          Literal *out_lits, uint32_t *out_n,
                          uint32_t *out_bt);

/* ================================================================== */
/* Event-driven clause propagation                                     */
/*                                                                     */
/* Called by ctx_tighten_lb/ub when a variable bound changes.          */
/* Checks only clauses that watch the affected variable.               */
/* ================================================================== */

/**
 * Notify the clause DB that variable var_id's lower bound was raised.
 * Checks clauses watching "var_id <= v" literals that may have become false.
 * @return PROP_OK or PROP_CONFLICT.
 */
PropResult clause_notify_lb(ClauseDB *db, SolveCtx *ctx,
                              uint32_t var_id, int64_t new_lb);

/**
 * Notify the clause DB that variable var_id's upper bound was lowered.
 * Checks clauses watching "var_id >= v" literals that may have become false.
 * @return PROP_OK or PROP_CONFLICT.
 */
PropResult clause_notify_ub(ClauseDB *db, SolveCtx *ctx,
                              uint32_t var_id, int64_t new_ub);

/**
 * Propagate unit clauses from the clause database.
 * Scans all learned clauses; enforces any that are unit.
 * @return PROP_OK or PROP_CONFLICT.
 */
PropResult clause_propagate(ClauseDB *db, SolveCtx *ctx);

#ifdef __cplusplus
}
#endif

/* Inline implementations (need SolveCtx definition) */
#include "zsp_ctx.h"

static inline int literal_is_true(const SolveCtx *ctx, Literal lit) {
    if (lit.is_lb)
        return var_lo64(ctx, &ctx->vars[lit.var_id]) >= lit.bound;
    else
        return var_hi64(ctx, &ctx->vars[lit.var_id]) <= lit.bound;
}

static inline int literal_is_false(const SolveCtx *ctx, Literal lit) {
    if (lit.is_lb)
        return var_hi64(ctx, &ctx->vars[lit.var_id]) < lit.bound;
    else
        return var_lo64(ctx, &ctx->vars[lit.var_id]) > lit.bound;
}

#endif /* ZSP_LCG_H */
