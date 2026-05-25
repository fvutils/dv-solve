#ifndef ZSP_SAT_H
#define ZSP_SAT_H

#include <stdint.h>

#include "zsp_alloc.h"

/**
 * zsp_sat — dv-solve native SAT solver abstraction.
 *
 * Phase B.0: thin wrapper over kissat with the IPASIR-style add/solve/value
 * interface. Non-incremental — a single check-sat per solver lifetime.
 * Phase B.1 will replace the backend with a forked-and-modified kissat and
 * add assumptions + push/pop.
 *
 * Literal convention (matches DIMACS / kissat):
 *   var ids are 1..max_var (NEVER 0)
 *   positive literal => `+var`, negative literal => `-var`
 *   clauses are terminated by adding literal 0
 *
 * Solver status codes (match kissat / IPASIR):
 *   ZSP_SAT_SAT       = 10
 *   ZSP_SAT_UNSAT     = 20
 *   ZSP_SAT_UNKNOWN   = 0
 */

typedef struct zsp_sat_s zsp_sat_t;
typedef int32_t          zsp_sat_lit_t;
typedef int32_t          zsp_sat_var_t;

#define ZSP_SAT_SAT      10
#define ZSP_SAT_UNSAT    20
#define ZSP_SAT_UNKNOWN  0

#ifdef __cplusplus
extern "C" {
#endif

/** Create a fresh solver instance. `alloc` may be NULL to use libc malloc. */
zsp_sat_t *zsp_sat_new(zsp_alloc_t *alloc);

/** Release a solver instance and all its resources. */
void zsp_sat_free(zsp_sat_t *s);

/** Hint the solver about the maximum variable id we will use. Optional. */
void zsp_sat_reserve(zsp_sat_t *s, zsp_sat_var_t max_var);

/**
 * Add a literal to the current clause buffer. Passing 0 finalizes the
 * clause (matching DIMACS / IPASIR semantics).
 */
void zsp_sat_add(zsp_sat_t *s, zsp_sat_lit_t lit);

/** Convenience: add a unit clause `lit`. */
void zsp_sat_add_unit(zsp_sat_t *s, zsp_sat_lit_t lit);

/** Convenience: add a binary clause `(a v b)`. */
void zsp_sat_add_binary(zsp_sat_t *s, zsp_sat_lit_t a, zsp_sat_lit_t b);

/** Convenience: add a ternary clause `(a v b v c)`. */
void zsp_sat_add_ternary(zsp_sat_t *s, zsp_sat_lit_t a, zsp_sat_lit_t b, zsp_sat_lit_t c);

/**
 * Solve. Returns ZSP_SAT_SAT, ZSP_SAT_UNSAT, or ZSP_SAT_UNKNOWN (e.g. on
 * conflict-limit termination). Phase B.0 is non-incremental: calling solve
 * a second time after SAT is unsupported.
 */
int zsp_sat_solve(zsp_sat_t *s);

/**
 * Query the assigned value of `var` after a SAT result. Returns +var if true,
 * -var if false. The sign convention follows kissat_value().
 */
zsp_sat_lit_t zsp_sat_value(zsp_sat_t *s, zsp_sat_var_t var);

/** Bound the search by conflict count. 0 (the default) means unlimited. */
void zsp_sat_set_conflict_limit(zsp_sat_t *s, uint32_t limit);

/** Bound the search by decision count. 0 (the default) means unlimited. */
void zsp_sat_set_decision_limit(zsp_sat_t *s, uint32_t limit);

/** Return number of clauses added (units + binary + general). */
uint64_t zsp_sat_num_clauses(const zsp_sat_t *s);

/** Return highest variable id seen so far. */
zsp_sat_var_t zsp_sat_max_var(const zsp_sat_t *s);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_SAT_H */
