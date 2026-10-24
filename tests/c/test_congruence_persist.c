/*
 * Item 3c, step 1/n: `_compile_congruence_facts` (dvs_compile.c) now
 * persists each var's merged congruence fact into ctx->cong_m/cong_r
 * (dvs_ctx.h) instead of discarding it after compile-time UNSAT detection.
 * This only checks the persisted data is correct; drawing `R + M*k` from it
 * in _pick_value (dvs_search.c) is a later step -- today's search behavior
 * (_pick_mod_aligned) is unchanged.
 */
#include <stdio.h>
#include <stdlib.h>

#include "dv_solve.h"
#include "dvs_ctx.h"

static int failures = 0;
#define CHECK(c) do { if (!(c)) { \
    fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); \
    failures++; } } while (0)

static dvs_expr_t V(dvs_builder_t *b, uint32_t id) { return dvs_builder_expr_var(b, id); }
static dvs_expr_t K(dvs_builder_t *b, int64_t v) { return dvs_builder_expr_const(b, v, 0); }
static dvs_expr_t BIN(dvs_builder_t *b, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}
static dvs_expr_t MOD_EQ(dvs_builder_t *b, uint32_t var_id, int64_t m, int64_t r) {
    return BIN(b, DVS_BIN_EQ, BIN(b, DVS_BIN_MOD, V(b, var_id), K(b, m)), K(b, r));
}

/* One mod fact, no link: x % 7 == 3 should persist as cong_m[x]=7, cong_r[x]=3. */
static void test_single_fact(void) {
    void *buf = malloc(64u << 10);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 1000);
    dvs_builder_add_constraint(b, MOD_EQ(b, 0, 7, 3));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_ctx_t *ctx = dvs_solver_create(buf, 64u << 10, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    CHECK(ctx->cong_m != NULL);
    CHECK(0 < ctx->cong_cap);
    CHECK(ctx->cong_m[0] == 7);
    CHECK(ctx->cong_r[0] == 3);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
}

/* Two mod facts on the same var CRT-merge: x%7==3 && x%11==4 -> x%77==59. */
static void test_crt_merged_fact(void) {
    void *buf = malloc(64u << 10);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 10000);
    dvs_builder_add_constraint(b, MOD_EQ(b, 0, 7, 3));
    dvs_builder_add_constraint(b, MOD_EQ(b, 0, 11, 4));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_ctx_t *ctx = dvs_solver_create(buf, 64u << 10, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    CHECK(ctx->cong_m != NULL);
    CHECK(ctx->cong_m[0] == 77);
    CHECK(ctx->cong_r[0] == 59);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
}

/* A fact carried across y == x + c (item 3b) persists on y too: x%7==3,
 * y==x+2, x in [0,100] (width 32: stays entirely in the no-wrap zone) ->
 * cong_m[y]=7, cong_r[y]=5. */
static void test_link_carried_fact(void) {
    void *buf = malloc(64u << 10);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 100);    /* x */
    dvs_builder_add_var(b, 1, 32, 0, 0, 1000);   /* y */
    dvs_builder_add_constraint(b, MOD_EQ(b, 0, 7, 3));
    dvs_builder_add_constraint(b,
        BIN(b, DVS_BIN_EQ, V(b, 1), BIN(b, DVS_BIN_ADD, V(b, 0), K(b, 2))));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_ctx_t *ctx = dvs_solver_create(buf, 64u << 10, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    CHECK(ctx->cong_m != NULL);
    CHECK(ctx->cong_m[0] == 7);
    CHECK(ctx->cong_r[0] == 3);
    CHECK(ctx->cong_m[1] == 7);
    CHECK(ctx->cong_r[1] == 5);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
}

/* No mod constraint anywhere: the table is never allocated (stays NULL),
 * same best-effort/no-cost-when-unused shape as dist_offsets/phase_save. */
static void test_no_fact_leaves_table_unallocated(void) {
    void *buf = malloc(64u << 10);
    dvs_block_alloc_t *ba = dvs_block_alloc_create(NULL, 1u << 16);
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 100);
    dvs_builder_add_constraint(b, BIN(b, DVS_BIN_GT, V(b, 0), K(b, 10)));
    size_t sz = 0;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_ctx_t *ctx = dvs_solver_create(buf, 64u << 10, ba);
    CHECK(ctx != NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_OK);
    CHECK(ctx->cong_m == NULL);
    CHECK(ctx->cong_r == NULL);
    CHECK(ctx->cong_cap == 0);
    dvs_solver_destroy(ctx);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_block_alloc_destroy(ba);
    free(buf);
}

int main(void) {
    test_single_fact();
    test_crt_merged_fact();
    test_link_carried_fact();
    test_no_fact_leaves_table_unallocated();
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("test_congruence_persist: OK\n");
    return 0;
}
