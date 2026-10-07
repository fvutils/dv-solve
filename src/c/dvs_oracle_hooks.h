/*
 * dvs_oracle_hooks -- the calls the solver API makes into the oracle check
 * (dvs_oracle.c). Each is a no-op unless an oracle is attached to the context
 * (ctx->oracle), so the cost when the check is off is one pointer test.
 */
#ifndef DVS_ORACLE_HOOKS_H
#define DVS_ORACLE_HOOKS_H

#include "dvs_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

double dvs_oracle_clock_ms(void);
void dvs_oracle_on_compile(dvs_ctx_t *ctx, const dvs_problem_t *p, int rc, double ms);
void dvs_oracle_on_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                         dvs_result_t r, double ms);
void dvs_oracle_on_reset(dvs_ctx_t *ctx);
void dvs_oracle_on_pin(dvs_ctx_t *ctx, uint32_t var_id, int64_t value, int rc);
void dvs_oracle_on_exclude(dvs_ctx_t *ctx, uint32_t var_id, int64_t value, int rc);
void dvs_oracle_on_add(dvs_ctx_t *ctx, const dvs_problem_t *p, int rc);
void dvs_oracle_on_checkpoint(dvs_ctx_t *ctx, int cp);
void dvs_oracle_on_restore(dvs_ctx_t *ctx, uint32_t cp);
void dvs_oracle_on_destroy(dvs_ctx_t *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DVS_ORACLE_HOOKS_H */
