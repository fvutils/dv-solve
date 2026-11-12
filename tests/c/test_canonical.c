/* Canonical sampling (DVS_CANONICAL=1, dvs_solve_opts_t.canonical): the values
 * a call returns depend only on the constraints, the STATE values and the
 * seed. The same seeds give the same values on a fresh handle per call, on
 * one handle reused in either seed order, and on a handle preloaded with the
 * learnt clauses of a default-mode handle. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_tmpl.h"      /* dvs_handle_export_state */

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

#define NV     8     /* variables per problem, at most */
#define NSEED  40

typedef struct {
    dvs_problem_t *p;
    size_t         sz;
    uint32_t       n;
    int            state;   /* var 1 is STATE: pinned to 20 + seed % 30 */
} Prob;

static dvs_expr_t V(dvs_builder_t *b, uint32_t i) { return dvs_builder_expr_var(b, i); }
static dvs_expr_t K(dvs_builder_t *b, int64_t k) { return dvs_builder_expr_const(b, k, 0); }
static dvs_expr_t B(dvs_builder_t *b, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}

static void vars(dvs_builder_t *b, uint32_t n, int64_t hi) {
    for (uint32_t i = 0; i < n; i++) dvs_builder_add_var(b, i, 16, 0, 0, hi);
}

static Prob finish(dvs_builder_t *b, uint32_t n, int state) {
    Prob pr = { NULL, 0, n, state };
    pr.p = dvs_builder_finalize(b, &pr.sz);
    return pr;
}

/* Conflicts: eight different values in [0, 11], the first four summing to 36,
 * the last four to 10, and (v0 * v1) % 7 == 3. */
static Prob hard(dvs_builder_t *b) {
    vars(b, 8, 11);
    uint32_t ids[8] = {0, 1, 2, 3, 4, 5, 6, 7};
    dvs_builder_add_all_different(b, 8, ids);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_ADD, B(b, DVS_BIN_ADD,
        V(b, 0), V(b, 1)), B(b, DVS_BIN_ADD, V(b, 2), V(b, 3))), K(b, 36)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_ADD, B(b, DVS_BIN_ADD,
        V(b, 4), V(b, 5)), B(b, DVS_BIN_ADD, V(b, 6), V(b, 7))), K(b, 10)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD,
        B(b, DVS_BIN_MUL, V(b, 0), V(b, 1)), K(b, 7)), K(b, 3)));
    return finish(b, 8, 0);
}

/* x & 7 == 0, x > y, y % 5 == 3: the default search learns `x != k`
 * clauses here, which the preload carries. */
static Prob aligned(dvs_builder_t *b) {
    vars(b, 2, 65535);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_BAND, V(b, 0), K(b, 7)), K(b, 0)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_GT, V(b, 0), V(b, 1)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD, V(b, 1), K(b, 5)), K(b, 3)));
    return finish(b, 2, 0);
}

/* base[7:0] == 0, base + size <= 0x100000 wrapping, size in [256, 16384]:
 * base lies in one of two intervals, which a default-mode handle learns
 * once and keeps (its clause policy), so the preload carries it. The
 * alignment is written `((base * 3) & 255) == 0` (the same values), which
 * the value picker's known bits do not read: it needs draws that miss.
 * The search's own known bits (dvs_kb.h) do read it and learn nothing, so
 * this problem runs with them off (DVS_KB=0, read when a context is made). */
static Prob wrapped(dvs_builder_t *b) {
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFll);
    dvs_builder_add_var(b, 1, 32, 0, 256, 16384);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ,
                                    B(b, DVS_BIN_BAND, B(b, DVS_BIN_MUL, V(b, 0), K(b, 3)),
                                      K(b, 255)),
                                    K(b, 0)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_LTE, B(b, DVS_BIN_ADD, V(b, 0), V(b, 1)),
                                    K(b, 0x100000)));
    return finish(b, 2, 0);
}

/* x < y (STATE), x % 5 == 2, z > 1000, z % 16 == x % 16. */
static Prob pinned(dvs_builder_t *b) {
    vars(b, 3, 65535);
    dvs_builder_set_var_kind(b, 1, DVS_VAR_STATE);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_LT, V(b, 0), V(b, 1)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD, V(b, 0), K(b, 5)), K(b, 2)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_GT, V(b, 2), K(b, 1000)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD, V(b, 2), K(b, 16)),
                                    B(b, DVS_BIN_MOD, V(b, 0), K(b, 16))));
    return finish(b, 3, 1);
}

/* x + y == 100; soft x == 200 (dropped first) and soft y < 10. */
static Prob soft(dvs_builder_t *b) {
    vars(b, 2, 100);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_ADD, V(b, 0), V(b, 1)), K(b, 100)));
    dvs_builder_add_soft_constraint(b, B(b, DVS_BIN_EQ, V(b, 0), K(b, 200)), 1);
    dvs_builder_add_soft_constraint(b, B(b, DVS_BIN_LT, V(b, 1), K(b, 10)), 0);
    return finish(b, 2, 0);
}

/* x dist {0 := 1, [1:9] :/ 1, 200 := 8}; y > x; y % 3 == 1. */
static Prob dist(dvs_builder_t *b) {
    vars(b, 2, 255);
    dvs_dist_entry_t e[3] = { {0, 0, 1, 1, {0}}, {1, 9, 1, 0, {0}}, {200, 200, 8, 1, {0}} };
    dvs_builder_add_dist(b, 0, 3, e);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_GT, V(b, 1), V(b, 0)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD, V(b, 1), K(b, 3)), K(b, 1)));
    return finish(b, 2, 0);
}

/* One call: the values in out[], or all 0xFFFF when it failed. */
static void call(dvs_handle_t *h, const Prob *pr, uint64_t seed, uint16_t *out) {
    uint16_t v[NV] = {0};
    dvs_ref_t refs[NV];
    for (uint32_t i = 0; i < pr->n; i++) refs[i] = (dvs_ref_t){ &v[i], 16, 0, DVS_REF_SCALAR, 0 };
    if (pr->state) v[1] = (uint16_t)(20 + seed % 30);
    if (dvs_randomize(h, refs, NULL, seed) != DVS_RANDOMIZE_OK) memset(v, 0xFF, sizeof v);
    memcpy(out, v, sizeof v);
}

static void check(const char *name, Prob pr) {
    static uint16_t fresh[NSEED][NV], up[NSEED][NV], down[NSEED][NV], pre[NSEED][NV];
    char msg[128];
    for (int s = 0; s < NSEED; s++) {
        dvs_handle_t *h = dvs_handle_new(pr.p, pr.sz);
        call(h, &pr, (uint64_t)s + 1, fresh[s]);
        dvs_handle_free(h);
    }
    dvs_handle_t *hu = dvs_handle_new(pr.p, pr.sz);
    for (int s = 0; s < NSEED; s++) call(hu, &pr, (uint64_t)s + 1, up[s]);
    dvs_handle_t *hd = dvs_handle_new(pr.p, pr.sz);
    for (int s = NSEED - 1; s >= 0; s--) call(hd, &pr, (uint64_t)s + 1, down[s]);
    /* Preload: the clauses a default-mode handle ended with. */
    setenv("DVS_CANONICAL", "0", 1);
    dvs_handle_t *hl = dvs_handle_new(pr.p, pr.sz);
    setenv("DVS_CANONICAL", "1", 1);
    dvs_handle_set_engine(hl, DVS_ENGINE_CDCL);
    uint16_t scratch[NV];
    for (int s = 0; s < NSEED; s++) call(hl, &pr, (uint64_t)s + 1000, scratch);
    size_t nw = dvs_handle_export_state(hl, NULL, 0);
    int64_t *w = malloc((nw ? nw : 1) * sizeof *w);
    dvs_handle_export_state(hl, w, nw);
    dvs_handle_t *hp = dvs_handle_new(pr.p, pr.sz);
    int imp = dvs_handle_import_state(hp, w, nw);
    uint32_t npre = dvs_handle_n_learnt(hp);
    for (int s = 0; s < NSEED; s++) call(hp, &pr, (uint64_t)s + 1, pre[s]);
    free(w);

    int ok_ok = 1, distinct = 0;
    for (int s = 0; s < NSEED; s++) {
        ok_ok &= fresh[s][0] != 0xFFFF;
        distinct += s && memcmp(fresh[s], fresh[s - 1], sizeof fresh[s]) != 0;
    }
    snprintf(msg, sizeof msg, "%s: every call solved", name);
    CHECK(ok_ok, msg);
    snprintf(msg, sizeof msg, "%s: values vary with the seed", name);
    CHECK(distinct >= NSEED / 4, msg);
    snprintf(msg, sizeof msg, "%s: reused handle == fresh handle per call", name);
    CHECK(!memcmp(fresh, up, sizeof fresh), msg);
    snprintf(msg, sizeof msg, "%s: reused handle, seeds descending == fresh", name);
    CHECK(!memcmp(fresh, down, sizeof fresh), msg);
    snprintf(msg, sizeof msg, "%s: preloaded clauses (%zu words, rc %d) == fresh", name, nw, imp);
    CHECK(imp == 0 && !memcmp(fresh, pre, sizeof fresh), msg);
    if (!strcmp(name, "wrapped")) {
        snprintf(msg, sizeof msg, "%s: the preload has clauses (%u)", name, npre);
        CHECK(npre > 0, msg);
    }
    dvs_handle_free(hl);
    dvs_handle_free(hu);
    dvs_handle_free(hd);
    dvs_handle_free(hp);
}

int main(void) {
    setenv("DVS_CANONICAL", "1", 1);   /* read when a handle is made */
    struct { const char *name; Prob (*make)(dvs_builder_t *); } t[] = {
        {"hard", hard}, {"aligned", aligned}, {"wrapped", wrapped}, {"pinned", pinned}, {"soft", soft}, {"dist", dist},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        dvs_builder_t *b = dvs_builder_create(0, NULL);
        Prob pr = t[i].make(b);
        if (!strcmp(t[i].name, "wrapped")) setenv("DVS_KB", "0", 1);
        check(t[i].name, pr);
        unsetenv("DVS_KB");
        dvs_builder_free_problem(b, pr.p, pr.sz);
        dvs_builder_destroy(b);
    }
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
