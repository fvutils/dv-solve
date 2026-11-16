/* `x inside {q}` as a value set (DvsInSet): an array reduction `q.or() with
 * (item == x)` expands to one IN_SET over the elements (dvs_tmpl.c
 * xp_reduce), and the value picker draws x from the fixed elements' values
 * (dvs_search.c _pick_in_set): in every set that holds, in none that is
 * negated, uniform over the distinct values. A 32-bit x then takes no
 * conflict, where it used to be drawn over 2^32 values. Sets with an
 * element not fixed yet leave the draw as it was, and an OR of more than
 * MAX_OR_CLAUSES `x == c` gets the same set, as does a constraint OR of
 * fewer with variables among the t of `x == t` (S12.3). One that covers a
 * range of constants is x's bounds (S12.1). */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_ctx.h"
#include "dvs_tmpl.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

static dvs_builder_t *B;

static dvs_expr_t bin(dvs_binop_t op, dvs_expr_t a, dvs_expr_t b) {
    return dvs_builder_expr_binary(B, op, a, b);
}
static dvs_expr_t var(uint32_t v) { return dvs_builder_expr_var(B, v); }
static dvs_expr_t k32(int64_t v) { return dvs_builder_expr_const_sized(B, v, 1, 32); }
static dvs_expr_t lnot(dvs_expr_t e) { return dvs_builder_expr_unary(B, DVS_UN_NOT, e); }
/* x inside {a}, as Verilator writes it: a.or() with (item == x) */
static dvs_expr_t inside(uint32_t a, uint32_t x) {
    return dvs_builder_expr_reduce(B, DVS_BIN_BOR, a, 0,
                                   bin(DVS_BIN_EQ, dvs_builder_expr_elem(B, a, dvs_builder_expr_loop_index(B, 0)),
                                       var(x)));
}
/* if (a.size() > 0) e */
static dvs_expr_t if_any(uint32_t a, dvs_expr_t e) {
    return bin(DVS_BIN_OR, lnot(bin(DVS_BIN_GT, dvs_builder_expr_array_size(B, a), k32(0))), e);
}

static dvs_handle_t *finish(void) {
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(B, &sz);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(B, p, sz);
    dvs_builder_destroy(B);
    return h;
}

/* Conflicts of the expanded problem's last call. */
static uint64_t conflicts(dvs_handle_t *h) {
    dvs_handle_t *e = dvs_tmpl_last_expansion(dvs_handle_tmpl(h));
    return e ? dvs_handle_n_conflicts(e) : ~0ull;
}

static int has(const int32_t *q, int n, int32_t v) {
    for (int i = 0; i < n; i++) if (q[i] == v) return 1;
    return 0;
}

/* Each of the k values was drawn within a quarter of n / k times. */
static int uniform(const int *count, int k, int n) {
    for (int i = 0; i < k; i++)
        if (count[i] * 4 * k < 3 * n || count[i] * 4 * k > 5 * n) return 0;
    return 1;
}

/* riscv-dv get_rand_instr: a 32-bit enum inside a queue, inside a second
 * one if it is not empty, and not inside a third, all read before the call.
 * The first two leave 7 values; the third excludes 2 of them. */
static void check_queue(void) {
    static int32_t names[] = {0, 1, 2, 3, 4, 5, 6, 7, 160, 161, 162, 255, 256, 257, 505, 1000000};
    static int32_t incl[]  = {1, 3, 5, 7, 161, 257, 505, 9, 300};
    static int32_t excl[]  = {5, 161, 400};
    const int32_t sol[] = {1, 3, 7, 257, 505};
    B = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(B);
    dvs_builder_add_var(B, 0, 32, 1, INT32_MIN, INT32_MAX);
    for (uint32_t a = 0; a < 3; a++) dvs_builder_add_array(B, a, 32, 1, DVS_VAR_STATE);
    dvs_builder_add_constraint(B, inside(0, 0));
    dvs_builder_add_constraint(B, if_any(1, inside(1, 0)));
    dvs_builder_add_constraint(B, if_any(2, lnot(inside(2, 0))));
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "queue: handle");
    if (!h) return;
    int32_t x;
    dvs_ref_t ref = {&x, 32, 1, DVS_REF_SCALAR, 0};
    dvs_array_ref_t arr[3] = {{names, 16, 0}, {incl, 9, 0}, {excl, 3, 0}};
    int ok = 1, calm = 1, count[5] = {0};
    const int n = 2500;
    for (int s = 1; s <= n; s++) {
        if (dvs_randomize_arrays(h, &ref, arr, NULL, (uint64_t)s) != DVS_RANDOMIZE_OK) { ok = 0; continue; }
        if (!has(names, 16, x) || !has(incl, 9, x) || has(excl, 3, x)) ok = 0;
        if (conflicts(h) != 0) calm = 0;
        for (int i = 0; i < 5; i++) count[i] += x == sol[i];
    }
    CHECK(ok, "queue: in both sets, not in the excluded one");
    CHECK(calm, "queue: no conflict (x is drawn from the sets)");
    CHECK(uniform(count, 5, n), "queue: each value about as often");

    /* An empty include list leaves only the first set and the exclusion. */
    arr[1].data = NULL;
    arr[1].n_elems = 0;
    ok = calm = 1;
    for (int s = 1; s <= 200; s++) {
        if (dvs_randomize_arrays(h, &ref, arr, NULL, (uint64_t)s) != DVS_RANDOMIZE_OK) { ok = 0; continue; }
        if (!has(names, 16, x) || has(excl, 3, x)) ok = 0;
        if (conflicts(h) != 0) calm = 0;
    }
    CHECK(ok && calm, "queue: no include list: the first set, no conflict");

    /* Contents change from call to call. */
    names[3] = 77;
    incl[0] = 77;
    excl[0] = 1;
    arr[1].data = incl;
    arr[1].n_elems = 9;
    int seen77 = 0;
    ok = 1;
    for (int s = 1; s <= 200; s++) {
        if (dvs_randomize_arrays(h, &ref, arr, NULL, (uint64_t)s) != DVS_RANDOMIZE_OK) { ok = 0; continue; }
        if (!has(names, 16, x) || !has(incl, 9, x) || has(excl, 3, x)) ok = 0;
        seen77 |= x == 77;
    }
    CHECK(ok && seen77, "queue: new contents, new set");
    dvs_handle_free(h);
}

/* A value the queue holds many times is drawn no more often than one it
 * holds once: inside is membership in a set of values. */
static void check_duplicates(void) {
    static int32_t q[] = {7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 1000};
    B = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(B);
    dvs_builder_add_var(B, 0, 32, 1, INT32_MIN, INT32_MAX);
    dvs_builder_add_array(B, 0, 32, 1, DVS_VAR_STATE);
    dvs_builder_add_constraint(B, inside(0, 0));
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "duplicates: handle");
    if (!h) return;
    int32_t x;
    dvs_ref_t ref = {&x, 32, 1, DVS_REF_SCALAR, 0};
    dvs_array_ref_t arr = {q, 12, 0};
    int ok = 1, count[2] = {0};
    const int n = 2000;
    for (int s = 1; s <= n; s++) {
        if (dvs_randomize_arrays(h, &ref, &arr, NULL, (uint64_t)s) != DVS_RANDOMIZE_OK) ok = 0;
        else if (x == 7) count[0]++;
        else if (x == 1000) count[1]++;
        else ok = 0;
    }
    CHECK(ok, "duplicates: x in the queue");
    CHECK(uniform(count, 2, n), "duplicates: 7 (11 times) as often as 1000 (once)");
    dvs_handle_free(h);
}

/* x inside a queue of random elements and inside a STATE one: the random
 * set's elements are not fixed when x is drawn, so it leaves the draw to the
 * STATE set and the search (its elements' lower bounds, 0, are no set). */
static void check_random_elements(void) {
    static int32_t q[] = {0, 2, 4, 6, 8, 3000};
    B = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(B);
    dvs_builder_add_var(B, 0, 32, 1, INT32_MIN, INT32_MAX);
    dvs_builder_add_array(B, 0, 32, 1, DVS_VAR_RAND);
    dvs_builder_add_array(B, 1, 32, 1, DVS_VAR_STATE);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, dvs_builder_expr_elem(B, 0, dvs_builder_expr_loop_index(B, 0)),
                                      k32(10)));
    dvs_builder_add_constraint(B, bin(DVS_BIN_GTE, dvs_builder_expr_elem(B, 0, dvs_builder_expr_loop_index(B, 0)),
                                      k32(0)));
    dvs_builder_foreach_end(B);
    dvs_builder_add_constraint(B, inside(0, 0));
    dvs_builder_add_constraint(B, inside(1, 0));
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "random elements: handle");
    if (!h) return;
    int32_t x, r[3];
    dvs_ref_t ref = {&x, 32, 1, DVS_REF_SCALAR, 0};
    dvs_array_ref_t arr[2] = {{r, 3, 0}, {q, 6, 0}};
    int ok = 1, seen[10] = {0}, distinct = 0;
    for (int s = 1; s <= 100; s++) {
        if (dvs_randomize_arrays(h, &ref, arr, NULL, (uint64_t)s) != DVS_RANDOMIZE_OK) { ok = 0; continue; }
        if (!has(r, 3, x) || !has(q, 6, x)) ok = 0;
        else if (x < 10 && !seen[x]++) distinct++;
    }
    CHECK(ok, "random elements: x is one of them, and in the STATE set");
    CHECK(distinct == 5, "random elements: x takes each value of both");
    dvs_handle_free(h);
}

typedef struct { void *buf; dvs_block_alloc_t *ba; dvs_ctx_t *ctx; } Ctx;

static Ctx compile(void) {
    Ctx c;
    c.buf = malloc(1u << 20);
    c.ba = dvs_block_alloc_create(NULL, 1u << 16);
    c.ctx = dvs_solver_create(c.buf, 1u << 20, c.ba);
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(B, &sz);
    if (dvs_solver_compile(c.ctx, p) != DVS_COMPILE_OK) {
        dvs_solver_destroy(c.ctx);
        c.ctx = NULL;
    }
    dvs_builder_free_problem(B, p, sz);
    dvs_builder_destroy(B);
    return c;
}
static void release(Ctx *c) {
    if (c->ctx) dvs_solver_destroy(c->ctx);
    dvs_block_alloc_destroy(c->ba);
    free(c->buf);
}

/* x inside {40 constants}, as Verilator writes it: an OR of `c == x` from
 * false. Past MAX_OR_CLAUSES there is no DisjClause and no holes; the set
 * gives the domain. */
static void check_large_constant_set(void) {
    int64_t vals[40];
    for (int i = 0; i < 40; i++) vals[i] = 1000 + 7919LL * i * i;
    B = dvs_builder_create(0, NULL);
    dvs_builder_add_var(B, 0, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_expr_t or = dvs_builder_expr_const_sized(B, 0, 0, 1);
    for (int i = 0; i < 40; i++)
        or = bin(DVS_BIN_OR, or, bin(DVS_BIN_EQ, dvs_builder_expr_const_sized(B, vals[i], 0, 32), var(0)));
    dvs_builder_add_constraint(B, or);
    Ctx c = compile();
    CHECK(c.ctx && c.ctx->n_in_sets == 1, "constants: one set");
    int ok = c.ctx != NULL, calm = 1, count[40] = {0};
    const int n = 8000;
    for (int s = 1; ok && s <= n; s++) {
        dvs_solve_opts_t o = {0};
        o.seed = (uint64_t)s;
        if (s > 1) dvs_solver_reset(c.ctx);
        uint32_t id = 0;
        int64_t x;
        if (dvs_solver_solve(c.ctx, &o) != DVS_SOLVE_OK) { ok = 0; break; }
        dvs_solver_get_values(c.ctx, 1, &id, &x);
        int i = 0;
        while (i < 40 && vals[i] != x) i++;
        if (i == 40) ok = 0; else count[i]++;
        if (c.ctx->conflict_count) calm = 0;
    }
    CHECK(ok, "constants: x in the set");
    CHECK(calm, "constants: no conflict");
    CHECK(uniform(count, 40, n), "constants: each value about as often");
    release(&c);
}

/* An enum's validity as Verilator writes it, `x == k` from false over 0..299
 * in shuffled order: x's bounds, with no propagator (S12.1: riscv-dv's 509
 * leaves were reified one by one, propagated and validated every call).
 * With one value repeated in place of 150 the leaves no longer cover their
 * range: the set stays, and x is never 150. Values past x's bounds: UNSAT.
 * The model check decides `a || b` on a true a and `a && b` on a false a,
 * though it cannot evaluate b ($countones). */
static void check_enum_validity(void) {
    for (int gap = 0; gap < 3; gap++) {
        B = dvs_builder_create(0, NULL);
        dvs_builder_add_var(B, 0, 32, 0, 0, gap == 2 ? 255 : 0xFFFFFFFFLL);
        dvs_builder_add_var(B, 1, 8, 0, 0, 255);
        dvs_expr_t or = dvs_builder_expr_const_sized(B, 0, 0, 1);
        for (int i = 0; i < 300; i++) {
            int64_t k = (i * 7) % 300 + (gap == 2 ? 300 : 0);
            or = bin(DVS_BIN_OR, or, bin(DVS_BIN_EQ, var(0), k32(gap == 1 && k == 150 ? 7 : k)));
        }
        dvs_builder_add_constraint(B, or);
        dvs_expr_t cnt = dvs_builder_expr_countones(B, var(1), var(1));
        dvs_expr_t lt = bin(DVS_BIN_LT, var(0), k32(300));
        dvs_expr_t any = bin(DVS_BIN_OR, lt, cnt), all = bin(DVS_BIN_AND, lnot(lt), cnt);
        size_t sz;
        dvs_problem_t *p = dvs_builder_finalize(B, &sz);
        dvs_ctx_t *ctx = dvs_solver_new(NULL);
        int rc = dvs_solver_compile(ctx, p);
        if (gap == 2) {
            CHECK(rc == DVS_COMPILE_UNSAT, "validity: values past x's bounds are UNSAT");
        } else if (!gap) {
            CHECK(rc == DVS_COMPILE_OK && ctx->n_props == 0 && ctx->n_in_sets == 0
                      && dvs_var_lo64(ctx, 0) == 0 && dvs_var_hi64(ctx, 0) == 299,
                  "validity: x's bounds, no propagator");
        } else {
            CHECK(rc == DVS_COMPILE_OK && ctx->n_in_sets == 1, "validity with a gap: the set");
        }
        int ok = rc == DVS_COMPILE_OK, decided = ok, calm = 1;
        for (int s = 1; ok && gap < 2 && s <= 600; s++) {
            dvs_solve_opts_t o = {0};
            o.seed = (uint64_t)s;
            if (s > 1) dvs_solver_reset(ctx);
            int t1 = -1, t2 = -1;
            if (dvs_solver_solve(ctx, &o) != DVS_SOLVE_OK) { ok = 0; break; }
            int64_t x = dvs_solver_get_value(ctx, 0);
            if (x < 0 || x > 299 || (gap && x == 150) || dvs_solver_validate_model(ctx, p, NULL))
                ok = 0;
            if (dvs_expr_truth(ctx, p, any, &t1) || t1 != 1 || dvs_expr_truth(ctx, p, all, &t2)
                    || t2 != 0)
                decided = 0;
            if (ctx->conflict_count) calm = 0;
        }
        if (gap < 2) {
            CHECK(ok, gap ? "validity with a gap: x in the set, never 150"
                          : "validity: x in range, the model checks");
            CHECK(calm, gap ? "validity with a gap: no conflict" : "validity: no conflict");
            CHECK(decided, "validity: the model check decides on the first operand");
        }
        dvs_solver_free(ctx);
        dvs_builder_free_problem(B, p, sz);
        dvs_builder_destroy(B);
    }
}

/* `x inside {s0, s1, s2, 9}` over STATE scalars as Verilator writes it, an
 * OR of `x == t` from false: a DisjClause, and since S12.3 x's set too. x is
 * drawn from it with no conflict, each value about as often; a 32-bit x was
 * drawn blind. Constants only: no set (their holes do it). A set added in a
 * checkpoint's scope goes with it. */
static dvs_expr_t eq_or(int n, const dvs_expr_t *t) {
    dvs_expr_t or = dvs_builder_expr_const_sized(B, 0, 0, 1);
    for (int i = 0; i < n; i++) or = bin(DVS_BIN_OR, or, bin(DVS_BIN_EQ, var(0), t[i]));
    return or;
}
static void check_root_vars(void) {
    const int64_t pin[3] = {1000, 4000000000LL, 7}, sol[4] = {1000, 4000000000LL, 7, 9};
    B = dvs_builder_create(0, NULL);
    for (uint32_t v = 0; v < 4; v++) dvs_builder_add_var(B, v, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_expr_t t[4] = {var(1), var(2), var(3), dvs_builder_expr_const_sized(B, 9, 0, 32)};
    dvs_builder_add_constraint(B, eq_or(4, t));
    Ctx c = compile();
    CHECK(c.ctx && c.ctx->n_in_sets == 1, "root vars: one set");
    int ok = c.ctx != NULL, calm = 1, count[4] = {0};
    const int n = 2000;
    for (int s = 1; ok && s <= n; s++) {
        dvs_solve_opts_t o = {0};
        o.seed = (uint64_t)s;
        if (s > 1) dvs_solver_reset(c.ctx);
        for (uint32_t v = 0; v < 3; v++) ok &= dvs_solver_pin_var(c.ctx, v + 1, pin[v]) == 0;
        if (!ok || dvs_solver_solve(c.ctx, &o) != DVS_SOLVE_OK) { ok = 0; break; }
        int64_t x = dvs_solver_get_value(c.ctx, 0);
        int i = 0;
        while (i < 4 && sol[i] != x) i++;
        if (i == 4) ok = 0; else count[i]++;
        if (c.ctx->conflict_count) calm = 0;
    }
    CHECK(ok, "root vars: x is one of them");
    CHECK(calm, "root vars: no conflict");
    CHECK(uniform(count, 4, n), "root vars: each value about as often");
    int cp = c.ctx ? dvs_solver_checkpoint(c.ctx) : -1;
    B = dvs_builder_create(0, NULL);
    dvs_expr_t k[2] = {dvs_builder_expr_const_sized(B, 1000, 0, 32), dvs_builder_expr_const_sized(B, 7, 0, 32)};
    dvs_expr_t u[2] = {var(1), var(3)};
    dvs_builder_add_constraint(B, eq_or(2, k));
    dvs_builder_add_constraint(B, eq_or(2, u));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(B, &sz);
    int rc = cp >= 0 ? dvs_solver_add_constraint(c.ctx, p) : -1;
    dvs_builder_free_problem(B, p, sz);
    dvs_builder_destroy(B);
    CHECK(rc == DVS_COMPILE_OK && c.ctx->n_in_sets == 2, "root vars: constants add no set");
    if (cp >= 0) dvs_solver_restore(c.ctx, (uint32_t)cp);
    CHECK(c.ctx && c.ctx->n_in_sets == 1, "root vars: a scope's set goes with it");
    release(&c);
}

/* x in {s, r, 9}, x 8 bits: s pinned to 200, r free (32 bits), so x is
 * decided first. r's bounds are not its value: the set is not drawn from,
 * and x is not held to {200, 9, r's lower bound}. */
static void check_mixed_elements(void) {
    B = dvs_builder_create(0, NULL);
    dvs_builder_add_var(B, 0, 8, 0, 0, 255);              /* x */
    dvs_builder_add_var(B, 1, 32, 0, 0, 0xFFFFFFFFLL);   /* s */
    dvs_builder_add_var(B, 2, 32, 0, 0, 0xFFFFFFFFLL);   /* r */
    dvs_expr_t el[3] = {var(1), var(2), dvs_builder_expr_const_sized(B, 9, 0, 32)};
    dvs_builder_add_constraint(B, dvs_builder_expr_in_set(B, var(0), 3, el));
    Ctx c = compile();
    CHECK(c.ctx && c.ctx->n_in_sets == 1, "mixed: one set");
    int ok = c.ctx != NULL, seen[256] = {0}, distinct = 0;
    for (int s = 1; ok && s <= 50; s++) {
        dvs_solve_opts_t o = {0};
        o.seed = (uint64_t)s;
        uint32_t id[3] = {0, 1, 2};
        int64_t v[3];
        if (s > 1) dvs_solver_reset(c.ctx);
        if (dvs_solver_pin_var(c.ctx, 1, 200) || dvs_solver_solve(c.ctx, &o) != DVS_SOLVE_OK) {
            ok = 0;
            break;
        }
        dvs_solver_get_values(c.ctx, 3, id, v);
        if (v[1] != 200 || (v[0] != v[1] && v[0] != v[2] && v[0] != 9)) ok = 0;
        if (!seen[v[0] & 255]++) distinct++;
    }
    CHECK(ok, "mixed: x is one of the elements");
    CHECK(distinct > 3, "mixed: an element not fixed leaves x free");
    release(&c);
}

/* A set added in a checkpoint's scope goes on restore. */
static void check_restore(void) {
    B = dvs_builder_create(0, NULL);
    dvs_builder_add_var(B, 0, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_builder_add_var(B, 1, 32, 0, 0, 0xFFFFFFFFLL);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, var(1), dvs_builder_expr_const_sized(B, 5, 0, 32)));
    Ctx c = compile();
    int cp = c.ctx ? dvs_solver_checkpoint(c.ctx) : -1;
    B = dvs_builder_create(0, NULL);
    dvs_expr_t el[2] = {var(1), dvs_builder_expr_const_sized(B, 77, 0, 32)};
    dvs_builder_add_constraint(B, dvs_builder_expr_in_set(B, var(0), 2, el));
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(B, &sz);
    int rc = cp >= 0 ? dvs_solver_add_constraint(c.ctx, p) : -1;
    dvs_builder_free_problem(B, p, sz);
    dvs_builder_destroy(B);
    CHECK(rc == DVS_COMPILE_OK && c.ctx->n_in_sets == 1, "restore: the added set is recorded");
    if (cp >= 0) dvs_solver_restore(c.ctx, (uint32_t)cp);
    CHECK(c.ctx && c.ctx->n_in_sets == 0, "restore: and dropped with its scope");
    dvs_solve_opts_t o = {0};
    o.seed = 3;
    CHECK(c.ctx && dvs_solver_solve(c.ctx, &o) == DVS_SOLVE_OK, "restore: solves after");
    release(&c);
}

int main(void) {
    check_queue();
    check_duplicates();
    check_random_elements();
    check_large_constant_set();
    check_enum_validity();
    check_root_vars();
    check_mixed_elements();
    check_restore();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}
