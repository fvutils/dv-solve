/* One context, two engines (src/c/dvs_engine.c): dvs_solver_compile chooses,
 * the caller makes no routing call. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

static dvs_expr_t V(dvs_builder_t *b, uint32_t v) { return dvs_builder_expr_var(b, v); }
static dvs_expr_t K(dvs_builder_t *b, int64_t c) { return dvs_builder_expr_const(b, c, 0); }
static dvs_expr_t OP(dvs_builder_t *b, int op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}

/* simbench arith.mul_div_64: p == a * b, p / b == a; a in [1, 2^32-1],
 * b in [1, 2^16-1] (64-bit unsigned vars 0, 1, 2). CDCL compiles it since
 * its div propagator reads unsigned 64-bit values as uint64. */
static dvs_problem_t *mul_div(dvs_builder_t *b, size_t *sz) {
    dvs_builder_add_var(b, 0, 64, 0, 1, 0xFFFFFFFFll);
    dvs_builder_add_var(b, 1, 64, 0, 1, 0xFFFF);
    dvs_builder_add_var(b, 2, 64, 0, 0, -1);
    dvs_builder_add_constraint(b, OP(b, DVS_BIN_EQ, V(b, 2), OP(b, DVS_BIN_MUL, V(b, 0), V(b, 1))));
    dvs_builder_add_constraint(b, OP(b, DVS_BIN_EQ, OP(b, DVS_BIN_DIV, V(b, 2), V(b, 1)), V(b, 0)));
    return dvs_builder_finalize(b, sz);
}

/* q == a SDIV b, a < 0 (signed): 64-bit a, q; b in [1, 2^16-1]. CDCL
 * declines a signed 64-bit division (its zero-divisor guard is a signed
 * 64-bit ite), so AUTO takes it on bit-blast. */
static dvs_problem_t *sdiv64(dvs_builder_t *b, size_t *sz) {
    dvs_builder_add_var(b, 0, 64, 0, 0, -1);
    dvs_builder_add_var(b, 1, 64, 0, 1, 0xFFFF);
    dvs_builder_add_var(b, 2, 64, 0, 0, -1);
    dvs_builder_add_constraint(b, OP(b, DVS_BIN_SLT, V(b, 0), K(b, 0)));
    dvs_builder_add_constraint(b, OP(b, DVS_BIN_EQ, V(b, 2), OP(b, DVS_BIN_SDIV, V(b, 0), V(b, 1))));
    return dvs_builder_finalize(b, sz);
}

/* (x * 3) ^ y == 0x55, 8-bit x, y: a circuit CDCL compiles whole. */
static dvs_problem_t *circuit(dvs_builder_t *b, size_t *sz) {
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, OP(b, DVS_BIN_EQ,
        OP(b, DVS_BIN_BXOR, OP(b, DVS_BIN_MUL, V(b, 0), K(b, 3)), V(b, 1)), K(b, 0x55)));
    return dvs_builder_finalize(b, sz);
}

/* x < y, 8-bit: orderings stay on CDCL. */
static dvs_problem_t *ordering(dvs_builder_t *b, size_t *sz) {
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, OP(b, DVS_BIN_LT, V(b, 0), V(b, 1)));
    return dvs_builder_finalize(b, sz);
}

static dvs_solve_opts_t opts(uint64_t seed) {
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    o.seed = seed;
    o.fair_pick = 1;
    return o;
}

int main(void) {
    size_t sz;
    dvs_builder_t *b = dvs_builder_create(0, NULL);

    /* AUTO takes a problem CDCL can't compile, on bit-blast. */
    dvs_problem_t *p = sdiv64(b, &sz);
    dvs_ctx_t *c = dvs_solver_new(NULL);
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_AUTO, "a new context is AUTO");
    CHECK(dvs_solver_compile(c, p) == DVS_COMPILE_OK, "sdiv64: compiles under AUTO");
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_BITBLAST, "sdiv64: on bit-blast");
    CHECK(dvs_solver_set_engine(c, DVS_ENGINE_CDCL) == -1, "engine fixed once compiled");
    int ok = 1, varied = 0;
    uint64_t first_a = 0;
    for (uint64_t s = 1; s <= 50; s++) {
        dvs_solver_reset(c);
        dvs_solve_opts_t o = opts(s);
        if (dvs_solver_solve(c, &o) != DVS_SOLVE_OK) { ok = 0; break; }
        int64_t a = dvs_solver_get_value(c, 0), bb = dvs_solver_get_value(c, 1),
                q = dvs_solver_get_value(c, 2);
        if (a >= 0 || bb < 1 || bb > 0xFFFF || q != a / bb) ok = 0;
        if (dvs_solver_validate_model(c, p, NULL) != 0) ok = 0;
        if (s == 1) first_a = (uint64_t)a; else varied |= (uint64_t)a != first_a;
    }
    CHECK(ok, "sdiv64: 50 solves, q == a / b truncated, validate_model clean");
    CHECK(varied, "sdiv64: a varies");
    CHECK(dvs_solver_pin_var(c, 0, -7) == -1, "sdiv64: a pin fails (CDCL could not compile it)");
    dvs_solver_free(c);

    /* CDCL asked for: the incomplete compile is reported, as before. */
    c = dvs_solver_new(NULL);
    CHECK(dvs_solver_set_engine(c, DVS_ENGINE_CDCL) == 0, "CDCL asked for");
    CHECK(dvs_solver_compile(c, p) > 0, "sdiv64 on CDCL: compile incomplete");
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_CDCL, "sdiv64 on CDCL: stays CDCL");
    dvs_solver_free(c);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_reset(b);

    /* mul_div_64 stays on CDCL under AUTO: unsigned 64-bit division compiles. */
    p = mul_div(b, &sz);
    c = dvs_solver_new(NULL);
    CHECK(dvs_solver_compile(c, p) == DVS_COMPILE_OK, "mul_div: compiles under AUTO");
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_CDCL, "mul_div: on CDCL");
    ok = 1;
    int seen_hi = 0;
    varied = 0;
    for (uint64_t s = 1; s <= 50; s++) {
        dvs_solver_reset(c);
        dvs_solve_opts_t o = opts(s);
        if (dvs_solver_solve(c, &o) != DVS_SOLVE_OK) { ok = 0; break; }
        uint64_t a = (uint64_t)dvs_solver_get_value(c, 0), bb = (uint64_t)dvs_solver_get_value(c, 1),
                 pp = (uint64_t)dvs_solver_get_value(c, 2);
        if (a < 1 || a > 0xFFFFFFFFu || bb < 1 || bb > 0xFFFF || pp != a * bb) ok = 0;
        if (dvs_solver_validate_model(c, p, NULL) != 0) ok = 0;
        if (s == 1) first_a = a; else varied |= a != first_a;
        seen_hi |= a >> 31;
    }
    CHECK(ok, "mul_div: 50 solves, p == a*b, in bounds, validate_model clean");
    CHECK(varied && seen_hi, "mul_div: a varies, reaching the top half of its range");
    dvs_solver_reset(c);
    CHECK(dvs_solver_pin_var(c, 0, 7) == 0, "mul_div: a pin is accepted (CDCL)");
    dvs_solve_opts_t o7 = opts(1);
    CHECK(dvs_solver_solve(c, &o7) == DVS_SOLVE_OK && dvs_solver_get_value(c, 0) == 7
          && dvs_solver_validate_model(c, p, NULL) == 0, "mul_div: solves under the pin");
    dvs_solver_free(c);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_reset(b);

    /* A circuit CDCL compiles whole stays on CDCL under AUTO; asked for,
     * bit-blast takes it, and a pin brings it back to CDCL for good. */
    p = circuit(b, &sz);
    c = dvs_solver_new(NULL);
    CHECK(dvs_solver_compile(c, p) == DVS_COMPILE_OK, "circuit: compiles");
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_CDCL, "circuit: CDCL first");
    dvs_solver_free(c);
    c = dvs_solver_new(NULL);
    CHECK(dvs_solver_set_engine(c, DVS_ENGINE_BITBLAST) == 0, "circuit: bit-blast asked for");
    CHECK(dvs_solver_compile(c, p) == DVS_COMPILE_OK, "circuit: compiles for bit-blast");
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_BITBLAST, "circuit: on bit-blast");
    dvs_solve_opts_t o = opts(3);
    ok = dvs_solver_solve(c, &o) == DVS_SOLVE_OK
      && ((((uint64_t)dvs_solver_get_value(c, 0) * 3u) ^ (uint64_t)dvs_solver_get_value(c, 1)) & 0xFF) == 0x55;
    CHECK(ok, "circuit: a bit-blast solution holds");
    dvs_solver_reset(c);
    CHECK(dvs_solver_pin_var(c, 0, 5) == 0, "circuit: pin accepted");
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_CDCL, "circuit: the pin moved it to CDCL");
    o = opts(4);
    ok = dvs_solver_solve(c, &o) == DVS_SOLVE_OK && dvs_solver_get_value(c, 0) == 5
      && dvs_solver_get_value(c, 1) == (0x55 ^ 15);
    CHECK(ok, "circuit: pinned x == 5 gives y == 0x55 ^ 15");
    dvs_solver_free(c);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_reset(b);

    /* Orderings stay on CDCL. */
    p = ordering(b, &sz);
    c = dvs_solver_new(NULL);
    CHECK(dvs_solver_compile(c, p) == DVS_COMPILE_OK, "ordering: compiles");
    CHECK(dvs_solver_engine(c) == DVS_ENGINE_CDCL, "ordering: on CDCL");
    dvs_solver_free(c);

    /* A bounded (embedded) context starts at CDCL. */
    size_t bsz = 1u << 20;
    void *buf = aligned_alloc(64, bsz);
    c = dvs_solver_create(buf, bsz, NULL);
    CHECK(c && dvs_solver_engine(c) == DVS_ENGINE_CDCL, "dvs_solver_create: CDCL");
    if (c) dvs_solver_destroy(c);
    free(buf);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_reset(b);

    dvs_builder_destroy(b);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
