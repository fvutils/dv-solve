/* Eviction is transparent (default sampling): the values a call returns are
 * the same on one handle reused across calls and on a fresh handle per call
 * that first imports the state the reused handle exported before that call
 * (dvs_handle_export_state: randc cycles, the clause policy and its kept
 * clauses, the box and enumeration tiers' verdicts). Where none of that
 * changes after the first call (no randc, clauses dropped, the tiers settled
 * the same at every root), a plain fresh handle per call gives the same
 * values too. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_tmpl.h"      /* dvs_handle_export_state */

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

#define NV     8     /* variables per problem, at most */
#define NSEED  60

typedef struct {
    dvs_problem_t *p;
    size_t         sz;
    uint32_t       n;
    uint32_t       width[NV];
    int            engine;  /* dvs_handle_set_engine() before the first call */
    int            state;   /* var 1 is STATE: pinned to 20 + seed % 30 */
    int            carries; /* a plain fresh handle per call may differ: randc
                             * cycles, or tier verdicts that later roots change */
    int            keeps;   /* the clause policy keeps clauses at this site */
} Prob;

static dvs_expr_t V(dvs_builder_t *b, uint32_t i) { return dvs_builder_expr_var(b, i); }
static dvs_expr_t K(dvs_builder_t *b, int64_t k) { return dvs_builder_expr_const(b, k, 0); }
static dvs_expr_t B(dvs_builder_t *b, dvs_binop_t op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}

static void var(dvs_builder_t *b, Prob *pr, uint32_t width, int64_t lo, int64_t hi) {
    dvs_builder_add_var(b, pr->n, width, 0, lo, hi);
    pr->width[pr->n++] = width;
}

static Prob finish(dvs_builder_t *b, Prob pr) {
    pr.p = dvs_builder_finalize(b, &pr.sz);
    return pr;
}

/* A conflict on most calls: 32-bit x with x[1:0] == 0, y < x. */
static Prob aligned(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_CDCL };
    var(b, &pr, 32, 0, 0xFFFFFFFFll);
    var(b, &pr, 16, 0, 65535);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_BAND, V(b, 0), K(b, 3)), K(b, 0)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_LT, V(b, 1), V(b, 0)));
    return finish(b, pr);
}

/* 32-bit x with x[1:0] == 0 on bit-blast. */
static Prob blasted(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_BITBLAST };
    var(b, &pr, 32, 0, 0xFFFFFFFFll);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_BAND, V(b, 0), K(b, 3)), K(b, 0)));
    return finish(b, pr);
}

/* Bit-blast with a product: a * b == c (32-bit), c[3:0] == 5, c > 70000,
 * a ^ b == 0x1234. */
static Prob blasted2(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_BITBLAST };
    var(b, &pr, 16, 0, 65535);
    var(b, &pr, 16, 0, 65535);
    var(b, &pr, 32, 0, 0xFFFFFFFFll);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MUL, dvs_builder_expr_cast(b, V(b, 0), 32, 0), V(b, 1)), V(b, 2)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_BAND, V(b, 2), K(b, 15)), K(b, 5)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_GT, V(b, 2), K(b, 70000)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_BXOR, V(b, 0), V(b, 1)), K(b, 0x1234)));
    return finish(b, pr);
}

/* Eight different values in [0, 11], the first four summing to 36, the last
 * four to 10, and (v0 * v1) % 7 == 3. */
static Prob hard(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_AUTO };
    uint32_t ids[8];
    for (uint32_t i = 0; i < 8; i++) { ids[i] = i; var(b, &pr, 16, 0, 11); }
    dvs_builder_add_all_different(b, 8, ids);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_ADD, B(b, DVS_BIN_ADD,
        V(b, 0), V(b, 1)), B(b, DVS_BIN_ADD, V(b, 2), V(b, 3))), K(b, 36)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_ADD, B(b, DVS_BIN_ADD,
        V(b, 4), V(b, 5)), B(b, DVS_BIN_ADD, V(b, 6), V(b, 7))), K(b, 10)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD,
        B(b, DVS_BIN_MUL, V(b, 0), V(b, 1)), K(b, 7)), K(b, 3)));
    return finish(b, pr);
}

/* randc x in [0, 11] (several cycles over the calls), y > x. */
static Prob randc(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_AUTO, .carries = 1 };
    var(b, &pr, 4, 0, 15);
    var(b, &pr, 8, 0, 255);
    dvs_builder_set_var_kind(b, 0, DVS_VAR_RANDC);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_LT, V(b, 0), K(b, 12)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_GT, V(b, 1), V(b, 0)));
    return finish(b, pr);
}

/* x < y (STATE), x % 5 == 2, z > 1000, z % 16 == x % 16. The enumeration
 * tier lists the solutions for some pins and not others, and gives up for
 * the context when a list fails: a verdict of the calls so far. */
static Prob pinned(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_AUTO, .state = 1, .carries = 1 };
    for (int i = 0; i < 3; i++) var(b, &pr, 16, 0, 65535);
    dvs_builder_set_var_kind(b, 1, DVS_VAR_STATE);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_LT, V(b, 0), V(b, 1)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD, V(b, 0), K(b, 5)), K(b, 2)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_GT, V(b, 2), K(b, 1000)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD, V(b, 2), K(b, 16)),
                                    B(b, DVS_BIN_MOD, V(b, 0), K(b, 16))));
    return finish(b, pr);
}

/* x + y == 100; soft x == 200 (dropped first) and soft y < 10. The box
 * tier answers until a call its draws miss lists the solutions; later calls
 * draw from the list. */
static Prob soft(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_AUTO, .carries = 1 };
    var(b, &pr, 16, 0, 100);
    var(b, &pr, 16, 0, 100);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_ADD, V(b, 0), V(b, 1)), K(b, 100)));
    dvs_builder_add_soft_constraint(b, B(b, DVS_BIN_EQ, V(b, 0), K(b, 200)), 1);
    dvs_builder_add_soft_constraint(b, B(b, DVS_BIN_LT, V(b, 1), K(b, 10)), 0);
    return finish(b, pr);
}

/* x dist {0 := 1, [1:9] :/ 1, 200 := 8}; y > x; y % 3 == 1. */
static Prob dist(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_AUTO };
    var(b, &pr, 16, 0, 255);
    var(b, &pr, 16, 0, 255);
    dvs_dist_entry_t e[3] = { {0, 0, 1, 1, {0}}, {1, 9, 1, 0, {0}}, {200, 200, 8, 1, {0}} };
    dvs_builder_add_dist(b, 0, 3, e);
    dvs_builder_add_constraint(b, B(b, DVS_BIN_GT, V(b, 1), V(b, 0)));
    dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_MOD, V(b, 1), K(b, 3)), K(b, 1)));
    return finish(b, pr);
}

/* memmap.nonoverlap_regions in small: three regions, base[7:0] == 0, size in
 * [256, 16384], base + size <= 0x100000 wrapping, pairwise apart. Each base
 * lies in one of two intervals, a fact bounds cannot hold: the clause that
 * says so is learnt once and fires on every later call, so the site keeps it. */
static Prob regions(dvs_builder_t *b) {
    Prob pr = { .engine = DVS_ENGINE_CDCL, .carries = 1, .keeps = 1 };
    for (int i = 0; i < 3; i++) var(b, &pr, 32, 0, 0xFFFFFFFFll);
    for (int i = 0; i < 3; i++) var(b, &pr, 32, 256, 16384);
    for (uint32_t i = 0; i < 3; i++) {
        dvs_builder_add_constraint(b, B(b, DVS_BIN_EQ, B(b, DVS_BIN_BAND, V(b, i), K(b, 255)), K(b, 0)));
        dvs_builder_add_constraint(b, B(b, DVS_BIN_LTE, B(b, DVS_BIN_ADD, V(b, i), V(b, i + 3)),
                                        K(b, 0x100000)));
        for (uint32_t j = 0; j < i; j++)
            dvs_builder_add_constraint(b, B(b, DVS_BIN_OR,
                B(b, DVS_BIN_LTE, B(b, DVS_BIN_ADD, V(b, i), V(b, i + 3)), V(b, j)),
                B(b, DVS_BIN_LTE, B(b, DVS_BIN_ADD, V(b, j), V(b, j + 3)), V(b, i))));
    }
    return finish(b, pr);
}

static dvs_handle_t *open_handle(const Prob *pr) {
    dvs_handle_t *h = dvs_handle_new(pr->p, pr->sz);
    if (h && pr->engine != DVS_ENGINE_AUTO) dvs_handle_set_engine(h, pr->engine);
    return h;
}

/* One call: the values in out[], or all ones when it failed. */
static void call(dvs_handle_t *h, const Prob *pr, uint64_t seed, uint64_t *out) {
    uint64_t v[NV] = {0};
    dvs_ref_t refs[NV];
    for (uint32_t i = 0; i < pr->n; i++)
        refs[i] = (dvs_ref_t){ &v[i], pr->width[i] <= 16 ? 16 : 32, 0, DVS_REF_SCALAR, 0 };
    if (pr->state) v[1] = 20 + seed % 30;
    if (dvs_randomize(h, refs, NULL, seed) != DVS_RANDOMIZE_OK) memset(v, 0xFF, sizeof v);
    memcpy(out, v, sizeof v);
}

static void check(const char *name, Prob pr) {
    static uint64_t reused[NSEED][NV], fresh[NSEED][NV], side[NSEED][NV];
    char msg[160];
    int imp_ok = 1;
    uint32_t kept = 0;   /* calls that started with kept clauses */
    size_t most = 0;     /* the largest state exported */
    dvs_handle_t *hu = open_handle(&pr);
    for (int s = 0; s < NSEED; s++) {
        uint64_t seed = 7919u * (uint64_t)s + 1;
        size_t nw = dvs_handle_export_state(hu, NULL, 0);
        int64_t *w = malloc((nw ? nw : 1) * sizeof *w);
        imp_ok &= dvs_handle_export_state(hu, w, nw) == nw;
        if (nw > most) most = nw;
        kept += dvs_handle_n_learnt(hu) != 0;
        dvs_handle_t *hs = open_handle(&pr);
        imp_ok &= dvs_handle_import_state(hs, w, nw) == 0;
        call(hs, &pr, seed, side[s]);
        dvs_handle_free(hs);
        free(w);
        dvs_handle_t *hf = open_handle(&pr);
        call(hf, &pr, seed, fresh[s]);
        dvs_handle_free(hf);
        call(hu, &pr, seed, reused[s]);
    }
    printf("  %s: engine %d, state at most %zu words, %u calls began with clauses\n", name,
           dvs_handle_engine(hu), most, kept);
    dvs_handle_free(hu);

    int ok_ok = 1, distinct = 0, first = -1;
    for (int s = 0; s < NSEED; s++) {
        ok_ok &= reused[s][0] != ~0ull;
        distinct += s && memcmp(reused[s], reused[s - 1], sizeof reused[s]) != 0;
        if (first < 0 && memcmp(reused[s], side[s], sizeof side[s])) first = s;
    }
    snprintf(msg, sizeof msg, "%s: every call solved", name);
    CHECK(ok_ok, msg);
    snprintf(msg, sizeof msg, "%s: values vary with the seed", name);
    CHECK(distinct >= NSEED / 4, msg);
    snprintf(msg, sizeof msg, "%s: export and import", name);
    CHECK(imp_ok, msg);
    snprintf(msg, sizeof msg, "%s: fresh handle + imported state == reused handle "
             "(first difference: call %d)", name, first);
    CHECK(first < 0, msg);
    if (pr.keeps) {
        snprintf(msg, sizeof msg, "%s: most calls begin with the clauses kept", name);
        CHECK(kept >= NSEED / 2, msg);
    }
    if (!pr.carries) {
        first = -1;
        for (int s = 0; s < NSEED && first < 0; s++)
            if (memcmp(reused[s], fresh[s], sizeof fresh[s])) first = s;
        snprintf(msg, sizeof msg, "%s: plain fresh handle == reused handle "
                 "(first difference: call %d)", name, first);
        CHECK(first < 0, msg);
    }
}

int main(int argc, char **argv) {   /* argv[1]: one problem only */
    struct { const char *name; Prob (*make)(dvs_builder_t *); } t[] = {
        {"aligned", aligned}, {"blasted", blasted}, {"blasted2", blasted2}, {"hard", hard}, {"randc", randc},
        {"pinned", pinned}, {"soft", soft}, {"dist", dist}, {"regions", regions},
    };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        if (argc > 1 && strcmp(argv[1], t[i].name)) continue;
        dvs_builder_t *b = dvs_builder_create(0, NULL);
        Prob pr = t[i].make(b);
        check(t[i].name, pr);
        dvs_builder_free_problem(b, pr.p, pr.sz);
        dvs_builder_destroy(b);
    }
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
