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

/**
 * Seed the SAT solver's randomness (random decisions / phase init) so repeated
 * solves of the same problem can return different satisfying models. Must be
 * called before zsp_sat_solve(). 0 reproduces the solver's default behavior.
 */
void zsp_sat_set_seed(zsp_sat_t *s, uint64_t seed);

/** Bound the search by conflict count. 0 (the default) means unlimited. */
void zsp_sat_set_conflict_limit(zsp_sat_t *s, uint32_t limit);

/** Bound the search by decision count. 0 (the default) means unlimited. */
void zsp_sat_set_decision_limit(zsp_sat_t *s, uint32_t limit);

/** Return number of clauses added (units + binary + general). */
uint64_t zsp_sat_num_clauses(const zsp_sat_t *s);

/** Return highest variable id seen so far. */
zsp_sat_var_t zsp_sat_max_var(const zsp_sat_t *s);

/* Phase B.1 step 3: clause-arena observers. Forward calls to the
 * underlying kissat fork. Used by telemetry now; substrate for the
 * future LevelMark integration that ties the SAT clause DB into the
 * dv-solve checkpoint system. */
size_t zsp_sat_arena_size_bytes(zsp_sat_t *s);
size_t zsp_sat_arena_capacity_bytes(zsp_sat_t *s);

/* Phase B.1 step 5 (plumbing slice): arena mark. Opaque to callers;
 * today aliases the kissat arena top in `ward` units. Saved into the
 * dv-solve LevelMark/CheckpointMark by the checkpoint code so a single
 * dv-solve checkpoint records the SAT-side arena position too.
 * No matching _rewind_to is exposed yet — sound rewind needs the
 * deep kissat refactor (invalidate watches/occurrences/etc.) which
 * this slice does not attempt. */
typedef size_t zsp_sat_arena_mark_t;
zsp_sat_arena_mark_t zsp_sat_arena_save_mark(zsp_sat_t *s);

/* Phase B.1 step 3 (incremental API surface — stub semantics).
 *
 * Kissat is non-incremental and the fork has not yet been adapted to
 * preserve state across solve calls. This block defines the public
 * surface that the bbsolver / step-5 LevelMark work will code against,
 * with conservative correct-but-non-incremental behavior today:
 *
 *   - push/pop track frame depth. If clauses were added inside a frame
 *     that is then popped, the solver is marked "tainted": the caller
 *     must discard the instance and rebuild (the next solve call returns
 *     ZSP_SAT_UNKNOWN). Use zsp_sat_is_tainted() to poll.
 *   - assume(lit) queues an assumption replayed as a unit clause at the
 *     next solve. One-shot: the assumed unit becomes permanent in the
 *     kissat instance, so a follow-on solve with conflicting assumptions
 *     will see them as hard constraints.
 *   - failed(lit) is reserved for the future real implementation that
 *     can report unsat-core membership. Always returns 0 in stub mode.
 *
 * When the deeper kissat refactor lands (snapshot trail / clause DB /
 * unit clauses on push, restore on pop; real assumption tracking), the
 * API stays the same — semantics tighten from "rebuild on taint" to
 * "true incremental rollback". */
void zsp_sat_push(zsp_sat_t *s);
void zsp_sat_pop(zsp_sat_t *s);
int  zsp_sat_push_depth(const zsp_sat_t *s);
int  zsp_sat_is_tainted(const zsp_sat_t *s);

void zsp_sat_assume(zsp_sat_t *s, zsp_sat_lit_t lit);
int  zsp_sat_failed(zsp_sat_t *s, zsp_sat_lit_t lit);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_SAT_H */
