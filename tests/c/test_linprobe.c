/* The probe evaluator on linear components (dvs_linprobe.c), checked
 * against propagation: under DVS_LINPROBE=2 every weighted-pick probe it can
 * run is run both ways, and any difference in the outcome, the bucket's range
 * or the box aborts. Address-map chains at 33 and 16 bits (the 64- and 32-bit
 * comparison forms), with a subtract and a strict order, an 8-bit sum
 * that can wrap, which it must leave to propagation, and an SV sum()'s
 * shape, one expression whose partial sums the evaluator folds. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_problem.h"
#include "dvs_linprobe.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

typedef dvs_builder_t B;

static dvs_expr_t var(B *b, uint32_t v) { return dvs_builder_expr_var(b, v); }
static void rel(B *b, int op, dvs_expr_t l, dvs_expr_t r) {
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, op, l, r));
}

/* n regions of base, size, end, gap (all w bits, unsigned):
 *   end_i == base_i + size_i, end_i <= base_{i+1},
 *   gap_i == base_{i+1} - end_i, size_0 < size_1 <= size_2 ..., end_{n-1} < limit,
 *   and a total: tot == size_0 + ... (a chain of adds) == fixed. */
#define NR 4
enum { V_BASE = 0, V_SIZE = NR, V_END = 2 * NR, V_GAP = 3 * NR, V_TOT = 4 * NR, NV = 5 * NR };

static dvs_handle_t *chain(uint32_t w, int64_t base_hi, int64_t size_lo, int64_t size_hi,
                           int64_t limit, int64_t total) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    const int64_t top = (int64_t)(((uint64_t)1 << (w - 1)) - 1);
    for (uint32_t i = 0; i < NR; i++) {
        dvs_builder_add_var(b, V_BASE + i, w, 0, 0, base_hi);
        dvs_builder_add_var(b, V_SIZE + i, w, 0, size_lo, size_hi);
        dvs_builder_add_var(b, V_END + i, w, 0, 0, top);
        dvs_builder_add_var(b, V_GAP + i, w, 0, 0, top);
        dvs_builder_add_var(b, V_TOT + i, w, 0, 0, top);
    }
    for (uint32_t i = 0; i < NR; i++) {
        rel(b, DVS_BIN_EQ, var(b, V_END + i),
            dvs_builder_expr_binary(b, DVS_BIN_ADD, var(b, V_BASE + i), var(b, V_SIZE + i)));
        if (i + 1 < NR) {
            rel(b, DVS_BIN_LTE, var(b, V_END + i), var(b, V_BASE + i + 1));
            rel(b, DVS_BIN_EQ, var(b, V_GAP + i),
                dvs_builder_expr_binary(b, DVS_BIN_SUB, var(b, V_BASE + i + 1), var(b, V_END + i)));
            rel(b, i == 0 ? DVS_BIN_LT : DVS_BIN_LTE, var(b, V_SIZE + i), var(b, V_SIZE + i + 1));
        }
        rel(b, DVS_BIN_EQ, var(b, V_TOT + i),
            i == 0 ? var(b, V_SIZE)
                   : dvs_builder_expr_binary(b, DVS_BIN_ADD, var(b, V_TOT + i - 1),
                                             var(b, V_SIZE + i)));
    }
    rel(b, DVS_BIN_LT, var(b, V_END + NR - 1), dvs_builder_expr_const_sized(b, limit, 0, w));
    rel(b, DVS_BIN_EQ, var(b, V_TOT + NR - 1), dvs_builder_expr_const_sized(b, total, 0, w));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    if (h) dvs_handle_set_engine(h, DVS_ENGINE_CDCL);
    return h;
}

/* 200 draws; each must satisfy the chain. */
static void check_chain(uint32_t w, int64_t base_hi, int64_t size_lo, int64_t size_hi,
                        int64_t limit, int64_t total, const char *msg) {
    dvs_handle_t *h = chain(w, base_hi, size_lo, size_hi, limit, total);
    uint64_t v[NV];
    dvs_ref_t refs[NV];
    for (int i = 0; i < NV; i++) refs[i] = (dvs_ref_t){ &v[i], w, 0, DVS_REF_SCALAR, 0 };
    int ok = h != NULL;
    for (uint64_t seed = 1; ok && seed <= 200; seed++) {
        memset(v, 0, sizeof v);
        ok &= dvs_randomize(h, refs, NULL, seed) == DVS_RANDOMIZE_OK;
        uint64_t tot = 0;
        for (int i = 0; ok && i < NR; i++) {
            tot += v[V_SIZE + i];
            ok &= v[V_END + i] == v[V_BASE + i] + v[V_SIZE + i] && v[V_TOT + i] == tot;
            if (i + 1 < NR)
                ok &= v[V_END + i] <= v[V_BASE + i + 1]
                      && (i == 0 ? v[V_SIZE] < v[V_SIZE + 1] : v[V_SIZE + i] <= v[V_SIZE + i + 1])
                      && v[V_GAP + i] == v[V_BASE + i + 1] - v[V_END + i];
        }
        ok &= v[V_END + NR - 1] < (uint64_t)limit && tot == (uint64_t)total;
    }
    CHECK(ok, msg);
    if (h) dvs_handle_free(h);
}

/* Each chain must reach the evaluator, not only the first. */
static void check_ran(uint64_t before, const char *msg) {
    printf("  %llu probes checked\n", (unsigned long long)(dvs_linprobe_checks() - before));
    CHECK(dvs_linprobe_checks() - before >= 200, msg);
}

/* x + y == z at 8 bits can wrap: the evaluator must decline, and the draws
 * still hold (mod 256). */
static void check_wrap(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    for (uint32_t i = 0; i < 3; i++) dvs_builder_add_var(b, i, 8, 0, 0, 255);
    rel(b, DVS_BIN_EQ, var(b, 2), dvs_builder_expr_binary(b, DVS_BIN_ADD, var(b, 0), var(b, 1)));
    rel(b, DVS_BIN_LTE, var(b, 0), var(b, 1));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_handle_set_engine(h, DVS_ENGINE_CDCL);
    uint8_t v[3];
    dvs_ref_t refs[3];
    for (int i = 0; i < 3; i++) refs[i] = (dvs_ref_t){ &v[i], 8, 0, DVS_REF_SCALAR, 0 };
    int ok = 1;
    for (uint64_t seed = 1; seed <= 200; seed++)
        ok &= dvs_randomize(h, refs, NULL, seed) == DVS_RANDOMIZE_OK
              && v[2] == (uint8_t)(v[0] + v[1]) && v[0] <= v[1];
    CHECK(ok, "8-bit sum that can wrap: every draw holds");
    dvs_handle_free(h);
}

/* x0 + ... + x7 == total, each x < 64, at 16 bits: one expression, so the
 * partial sums are aux variables, folded into one sum row. 200 draws; each
 * must hold. */
static void check_sum(int64_t total, const char *msg) {
    enum { N = 8 };
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    dvs_expr_t s = EXPR_NULL;
    for (uint32_t i = 0; i < N; i++) {
        dvs_builder_add_var(b, i, 16, 0, 0, 63);
        s = i ? dvs_builder_expr_binary(b, DVS_BIN_ADD, s, var(b, i)) : var(b, i);
    }
    rel(b, DVS_BIN_EQ, s, dvs_builder_expr_const_sized(b, total, 0, 16));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    dvs_handle_set_engine(h, DVS_ENGINE_CDCL);
    uint16_t v[N];
    dvs_ref_t refs[N];
    for (int i = 0; i < N; i++) refs[i] = (dvs_ref_t){ &v[i], 16, 0, DVS_REF_SCALAR, 0 };
    int ok = 1;
    for (uint64_t seed = 1; ok && seed <= 200; seed++) {
        ok &= dvs_randomize(h, refs, NULL, seed) == DVS_RANDOMIZE_OK;
        int64_t t = 0;
        for (int i = 0; i < N; i++) {
            ok &= v[i] < 64;
            t += v[i];
        }
        ok &= t == total;
    }
    CHECK(ok, msg);
    dvs_handle_free(h);
}

int main(void) {
    setenv("DVS_LINPROBE", "2", 1);
    uint64_t n = dvs_linprobe_checks();
    check_chain(33, 0x60000000, 0x100000, 0x2000000, 0x7fffffff, 0x4000000,
                "33-bit chain: probes agree, every draw holds");
    check_ran(n, "33-bit chain: the evaluator ran");
    n = dvs_linprobe_checks();
    check_chain(16, 4000, 16, 512, 8000, 1024, "16-bit chain: probes agree, every draw holds");
    check_ran(n, "16-bit chain: the evaluator ran");
    n = dvs_linprobe_checks();
    check_wrap();
    CHECK(dvs_linprobe_checks() == n, "8-bit sum that can wrap: left to propagation");
    n = dvs_linprobe_checks();
    check_sum(256, "sum of 8, mid total: probes agree, every draw holds");
    check_ran(n, "sum of 8, mid total: the evaluator ran");
    n = dvs_linprobe_checks();
    check_sum(480, "sum of 8, tight total: probes agree, every draw holds");
    check_ran(n, "sum of 8, tight total: the evaluator ran");
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
