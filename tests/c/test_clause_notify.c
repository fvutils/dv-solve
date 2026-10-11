/*
 * A clause notification for a variable the clause database does not watch
 * must not stop later notifications (B115).
 *
 * clause_notify_lb/_ub set a static reentrancy flag and then returned early,
 * flag still set, for a variable created after the watch lists were sized (an
 * auxiliary variable from an incremental assert). Every later notification in
 * the process then returned at once: learnt clauses stopped propagating, for
 * every solver. The flag is now per database and set only after the check.
 *
 * Also: a tightening made by a unit clause during a notification must still
 * reach the clauses that watch that variable (clause-to-clause chains). It
 * used to be dropped by the same reentrancy guard; it is now queued
 * (docs/cdcl_construct_plan.md 3.1).
 */
#include "dv_solve.h"
#include "dvs_ctx.h"
#include "dvs_lcg.h"
#include "dvs_propagator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

#define CTX_SIZE (1u << 20)

static Literal _ge(uint32_t var, int64_t bound) {
    Literal l;
    memset(&l, 0, sizeof l);
    l.var_id = var;
    l.is_lb = 1;
    l.bound = bound;
    return l;
}

static Literal _le(uint32_t var, int64_t bound) {
    Literal l = _ge(var, bound);
    l.is_lb = 0;
    return l;
}

/* x0 <= 4 makes (x0 >= 5 | x1 >= 5) unit: x1 >= 5. That makes
 * (x1 <= 4 | x2 >= 7) unit in turn: x2 >= 7, and then (x2 <= 6 | x0 >= 3)
 * gives x0 >= 3. All through the context's own clause database, from one
 * tightening. */
static void _chain(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    for (uint32_t i = 0; i < 3; i++) dvs_builder_add_var(b, i, 8, 0, 0, 255);
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    void *buf = calloc(1, CTX_SIZE);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, CTX_SIZE);
    dvs_ctx_t *ctx = dvs_solver_create(buf, CTX_SIZE, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);

    LCGCtx *lcg = (LCGCtx *)calloc(1, sizeof(LCGCtx));
    CHECK(lcg_init(lcg, 3) == 0);
    lcg->enabled = 1;
    ctx->lcg = lcg;                       /* freed by dvs_solver_destroy */
    Literal c1[2] = { _ge(0, 5), _ge(1, 5) };
    Literal c2[2] = { _le(1, 4), _ge(2, 7) };
    Literal c3[2] = { _le(2, 6), _ge(0, 3) };
    CHECK(clause_db_add(&lcg->clause_db, 2, c1, 2) != UINT32_MAX);
    CHECK(clause_db_add(&lcg->clause_db, 2, c2, 2) != UINT32_MAX);
    CHECK(clause_db_add(&lcg->clause_db, 2, c3, 2) != UINT32_MAX);

    CHECK(ctx_tighten_ub64(ctx, 0, 4) == PROP_OK);
    CHECK(dvs_var_lo64(ctx, 1) == 5);
    CHECK(dvs_var_lo64(ctx, 2) == 7);
    CHECK(dvs_var_lo64(ctx, 0) == 3);
    CHECK(lcg->clause_db.in_notify == 0 && lcg->clause_db.n_pend == 0);

    /* A clause found false through the chain is a conflict, and leaves the
     * queue empty: x1 >= 6 checks (x1 <= 4 | x2 <= 6), already false. */
    Literal c4[2] = { _le(1, 4), _le(2, 6) };
    CHECK(clause_db_add(&lcg->clause_db, 2, c4, 2) != UINT32_MAX);
    CHECK(ctx_tighten_lb64(ctx, 1, 6) == PROP_CONFLICT);
    CHECK(lcg->clause_db.in_notify == 0 && lcg->clause_db.n_pend == 0);

    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
}

int main(void) {
    _chain();

    dvs_builder_t *b = dvs_builder_create(0, NULL);
    for (uint32_t i = 0; i < 3; i++) dvs_builder_add_var(b, i, 8, 0, 0, 255);
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    void *buf = calloc(1, CTX_SIZE);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, CTX_SIZE);
    dvs_ctx_t *ctx = dvs_solver_create(buf, CTX_SIZE, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);

    /* Watch vars 0 and 1 only; var 2 stands for one created later. */
    ClauseDB db;
    CHECK(clause_db_init(&db, 2) == 0);
    Literal c[2] = { _ge(0, 5), _ge(1, 5) };     /* x0 >= 5  or  x1 >= 5 */
    CHECK(clause_db_add(&db, 2, c, 2) != UINT32_MAX);

    CHECK(clause_notify_ub(&db, ctx, 2, 0) == PROP_OK);   /* unwatched var */
    CHECK(clause_notify_lb(&db, ctx, 2, 9) == PROP_OK);

    /* x0 <= 4 falsifies the first literal: the clause is now unit. */
    CHECK(ctx_tighten_ub64(ctx, 0, 4) == PROP_OK);
    CHECK(clause_notify_ub(&db, ctx, 0, 4) == PROP_OK);
    CHECK(dvs_var_lo64(ctx, 1) == 5);

    /* A second database is not blocked by the first one being busy. */
    ClauseDB db2;
    CHECK(clause_db_init(&db2, 3) == 0);
    Literal c2[2] = { _ge(0, 5), _ge(2, 7) };    /* x0 >= 5  or  x2 >= 7 */
    CHECK(clause_db_add(&db2, 2, c2, 2) != UINT32_MAX);
    db.in_notify = 1;
    CHECK(clause_notify_ub(&db2, ctx, 0, 4) == PROP_OK);
    CHECK(dvs_var_lo64(ctx, 2) == 7);
    db.in_notify = 0;

    clause_db_destroy(&db);
    clause_db_destroy(&db2);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);

    if (failures) fprintf(stderr, "%d check(s) failed\n", failures);
    else printf("test_clause_notify: OK\n");
    return failures ? 1 : 0;
}
