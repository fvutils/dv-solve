#ifndef ZSP_EXPLAIN_H
#define ZSP_EXPLAIN_H

#include "zsp_propagator.h"

struct Explanation;
typedef struct SolveCtx SolveCtx;

/**
 * Walk all propagators and set explain callbacks based on fire function.
 * Called from contradiction analysis before proof extraction.
 */
void contra_register_explanations(SolveCtx *ctx);

/* Explanation functions for standard propagators.
 * Wire these into propagator constructors to enable LCG. */

int explain_bounds_le(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_lt(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_eq(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_ne(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_add(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

#endif /* ZSP_EXPLAIN_H */

/* ---- Additional explain functions (Sprint 3) ---- */

int explain_bounds_mul(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_div(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_mod(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_unary_neg(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_implication(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_ite_value(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_in_set(Propagator *self, SolveCtx *ctx,
                    uint32_t var_id, uint8_t is_lb,
                    int64_t new_bound, struct Explanation *out);

int explain_disj_clause(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_sum_eq(Propagator *self, SolveCtx *ctx,
                    uint32_t var_id, uint8_t is_lb,
                    int64_t new_bound, struct Explanation *out);

int explain_all_different(Propagator *self, SolveCtx *ctx,
                           uint32_t var_id, uint8_t is_lb,
                           int64_t new_bound, struct Explanation *out);

int explain_reification(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_reification_eq(Propagator *self, SolveCtx *ctx,
                            uint32_t var_id, uint8_t is_lb,
                            int64_t new_bound, struct Explanation *out);

int explain_bit_slice(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_band(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_bor(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_bxor(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_bnot(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_shl(Propagator *self, SolveCtx *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_lshr(Propagator *self, SolveCtx *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_concat(Propagator *self, SolveCtx *ctx,
                           uint32_t var_id, uint8_t is_lb,
                           int64_t new_bound, struct Explanation *out);

int explain_countones(Propagator *self, SolveCtx *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_clog2(Propagator *self, SolveCtx *ctx,
                   uint32_t var_id, uint8_t is_lb,
                   int64_t new_bound, struct Explanation *out);
