/* dvs_handle_t / dvs_randomize(): STATE vars pinned from refs, RAND values
 * written through refs, refs untouched when a call fails; CDCL and bitblast routes. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_problem.h"   /* where the image header keeps magic and version */
#include "dvs_tmpl.h"      /* dvs_handle_n_learnt */

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

/* The image is read from an unaligned copy, and refused when its magic or
 * format version is not this library's. */
static void check_image_header(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    uint8_t *raw = malloc(sz + 1);
    memcpy(raw + 1, p, sz);
    dvs_handle_t *h = dvs_handle_new(raw + 1, sz);
    CHECK(h != NULL, "unaligned image accepted");
    dvs_handle_free(h);
    CHECK(p->format_version == dvs_problem_format_version()
          && p->format_version == DVS_PROBLEM_FORMAT_VERSION,
          "image carries the format version");
    uint8_t *ver = raw + 1 + offsetof(dvs_problem_t, format_version);
    uint8_t *magic = raw + 1 + offsetof(dvs_problem_t, magic);
    *ver ^= 0x80;
    CHECK(dvs_handle_new(raw + 1, sz) == NULL, "wrong format version refused");
    *ver ^= 0x80;
    *magic ^= 0x01;
    CHECK(dvs_handle_new(raw + 1, sz) == NULL, "wrong magic refused");
    free(raw);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

/* Learnt clauses do not pile up across calls. x (32-bit RAND) with
 * x[1:0] == 0: most picks are misaligned, each rejection learns `x != k`,
 * and with no STATE var the reset alone kept them all -- every later call
 * walked them, so each cost more than the last. */
static void check_learnt_dropped(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFll);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_BAND, dvs_builder_expr_var(b, 0),
                                dvs_builder_expr_const(b, 3, 0)),
        dvs_builder_expr_const(b, 0, 0)));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_handle_set_engine(h, DVS_ENGINE_CDCL);   /* AUTO would bit-blast it */
    uint32_t x = 0;
    dvs_ref_t ref = {&x, 32, 0, DVS_REF_SCALAR, 0};
    int ok = 1;
    uint32_t most = 0;
    for (uint64_t seed = 1; seed <= 200; seed++) {
        ok &= dvs_randomize(h, &ref, NULL, seed) == DVS_RANDOMIZE_OK && (x & 3) == 0;
        uint32_t n = dvs_handle_n_learnt(h);
        if (n > most) most = n;
    }
    CHECK(ok, "aligned x on every call");
    printf("  most learnt clauses after a call: %u\n", most);
    CHECK(most < 50, "learnt clauses do not accumulate across calls");
    dvs_handle_free(h);
}

/* A learnt clause that fires on every call is kept. base (32-bit RAND) with
 * base[7:0] == 0 and base + size <= 0x100000, wrapping, size in [256, 16384]:
 * base lies in [0, 0xFFF00] or [2^32 - 16384, 2^32 - 256], which bounds
 * cannot hold. The first call learns that (one clause on base alone) after
 * some 30 conflicts, and every later call's picks fire it. Kept, a later
 * call holds it and at most the odd clause it learns; dropped, each call
 * learnt it again, with the 30 others. */
static void check_used_kept(void) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFll);
    dvs_builder_add_var(b, 1, 32, 0, 256, 16384);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_BAND, dvs_builder_expr_var(b, 0),
                                dvs_builder_expr_const(b, 255, 0)),
        dvs_builder_expr_const(b, 0, 0)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LTE,
        dvs_builder_expr_binary(b, DVS_BIN_ADD, dvs_builder_expr_var(b, 0),
                                dvs_builder_expr_var(b, 1)),
        dvs_builder_expr_const(b, 0x100000, 0)));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_handle_set_engine(h, DVS_ENGINE_CDCL);
    uint32_t base = 0, size = 0;
    dvs_ref_t refs[2] = { {&base, 32, 0, DVS_REF_SCALAR, 0}, {&size, 32, 0, DVS_REF_SCALAR, 0} };
    int ok = 1;
    uint32_t quiet = 0;
    for (uint64_t seed = 1; seed <= 200; seed++) {
        ok &= dvs_randomize(h, refs, NULL, seed) == DVS_RANDOMIZE_OK
              && (base & 255) == 0 && (uint32_t)(base + size) <= 0x100000;
        uint32_t n = dvs_handle_n_learnt(h);
        if (seed > 100 && n >= 1 && n <= 2) quiet++;
    }
    CHECK(ok, "wrapping base + size: valid on every call");
    printf("  calls 101-200 holding 1 or 2 clauses: %u\n", quiet);
    CHECK(quiet >= 90, "a clause every call uses is kept (and spares the relearning)");
    dvs_handle_free(h);
}

int main(void) {
    /* The two clause checks need draws that miss: with the value picker's
     * known bits (dvs_search.c, the pick domain) every draw of an aligned
     * variable is aligned, and nothing is learnt. Read once, before any
     * solve. */
    setenv("DVS_PICKDOM", "0", 1);
    check_learnt_dropped();
    check_used_kept();
    check_image_header();
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

    /* A sized constant is its low `width` bits: Verilator spells the signed
     * 32-bit -3 as 0xFFFFFFFD. Explicit sext(r, 8 -> 32) > -3 and < 5, r
     * an 8-bit signed RAND, on CDCL (which could not compile it). */
    b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    dvs_builder_add_var(b, 0, 8, 1, -128, 127);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT,
        dvs_builder_expr_extend(b, dvs_builder_expr_var(b, 0), 8, 32, 1),
        dvs_builder_expr_const_sized(b, 0xFFFFFFFDll, 1, 32)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_extend(b, dvs_builder_expr_var(b, 0), 8, 32, 1),
        dvs_builder_expr_const_sized(b, 5, 1, 32)));
    p = dvs_builder_finalize(b, &sz);
    hs = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    CHECK(dvs_handle_set_engine(hs, DVS_ENGINE_CDCL) == 0, "sized signed constant: CDCL chosen");
    ok = 1;
    for (uint64_t s = 1; s <= 20; s++)
        if (dvs_randomize(hs, srefs, NULL, s) != DVS_RANDOMIZE_OK || r <= -3 || r >= 5) ok = 0;
    CHECK(ok, "sized signed constant 0xFFFFFFFD is -3: -3 < r < 5 each call");
    dvs_handle_free(hs);

    /* A signed ITE value with a negative branch: x in [1,10] (32-bit signed),
     * (1'b0 ? x : 0 - x) <= 20. Its result var was unsigned: UNSAT. */
    b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    dvs_builder_add_var(b, 0, 32, 1, 1, 10);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LTE,
        dvs_builder_expr_ite(b, dvs_builder_expr_const_sized(b, 0, 0, 1),
            dvs_builder_expr_var(b, 0),
            dvs_builder_expr_binary(b, DVS_BIN_SUB,
                dvs_builder_expr_const_sized(b, 0, 1, 32), dvs_builder_expr_var(b, 0))),
        dvs_builder_expr_const_sized(b, 20, 1, 32)));
    p = dvs_builder_finalize(b, &sz);
    hs = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    int32_t x32 = 0;
    dvs_ref_t xref = {&x32, 32, 1, DVS_REF_SCALAR, 0};
    ok = 1;
    for (uint64_t s = 1; s <= 10; s++)
        if (dvs_randomize(hs, &xref, NULL, s) != DVS_RANDOMIZE_OK || x32 < 1 || x32 > 10) ok = 0;
    CHECK(ok, "signed ITE with a negative branch: (0 ? x : -x) <= 20 is SAT");
    dvs_handle_free(hs);

    /* A soft constraint gives way to a STATE value: len <= m, soft
     * len == 64 (8-bit; len RAND, m STATE). m = 32 was UNSAT. */
    b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_set_var_kind(b, 1, DVS_VAR_STATE);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LTE,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 1)));
    dvs_builder_add_soft_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_const_sized(b, 64, 0, 8)), 0);
    p = dvs_builder_finalize(b, &sz);
    hs = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    uint8_t len = 0, lm = 32;
    dvs_ref_t lrefs[2] = { {&len, 8, 0, DVS_REF_SCALAR, 0}, {&lm, 8, 0, DVS_REF_SCALAR, 0} };
    ok = 1;
    for (uint64_t s = 1; s <= 20; s++)
        if (dvs_randomize(hs, lrefs, NULL, s) != DVS_RANDOMIZE_OK || len > 32) ok = 0;
    CHECK(ok, "soft vs STATE: m=32, soft len == 64 dropped, len <= 32 each call");
    lm = 100;
    CHECK(dvs_randomize(hs, lrefs, NULL, 1) == DVS_RANDOMIZE_OK && len == 64,
          "soft vs STATE: m=100, soft len == 64 kept");
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

    CHECK(dvs_handle_set_engine(h, DVS_ENGINE_CDCL) == -1, "engine is fixed after the first call");
    CHECK(dvs_handle_engine(h) == DVS_ENGINE_CDCL, "orderings route to CDCL");

    /* (x * 3) ^ y == 0x55 (x, y 8-bit RAND); w (RAND in 5..9) is in no
     * constraint. AUTO keeps CDCL, which compiles it whole. */
    b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_var(b, 2, 8, 0, 5, 9);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_BXOR,
            dvs_builder_expr_binary(b, DVS_BIN_MUL, dvs_builder_expr_var(b, 0),
                                    dvs_builder_expr_const(b, 3, 0)),
            dvs_builder_expr_var(b, 1)),
        dvs_builder_expr_const(b, 0x55, 0)));
    p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *hb = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    uint8_t bx = 0, by = 0, bw = 0;
    dvs_ref_t brefs[3] = { {&bx, 8, 0, DVS_REF_SCALAR, 0}, {&by, 8, 0, DVS_REF_SCALAR, 0},
                           {&bw, 8, 0, DVS_REF_SCALAR, 0} };
    int bseen[256] = {0}, bdistinct = 0;
    ok = 1;
    for (uint64_t s = 1; s <= 30; s++) {
        if (dvs_randomize(hb, brefs, NULL, s) != DVS_RANDOMIZE_OK
                || ((bx * 3u) ^ by) != 0x55 || bw < 5 || bw > 9)
            ok = 0;
        if (!bseen[bx]++) bdistinct++;
    }
    CHECK(dvs_handle_engine(hb) == DVS_ENGINE_CDCL, "CDCL first: it compiles the multiplier whole");
    CHECK(ok, "auto: 30 calls ok, (x*3)^y == 0x55, w in its range");
    CHECK(bdistinct >= 3, "auto: x varies across seeds");
    dvs_handle_free(hb);

    /* The same multiplier with a soft constraint, a dist or a solve-order
     * stays on CDCL, which honours them (bitblast's resample does not). */
    for (int extra = 0; extra < 3; extra++) {
        b = dvs_builder_create(0, NULL);
        dvs_builder_add_var(b, 0, 8, 0, 0, 255);
        dvs_builder_add_var(b, 1, 8, 0, 0, 255);
        dvs_builder_add_var(b, 2, 8, 0, 5, 9);
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
            dvs_builder_expr_binary(b, DVS_BIN_BXOR,
                dvs_builder_expr_binary(b, DVS_BIN_MUL, dvs_builder_expr_var(b, 0),
                                        dvs_builder_expr_const(b, 3, 0)),
                dvs_builder_expr_var(b, 1)),
            dvs_builder_expr_const(b, 0x55, 0)));
        dvs_dist_entry_t de = { 7, 7, 1, 1, {0} };
        if (extra == 0)
            dvs_builder_add_soft_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
                dvs_builder_expr_var(b, 0), dvs_builder_expr_const_sized(b, 7, 0, 8)), 0);
        else if (extra == 1)
            dvs_builder_add_dist(b, 2, 1, &de);
        else
            dvs_builder_set_var_order(b, 0, 1);
        p = dvs_builder_finalize(b, &sz);
        hb = dvs_handle_new(p, sz);
        dvs_builder_free_problem(b, p, sz);
        dvs_builder_destroy(b);
        static const char *what[] = { "soft", "dist", "solve-order" };
        char msg[96];
        snprintf(msg, sizeof msg, "%s: bitblast refused", what[extra]);
        CHECK(dvs_handle_set_engine(hb, DVS_ENGINE_BITBLAST) == -1, msg);
        ok = 1;
        for (uint64_t s = 1; s <= 10; s++) {
            if (dvs_randomize(hb, brefs, NULL, s) != DVS_RANDOMIZE_OK
                    || ((bx * 3u) ^ by) != 0x55 || bw < 5 || bw > 9)
                ok = 0;
            if (extra == 0 && bx != 7) ok = 0;   /* the soft holds */
        }
        snprintf(msg, sizeof msg, "%s: a multiplier stays on CDCL", what[extra]);
        CHECK(dvs_handle_engine(hb) == DVS_ENGINE_CDCL, msg);
        snprintf(msg, sizeof msg, "%s: 10 calls ok", what[extra]);
        CHECK(ok, msg);
        dvs_handle_free(hb);
    }

    /* Bitblast UNSAT: x * 2 == 1 (x 8-bit), chosen explicitly. */
    b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_MUL, dvs_builder_expr_var(b, 0),
                                dvs_builder_expr_const(b, 2, 0)),
        dvs_builder_expr_const(b, 1, 0)));
    p = dvs_builder_finalize(b, &sz);
    hb = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    CHECK(dvs_handle_set_engine(hb, DVS_ENGINE_BITBLAST) == 0, "bitblast chosen");
    bx = 0x5C;
    ok = dvs_randomize(hb, brefs, NULL, 1) == DVS_RANDOMIZE_UNSAT
      && dvs_randomize(hb, brefs, NULL, 2) == DVS_RANDOMIZE_UNSAT && bx == 0x5C;
    CHECK(ok, "bitblast: x*2 == 1 UNSAT on every call, ref untouched");
    dvs_handle_free(hb);

    /* Bitblast, unsigned 64-bit over its full range (hi = ~0): (x & 0xF) == 5.
     * The bounds are uint64_t patterns; the model's value is kept, not
     * redrawn, including values past INT64_MAX. */
    b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 64, 0, 0, -1);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_BAND, dvs_builder_expr_var(b, 0),
                                dvs_builder_expr_const(b, 0xF, 0)),
        dvs_builder_expr_const(b, 5, 0)));
    p = dvs_builder_finalize(b, &sz);
    hb = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    CHECK(dvs_handle_set_engine(hb, DVS_ENGINE_BITBLAST) == 0, "bitblast chosen (64-bit)");
    uint64_t ux = 0;
    dvs_ref_t urefs[1] = { {&ux, 64, 0, DVS_REF_SCALAR, 0} };
    int high = 0;
    ok = 1;
    for (uint64_t s = 1; s <= 30; s++) {
        if (dvs_randomize(hb, urefs, NULL, s) != DVS_RANDOMIZE_OK || (ux & 0xF) != 5) ok = 0;
        high += ux > (uint64_t)INT64_MAX;
    }
    CHECK(ok, "bitblast: unsigned 64-bit, 30 calls ok, (x & 0xF) == 5");
    CHECK(high > 0 && high < 30, "bitblast: unsigned 64-bit values on both sides of 2^63");
    dvs_handle_free(hb);

    /* A dist keeps its weights under AUTO even when a multiplier is present:
     * (x * 3) ^ y == 0x55 as above, and w dist {0 := 90, [1:9] :/ 10}, so
     * w == 0 on about 90% of calls (uniform over 0..9 would give 10%). */
    b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_var(b, 2, 8, 0, 0, 9);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_BXOR,
            dvs_builder_expr_binary(b, DVS_BIN_MUL, dvs_builder_expr_var(b, 0),
                                    dvs_builder_expr_const(b, 3, 0)),
            dvs_builder_expr_var(b, 1)),
        dvs_builder_expr_const(b, 0x55, 0)));
    dvs_dist_entry_t wd[2] = { {0, 0, 90, 1, {0}}, {1, 9, 10, 0, {0}} };
    dvs_builder_add_dist(b, 2, 2, wd);
    p = dvs_builder_finalize(b, &sz);
    hb = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    int zeros = 0;
    ok = 1;
    for (uint64_t s = 1; s <= 400; s++) {
        if (dvs_randomize(hb, brefs, NULL, s) != DVS_RANDOMIZE_OK
                || ((bx * 3u) ^ by) != 0x55 || bw > 9)
            ok = 0;
        zeros += bw == 0;
    }
    printf("     dist+multiplier: engine %d, w == 0 on %d of 400\n", dvs_handle_engine(hb), zeros);
    CHECK(ok, "dist+multiplier: 400 calls ok");
    CHECK(zeros >= 320 && zeros <= 400, "dist+multiplier: w == 0 on about 90% (weights kept)");
    dvs_handle_free(hb);

    /* CDCL (a STATE var keeps it there), a signed 32-bit x read unsigned in
     * full, as Verilator builds 'int' == 32'hA5 and == 32'hFFFFFFFB: the
     * slice fixes x itself, negative or not, rather than a search over 2^32. */
    for (int neg = 0; neg < 2; neg++) {
        b = dvs_builder_create(0, NULL);
        dvs_builder_set_explicit(b);
        dvs_builder_add_var(b, 0, 32, 1, INT32_MIN, INT32_MAX);
        dvs_builder_add_var(b, 1, 1, 0, 0, 1);
        dvs_builder_set_var_kind(b, 1, DVS_VAR_STATE);
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
            dvs_builder_expr_extract(b, dvs_builder_expr_var(b, 0), 31, 0),
            dvs_builder_expr_const_sized(b, neg ? 0xFFFFFFFB : 0xA5, 0, 32)));
        p = dvs_builder_finalize(b, &sz);
        hb = dvs_handle_new(p, sz);
        dvs_builder_free_problem(b, p, sz);
        dvs_builder_destroy(b);
        int32_t sx = 0;
        uint8_t st = 0;
        dvs_ref_t srefs[2] = { {&sx, 32, 1, DVS_REF_SCALAR, 0}, {&st, 1, 0, DVS_REF_SCALAR, 0} };
        ok = dvs_randomize(hb, srefs, NULL, 1) == DVS_RANDOMIZE_OK && sx == (neg ? -5 : 0xA5);
        CHECK(ok, neg ? "cdcl: a full slice of a signed var fixes it (negative)"
                      : "cdcl: a full slice of a signed var fixes it");
        dvs_handle_free(hb);
    }

    /* The negation of a bitwise or of comparisons (an or reduction, as
     * Verilator lowers `!(x inside {arr})`): !((x == s0) | (x == s1)), x
     * 32-bit RAND, s0 and s1 STATE. It was left uncompiled: ERROR. */
    {
        b = dvs_builder_create(0, NULL);
        dvs_builder_set_explicit(b);
        for (uint32_t v = 0; v < 3; v++) {
            dvs_builder_add_var(b, v, 32, 1, INT32_MIN, INT32_MAX);
            if (v) dvs_builder_set_var_kind(b, v, DVS_VAR_STATE);
        }
        dvs_expr_t x = dvs_builder_expr_var(b, 0);
        dvs_builder_add_constraint(b, dvs_builder_expr_unary(b, DVS_UN_NOT,
            dvs_builder_expr_binary(b, DVS_BIN_BOR,
                dvs_builder_expr_binary(b, DVS_BIN_EQ, x, dvs_builder_expr_var(b, 1)),
                dvs_builder_expr_binary(b, DVS_BIN_EQ, x, dvs_builder_expr_var(b, 2)))));
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GTE, x,
            dvs_builder_expr_const_sized(b, 3, 1, 32)));
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LTE, x,
            dvs_builder_expr_const_sized(b, 5, 1, 32)));
        p = dvs_builder_finalize(b, &sz);
        hb = dvs_handle_new(p, sz);
        dvs_builder_free_problem(b, p, sz);
        dvs_builder_destroy(b);
        int32_t nx = 0, s0 = 3, s1 = 5;
        dvs_ref_t nrefs[3] = { {&nx, 32, 1, DVS_REF_SCALAR, 0}, {&s0, 32, 1, DVS_REF_SCALAR, 0},
                               {&s1, 32, 1, DVS_REF_SCALAR, 0} };
        ok = 1;
        for (uint64_t s = 1; s <= 10; s++)
            if (dvs_randomize(hb, nrefs, NULL, s) != DVS_RANDOMIZE_OK || nx != 4) ok = 0;
        CHECK(ok, "!((x == s0) | (x == s1)) is solved: 3 <= x <= 5, s0 = 3, s1 = 5 gives 4");
        dvs_handle_free(hb);
    }

    /* No pins on bitblast: a handle with STATE vars refuses it. */
    hb = make();
    CHECK(dvs_handle_set_engine(hb, DVS_ENGINE_BITBLAST) == -1, "bitblast refused with STATE vars");
    dvs_handle_free(hb);

    dvs_handle_free(h);
    printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
