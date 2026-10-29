/* dvs_handle_t / dvs_randomize(): STATE vars pinned from refs, RAND values
 * written through refs, refs untouched when a call fails. */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "dv_solve.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

/* x (8-bit RAND, var 0) < y (8-bit STATE, var 1); z (16-bit RAND, var 2) > 1000. */
static dvs_handle_t *make(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_var(b, 2, 16, 0, 0, 65535);
    dvs_builder_set_var_kind(b, 1, DVS_VAR_STATE);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 1)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT,
        dvs_builder_expr_var(b, 2), dvs_builder_expr_const(b, 1000, 0)));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);   /* the handle keeps its own copy */
    dvs_builder_destroy(b);
    return h;
}

int main(void) {
    dvs_handle_t *h = make();
    CHECK(h != NULL, "handle made");
    if (!h) return 1;
    CHECK(dvs_handle_n_vars(h) == 3, "n_vars == 3");

    /* x and y sit between guard bytes: a write must touch only its own word. */
    struct { uint8_t g0, x, g1, y; uint16_t z; uint8_t g2; } m;
    memset(&m, 0xAA, sizeof m);
    dvs_ref_t refs[3] = {
        {&m.x, 8, 0, DVS_REF_SCALAR, 0},
        {&m.y, 8, 0, DVS_REF_SCALAR, 0},
        {&m.z, 16, 0, DVS_REF_SCALAR, 0},
    };

    int ok = 1, seen[256] = {0}, distinct = 0;
    m.y = 10;
    for (uint64_t s = 1; s <= 50; s++) {
        int rc = dvs_randomize(h, refs, NULL, s);
        if (rc != DVS_RANDOMIZE_OK || m.x >= 10 || m.z <= 1000 || m.y != 10) ok = 0;
        if (!seen[m.x]++) distinct++;
    }
    CHECK(ok, "y=10: 50 calls ok, x < y, z > 1000, y not written");
    CHECK(distinct >= 3, "y=10: x varies across seeds");
    CHECK(m.g0 == 0xAA && m.g1 == 0xAA && m.g2 == 0xAA, "guard bytes untouched");

    m.y = 200;
    int big = 0;
    for (uint64_t s = 1; s <= 50; s++) {
        if (dvs_randomize(h, refs, NULL, s) != DVS_RANDOMIZE_OK || m.x >= 200) ok = 0;
        if (m.x >= 10) big = 1;
    }
    CHECK(ok, "y=200: x < y each call");
    CHECK(big, "y=200: the new STATE value replaces the old pin");

    m.y = 5;
    dvs_randomize(h, refs, NULL, 7);
    uint8_t x7 = m.x; uint16_t z7 = m.z;
    dvs_randomize(h, refs, NULL, 8);
    dvs_randomize(h, refs, NULL, 7);
    CHECK(m.x == x7 && m.z == z7, "same seed and state, same result");

    m.y = 0;
    m.x = 0x5C; m.z = 0x1234;
    CHECK(dvs_randomize(h, refs, NULL, 3) == DVS_RANDOMIZE_UNSAT, "y=0 (x < 0) is UNSAT");
    CHECK(m.x == 0x5C && m.z == 0x1234 && m.y == 0, "refs untouched on UNSAT");

    m.y = 3;
    CHECK(dvs_randomize(h, refs, NULL, 3) == DVS_RANDOMIZE_OK && m.x < 3,
          "solves again after an UNSAT call");

    uint32_t modes[1] = {0};
    m.x = 0x5C;
    CHECK(dvs_randomize(h, refs, modes, 3) == DVS_RANDOMIZE_ERROR && m.x == 0x5C,
          "mode bits are refused (not in the format yet)");
    dvs_ref_t wide[3] = { refs[0], refs[1], {&m.z, 65, 0, DVS_REF_SCALAR, 0} };
    CHECK(dvs_randomize(h, wide, NULL, 3) == DVS_RANDOMIZE_ERROR, "a 65-bit scalar ref is refused");

    /* A signed STATE value is sign-extended: s (8-bit signed STATE) = -3,
     * r (8-bit signed RAND) < s, r > -10. */
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 1, -128, 127);
    dvs_builder_add_var(b, 1, 8, 1, -128, 127);
    dvs_builder_set_var_kind(b, 1, DVS_VAR_STATE);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 1)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_const(b, -10, 1)));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *hs = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    int8_t r = 0, st = -3;
    dvs_ref_t srefs[2] = { {&r, 8, 1, DVS_REF_SCALAR, 0}, {&st, 8, 1, DVS_REF_SCALAR, 0} };
    ok = 1;
    for (uint64_t s = 1; s <= 20; s++)
        if (dvs_randomize(hs, srefs, NULL, s) != DVS_RANDOMIZE_OK || r >= -3 || r <= -10) ok = 0;
    CHECK(ok, "signed STATE -3: -10 < r < -3 each call");
    dvs_handle_free(hs);

    /* randc: c (8-bit RANDC) < 4 ends each cycle by UNSAT; d (2-bit RANDC,
     * unconstrained) by exhausting its declared range. Each run of 4 calls
     * is a permutation of 0..3, for 4 cycles. */
    b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 2, 0, 0, 3);
    dvs_builder_set_var_kind(b, 0, DVS_VAR_RANDC);
    dvs_builder_set_var_kind(b, 1, DVS_VAR_RANDC);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_const(b, 4, 0)));
    p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *hc = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    uint8_t c = 0, d = 0;
    dvs_ref_t crefs[2] = { {&c, 8, 0, DVS_REF_SCALAR, 0}, {&d, 2, 0, DVS_REF_SCALAR, 0} };
    int cperm = 1, dperm = 1;
    for (int cyc = 0; cyc < 4; cyc++) {
        unsigned cm = 0, dm = 0;
        for (int k = 0; k < 4; k++) {
            if (dvs_randomize(hc, crefs, NULL, 100 + 4 * cyc + k) != DVS_RANDOMIZE_OK) cperm = 0;
            if (c < 4) cm |= 1u << c;
            if (d < 4) dm |= 1u << d;
        }
        if (cm != 0xF) cperm = 0;
        if (dm != 0xF) dperm = 0;
    }
    CHECK(cperm, "randc under a constraint: each cycle is a permutation");
    CHECK(dperm, "randc over its declared range: each cycle is a permutation");
    dvs_handle_free(hc);

    dvs_handle_free(h);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
