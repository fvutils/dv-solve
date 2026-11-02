/*
 * Solve statistics (docs/solve_stats_capture_plan.md, S0): every solve on
 * every path is counted exactly once, with the engine that answered, and
 * nothing when statistics are off.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_bbsolver.h"
#include "dvs_search.h"
#include "dvs_thread.h"
#include "smt2/smt2_lexer.h"
#include "smt2/smt2_parser.h"
#include "smt2/smt2_frontend.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr, "FAIL %s\n", msg); failures++; } \
                           else printf("PASS %s\n", msg); } while (0)

/* ------------------------------------------------------------------ */
/* Snapshot access                                                     */
/* ------------------------------------------------------------------ */

static char *snap(void) {
    size_t n = dvs_stats_snapshot_json(NULL, 0);
    char *buf = malloc(n + 1);
    size_t m = dvs_stats_snapshot_json(buf, n + 1);
    if (m != n) { fprintf(stderr, "FAIL snapshot length changed\n"); failures++; }
    if (getenv("DVS_TEST_DUMP")) printf("%s\n", buf);
    return buf;
}

/* The number after the last of `keys`, each searched for (as "key":) after the
 * one before it; -1 if one is missing. Keys are distinct enough in the
 * snapshot for this to name one value. */
static long long jnum(const char *js, int n, const char *const *keys) {
    const char *p = js;
    for (int i = 0; i < n; i++) {
        char k[64];
        snprintf(k, sizeof k, "\"%s\":", keys[i]);
        p = strstr(p, k);
        if (!p) return -1;
        p += strlen(k);
    }
    return strtoll(p, NULL, 10);
}
/* The sum of the numbers in the object under top-level key `key`. */
static long long jsum(const char *js, const char *key) {
    char k[64];
    snprintf(k, sizeof k, "\"%s\":{", key);
    const char *p = strstr(js, k);
    if (!p) return -1;
    p += strlen(k);
    long long sum = 0;
    while (*p && *p != '}') {
        p = strchr(p, ':');
        if (!p) return -1;
        sum += strtoll(p + 1, (char **)&p, 10);
        if (*p == ',') p++;
    }
    return sum;
}

#define J(js, ...) jnum(js, (int)(sizeof((const char *[]){__VA_ARGS__}) / sizeof(const char *)), \
                        (const char *[]){__VA_ARGS__})

/* Every solve has one answer path and one answering engine (or none). */
static void check_totals(const char *js, const char *who) {
    char msg[96];
    snprintf(msg, sizeof msg, "%s: via and answered_by each sum to solves", who);
    long long n = J(js, "solves");
    CHECK(jsum(js, "via") == n && jsum(js, "answered_by") == n, msg);
}

/* The number of per-class records. */
static int n_classes(const char *js) {
    int n = 0;
    for (const char *p = js; (p = strstr(p, "{\"class\":\"")) != NULL; p++) n++;
    return n;
}

/* The class id (16 hex digits) of the record with `solves` solves, in `out`;
 * 0 if there is none. */
static int class_with(const char *js, long long solves, char out[17]) {
    for (const char *p = js; (p = strstr(p, "{\"class\":\"")) != NULL; p++) {
        const char *id = p + 10;
        const char *q = strstr(id, "\"solves\":");
        if (q && strtoll(q + 9, NULL, 10) == solves) {
            memcpy(out, id, 16);
            out[16] = '\0';
            return 1;
        }
    }
    return 0;
}

static char lt_class[17];   /* x < y's class, from test_api */

/* ------------------------------------------------------------------ */
/* Problems                                                            */
/* ------------------------------------------------------------------ */

/* x < y over 8 bits; with `unsat`, also x > y. */
static dvs_problem_t *lt_problem(int unsat, size_t *sz) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 1)));
    if (unsat)
        dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_GT,
            dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 1)));
    dvs_problem_t *p = dvs_builder_finalize(b, sz);
    /* finalize hands back memory the builder owns: keep a copy. */
    dvs_problem_t *cp = malloc(*sz);
    memcpy(cp, p, *sz);
    dvs_builder_free_problem(b, p, *sz);
    dvs_builder_destroy(b);
    return cp;
}

/* x * y == 77 over 8 bits: the shape rule routes it to bitblast. */
static dvs_problem_t *mul_problem(size_t *sz) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_binary(b, DVS_BIN_MUL, dvs_builder_expr_var(b, 0),
                                dvs_builder_expr_var(b, 1)),
        dvs_builder_expr_const(b, 77, 0)));
    dvs_problem_t *p = dvs_builder_finalize(b, sz);
    dvs_problem_t *cp = malloc(*sz);
    memcpy(cp, p, *sz);
    dvs_builder_free_problem(b, p, *sz);
    dvs_builder_destroy(b);
    return cp;
}

static int smt2_run(Smt2Frontend *fe, const char *input) {
    Smt2Lexer lex;
    SexprArena arena;
    smt2_lexer_init(&lex, input, strlen(input));
    sexpr_arena_init(&arena, 0);
    int rc = 0;
    for (;;) {
        Sexpr *s = sexpr_parse(&lex, &arena);
        if (!s) break;
        rc = smt2_frontend_dispatch(fe, s);
        if (rc != 0) break;
    }
    sexpr_arena_destroy(&arena);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Tests                                                               */
/* ------------------------------------------------------------------ */

/* Must run first: DV_STATS is read on first use. */
static void test_env(void) {
    static char on[] = "DV_STATS=1";
    putenv(on);
    CHECK(dvs_stats_enabled() == 1, "env: DV_STATS=1 turns statistics on");
}

static void test_off(void) {
    size_t sz;
    dvs_problem_t *p = lt_problem(0, &sz);
    dvs_stats_enable(0);
    dvs_stats_reset();
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, p);
    for (int i = 0; i < 10; i++) dvs_solver_solve(ctx, NULL);
    dvs_solver_free(ctx);
    char *js = snap();
    CHECK(J(js, "solves") == 0 && J(js, "compiles") == 0, "off: nothing counted");
    free(js);
    free(p);
}

static void test_api(void) {
    size_t sz;
    dvs_problem_t *p = lt_problem(0, &sz), *u = lt_problem(1, &sz);
    dvs_stats_enable(1);
    dvs_stats_reset();
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, p);
    for (int i = 0; i < 7; i++) {
        dvs_solver_reset(ctx);
        dvs_solver_solve(ctx, NULL);
    }
    uint32_t ids[2] = { 0, 1 };
    int64_t out[2 * 5];
    dvs_solver_solve_n(ctx, 5, 2, ids, out, 1, 0);
    dvs_solver_free(ctx);

    dvs_ctx_t *cu = dvs_solver_new(NULL);
    dvs_solver_compile(cu, u);
    dvs_solver_solve(cu, NULL);
    dvs_solver_free(cu);

    char *js = snap();
    CHECK(J(js, "solves") == 13, "api: 7 solves + solve_n(5) + 1 unsat = 13");
    CHECK(J(js, "compiles") == 2, "api: 2 compiles");
    CHECK(J(js, "paths", "api", "solves") == 13, "api: all on the api path");
    CHECK(J(js, "results", "sat") == 12 && J(js, "results", "unsat") == 1,
          "api: 12 sat, 1 unsat");
    CHECK(J(js, "engine_ran", "cdcl") == 13 && J(js, "answered_by", "cdcl") == 13,
          "api: cdcl ran and answered each");
    CHECK(J(js, "fallbacks") == 0, "api: no fallbacks");
    CHECK(J(js, "work", "cdcl") > 0, "api: cdcl work counted");
    CHECK(J(js, "via", "cdcl-root") == 1, "api: the unsat solve was decided before search");
    CHECK(n_classes(js) == 2 && class_with(js, 12, lt_class),
          "api: two classes, x<y with its 12 solves");
    CHECK(J(js, "motifs", "var-cmp-var", "count") == 3,
          "api: motifs summed over classes (x<y once, x<y && x>y twice)");
    CHECK(J(js, "classes", "features", "vars") == 2 && J(js, "classes", "motifs", "var-cmp-var") >= 1,
          "api: each class carries its features and motifs");
    check_totals(js, "api");
    free(js);
    free(p);
    free(u);
}

static void test_handle(void) {
    size_t sz;
    dvs_problem_t *p = lt_problem(0, &sz);
    dvs_stats_enable(1);
    dvs_stats_reset();
    dvs_handle_t *h = dvs_handle_new(p, sz);
    uint8_t x, y;
    dvs_ref_t refs[2] = { {&x, 8, 0, DVS_REF_SCALAR, 0}, {&y, 8, 0, DVS_REF_SCALAR, 0} };
    for (uint64_t s = 1; s <= 9; s++) dvs_randomize(h, refs, NULL, s);
    dvs_handle_free(h);
    free(p);

    dvs_problem_t *m = mul_problem(&sz);
    h = dvs_handle_new(m, sz);
    for (uint64_t s = 1; s <= 4; s++) dvs_randomize(h, refs, NULL, s);
    dvs_handle_free(h);
    free(m);

    char *js = snap();
    CHECK(J(js, "solves") == 13, "handle: 9 + 4 randomize calls, inner solves not counted again");
    CHECK(J(js, "paths", "handle", "solves") == 13, "handle: all on the handle path");
    CHECK(J(js, "answered_by", "cdcl") == 9, "handle: cdcl answered the x<y calls");
    CHECK(J(js, "answered_by", "bitblast") == 4, "handle: bitblast answered the multiplier calls");
    CHECK(J(js, "reasons", "multiplier") == 4, "handle: the shape rule is the reason, every call");
    char c9[17];
    CHECK(n_classes(js) == 2 && class_with(js, 9, c9) && strcmp(c9, lt_class) == 0,
          "handle: x<y has the class it had on the api path");
    check_totals(js, "handle");
    free(js);
}

static void test_bvsat(void) {
    size_t sz;
    dvs_problem_t *m = mul_problem(&sz);
    dvs_stats_enable(1);
    dvs_stats_reset();
    for (int i = 0; i < 3; i++) {
        dvs_bbsolver_t *bb = dvs_bbsolver_new(NULL, m);
        dvs_bbsolver_check(bb, (uint64_t)i + 1);
        dvs_bbsolver_free(bb);
    }
    free(m);
    char *js = snap();
    CHECK(J(js, "paths", "bvsat", "solves") == 3, "bvsat: 3 checks on the bvsat path");
    CHECK(J(js, "answered_by", "bitblast") == 3, "bvsat: bitblast answered");
    check_totals(js, "bvsat");
    free(js);
}

static const char *k_script =
    "(set-logic QF_BV)\n"
    "(declare-const a (_ BitVec 8))\n"
    "(declare-const b (_ BitVec 8))\n"
    "(declare-const p Bool)\n"
    "(assert (bvult a b))\n"
    "(assert (=> p (bvult a #x10)))\n"
    "(check-sat)\n"
    "(check-sat)\n"
    "(push 1)\n"
    "(assert (bvugt a b))\n"
    "(check-sat)\n"
    "(pop 1)\n"
    "(check-sat-assuming (p))\n";

static void test_smt2(void) {
    dvs_stats_enable(1);
    dvs_stats_reset();
    Smt2Frontend *fe = malloc(sizeof *fe);
    FILE *sink = tmpfile();
    smt2_frontend_init(fe, sink, sink);
    smt2_run(fe, k_script);
    smt2_frontend_destroy(fe);

    smt2_frontend_init(fe, sink, sink);
    fe->verilator_mode = 1;
    smt2_run(fe, "(set-logic QF_BV)\n(declare-const a (_ BitVec 8))\n"
                 "(assert (bvult a #x20))\n(check-sat)\n(check-sat)\n");
    smt2_frontend_destroy(fe);

    smt2_frontend_init(fe, sink, sink);
    smt2_run(fe, "(set-option :produce-unsat-cores true)\n(set-logic QF_BV)\n"
                 "(declare-const a (_ BitVec 8))\n"
                 "(assert (! (bvult a #x10) :named c1))\n"
                 "(assert (! (bvugt a #x20) :named c2))\n"
                 "(assert (! (bvult a #x40) :named c3))\n"
                 "(check-sat)\n(get-unsat-core)\n");
    smt2_frontend_destroy(fe);
    fclose(sink);
    free(fe);

    char *js = snap();
    CHECK(J(js, "paths", "smt2", "solves") == 5, "smt2: 4 check-sats + 1 in the core script");
    CHECK(J(js, "paths", "smt2", "sat") == 3 && J(js, "paths", "smt2", "unsat") == 2,
          "smt2: 3 sat, 2 unsat");
    CHECK(J(js, "paths", "verilator", "solves") == 2, "smt2: 2 verilator-mode check-sats");
    CHECK(J(js, "solves") == 7, "smt2: core replays are not counted as solves");
    CHECK(J(js, "aux", "calls") == 1, "smt2: get-unsat-core counted once as auxiliary work");
    CHECK(J(js, "reasons", "probe") == 1 && J(js, "reasons", "sticky") == 1,
          "smt2: verilator mode probes CDCL once, then keeps the route");
    check_totals(js, "smt2");
    free(js);
}

/* x < c over 8 bits. */
static dvs_problem_t *lt_const_problem(int64_t c, size_t *sz) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_const(b, c, 0)));
    dvs_problem_t *p = dvs_builder_finalize(b, sz);
    dvs_problem_t *cp = malloc(*sz);
    memcpy(cp, p, *sz);
    dvs_builder_free_problem(b, p, *sz);
    dvs_builder_destroy(b);
    return cp;
}

/* Constants of one class: one class record, counting distinct instances. */
static void test_instances(void) {
    static const int64_t small[] = { 3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18,
                                     19, 20, 21, 22, 23 };
    const int n = (int)(sizeof small / sizeof small[0]);
    dvs_stats_enable(1);
    dvs_stats_reset();
    for (int i = 0; i < n; i++) {
        size_t sz;
        dvs_problem_t *p = lt_const_problem(small[i], &sz);
        dvs_ctx_t *ctx = dvs_solver_new(NULL);
        dvs_solver_compile(ctx, p);
        for (int k = 0; k < 3; k++) {
            dvs_solver_reset(ctx);
            dvs_solver_solve(ctx, NULL);
        }
        dvs_solver_free(ctx);
        free(p);
    }
    char *js = snap();
    long long est = J(js, "classes", "instances");
    CHECK(n_classes(js) == 1 && J(js, "classes", "solves") == 3 * n,
          "instances: 18 constants of one class make one class record");
    CHECK(est >= 14 && est <= 23, "instances: about 18 distinct instances estimated");
    free(js);
}

/* x < y and z == 5, three 8-bit variables declared 0..255. */
static dvs_problem_t *pinned_problem(size_t *sz) {
    dvs_builder_t *b = dvs_builder_create(0, NULL);
    for (uint32_t i = 0; i < 3; i++) dvs_builder_add_var(b, i, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 1)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_var(b, 2), dvs_builder_expr_const(b, 5, 0)));
    dvs_problem_t *p = dvs_builder_finalize(b, sz);
    dvs_problem_t *cp = malloc(*sz);
    memcpy(cp, p, *sz);
    dvs_builder_free_problem(b, p, *sz);
    dvs_builder_destroy(b);
    return cp;
}

static int count_of(const char *js, const char *needle) {
    int n = 0;
    for (const char *p = js; (p = strstr(p, needle)) != NULL; p++) n++;
    return n;
}

/* Value quality (signals 4 and 7): what the models spanned, per tracked
 * variable, and how many distinct models there were. */
static void test_values(void) {
    size_t sz;
    dvs_problem_t *p = pinned_problem(&sz);
    dvs_stats_enable(1);
    dvs_stats_reset();
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, p);
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    for (int i = 0; i < 40; i++) {
        o.seed = (uint64_t)i + 1;
        dvs_solver_reset(ctx);
        dvs_solver_solve(ctx, &o);
    }
    dvs_solver_free(ctx);
    char *js = snap();
    CHECK(J(js, "values", "models") == 40, "values: 40 models noted");
    long long sol = J(js, "values", "solutions");
    CHECK(sol >= 25 && sol <= 48, "values: about 40 distinct models estimated");
    CHECK(count_of(js, "{\"width\":8,") == 3, "values: three 8-bit variables tracked");
    CHECK(count_of(js, "\"free\":0,\"used\":3,\"varies\":0,\"max_bias\":0,\"stuck\":0") == 1,
          "values: z == 5 spans 3 bits and never varies; compile pinned it, so not stuck");
    CHECK(count_of(js, "\"free\":1,") == 2 && count_of(js, "\"stuck\":0") == 3,
          "values: x and y are free and vary");
    CHECK(strstr(js, "\"mismatched\":0") != NULL, "values: one layout");
    free(js);

    /* the same seed every time: one model, and x and y are stuck */
    dvs_stats_reset();
    ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, p);
    o.seed = 7;
    for (int i = 0; i < 20; i++) {
        dvs_solver_reset(ctx);
        dvs_solver_solve(ctx, &o);
    }
    dvs_solver_free(ctx);
    js = snap();
    CHECK(J(js, "values", "models") == 20 && J(js, "values", "solutions") == 1,
          "values: one distinct model of 20");
    CHECK(count_of(js, "\"stuck\":1") == 2, "values: x and y never changed: stuck");
    free(js);

    /* the same on the handle and bit-level paths; unsat solves add none */
    dvs_stats_reset();
    dvs_handle_t *h = dvs_handle_new(p, sz);
    uint8_t x[3];
    dvs_ref_t refs[3] = { {&x[0], 8, 0, DVS_REF_SCALAR, 0}, {&x[1], 8, 0, DVS_REF_SCALAR, 0},
                          {&x[2], 8, 0, DVS_REF_SCALAR, 0} };
    for (uint64_t s = 1; s <= 9; s++) dvs_randomize(h, refs, NULL, s);
    dvs_handle_free(h);
    free(p);
    js = snap();
    CHECK(J(js, "values", "models") == 9, "values: 9 models on the handle path");
    CHECK(J(js, "classes", "uniform") == 9, "uniform: the handle path asks for uniform sampling");
    free(js);

    dvs_stats_reset();
    dvs_problem_t *m = mul_problem(&sz);
    for (int i = 0; i < 6; i++) {
        dvs_bbsolver_t *bb = dvs_bbsolver_new(NULL, m);
        dvs_bbsolver_check(bb, (uint64_t)i + 1);
        dvs_bbsolver_free(bb);
    }
    free(m);
    dvs_problem_t *u = lt_problem(1, &sz);
    ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, u);
    dvs_solver_solve(ctx, NULL);
    dvs_solver_free(ctx);
    free(u);
    js = snap();
    CHECK(J(js, "values", "models") == 6 && count_of(js, "\"values\":") == 1,
          "values: 6 bit-level models; the unsat class has none");
    free(js);
}

/* Signals 3, 5 and 6: constrainedness at the root, binding constraints, and
 * time outside the solves and repeats. */
static void test_signals(void) {
    size_t sz;
    dvs_problem_t *p = pinned_problem(&sz);
    dvs_stats_enable(1);
    dvs_stats_reset();
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, p);
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    for (int i = 0; i < 70; i++) {
        o.seed = (uint64_t)i + 1;
        dvs_solver_reset(ctx);
        dvs_solver_solve(ctx, &o);
    }
    dvs_solver_free(ctx);
    free(p);
    char *js = snap();
    /* 24 declared bits; x < y leaves 255 values each, z == 5 one */
    CHECK(J(js, "classes", "root", "count") == 1, "root: measured once per compile");
    CHECK(J(js, "classes", "root", "vars") == 3 && J(js, "classes", "root", "fixed") == 1
          && J(js, "classes", "root", "tied") == 0, "root: three vars, z fixed");
    CHECK(J(js, "classes", "root", "bits") == 24 && J(js, "classes", "root", "free_bits") == 16,
          "root: 2^24 declared, 2^16 at the root");
    CHECK(J(js, "classes", "root", "constraints") == 2 && J(js, "classes", "root", "quiet") == 1
          && J(js, "classes", "root", "dropped") == 0, "root: z == 5 compiled to a bound");
    /* sampled: the first 64 solves, then one in 16 (the 65th) */
    CHECK(J(js, "classes", "binding", "solves") == 65, "binding: 65 of 70 solves sampled");
    CHECK(J(js, "classes", "binding", "narrowed") == 65
          && J(js, "classes", "binding", "busy") == 65
          && J(js, "classes", "binding", "top_share") == 65000
          && J(js, "classes", "binding", "other_share") == 0,
          "binding: x < y does all the narrowing");
    CHECK(J(js, "classes", "previous", "same") == 69
          && J(js, "classes", "previous", "shape") == 1, "previous: 69 repeats");
    CHECK(J(js, "classes", "time_ns", "compile") > 0, "time: compile timed");
    CHECK(J(js, "classes", "uniform") == 0, "uniform: plain seeded API solves do not");
    free(js);

    /* two instances of one class, solved in turn: other constants each time */
    dvs_stats_reset();
    dvs_problem_t *pa = lt_const_problem(3, &sz), *pb = lt_const_problem(9, &sz);
    dvs_ctx_t *a = dvs_solver_new(NULL), *b = dvs_solver_new(NULL);
    dvs_solver_compile(a, pa);
    dvs_solver_compile(b, pb);
    for (int i = 0; i < 4; i++) {
        dvs_ctx_t *c = i & 1 ? b : a;
        dvs_solver_reset(c);
        dvs_solver_solve(c, NULL);
    }
    dvs_solver_free(a);
    dvs_solver_free(b);
    free(pa);
    free(pb);
    js = snap();
    CHECK(J(js, "classes", "previous", "same") == 0
          && J(js, "classes", "previous", "constants") == 3
          && J(js, "classes", "previous", "shape") == 1, "previous: constants changed");
    CHECK(J(js, "classes", "root", "count") == 2, "root: two compiles");
    free(js);

    /* API: the builder's finalize is build time and get_values read time,
     * both outside a solve; a caller adds its own with dvs_stats_add_time */
    dvs_stats_reset();
    dvs_stats_add_time(DVS_STATS_TIME_BUILD, 1000000000ull);
    p = pinned_problem(&sz);                /* finalize: build time */
    ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, p);
    dvs_solver_solve(ctx, NULL);
    uint32_t ids[3] = { 0, 1, 2 };
    int64_t vals[3];
    dvs_solver_get_values(ctx, 3, ids, vals);
    dvs_stats_add_time(DVS_STATS_TIME_READ, 1000000000ull);
    dvs_solver_free(ctx);
    free(p);
    js = snap();
    CHECK(J(js, "classes", "time_ns", "build") > 1000000000ll,
          "time: reported build time plus the builder's, on the next solve");
    CHECK(J(js, "classes", "time_ns", "read") > 1000000000ll,
          "time: get_values plus reported read time, on the last solved class");
    free(js);

    /* SMT2: build time goes to the next solve, read-back to the last one */
    dvs_stats_reset();
    Smt2Frontend *fe = malloc(sizeof *fe);
    FILE *sink = tmpfile();
    smt2_frontend_init(fe, sink, sink);
    smt2_run(fe, "(set-logic QF_BV)\n(declare-const a (_ BitVec 8))\n"
                 "(declare-const b (_ BitVec 8))\n(assert (bvult a b))\n(check-sat)\n"
                 "(get-value (a b))\n(get-value (a))\n");
    smt2_frontend_destroy(fe);
    free(fe);
    fclose(sink);
    js = snap();
    CHECK(J(js, "classes", "time_ns", "build") > 0, "time: SMT2 build time");
    CHECK(J(js, "classes", "time_ns", "read") > 0, "time: SMT2 read-back time");
    free(js);
}

#define N_THREADS 4
#define N_PER     150

static void *worker(void *arg) {
    dvs_problem_t *p = arg;
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    dvs_solver_compile(ctx, p);
    for (int i = 0; i < N_PER; i++) {
        dvs_solver_reset(ctx);
        dvs_solver_solve(ctx, NULL);
    }
    dvs_solver_free(ctx);
    return NULL;
}

static void test_threads(void) {
    size_t sz;
    dvs_problem_t *p = lt_problem(0, &sz);
    dvs_stats_enable(1);
    dvs_stats_reset();
    dvs_thread_t t[N_THREADS];
    for (int i = 0; i < N_THREADS; i++) dvs_thread_create(&t[i], worker, p);
    for (int i = 0; i < N_THREADS; i++) dvs_thread_join(&t[i], NULL);
    free(p);
    char *js = snap();
    CHECK(J(js, "solves") == N_THREADS * N_PER, "threads: exact total");
    CHECK(J(js, "compiles") == N_THREADS, "threads: one compile per thread");
    char ct[17];
    CHECK(n_classes(js) == 1 && class_with(js, N_THREADS * N_PER, ct)
          && strcmp(ct, lt_class) == 0, "threads: one class, exact count");
    check_totals(js, "threads");
    free(js);
}

static void test_truncation(void) {
    char small[16];
    size_t n = dvs_stats_snapshot_json(small, sizeof small);
    CHECK(n >= sizeof small && strlen(small) == sizeof small - 1,
          "snapshot: truncated text is terminated, full length returned");
}

int main(void) {
    test_env();
    test_off();
    test_api();
    test_handle();
    test_bvsat();
    test_smt2();
    test_instances();
    test_values();
    test_signals();
    test_threads();
    test_truncation();
    printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
