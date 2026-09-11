#ifndef ZSP_BBSOLVER_H
#define ZSP_BBSOLVER_H

#include <stdint.h>
#include <stddef.h>

#include "zsp_alloc.h"
#include "zsp_problem.h"
#include "zsp_sat.h"

/**
 * zsp_bbsolver — bit-blast theory back-end for dv-solve.
 *
 * Phase B.0 of the bitwuzla adoption plan. Takes a SolveProblem (built by
 * the SMT2 frontend or via the builder API), bit-blasts each constraint to
 * AIG via zsp_bitblast, Tseitin-encodes via zsp_aig_cnf, and solves with
 * zsp_sat (kissat). On SAT, reads back integer values for each declared
 * variable.
 *
 * Single-shot, non-incremental: one zsp_bbsolver_t per check-sat call.
 *
 * Width handling: widths are inferred bottom-up from VarSpec widths and
 * operator semantics. EXPR_CONST values are sized to the surrounding
 * context (the typical SMT2 frontend lowering uses 32-bit defaults; the
 * bbsolver respects the operand width context).
 *
 * Signedness: for the operand widths in arithmetic, we use the width of
 * each operand as-is. For BIN_LT/LTE/GT/GTE the signedness comes from
 * the VarSpec of either side: if any operand traces back to a signed
 * variable, we use signed comparison (SLT). Mixed-signedness yields a
 * conservative signed comparison.
 */

#define ZSP_BB_SAT      10
#define ZSP_BB_UNSAT    20
#define ZSP_BB_UNKNOWN  0
#define ZSP_BB_ERROR    (-1)

/* Encode-phase "ready to solve" sentinel. Deliberately distinct from every
 * ZSP_BB_* verdict above (esp. ZSP_BB_UNKNOWN == 0): the encode step defers an
 * unsupported construct by returning ZSP_BB_UNKNOWN, so "ready" must NOT also be
 * 0 or the caller would treat a deferral as success and solve the instance with
 * the unsupported (hard) constraint silently dropped — a bogus SAT. Callers
 * test `enc != ZSP_BB_ENCODE_READY` to detect a defer/error and propagate it. */
#define ZSP_BB_ENCODE_READY  1

typedef struct zsp_bbsolver_s zsp_bbsolver_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Create a bbsolver for the given problem. `alloc` may be NULL. The SAT
 * backend is env-driven (DV_SAT_BACKEND, default kissat). */
zsp_bbsolver_t *zsp_bbsolver_new(zsp_alloc_t *alloc, SolveProblem *problem);

/** Create a bbsolver with an explicit backend preference: prefer_cadical -1 =
 * env-driven (as zsp_bbsolver_new), 0 = force kissat, 1 = prefer CaDiCaL (falls
 * back to kissat when CaDiCaL is not compiled in). The cube engine uses 1 so it
 * gets retractable assumptions without the caller setting DV_SAT_BACKEND. */
zsp_bbsolver_t *zsp_bbsolver_new_backend(zsp_alloc_t *alloc,
                                         SolveProblem *problem,
                                         int prefer_cadical);

/** Free the bbsolver and all transient resources (AIG, SAT, bit-blaster). */
void zsp_bbsolver_free(zsp_bbsolver_t *bb);

/**
 * Bit-blast every constraint, encode CNF, run kissat.
 * `seed` perturbs the SAT search so repeated checks of the same problem can
 * return different satisfying models (0 = solver default).
 * Returns ZSP_BB_SAT / ZSP_BB_UNSAT / ZSP_BB_UNKNOWN / ZSP_BB_ERROR.
 */
int zsp_bbsolver_check(zsp_bbsolver_t *bb, uint64_t seed);

/**
 * Re-diversify an already-solved SAT instance with a fresh seed WITHOUT
 * re-solving. Re-runs only the don't-care flip-check over the cached model, so
 * repeated randomize()s of an identical problem produce different (still sound)
 * models at a fraction of a full bit-blast + SAT cost. The kept model is read
 * back via zsp_bbsolver_value / _value_wide as usual. Uses only solver-owned
 * state (AIG/CNF/vars/model) — safe even if the SolveProblem it was built from
 * has since been freed. Returns 0 on success, -1 if the instance is not SAT.
 */
int zsp_bbsolver_rediversify(zsp_bbsolver_t *bb, uint64_t seed);

/**
 * Incremental extension: keep one live instance across solves.
 *
 * zsp_bbsolver_assert bit-blasts and asserts one more predicate (an ExprRef
 * into the SolveProblem the solver was built from) into the running instance,
 * building/bounding any newly referenced variables; it does not solve.
 * zsp_bbsolver_resolve then re-solves, reusing the accumulated clause DB and —
 * on an incremental SAT backend (CaDiCaL) — the retained learned clauses.
 *
 * Monotonic add only: there is no scoped retraction (that needs selector
 * push/pop minted in the AIG var-id space; deliberately out of this layer).
 * This is the library foundation for incremental BMC reuse; the frontend
 * plumbing that drives it is a separate, held effort. See
 * docs/phase5_incremental_bitblast_scope.md.
 *
 * Requires an incremental SAT backend (CaDiCaL): kissat aborts on
 * add-after-solve, so on a non-incremental instance both calls return
 * ZSP_BB_ERROR without touching the solver and the caller must free + rebuild
 * (the existing one-shot path). Query via zsp_sat_is_incremental if needed.
 *
 * zsp_bbsolver_assert returns 0 on success, ZSP_BB_UNKNOWN for an unsupported
 * construct, or ZSP_BB_ERROR on a hard error / non-incremental backend.
 * zsp_bbsolver_resolve returns ZSP_BB_SAT/UNSAT/UNKNOWN/ERROR.
 */
int zsp_bbsolver_assert(zsp_bbsolver_t *bb, ExprRef pred_ref);
int zsp_bbsolver_resolve(zsp_bbsolver_t *bb, uint64_t seed);

/* Like zsp_bbsolver_resolve but WITHOUT don't-care diversification: reads back
 * the solver's raw SAT assignment (seed 0). Required by the lazy array
 * refinement loop, whose array-consistency check must see the true model --
 * diversify randomizes free/don't-care bits, which include lazily-unconstrained
 * array read vars, and would produce a spurious SAT. */
int zsp_bbsolver_resolve_raw(zsp_bbsolver_t *bb);

/**
 * Cube-and-conquer support (docs/cube_and_conquer_design.md, zsp_cube.[ch]).
 *
 * These split zsp_bbsolver_check's monolithic "encode then solve" into an
 * encode-once step plus repeated assumption-scoped solves over the same
 * bit-blasted instance — the substrate for search-space partitioning.
 *
 * zsp_bbsolver_prepare bit-blasts and CNF-encodes the whole problem WITHOUT
 * solving. Returns ZSP_BB_ENCODE_READY when ready to solve, or ZSP_BB_UNKNOWN /
 * ZSP_BB_ERROR for an unsupported construct / hard error (identical verdicts to
 * _check). "Ready" is intentionally not 0 — see ZSP_BB_ENCODE_READY above.
 *
 * zsp_bbsolver_is_incremental reports whether the SAT backend supports
 * retractable per-solve assumptions (CaDiCaL). Cube solving requires it; the
 * driver falls back to a single plain solve otherwise.
 *
 * zsp_bbsolver_split_lits fills `out` (up to `cap`) with candidate split
 * literals — the SAT variables backing bits of referenced problem variables —
 * and returns the count written. Each literal `v` yields two exhaustive cubes
 * {+v} and {-v}. Call after zsp_bbsolver_prepare.
 *
 * zsp_bbsolver_solve_assuming queues `lits` as assumptions (a cube), bounds the
 * search by `conflict_limit` (0 = unlimited), solves, and on SAT re-diversifies
 * so the model read back via zsp_bbsolver_value[_wide] is THIS cube's model.
 * Returns ZSP_BB_SAT/UNSAT/UNKNOWN/ERROR. On the incremental backend it may be
 * called repeatedly with different cubes (assumptions retract between solves).
 */
int zsp_bbsolver_prepare(zsp_bbsolver_t *bb, uint64_t seed);
int zsp_bbsolver_is_incremental(const zsp_bbsolver_t *bb);
uint32_t zsp_bbsolver_split_lits(zsp_bbsolver_t *bb, int32_t *out, uint32_t cap);

/**
 * Structural split (cube-and-conquer P1): fill `out` (up to `cap`) with one
 * assumption literal per disjunct of the widest surviving top-level `(or ...)`
 * constraint, and return the disjunct count. Because that `or` is asserted
 * true, the returned literals form an EXHAUSTIVE k-way cover (some disjunct
 * must hold), so a caller that tries all of them may soundly conclude UNSAT.
 * Returns 0 when there is no usable disjunction, or when the widest one has
 * more disjuncts than `cap` (a partial split would not be exhaustive). Call
 * after zsp_bbsolver_prepare.
 */
uint32_t zsp_bbsolver_or_split_lits(zsp_bbsolver_t *bb, int32_t *out, uint32_t cap);

/**
 * Cartesian structural split (cube-and-conquer P2.5): enumerate the surviving
 * top-level `(or ...)` constraints as GROUPS of disjunct assumption literals.
 * Flattened disjunct literals are written to `lits` (capacity `lits_cap`), and
 * `sizes[g]` receives the number of literals in group g; returns the group
 * count (<= `max_groups`). Only ors with arity in [2, per_group_cap] that fit
 * the buffers are emitted. Each group is individually exhaustive (its `or` is
 * asserted), so the caller's cartesian product across any subset of groups —
 * one disjunct chosen per group — is itself an EXHAUSTIVE partition: a caller
 * that proves every product cube UNSAT may soundly conclude UNSAT. This is the
 * granularity fix over zsp_bbsolver_or_split_lits (which splits only the single
 * widest or): folding K ors fixes K disjuncts per cube, shrinking each residual
 * enough that cubes actually resolve. Call after zsp_bbsolver_prepare.
 */
uint32_t zsp_bbsolver_or_groups(zsp_bbsolver_t *bb,
                                int32_t *lits, uint32_t lits_cap,
                                uint32_t *sizes, uint32_t max_groups,
                                uint32_t per_group_cap);
int zsp_bbsolver_solve_assuming(zsp_bbsolver_t *bb, const int32_t *lits,
                                uint32_t n, uint32_t conflict_limit);

/*
 * Parallel cube-and-conquer support (P2, zsp_cube.c).
 *
 * The parallel engine can't share one solver instance across threads (CaDiCaL
 * instances aren't thread-safe). Instead it clones the CNF into one private
 * solver per worker thread. These three calls make that possible:
 *
 * zsp_bbsolver_record_clauses turns on clause-DB recording on the primary
 * instance; call it BEFORE zsp_bbsolver_prepare so the whole encode (and any
 * split-literal encoding done afterwards) is captured.
 *
 * zsp_bbsolver_clause_db returns the recorded literal stream (0-terminated
 * clauses, verbatim var ids) and, via *max_var, the highest variable id — the
 * two inputs a worker needs to rebuild an identical instance with zsp_sat_add.
 * Returns NULL / *n_lits=0 if recording wasn't started or hit OOM (caller then
 * falls back to a single-threaded solve — never a wrong answer).
 *
 * zsp_bbsolver_install_worker_model reads the winning worker's model straight
 * out of its solver instance (AIG node id == SAT var id, an identity mapping)
 * into this solver's node_val buffer, so zsp_bbsolver_value[_wide] and
 * (get-value) report the parallel winner's model. Returns 0 on success. It
 * needs only the shared AIG (owned by this solver), not the worker's — the
 * worker's zsp_sat_t is read but not retained.
 */
void zsp_bbsolver_record_clauses(zsp_bbsolver_t *bb);
const int32_t *zsp_bbsolver_clause_db(zsp_bbsolver_t *bb, size_t *n_lits,
                                      int32_t *max_var);
int zsp_bbsolver_install_worker_model(zsp_bbsolver_t *bb, zsp_sat_t *worker_sat);

/**
 * Soft-aware MaxSAT serve (DSE-2). Keeps the maximal priority-respecting set of
 * the problem's soft constraints (`softs_head`), mirroring the primary engine's
 * relaxation policy: all softs kept, dropping the lowest-preference (highest
 * priority value) one per UNSAT until SAT. Creates the bbsolver(s) internally
 * (the SAT layer is non-incremental). On ZSP_BB_SAT, *out_bb receives the solved
 * bbsolver — the caller reads the model via zsp_bbsolver_value[_wide] and frees
 * it with zsp_bbsolver_free. On UNSAT/UNKNOWN/ERROR, *out_bb is NULL.
 *
 * If out_keep is non-NULL it receives, on SAT, the kept-soft mask in softs_head
 * walk order (min(n_softs, keep_cap) bytes; 1 = kept-as-hard). The caller passes
 * this mask to zsp_bbsolver_set_soft_keep on each sampler bbsolver so the
 * distribution draw enforces exactly the kept softs (the model order is fixed
 * by a deterministic rebuild, so the mask stays valid across rebuilds).
 *
 * Returns ZSP_BB_SAT / ZSP_BB_UNSAT / ZSP_BB_UNKNOWN / ZSP_BB_ERROR.
 */
int zsp_bbsolver_check_maxsat(zsp_alloc_t *alloc, SolveProblem *problem,
                              uint64_t seed, zsp_bbsolver_t **out_bb,
                              uint8_t *out_keep, uint32_t keep_cap);

/**
 * Set the kept-soft mask on a bbsolver before zsp_bbsolver_check, so the check
 * encodes the selected softs (softs_head walk order; 1 = keep) as hard top-level
 * assertions. NULL/0 restores the legacy soft-less behavior. The array is
 * borrowed — it must outlive the check.
 */
void zsp_bbsolver_set_soft_keep(zsp_bbsolver_t *bb, const uint8_t *keep,
                                uint32_t n);

/**
 * After a SAT result, read back the integer value for variable `var_id`.
 * Returns 0 on success; sets *out_value to the model value.
 * Returns non-zero (and leaves *out_value unmodified) if the variable
 * isn't in the problem or the solver isn't in a SAT state.
 */
int zsp_bbsolver_value(zsp_bbsolver_t *bb, uint32_t var_id, int64_t *out_value);

/**
 * After a SAT result, read back the full (possibly >64-bit) model value for
 * variable `var_id` into little-endian 64-bit limbs: `limbs[0]` is bits [0,63],
 * `limbs[1]` is bits [64,127], etc. Writes `min(n_limbs, ceil(width/64))` limbs
 * and zero-fills any remaining requested limbs. The value is the raw unsigned
 * bit pattern (no sign extension across limbs — the caller applies signedness
 * from the var's declared width). Use this instead of zsp_bbsolver_value() for
 * width > 64, where the int64 reader cannot represent the value.
 * Returns 0 on success; non-zero if the variable isn't in the problem or the
 * solver isn't in a SAT state.
 */
int zsp_bbsolver_value_wide(zsp_bbsolver_t *bb, uint32_t var_id,
                            uint64_t *limbs, uint32_t n_limbs);

/** Diagnostic counters (post-solve). */
uint64_t zsp_bbsolver_num_aig_ands(const zsp_bbsolver_t *bb);
uint64_t zsp_bbsolver_num_sat_clauses(const zsp_bbsolver_t *bb);
uint64_t zsp_bbsolver_num_sat_vars(const zsp_bbsolver_t *bb);

#ifdef __cplusplus
}
#endif

#endif /* ZSP_BBSOLVER_H */
