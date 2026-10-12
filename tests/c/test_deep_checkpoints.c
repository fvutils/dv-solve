/*
 * Checkpoint depth: a growable context (dvs_solver_new) opens checkpoints
 * past the old fixed limit of 32, and a search under them still has its full
 * MAX_DECISION_DEPTH above its root. A fixed-pool context keeps 32. See
 * docs/dynamic_pool_plan.md, follow-ups.
 */
#include <stdio.h>
#include <stdlib.h>

#include "dv_solve.h"
#include "dvs_ctx.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

/* 240 decision variables: x_i != x_(i+1), 16-bit. Propagation fixes none of
 * them, so a solve makes about 240 decisions: more than the 224 levels the
 * old absolute limit left a search under 32 checkpoints. */
#define N_VARS 240u
#define DEPTH  1000u

static dvs_problem_t *_problem(dvs_builder_t *b, size_t *sz) {
    for (uint32_t i = 0; i < N_VARS; i++) dvs_builder_add_var(b, i, 16, 0, 0, 65535);
    for (uint32_t i = 0; i + 1 < N_VARS; i++)
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ,
            dvs_builder_expr_var(b, i), dvs_builder_expr_var(b, i + 1)));
    return dvs_builder_finalize(b, sz);
}

static int _model_ok(dvs_ctx_t *ctx) {
    for (uint32_t i = 0; i + 1 < N_VARS; i++)
        if (dvs_solver_get_value(ctx, i) == dvs_solver_get_value(ctx, i + 1)) return 0;
    return 1;
}

/* The pins made after checkpoints 0, 100, ... below `upto` hold. */
static int _pins_ok(dvs_ctx_t *ctx, uint32_t upto) {
    for (uint32_t c = 0; c < upto; c += 100)
        if (dvs_solver_get_value(ctx, c / 100) != (int64_t)c) return 0;
    return 1;
}

int main(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    size_t sz = 0;
    dvs_problem_t *p = _problem(b, &sz);
    CHECK(p != NULL);
    dvs_solve_opts_t opts = {0};
    opts.seed = 7;

    /* Growable: DEPTH nested checkpoints; after checkpoint 100k the scope
     * pins x_k = 100k (a pin lasts until its scope is restored). */
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    for (uint32_t d = 0; d < DEPTH; d++) {
        CHECK(dvs_solver_checkpoint(ctx) == (int)d);
        if (d % 100 == 0) {
            CHECK(dvs_solver_pin_var(ctx, d / 100, (int64_t)d) == 0);
            CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
            CHECK(_pins_ok(ctx, d + 1) && _model_ok(ctx));
        }
    }
    CHECK(ctx->n_checkpoints == DEPTH);
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
    CHECK(_pins_ok(ctx, DEPTH) && _model_ok(ctx));
    /* Back out in steps: restoring checkpoint d drops the pins made at d
     * and later, and keeps those before. */
    for (int d = (int)DEPTH - 1; d >= 0; d -= 150) {
        dvs_solver_restore(ctx, (uint32_t)d);
        CHECK(ctx->n_checkpoints == (uint32_t)d);
        CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK);
        CHECK(_pins_ok(ctx, (uint32_t)d) && _model_ok(ctx));
    }
    dvs_solver_restore(ctx, 0);
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK && _model_ok(ctx));
    dvs_solver_free(ctx);

    /* Fixed pool: 32 checkpoints, the 33rd refused, and a full-depth search
     * still runs under all 32. */
    size_t fsz = 4u << 20;
    void *buf = malloc(fsz);
    ctx = dvs_solver_create(buf, fsz, NULL);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    for (uint32_t d = 0; d < DVS_FIXED_CHECKPOINTS; d++)
        CHECK(dvs_solver_checkpoint(ctx) == (int)d);
    CHECK(dvs_solver_checkpoint(ctx) == -1);
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK && _model_ok(ctx));
    dvs_solver_restore(ctx, 0);
    CHECK(dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK && _model_ok(ctx));
    dvs_solver_destroy(ctx);
    free(buf);

    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    if (failures) fprintf(stderr, "%d failure(s)\n", failures);
    else printf("deep checkpoints: ok\n");
    return failures ? 1 : 0;
}
