/* Linear-equality presolve (dvs_presolve.c): the constants it adds, the
 * wraps and problem kinds it leaves alone, and that both engines solve what
 * it adds. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_problem.h"
#include "dvs_presolve.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

typedef dvs_builder_t B;

static dvs_expr_t var(B *b, uint32_t v) { return dvs_builder_expr_var(b, v); }
static dvs_expr_t bin(B *b, int op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}
/* An 8-bit variable widened to 32 bits, as Verilator writes an operand. */
static dvs_expr_t wide(B *b, uint32_t v, int s) {
    return dvs_builder_expr_sv_cast(b, var(b, v), 8, 32, (uint8_t)s, (uint8_t)s);
}
/* v0 + v1 + ... at 32 bits. */
static dvs_expr_t sum32(B *b, uint32_t v0, uint32_t n) {
    dvs_expr_t acc = wide(b, v0, 0);
    for (uint32_t i = 1; i < n; i++) acc = bin(b, DVS_BIN_ADD, acc, wide(b, v0 + i, 0));
    return acc;
}

/* A malloc'ed copy of the finalized problem, as a handle holds it. */
static dvs_problem_t *finish(B *b, size_t *sz) {
    dvs_problem_t *p = dvs_builder_finalize(b, sz);
    dvs_problem_t *c = malloc(*sz);
    memcpy(c, p, *sz);
    dvs_builder_free_problem(b, p, *sz);
    return c;
}

/* a[4], b[4] (8-bit): sum(a) == sum(b), sum(a) + sum(b) == 400, each sum
 * built separately, as Verilator does. */
static B *sum_partition(int explicit_) {
    B *b = dvs_builder_create(0, NULL);
    if (explicit_) dvs_builder_set_explicit(b);
    for (uint32_t i = 0; i < 8; i++) dvs_builder_add_var(b, i, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, bin(b, DVS_BIN_EQ, sum32(b, 0, 4), sum32(b, 4, 4)));
    dvs_builder_add_constraint(b, bin(b, DVS_BIN_EQ,
        bin(b, DVS_BIN_ADD, sum32(b, 0, 4), sum32(b, 4, 4)),
        dvs_builder_expr_const_sized(b, 400, 0, 32)));
    return b;
}

static void check_sum_partition(void) {
    B *b = sum_partition(1);
    size_t sz;
    dvs_problem_t *p = finish(b, &sz);
    uint32_t n0 = p->n_constraints;
    CHECK(dvs_presolve_linear(&p, &sz) == 2 && p->n_constraints == n0 + 2,
          "sum(a) == 200 and sum(b) == 200 added");
    char why[128] = "";
    CHECK(dvs_problem_check_image(p, sz, why, sizeof why) == 0, "result is a valid image");
    free(p);

    p = finish(b, &sz);
    setenv("DVS_PRESOLVE", "0", 1);
    CHECK(dvs_presolve_linear(&p, &sz) == 0, "DVS_PRESOLVE=0 turns it off");
    unsetenv("DVS_PRESOLVE");
    free(p);
    dvs_builder_destroy(b);

    b = sum_partition(0);
    p = finish(b, &sz);
    CHECK(dvs_presolve_linear(&p, &sz) == 0, "a problem that is not EXPLICIT is left alone");
    free(p);
    dvs_builder_destroy(b);
}

/* Both engines solve the presolved problem: every draw has both sums 200. */
static void check_solved(int engine, const char *msg) {
    B *b = sum_partition(1);
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_handle_set_engine(h, engine);
    uint8_t v[8];
    dvs_ref_t refs[8];
    for (int i = 0; i < 8; i++) refs[i] = (dvs_ref_t){ &v[i], 8, 0, DVS_REF_SCALAR, 0 };
    int ok = 1;
    for (uint64_t seed = 1; seed <= 50; seed++) {
        ok &= dvs_randomize(h, refs, NULL, seed) == DVS_RANDOMIZE_OK
              && v[0] + v[1] + v[2] + v[3] == 200 && v[4] + v[5] + v[6] + v[7] == 200;
    }
    CHECK(ok, msg);
    dvs_handle_free(h);
}

/* x, y (8-bit) with x == y and x + y == 10 at 8 bits: the sum wraps
 * (x = y = 133 is a solution), so nothing may be derived. */
static void check_wrap_left_alone(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, bin(b, DVS_BIN_EQ, var(b, 0), var(b, 1)));
    dvs_builder_add_constraint(b, bin(b, DVS_BIN_EQ, bin(b, DVS_BIN_ADD, var(b, 0), var(b, 1)),
                                      dvs_builder_expr_const_sized(b, 10, 0, 8)));
    size_t sz;
    dvs_problem_t *p = finish(b, &sz);
    CHECK(dvs_presolve_linear(&p, &sz) == 0, "an 8-bit sum that can wrap gives no row");
    free(p);
    dvs_builder_destroy(b);
}

/* x, y (8-bit signed): x - y == 0, x + y == -20 at 32 bits: x == y == -10. */
static void check_signed(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    dvs_builder_add_var(b, 0, 8, 1, -128, 127);
    dvs_builder_add_var(b, 1, 8, 1, -128, 127);
    dvs_builder_add_constraint(b, bin(b, DVS_BIN_EQ,
        bin(b, DVS_BIN_SUB, wide(b, 0, 1), wide(b, 1, 1)),
        dvs_builder_expr_const_sized(b, 0, 1, 32)));
    dvs_builder_add_constraint(b, bin(b, DVS_BIN_EQ,
        bin(b, DVS_BIN_ADD, wide(b, 0, 1), wide(b, 1, 1)),
        dvs_builder_expr_const_sized(b, -20, 1, 32)));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    int8_t x = 0, y = 0;
    dvs_ref_t refs[2] = { {&x, 8, 1, DVS_REF_SCALAR, 0}, {&y, 8, 1, DVS_REF_SCALAR, 0} };
    CHECK(dvs_randomize(h, refs, NULL, 1) == DVS_RANDOMIZE_OK && x == -10 && y == -10,
          "signed: x == y == -10");
    dvs_handle_free(h);
}

int main(void) {
    check_sum_partition();
    check_solved(DVS_ENGINE_CDCL, "CDCL: both sums 200 on every call");
    check_solved(DVS_ENGINE_BITBLAST, "bit-blast: both sums 200 on every call");
    check_wrap_left_alone();
    check_signed();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
