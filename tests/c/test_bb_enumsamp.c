/* Soundness of the bitblast enumeration sampler's table (_enumsamp).
 *
 * dvs_bbsolver_resample builds a table of the problem's solutions on the first
 * fresh base and draws later models from it while the assert set is the same.
 * These tests change the assert set after the table is built (an incremental
 * assert, one that leaves no solution) and check every model resample returns
 * against the constraints in force, so a stale table shows up as a wrong model.
 * (dvs_bbsolver_rebase refuses builder problems, which are SV-elaborated, so
 * a rebase is not covered here.) 5-queens (10 solutions) is a shape the other
 * samplers decline; main() checks the table is really built (DV_LOG "-> table").
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dvs_bbsolver.h"
#include "dvs_builder.h"
#include "dvs_problem.h"
#include "dvs_sat.h"

#define NQ    5
#define DRAWS 80

static int failures = 0;
#define CHECK(c, msg) do { \
    if (!(c)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
    else printf("PASS %s\n", msg); } while (0)

/* q[i] in [0,NQ-1], no two on a row or a diagonal. */
static void add_queens(dvs_builder_t *b) {
    dvs_expr_t q[NQ];
    for (uint32_t i = 0; i < NQ; i++) {
        dvs_builder_add_var(b, i, 3, 0, 0, NQ - 1);
        q[i] = dvs_builder_expr_var(b, i);
    }
    for (uint32_t i = 0; i < NQ; i++)
        for (uint32_t j = i + 1; j < NQ; j++) {
            dvs_expr_t d = dvs_builder_expr_const(b, (int64_t)(j - i), 0);
            dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ, q[i], q[j]));
            dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ,
                dvs_builder_expr_binary(b, DVS_BIN_SUB, q[i], q[j]), d));
            dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ,
                dvs_builder_expr_binary(b, DVS_BIN_SUB, q[j], q[i]), d));
        }
}

static int queens_ok(const int64_t *q) {
    for (int i = 0; i < NQ; i++) {
        if (q[i] < 0 || q[i] >= NQ) return 0;
        for (int j = i + 1; j < NQ; j++)
            if (q[i] == q[j] || q[i] - q[j] == j - i || q[j] - q[i] == j - i) return 0;
    }
    return 1;
}

/* Resample DRAWS seeds; every model must pass queens_ok and q0 < lim (lim <= 0:
 * no bound). Returns 0, or -1 on a bad model. */
static int draw_all(dvs_bbsolver_t *S, uint64_t seed0, int64_t lim) {
    for (uint64_t s = 0; s < DRAWS; s++) {
        if (dvs_bbsolver_resample(S, seed0 + s) != 0) return -1;
        int64_t q[NQ];
        for (uint32_t i = 0; i < NQ; i++)
            if (dvs_bbsolver_value(S, i, &q[i]) != 0) return -1;
        if (!queens_ok(q) || (lim > 0 && q[0] >= lim)) {
            fprintf(stderr, "bad model q=%" PRId64 "%" PRId64 "%" PRId64 "%" PRId64 "%" PRId64 "\n",
                    q[0], q[1], q[2], q[3], q[4]);
            return -1;
        }
    }
    return 0;
}

/* Count distinct models over DRAWS resamples (assumes draw_all passed). */
static int distinct(dvs_bbsolver_t *S, uint64_t seed0) {
    static uint8_t hit[3125];
    memset(hit, 0, sizeof hit);
    int n = 0;
    for (uint64_t s = 0; s < DRAWS; s++) {
        if (dvs_bbsolver_resample(S, seed0 + s) != 0) return -1;
        uint32_t code = 0;
        for (uint32_t i = 0; i < NQ; i++) {
            int64_t v = 0;
            dvs_bbsolver_value(S, i, &v);
            code = code * NQ + (uint32_t)v;
        }
        if (code < sizeof hit && !hit[code]) { hit[code] = 1; n++; }
    }
    return n;
}

/* Table built, then an incremental assert narrows the set (CaDiCaL only).
 * The new key rebuilds the table from the problem's constraints, which do not
 * hold the assert, so its first draw fails the assert check and the table turns
 * off; the narrowed draws come from the other samplers. A table that skipped
 * the check or kept the old key fails "honour q0<2". */
static void test_assert_after_table(void) {
    char label[128];
    dvs_builder_t *b = dvs_builder_create(4096, NULL);
    add_queens(b);
    dvs_expr_t lt2 = dvs_builder_expr_binary(b, DVS_BIN_LT, dvs_builder_expr_var(b, 0),
                                             dvs_builder_expr_const(b, 2, 0));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_bbsolver_t *S = dvs_bbsolver_new_backend(NULL, p, 1);
    dvs_bbsolver_set_presim(S, 1);
    dvs_bbsolver_set_resample(S, 1);
    CHECK(dvs_bbsolver_check(S, 1) == DVS_BB_SAT, "[assert] base SAT");
    CHECK(draw_all(S, 100, 0) == 0, "[assert] base resamples are queens");
    int n = distinct(S, 300);
    snprintf(label, sizeof label, "[assert] base resamples vary (%d distinct of 10)", n);
    CHECK(n >= 8, label);

    if (dvs_bbsolver_is_incremental(S)) {
        CHECK(dvs_bbsolver_assert(S, lt2) == 0, "[assert] assert(q0<2) accepted");
        CHECK(dvs_bbsolver_resolve(S, 2) == DVS_BB_SAT, "[assert] resolve SAT");
        CHECK(draw_all(S, 500, 2) == 0, "[assert] resamples honour q0<2");
        n = distinct(S, 700);
        snprintf(label, sizeof label, "[assert] narrowed resamples vary (%d distinct of 4)", n);
        CHECK(n >= 3, label);
    } else {
        printf("SKIP assert after table (no incremental backend)\n");
    }
    dvs_bbsolver_free(S);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

/* Table built, then an assert that leaves no solution: the table must not
 * answer for the new assert set (CaDiCaL only). */
static void test_unsat_after_table(void) {
    dvs_builder_t *b = dvs_builder_create(4096, NULL);
    add_queens(b);
    dvs_expr_t eq = dvs_builder_expr_binary(b, DVS_BIN_EQ, dvs_builder_expr_var(b, 0),
                                            dvs_builder_expr_var(b, 1));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_bbsolver_t *S = dvs_bbsolver_new_backend(NULL, p, 1);
    dvs_bbsolver_set_presim(S, 1);
    dvs_bbsolver_set_resample(S, 1);
    CHECK(dvs_bbsolver_check(S, 3) == DVS_BB_SAT, "[unsat] base SAT");
    CHECK(draw_all(S, 900, 0) == 0, "[unsat] base resamples are queens");
    if (dvs_bbsolver_is_incremental(S)) {
        CHECK(dvs_bbsolver_assert(S, eq) == 0, "[unsat] assert(q0==q1) accepted");
        CHECK(dvs_bbsolver_resolve(S, 4) == DVS_BB_UNSAT, "[unsat] resolve UNSAT");
        CHECK(dvs_bbsolver_resample(S, 5) != 0, "[unsat] resample refuses");
    } else {
        printf("SKIP unsat after table (no incremental backend)\n");
    }
    dvs_bbsolver_free(S);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
}

/* 1 when a fresh solver's resamples log a built table and no draw from it
 * fails the asserts (a failed draw turns the table off, which would leave the
 * tests below passing on the SAT path alone). */
static int table_logged(void) {
    char path[] = "/tmp/test_bb_enumsamp_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return 0;
    fflush(stderr);
    int saved = dup(2);
    dup2(fd, 2);
    setenv("DV_LOG", "1", 1);
    dvs_builder_t *b = dvs_builder_create(4096, NULL);
    add_queens(b);
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_bbsolver_t *S = dvs_bbsolver_new(NULL, p);
    dvs_bbsolver_set_presim(S, 1);
    dvs_bbsolver_set_resample(S, 1);
    if (dvs_bbsolver_check(S, 1) == DVS_BB_SAT)
        for (uint64_t s = 2; s < 2 + DRAWS; s++) dvs_bbsolver_resample(S, s);
    dvs_bbsolver_free(S);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    unsetenv("DV_LOG");
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    char buf[65536];
    ssize_t n = pread(fd, buf, sizeof buf - 1, 0);
    close(fd);
    unlink(path);
    if (n <= 0) return 0;
    buf[n] = 0;
    return strstr(buf, "bb-enumsamp:") && strstr(buf, "-> table") && !strstr(buf, "-> off");
}

/* The hull reads zero_extend(x) cmp c as x cmp c when c fits x: x in 250..255
 * (zext(x) >= 250) with zext(x) < 300 and != 0x1ff, whose c does not fit and
 * must not narrow. Every value must be drawn; zext(x) >= 300 is unsat. The
 * other samplers would answer this shape, so they are off. */
static void test_zext_hull(void) {
    static const char *off[] = {"DV_POPSAMP", "DV_UNIQSAMP", "DV_SORTSAMP", "DV_DIVSAMP"};
    for (int i = 0; i < 4; i++) setenv(off[i], "0", 1);
    for (int unsat = 0; unsat < 2; unsat++) {
        dvs_builder_t *b = dvs_builder_create(4096, NULL);
        dvs_builder_add_var(b, 0, 8, 0, 0, 255);
        dvs_builder_add_var(b, 1, 8, 0, 0, 255);
        dvs_expr_t zx = dvs_builder_expr_extend(b, dvs_builder_expr_var(b, 0), 8, 16, 0);
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GTE, zx,
            dvs_builder_expr_const_sized(b, unsat ? 300 : 250, 0, 16)));
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT, zx,
            dvs_builder_expr_const_sized(b, 300, 0, 16)));
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ, zx,
            dvs_builder_expr_const_sized(b, 0x1ff, 0, 16)));
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
            dvs_builder_expr_var(b, 1), dvs_builder_expr_const_sized(b, 4, 0, 8)));
        size_t sz;
        dvs_problem_t *p = dvs_builder_finalize(b, &sz);
        dvs_bbsolver_t *S = dvs_bbsolver_new(NULL, p);
        dvs_bbsolver_set_presim(S, 1);
        dvs_bbsolver_set_resample(S, 1);
        int rc = dvs_bbsolver_check(S, 1);
        if (unsat) {
            CHECK(rc == DVS_BB_UNSAT, "[zext] zext(x) >= 300 is unsat");
        } else {
            int bad = rc != DVS_BB_SAT, seen = 0;
            for (uint64_t s = 2; !bad && s < 2 + 4 * DRAWS; s++) {
                int64_t x = -1, y = -1;
                bad = dvs_bbsolver_resample(S, s) != 0 || dvs_bbsolver_value(S, 0, &x) != 0 ||
                      dvs_bbsolver_value(S, 1, &y) != 0 || x < 250 || x > 255 || y < 0 || y > 3;
                if (!bad) seen |= 1 << (x - 250);
            }
            CHECK(!bad, "[zext] models have x in 250..255, y < 4");
            CHECK(seen == 0x3f, "[zext] every x in 250..255 drawn");
        }
        dvs_bbsolver_free(S);
        dvs_builder_free_problem(b, p, sz);
        dvs_builder_destroy(b);
    }
    for (int i = 0; i < 4; i++) unsetenv(off[i]);
}

int main(void) {
    unsetenv("DV_ENUMSAMP");
    test_zext_hull();
    CHECK(table_logged(), "5-queens resamples draw from an enumeration table");
    test_assert_after_table();
    test_unsat_after_table();
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("all enumsamp tests passed\n");
    return 0;
}
