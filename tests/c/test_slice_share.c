/* Part-selects of one word (dvs_compile.c _slice_var, _link_slices): a second
 * select of the same bits reuses the first one's variable, selects that share
 * bits are linked through them, a select of a select is a select of the word,
 * and the links go with a checkpoint's scope. Selects that are not constant
 * part-selects of a variable (a shift by a variable) are left alone. Every
 * draw is checked against the constraints.
 *
 * Elaboration splits an unsigned word whose selects nest or are disjoint
 * into fields (dvs_sv.c _fd_split), and its selects never reach the compile
 * as selects, so each such word here also has a select that crosses the
 * others, `w[20:14] != 0x7F` (cross()), which keeps it whole as in riscv-dv's
 * instr_bin. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#include "dv_solve.h"
#include "dvs_ctx.h"
#include "dvs_propagator.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

typedef dvs_builder_t B;

static dvs_expr_t V(B *b, uint32_t v) { return dvs_builder_expr_var(b, v); }
static dvs_expr_t K(B *b, int64_t v, uint32_t w) { return dvs_builder_expr_const_sized(b, v, 0, w); }
static dvs_expr_t X(B *b, dvs_expr_t e, uint32_t hi, uint32_t lo) {
    return dvs_builder_expr_extract(b, e, hi, lo);
}
static dvs_expr_t BIN(B *b, int op, dvs_expr_t l, dvs_expr_t r) {
    return dvs_builder_expr_binary(b, op, l, r);
}
static void add(B *b, dvs_expr_t e) { dvs_builder_add_constraint(b, e); }
/* g -> e, as (g == 0) || e: compiled as a reified Boolean, as SV `->` is. */
static dvs_expr_t IMPL(B *b, uint32_t g, dvs_expr_t e) {
    return BIN(b, DVS_BIN_OR, BIN(b, DVS_BIN_EQ, V(b, g), K(b, 0, 1)), e);
}

typedef struct { void *buf; dvs_block_alloc_t *ba; dvs_ctx_t *ctx; } Ctx;

/* Compile b's problem into a fresh context; ctx NULL if it does not compile. */
static Ctx compile(B *b) {
    Ctx c;
    c.buf = malloc(256u << 10);
    c.ba = dvs_block_alloc_create(NULL, 1u << 16);
    c.ctx = dvs_solver_create(c.buf, 256u << 10, c.ba);
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    if (dvs_solver_compile(c.ctx, p) != DVS_COMPILE_OK) {
        dvs_solver_destroy(c.ctx);
        c.ctx = NULL;
    }
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    return c;
}
static void release(Ctx *c) {
    if (c->ctx) dvs_solver_destroy(c->ctx);
    dvs_block_alloc_destroy(c->ba);
    free(c->buf);
}

/* Draw n solutions of c (seeds 1..n); ok(vals) checks each. */
static int draws(Ctx *c, uint32_t nv, int n, int (*ok)(const int64_t *)) {
    uint32_t ids[16];
    int64_t v[16];
    for (uint32_t i = 0; i < nv; i++) ids[i] = i;
    for (int s = 1; s <= n; s++) {
        dvs_solve_opts_t o = {0};
        o.seed = (uint64_t)s;
        if (s > 1) dvs_solver_reset(c->ctx);
        if (dvs_solver_solve(c->ctx, &o) != DVS_SOLVE_OK) return 0;
        dvs_solver_get_values(c->ctx, nv, ids, v);
        if (!ok(v)) return 0;
    }
    return 1;
}

#define BITS(x, hi, lo) (((uint64_t)(x) >> (lo)) & ((1ULL << ((hi) - (lo) + 1)) - 1))

static void cross(B *b, uint32_t w) {
    add(b, BIN(b, DVS_BIN_NEQ, X(b, V(b, w), 20, 14), K(b, 0x7F, 7)));
}
static int crossed(uint64_t w) { return BITS(w, 20, 14) != 0x7F; }

/* The recorded selects of src[hi:lo]. */
static int n_sel(const Ctx *c, uint32_t src, uint32_t hi, uint32_t lo) {
    int n = 0;
    for (uint32_t i = 0; c->ctx && i < c->ctx->n_slice_memo; i++) {
        const DvsSliceMemo *m = &c->ctx->slice_memo[i];
        n += m->src == src && m->hi == hi && m->lo == lo;
    }
    return n;
}

/* 40 copies of !(w[31:20] inside {...}) under a guard and one at the top: one
 * select variable for all of them. */
static void test_dedup(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFLL);   /* w */
    dvs_builder_add_var(b, 1, 1, 0, 0, 1);               /* g */
    for (int i = 0; i < 40; i++)
        add(b, IMPL(b, 1, BIN(b, DVS_BIN_NEQ, X(b, V(b, 0), 31, 20), K(b, 0x300 + i, 12))));
    add(b, BIN(b, DVS_BIN_NEQ, X(b, V(b, 0), 31, 20), K(b, 0x200, 12)));
    cross(b, 0);
    Ctx c = compile(b);
    CHECK(c.ctx && c.ctx->n_slice_memo == 2 && n_sel(&c, 0, 31, 20) == 1, "dedup: 41 copies of w[31:20] are one variable");
    release(&c);
}

/* w[15:13] == h at the top (h becomes the select) and g -> w[15:5] == 0:
 * with g set, the link fixes h to 0, and a wrong h conflicts at once. Each
 * select alone meets w only at w's bounds, which a wrong h walks over w's
 * free bits [31:16]. */
static B *overlap(int h_guarded) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFLL);   /* w */
    dvs_builder_add_var(b, 1, 3, 0, 0, 7);               /* h */
    dvs_builder_add_var(b, 2, 1, 0, 0, 1);               /* g */
    dvs_expr_t eq = BIN(b, DVS_BIN_EQ, X(b, V(b, 0), 15, 13), V(b, 1));
    add(b, h_guarded ? IMPL(b, 2, eq) : eq);
    add(b, IMPL(b, 2, BIN(b, DVS_BIN_EQ, X(b, V(b, 0), 15, 5), K(b, 0, 11))));
    cross(b, 0);
    return b;
}

static void test_link(void) {
    Ctx c = compile(overlap(0));
    CHECK(n_sel(&c, 0, 15, 13) == 1 && n_sel(&c, 0, 15, 5) == 1, "link: both selects recorded");
    int ok = c.ctx && dvs_solver_pin_var(c.ctx, 2, 1) == 0;
    const Variable *h = ok ? &c.ctx->vars[1] : NULL;
    CHECK(ok && var_lo64(c.ctx, h) == 0 && var_hi64(c.ctx, h) == 0,
          "link: w[15:5] == 0 fixes the top-level w[15:13] == h to 0");
    release(&c);

    for (int guarded = 0; guarded < 2; guarded++) {
        c = compile(overlap(guarded));
        ok = c.ctx && dvs_solver_pin_var(c.ctx, 2, 1) == 0;
        uint64_t f0 = ok ? c.ctx->prop_fires : 0;
        ok = ok && dvs_solver_pin_var(c.ctx, 1, 5) != 0;
        CHECK(ok && c.ctx->prop_fires - f0 < 1000,
              guarded ? "link: a wrong guarded h conflicts in few firings"
                      : "link: a wrong h conflicts in few firings");
        release(&c);
    }
}

/* arith.slice_overlap's shape: by kind, a wider select fixes w[15:13] and
 * w[1:0] (partial overlap: [15:10] and [12:5] share [12:10]). */
enum { W, HI3, LO2, KIND, COMP, OP7, NV_OV };
static int ok_overlap(const int64_t *v) {
    uint64_t w = (uint64_t)v[W];
    if (v[KIND] == 3 || v[LO2] == 3 || !crossed(w)) return 0;
    if (v[COMP]) {
        if (BITS(w, 15, 13) != (uint64_t)v[HI3] || BITS(w, 1, 0) != (uint64_t)v[LO2]) return 0;
        if (v[KIND] == 0 && !(BITS(w, 15, 5) == 0 && v[LO2] == 0)) return 0;
        if (v[KIND] == 1 && !(BITS(w, 15, 10) == 0x27 && BITS(w, 12, 5) == 0xE5)) return 0;
        if (v[KIND] == 2 && BITS(w, 15, 0) != 0) return 0;
    } else {
        if (BITS(w, 6, 0) != (uint64_t)v[OP7] || (v[OP7] & 3) != 3) return 0;
        if (BITS(w, 31, 20) >= 0x300 && BITS(w, 31, 20) < 0x340) return 0;
    }
    return 1;
}

static void test_solutions(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, W, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_builder_add_var(b, HI3, 3, 0, 0, 7);
    dvs_builder_add_var(b, LO2, 2, 0, 0, 3);
    dvs_builder_add_var(b, KIND, 2, 0, 0, 3);
    dvs_builder_add_var(b, COMP, 1, 0, 0, 1);
    dvs_builder_add_var(b, OP7, 7, 0, 0, 127);
    dvs_expr_t w = V(b, W);
    add(b, BIN(b, DVS_BIN_NEQ, V(b, KIND), K(b, 3, 2)));
    add(b, BIN(b, DVS_BIN_NEQ, V(b, LO2), K(b, 3, 2)));
    add(b, IMPL(b, COMP, BIN(b, DVS_BIN_EQ, X(b, w, 15, 13), V(b, HI3))));
    add(b, IMPL(b, COMP, BIN(b, DVS_BIN_EQ, X(b, w, 1, 0), V(b, LO2))));
    dvs_expr_t k0 = BIN(b, DVS_BIN_AND, BIN(b, DVS_BIN_EQ, X(b, w, 15, 5), K(b, 0, 11)),
                        BIN(b, DVS_BIN_EQ, V(b, LO2), K(b, 0, 2)));
    dvs_expr_t k1 = BIN(b, DVS_BIN_AND, BIN(b, DVS_BIN_EQ, X(b, w, 15, 10), K(b, 0x27, 6)),
                        BIN(b, DVS_BIN_EQ, X(b, w, 12, 5), K(b, 0xE5, 8)));
    dvs_expr_t k2 = BIN(b, DVS_BIN_EQ, X(b, w, 15, 0), K(b, 0, 16));
    dvs_expr_t kinds[3] = { k0, k1, k2 };
    for (int k = 0; k < 3; k++)
        add(b, BIN(b, DVS_BIN_OR,
                   BIN(b, DVS_BIN_OR, BIN(b, DVS_BIN_EQ, V(b, COMP), K(b, 0, 1)),
                       BIN(b, DVS_BIN_NEQ, V(b, KIND), K(b, k, 2))),
                   kinds[k]));
    add(b, BIN(b, DVS_BIN_OR, V(b, COMP), BIN(b, DVS_BIN_EQ, X(b, w, 6, 0), V(b, OP7))));
    add(b, BIN(b, DVS_BIN_EQ, X(b, V(b, OP7), 1, 0), K(b, 3, 2)));
    for (int i = 0; i < 64; i++)
        add(b, BIN(b, DVS_BIN_OR, V(b, COMP),
                   BIN(b, DVS_BIN_NEQ, X(b, w, 31, 20), K(b, 0x300 + i, 12))));
    cross(b, W);
    Ctx c = compile(b);
    CHECK(c.ctx && draws(&c, NV_OV, 300, ok_overlap), "solutions: 300 draws hold every constraint");
    release(&c);
}

/* A signed word: its selects read its two's complement bits, the same for
 * every select, so links between them hold for negative values too. With g
 * set, s[11:4] == 0xF5 gives s[11:8] = 0xF, which the link passes to
 * hs = s[15:8]. */
static int ok_signed(const int64_t *v) {
    uint64_t s = (uint64_t)v[0];
    return BITS(s, 15, 8) == (uint64_t)v[1] && v[0] < 0 && (!v[2] || BITS(s, 11, 4) == 0xF5);
}

static void test_signed(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 16, 1, -32768, 32767);   /* s, signed */
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);           /* hs */
    dvs_builder_add_var(b, 2, 1, 0, 0, 1);             /* g */
    add(b, BIN(b, DVS_BIN_LT, V(b, 0), dvs_builder_expr_const_sized(b, 0, 1, 16)));
    add(b, BIN(b, DVS_BIN_EQ, X(b, V(b, 0), 15, 8), V(b, 1)));
    add(b, IMPL(b, 2, BIN(b, DVS_BIN_EQ, X(b, V(b, 0), 11, 4), K(b, 0xF5, 8))));
    Ctx c = compile(b);
    CHECK(c.ctx && draws(&c, 3, 100, ok_signed), "signed: 100 draws hold");
    if (c.ctx) dvs_solver_reset(c.ctx);
    int ok = c.ctx && dvs_solver_pin_var(c.ctx, 2, 1) == 0;
    CHECK(ok && BITS(var_lo64(c.ctx, &c.ctx->vars[1]), 3, 0) == 0xF
             && BITS(var_hi64(c.ctx, &c.ctx->vars[1]), 3, 0) == 0xF,
          "signed: the link reaches the other select");
    release(&c);
}

/* w[15:5][10:8] is w[15:13]: one variable with the direct select. Above the
 * inner select's bits the outer one reads zeros, and is not composed. */
static int ok_nested(const int64_t *v) {
    uint64_t w = (uint64_t)v[0];
    return BITS(w, 15, 13) == (uint64_t)v[1] && v[2] == 0 && BITS(w, 15, 13) != 0 && crossed(w);
}

static void test_nested(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFLL);   /* w */
    dvs_builder_add_var(b, 1, 3, 0, 0, 7);               /* q */
    dvs_builder_add_var(b, 2, 4, 0, 0, 15);              /* z */
    dvs_expr_t inner = X(b, V(b, 0), 15, 5);             /* 11 bits */
    add(b, BIN(b, DVS_BIN_EQ, X(b, inner, 10, 8), V(b, 1)));
    add(b, BIN(b, DVS_BIN_NEQ, X(b, V(b, 0), 15, 13), K(b, 0, 3)));
    add(b, BIN(b, DVS_BIN_EQ, X(b, X(b, V(b, 0), 3, 0), 7, 4), V(b, 2)));   /* above [3:0]: 0 */
    cross(b, 0);
    Ctx c = compile(b);
    uint32_t inner_var = EXPR_NULL;
    for (uint32_t i = 0; c.ctx && i < c.ctx->n_slice_memo; i++)
        if (c.ctx->slice_memo[i].src == 0 && c.ctx->slice_memo[i].hi == 15 && c.ctx->slice_memo[i].lo == 5)
            inner_var = c.ctx->slice_memo[i].r;
    CHECK(c.ctx && n_sel(&c, 0, 15, 13) == 1 && inner_var != EXPR_NULL && n_sel(&c, inner_var, 10, 8) == 0,
          "nested: w[15:5][10:8] and w[15:13] are one variable");
    CHECK(c.ctx && n_sel(&c, 0, 3, 0) == 1 && n_sel(&c, 0, 7, 4) == 0, "nested: a select above the inner one's bits is not w[7:4]");
    CHECK(c.ctx && draws(&c, 3, 100, ok_nested), "nested: 100 draws hold");
    release(&c);
}

/* (w >> i) & 7 == v selects by a variable index: no part-select, left
 * alone. */
static int ok_varidx(const int64_t *v) {
    return (((uint64_t)v[0] >> v[1]) & 7) == (uint64_t)v[2] && v[2] == 5;
}

static void test_left_alone(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFLL);   /* w */
    dvs_builder_add_var(b, 1, 32, 0, 0, 29);             /* i */
    dvs_builder_add_var(b, 2, 32, 0, 0, 7);              /* v */
    add(b, BIN(b, DVS_BIN_EQ,
               BIN(b, DVS_BIN_BAND, BIN(b, DVS_BIN_RSHIFT, V(b, 0), V(b, 1)), K(b, 7, 32)),
               V(b, 2)));
    add(b, BIN(b, DVS_BIN_EQ, V(b, 2), K(b, 5, 32)));
    Ctx c = compile(b);
    CHECK(c.ctx && c.ctx->n_slice_memo == 0, "left alone: a select by a variable index");
    CHECK(c.ctx && draws(&c, 3, 100, ok_varidx), "left alone: 100 draws hold");
    release(&c);

    /* A top-level r == w[15:12] with r signed: r is not the select's unsigned
     * value, so it does not stand for the select. */
    b = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(b);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_builder_add_var(b, 1, 4, 1, -8, 7);
    add(b, BIN(b, DVS_BIN_EQ, V(b, 1), X(b, V(b, 0), 15, 12)));
    cross(b, 0);
    c = compile(b);
    int as_r = 0;
    for (uint32_t i = 0; c.ctx && i < c.ctx->n_slice_memo; i++) as_r |= c.ctx->slice_memo[i].r == 1;
    CHECK(c.ctx && n_sel(&c, 0, 20, 14) == 1 && !as_r, "left alone: a signed r == w[15:12]");
    release(&c);
}

/* A select added in a checkpoint's scope, and its links, go on restore. */
static void test_restore(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    add(b, BIN(b, DVS_BIN_EQ, X(b, V(b, 0), 15, 8), V(b, 1)));
    cross(b, 0);
    Ctx c = compile(b);
    uint32_t n0 = c.ctx ? c.ctx->n_slice_memo : 0;
    int cp = c.ctx ? dvs_solver_checkpoint(c.ctx) : -1;
    B *a = dvs_builder_create(0, NULL);
    add(a, BIN(a, DVS_BIN_NEQ, X(a, V(a, 0), 11, 4), K(a, 3, 8)));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(a, &sz);
    int rc = cp >= 0 ? dvs_solver_add_constraint(c.ctx, p) : -1;
    dvs_builder_free_problem(a, p, sz);
    dvs_builder_destroy(a);
    CHECK(rc == DVS_COMPILE_OK && c.ctx->n_slice_memo == n0 + 1, "restore: the added select is recorded");
    if (cp >= 0) dvs_solver_restore(c.ctx, (uint32_t)cp);
    CHECK(c.ctx && c.ctx->n_slice_memo == n0, "restore: and dropped with its scope");
    dvs_solve_opts_t o = {0};
    o.seed = 3;
    CHECK(c.ctx && dvs_solver_solve(c.ctx, &o) == DVS_SOLVE_OK, "restore: solves after");
    release(&c);
}

/* A split word keeps its solve-order stage. f, first, is 3 or 200 evenly,
 * though f == 3 leaves w a single value. Split by f[7:4], its fields had no
 * stage, so w was drawn first (riscv-dv's riscv_illegal_instr: opcode and
 * func7, before instr_bin, took 3 ms a call). */
static void test_order(void) {
    B *b = dvs_builder_create(0, NULL);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_builder_set_var_order(b, 0, 1);
    dvs_builder_set_var_order(b, 1, 2);
    add(b, BIN(b, DVS_BIN_OR, BIN(b, DVS_BIN_EQ, V(b, 0), K(b, 3, 8)),
               BIN(b, DVS_BIN_EQ, V(b, 0), K(b, 200, 8))));
    add(b, BIN(b, DVS_BIN_NEQ, X(b, V(b, 0), 7, 4), K(b, 15, 4)));
    add(b, BIN(b, DVS_BIN_EQ, X(b, V(b, 1), 7, 0), V(b, 0)));
    add(b, BIN(b, DVS_BIN_OR, BIN(b, DVS_BIN_NEQ, V(b, 0), K(b, 3, 8)),
               BIN(b, DVS_BIN_EQ, X(b, V(b, 1), 31, 8), K(b, 0, 24))));
    Ctx c = compile(b);
    uint32_t ids[2] = { 0, 1 };
    int64_t v[2];
    int ok = c.ctx != NULL, threes = 0;
    for (int s = 1; s <= 200 && ok; s++) {
        dvs_solve_opts_t o = {0};
        o.seed = (uint64_t)s;
        o.fair_pick = 1;
        if (s > 1) dvs_solver_reset(c.ctx);
        ok = dvs_solver_solve(c.ctx, &o) == DVS_SOLVE_OK;
        dvs_solver_get_values(c.ctx, 2, ids, v);
        ok = ok && (v[0] == 3 || v[0] == 200) && (v[1] & 0xFF) == v[0]
            && (v[0] != 3 || v[1] == 3);
        threes += v[0] == 3;
    }
    CHECK(ok, "order: 200 draws satisfy the constraints");
    CHECK(threes >= 70 && threes <= 130, "order: the split word is decided first");
    release(&c);
}

int main(void) {
    test_dedup();
    test_link();
    test_solutions();
    test_signed();
    test_nested();
    test_left_alone();
    test_restore();
    test_order();
    if (failures) { fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
