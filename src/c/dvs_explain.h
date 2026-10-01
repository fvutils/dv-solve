#ifndef DVS_EXPLAIN_H
#define DVS_EXPLAIN_H

#include "dvs_propagator.h"

struct Explanation;
typedef struct dvs_ctx_s dvs_ctx_t;

/**
 * Walk all propagators and set explain callbacks based on fire function.
 * Called from contradiction analysis before proof extraction.
 */
void contra_register_explanations(dvs_ctx_t *ctx);

/**
 * Look up a short, human-readable name for a propagator fire-fn pointer.
 * Returns "?fire" for unknown pointers. Used by DV_LCG_TRACE.
 */
const char *prop_fire_name(PropResult (*fire)(Propagator *, dvs_ctx_t *));

/* Explanation functions for standard propagators.
 * Wire these into propagator constructors to enable LCG. */

int explain_bounds_le(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_lt(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_eq(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_ne(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_add(Propagator *self, dvs_ctx_t *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

#endif /* DVS_EXPLAIN_H */

/* ---- Additional explain functions (Sprint 3) ---- */

int explain_bounds_mul(Propagator *self, dvs_ctx_t *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_div(Propagator *self, dvs_ctx_t *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_mod(Propagator *self, dvs_ctx_t *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_unary_neg(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_implication(Propagator *self, dvs_ctx_t *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_ite_value(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_in_set(Propagator *self, dvs_ctx_t *ctx,
                    uint32_t var_id, uint8_t is_lb,
                    int64_t new_bound, struct Explanation *out);

int explain_disj_clause(Propagator *self, dvs_ctx_t *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_sum_eq(Propagator *self, dvs_ctx_t *ctx,
                    uint32_t var_id, uint8_t is_lb,
                    int64_t new_bound, struct Explanation *out);

int explain_all_different(Propagator *self, dvs_ctx_t *ctx,
                           uint32_t var_id, uint8_t is_lb,
                           int64_t new_bound, struct Explanation *out);

int explain_reification(Propagator *self, dvs_ctx_t *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_reification_eq(Propagator *self, dvs_ctx_t *ctx,
                            uint32_t var_id, uint8_t is_lb,
                            int64_t new_bound, struct Explanation *out);

int explain_bit_slice(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_bounds_band(Propagator *self, dvs_ctx_t *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_bor(Propagator *self, dvs_ctx_t *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_bxor(Propagator *self, dvs_ctx_t *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_bnot(Propagator *self, dvs_ctx_t *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_shl(Propagator *self, dvs_ctx_t *ctx,
                        uint32_t var_id, uint8_t is_lb,
                        int64_t new_bound, struct Explanation *out);

int explain_bounds_lshr(Propagator *self, dvs_ctx_t *ctx,
                         uint32_t var_id, uint8_t is_lb,
                         int64_t new_bound, struct Explanation *out);

int explain_bounds_concat(Propagator *self, dvs_ctx_t *ctx,
                           uint32_t var_id, uint8_t is_lb,
                           int64_t new_bound, struct Explanation *out);

int explain_countones(Propagator *self, dvs_ctx_t *ctx,
                       uint32_t var_id, uint8_t is_lb,
                       int64_t new_bound, struct Explanation *out);

int explain_clog2(Propagator *self, dvs_ctx_t *ctx,
                   uint32_t var_id, uint8_t is_lb,
                   int64_t new_bound, struct Explanation *out);
