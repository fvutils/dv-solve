#ifndef ZSP_BBSOLVER_H
#define ZSP_BBSOLVER_H

#include <stdint.h>

#include "zsp_alloc.h"
#include "zsp_problem.h"

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

typedef struct zsp_bbsolver_s zsp_bbsolver_t;

#ifdef __cplusplus
extern "C" {
#endif

/** Create a bbsolver for the given problem. `alloc` may be NULL. */
zsp_bbsolver_t *zsp_bbsolver_new(zsp_alloc_t *alloc, SolveProblem *problem);

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
