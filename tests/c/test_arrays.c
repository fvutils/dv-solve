/* Problems with arrays whose size is given on each call (dvs_randomize_arrays):
 * element-local arrays solved one element at a time, for any size; the rest
 * instantiated per size and kept; index folding, out-of-range reads, random
 * indices, reductions, unique, dist and soft in a foreach, STATE arrays;
 * nothing written on a failure. Problems are EXPLICIT, as Verilator builds
 * them. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_problem.h"
#include "dvs_tmpl.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

static dvs_builder_t *B;

static dvs_expr_t c32(int64_t v) { return dvs_builder_expr_const_sized(B, v, 1, 32); }
static dvs_expr_t c8(int64_t v) { return dvs_builder_expr_const_sized(B, v, 0, 8); }
static dvs_expr_t idx(uint32_t level) { return dvs_builder_expr_loop_index(B, level); }
static dvs_expr_t bin(dvs_binop_t op, dvs_expr_t a, dvs_expr_t b) {
    return dvs_builder_expr_binary(B, op, a, b);
}
static dvs_expr_t elem(uint32_t a, dvs_expr_t i) { return dvs_builder_expr_elem(B, a, i); }
/* An 8-bit unsigned value as a 32-bit signed int. */
static dvs_expr_t to32(dvs_expr_t e) { return dvs_builder_expr_sv_cast(B, e, 8, 32, 0, 1); }

static void begin(void) {
    B = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(B);
}

static dvs_handle_t *finish(void) {
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(B, &sz);
    char why[200] = "";
    if (dvs_problem_validate(p, why, sizeof why)) fprintf(stderr, "validate: %s\n", why);
    dvs_handle_t *h = dvs_handle_new(p, sz);
    dvs_builder_free_problem(B, p, sz);
    dvs_builder_destroy(B);
    return h;
}

static int randomize(dvs_handle_t *h, const dvs_ref_t *refs, uint8_t **data, uint32_t *n,
                     uint32_t n_arrays, uint64_t seed) {
    dvs_array_ref_t arr[4];
    memset(arr, 0, sizeof arr);
    for (uint32_t a = 0; a < n_arrays; a++) {
        arr[a].data = data[a];
        arr[a].n_elems = n[a];
    }
    return dvs_randomize_arrays(h, refs, arr, NULL, seed);
}

/* foreach (a[i]) a[i] < 10 && int'(a[i]) != i: element-local, any size. */
static void check_local(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, elem(0, idx(0)), c8(10)));
    dvs_builder_add_constraint(B, bin(DVS_BIN_NEQ, to32(elem(0, idx(0))), idx(0)));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h && dvs_handle_n_arrays(h) == 1, "local: handle with one array");
    if (!h) return;
    CHECK(dvs_randomize(h, NULL, NULL, 1) == DVS_RANDOMIZE_ERROR,
          "local: dvs_randomize refuses a problem with arrays");
    static const uint32_t sizes[] = {0, 1, 5, 100, 3000};
    int ok = 1, seen[10] = {0};
    for (uint32_t s = 0; s < sizeof sizes / sizeof sizes[0]; s++) {
        uint32_t n = sizes[s];
        uint8_t *d = malloc(n + 1);
        int rc = randomize(h, NULL, &d, &n, 1, 7 + s);
        if (rc != DVS_RANDOMIZE_OK) ok = 0;
        for (uint32_t k = 0; k < n && ok; k++) {
            if (d[k] >= 10 || d[k] == k) ok = 0;
            else seen[d[k]]++;
        }
        free(d);
    }
    CHECK(ok, "local: every element < 10 and != its index, sizes 0..3000");
    int distinct = 0;
    for (int v = 0; v < 10; v++) distinct += seen[v] != 0;
    if (distinct != 10)
        for (int v = 0; v < 10; v++) fprintf(stderr, "  value %d: %d\n", v, seen[v]);
    CHECK(distinct == 10, "local: elements take every allowed value");
    CHECK(dvs_tmpl_array_is_local(dvs_handle_tmpl(h), 0), "local: classified local");
    CHECK(dvs_tmpl_n_expansions(dvs_handle_tmpl(h)) == 1,
          "local: one expansion serves every size");
    dvs_handle_free(h);
}

/* foreach (a[i]) i + 1 < a.size() -> a[i] < a[i+1]: expanded per size,
 * kept per size. */
static void check_sorted(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_expr_t next = bin(DVS_BIN_ADD, idx(0), c32(1));
    dvs_builder_add_constraint(B, bin(DVS_BIN_OR,
        bin(DVS_BIN_GTE, next, dvs_builder_expr_array_size(B, 0)),
        bin(DVS_BIN_LT, elem(0, idx(0)), elem(0, next))));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "sorted: handle");
    if (!h) return;
    CHECK(!dvs_tmpl_array_is_local(dvs_handle_tmpl(h), 0), "sorted: classified expanded");
    int ok = 1;
    static const uint32_t sizes[] = {6, 6, 20, 6, 0};
    for (uint32_t s = 0; s < 5; s++) {
        uint32_t n = sizes[s];
        uint8_t d[32];
        if (randomize(h, NULL, (uint8_t *[]){d}, &n, 1, 3 + s) != DVS_RANDOMIZE_OK) ok = 0;
        for (uint32_t k = 0; k + 1 < n; k++)
            if (d[k] >= d[k + 1]) ok = 0;
    }
    CHECK(ok, "sorted: strictly increasing for sizes 6, 20, 0");
    CHECK(dvs_tmpl_n_expansions(dvs_handle_tmpl(h)) == 3,
          "sorted: one expansion per size, reused");
    dvs_handle_free(h);
}

/* a.sum() with (int'(item)) == 100 over 4 elements; sum over none. */
static void check_sum(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_add_var(B, 0, 32, 1, INT32_MIN, INT32_MAX);
    dvs_builder_set_var_kind(B, 0, DVS_VAR_STATE);
    dvs_builder_add_constraint(B, bin(DVS_BIN_EQ,
        dvs_builder_expr_reduce(B, DVS_BIN_ADD, 0, 1, to32(elem(0, idx(1)))),
        dvs_builder_expr_var(B, 0)));
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "sum: handle");
    if (!h) return;
    int32_t want = 100;
    dvs_ref_t ref = {&want, 32, 1, DVS_REF_SCALAR, 0};
    uint8_t d[4];
    uint32_t n = 4;
    int ok = 1;
    for (uint64_t s = 1; s <= 20; s++) {
        if (randomize(h, &ref, (uint8_t *[]){d}, &n, 1, s) != DVS_RANDOMIZE_OK) ok = 0;
        if (d[0] + d[1] + d[2] + d[3] != 100) ok = 0;
    }
    CHECK(ok, "sum: four elements sum to 100");
    n = 0;
    want = 0;
    CHECK(randomize(h, &ref, (uint8_t *[]){NULL}, &n, 1, 1) == DVS_RANDOMIZE_OK,
          "sum: an empty array sums to 0");
    want = 5;
    CHECK(randomize(h, &ref, (uint8_t *[]){NULL}, &n, 1, 1) == DVS_RANDOMIZE_UNSAT,
          "sum: an empty array does not sum to 5");
    dvs_handle_free(h);
}

/* x in [0:3] random, a[x] == 77, foreach (a[i]) a[i] != 77 || i == 2;
 * a[9] (out of range) == 0. */
static void check_random_index(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_add_var(B, 0, 8, 0, 0, 3);
    dvs_builder_add_constraint(B, bin(DVS_BIN_EQ,
        elem(0, dvs_builder_expr_var(B, 0)), c8(77)));
    dvs_builder_add_constraint(B, bin(DVS_BIN_EQ, elem(0, c32(9)), c8(0)));
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_OR,
        bin(DVS_BIN_NEQ, elem(0, idx(0)), c8(77)), bin(DVS_BIN_EQ, idx(0), c32(2))));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "index: handle");
    if (!h) return;
    uint8_t x, d[4];
    uint32_t n = 4;
    dvs_ref_t ref = {&x, 8, 0, DVS_REF_SCALAR, 0};
    int ok = 1;
    for (uint64_t s = 1; s <= 20; s++) {
        if (randomize(h, &ref, (uint8_t *[]){d}, &n, 1, s) != DVS_RANDOMIZE_OK) ok = 0;
        if (x != 2 || d[2] != 77) ok = 0;
    }
    CHECK(ok, "index: a random index selects the element");
    dvs_handle_free(h);
}

/* foreach (a[i]) a[i] > b[i], b STATE: element-local, reading b per element;
 * b shorter than a reads 0. */
static void check_state_array(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_add_array(B, 1, 8, 0, DVS_VAR_STATE);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_GT, elem(0, idx(0)), elem(1, idx(0))));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "state: handle");
    if (!h) return;
    CHECK(dvs_tmpl_array_is_local(dvs_handle_tmpl(h), 0), "state: classified local");
    uint8_t a[8], b[5] = {250, 3, 100, 0, 254};
    uint32_t n[2] = {8, 5};
    int ok = 1;
    for (uint64_t s = 1; s <= 20; s++) {
        if (randomize(h, NULL, (uint8_t *[]){a, b}, n, 2, s) != DVS_RANDOMIZE_OK) ok = 0;
        for (uint32_t k = 0; k < 8; k++)
            if (a[k] <= (k < 5 ? b[k] : 0)) ok = 0;
    }
    CHECK(ok, "state: each element above the STATE element (0 past its end)");
    b[1] = 255;
    memset(a, 0x5A, sizeof a);
    CHECK(randomize(h, NULL, (uint8_t *[]){a, b}, n, 2, 1) == DVS_RANDOMIZE_UNSAT,
          "state: no value above 255");
    int kept = 1;
    for (uint32_t k = 0; k < 8; k++) kept &= a[k] == 0x5A;
    CHECK(kept, "state: nothing written on UNSAT");
    dvs_handle_free(h);
}

/* unique {a}, foreach (a[i]) a[i] < 8, over 8 elements: a permutation. */
static void check_unique(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    uint32_t m = DVS_ARRAY_MEMBER(0);
    dvs_builder_add_all_different(B, 1, &m);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, elem(0, idx(0)), c8(8)));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "unique: handle");
    if (!h) return;
    CHECK(!dvs_tmpl_array_is_local(dvs_handle_tmpl(h), 0), "unique: classified expanded");
    uint8_t d[8];
    uint32_t n = 8;
    int ok = 1;
    for (uint64_t s = 1; s <= 10; s++) {
        if (randomize(h, NULL, (uint8_t *[]){d}, &n, 1, s) != DVS_RANDOMIZE_OK) ok = 0;
        int seen = 0;
        for (uint32_t k = 0; k < 8; k++) seen |= 1 << d[k];
        if (seen != 0xFF) ok = 0;
    }
    CHECK(ok, "unique: a permutation of 0..7");
    n = 9;
    uint8_t e[9];
    CHECK(randomize(h, NULL, (uint8_t *[]){e}, &n, 1, 1) == DVS_RANDOMIZE_UNSAT,
          "unique: nine elements below 8 cannot differ");
    dvs_handle_free(h);
}

/* Whether the n values are pairwise different and all below lim. */
static int all_differ(const int64_t *v, uint32_t n, int64_t lim) {
    static uint8_t seen[1u << 16];
    memset(seen, 0, sizeof seen);
    for (uint32_t k = 0; k < n; k++) {
        if (v[k] < 0 || v[k] >= lim || seen[v[k]]) return 0;
        seen[v[k]] = 1;
    }
    return 1;
}

/* unique {a}, foreach (a[i]) a[i] < 1500, over 17, 100 and 1000 16-bit
 * elements; unique {a} over 20 4-bit elements, which cannot differ. */
static void check_unique_large(void) {
    static const uint32_t sizes[] = {17, 100, 1000};
    begin();
    dvs_builder_add_array(B, 0, 16, 0, DVS_VAR_RAND);
    uint32_t m = DVS_ARRAY_MEMBER(0);
    dvs_builder_add_all_different(B, 1, &m);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, elem(0, idx(0)),
                                      dvs_builder_expr_const_sized(B, 1500, 0, 16)));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "unique large: handle");
    if (!h) return;
    for (int z = 0; z < 3; z++) {
        uint32_t n = sizes[z];
        uint16_t d[1000];
        int64_t v[1000];
        int ok = 1;
        for (uint64_t s = 1; s <= (n < 1000 ? 3u : 1u); s++) {
            if (randomize(h, NULL, (uint8_t *[]){(uint8_t *)d}, &n, 1, s) != DVS_RANDOMIZE_OK)
                ok = 0;
            for (uint32_t k = 0; k < n; k++) v[k] = d[k];
            ok &= all_differ(v, n, 1500);
        }
        char msg[80];
        snprintf(msg, sizeof msg, "unique large: %u distinct elements below 1500", n);
        CHECK(ok, msg);
    }
    dvs_handle_free(h);

    begin();
    dvs_builder_add_array(B, 0, 4, 0, DVS_VAR_RAND);
    dvs_builder_add_all_different(B, 1, &m);
    h = finish();
    CHECK(h != NULL, "unique large: 4-bit handle");
    if (!h) return;
    uint8_t e[20];
    uint32_t n = 20;
    CHECK(randomize(h, NULL, (uint8_t *[]){e}, &n, 1, 1) == DVS_RANDOMIZE_UNSAT,
          "unique large: twenty 4-bit elements cannot differ");
    dvs_handle_free(h);
}

/* A fixed problem: all-different over 17, 100 and 1000 16-bit variables below
 * 1500; and over 20 4-bit variables, which cannot differ. */
static void check_all_different_fixed(void) {
    static const uint32_t sizes[] = {17, 100, 1000, 20};
    static uint32_t ids[1000];
    static int64_t v[1000];
    for (int z = 0; z < 4; z++) {
        uint32_t n = sizes[z], w = z == 3 ? 4 : 16;
        begin();
        for (uint32_t k = 0; k < n; k++) {
            dvs_builder_add_var(B, k, w, 0, 0, w == 4 ? 15 : 1499);
            ids[k] = k;
        }
        dvs_builder_add_all_different(B, n, ids);
        size_t sz;
        dvs_problem_t *p = dvs_builder_finalize(B, &sz);
        dvs_ctx_t *ctx = dvs_solver_new(NULL);
        int rc = dvs_solver_compile(ctx, p);
        if (w == 4) {
            CHECK(rc == DVS_COMPILE_UNSAT ||
                  (rc == DVS_COMPILE_OK && dvs_solver_solve(ctx, NULL) == DVS_SOLVE_UNSAT),
                  "all-different fixed: twenty 4-bit variables cannot differ");
        } else {
            int ok = rc == DVS_COMPILE_OK;
            for (uint64_t s = 1; s <= (n < 1000 ? 3u : 1u) && ok; s++) {
                dvs_solve_opts_t opts = {0};
                opts.seed = s;
                opts.fair_pick = 1;
                dvs_solver_reset(ctx);
                ok = dvs_solver_solve(ctx, &opts) == DVS_SOLVE_OK;
                if (ok) dvs_solver_get_values(ctx, n, ids, v);
                ok = ok && all_differ(v, n, 1500);
            }
            char msg[80];
            snprintf(msg, sizeof msg, "all-different fixed: %u distinct variables", n);
            CHECK(ok, msg);
        }
        dvs_solver_free(ctx);
        dvs_builder_free_problem(B, p, sz);
        dvs_builder_destroy(B);
    }
}

/* foreach (a[i]) { a[i] dist {0 := 1, 1 := 3}; soft a[i] == 1; i == 0 -> a[i] == 0 } */
static void check_dist_soft(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_dist_entry_t de[2];
    memset(de, 0, sizeof de);
    de[0].lo = de[0].hi = 0; de[0].weight = 1; de[0].is_per_value = 1;
    de[1].lo = de[1].hi = 1; de[1].weight = 3; de[1].is_per_value = 1;
    CHECK(dvs_builder_add_dist(B, 0, 2, de) == EXPR_NULL, "dist: add_dist refused in a foreach");
    dvs_builder_add_dist_elem(B, 0, idx(0), 2, de);
    dvs_builder_add_constraint(B, bin(DVS_BIN_OR,
        bin(DVS_BIN_NEQ, idx(0), c32(0)), bin(DVS_BIN_EQ, elem(0, idx(0)), c8(0))));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "dist: handle");
    if (!h) return;
    CHECK(dvs_tmpl_array_is_local(dvs_handle_tmpl(h), 0), "dist: classified local");
    uint8_t d[400];
    uint32_t n = 400;
    int ok = randomize(h, NULL, (uint8_t *[]){d}, &n, 1, 5) == DVS_RANDOMIZE_OK;
    uint32_t ones = 0;
    for (uint32_t k = 0; k < n; k++) {
        if (d[k] > 1 || (k == 0 && d[k] != 0)) ok = 0;
        ones += d[k];
    }
    CHECK(ok, "dist: values in {0, 1}, element 0 is 0");
    CHECK(ones > 250 && ones < 350, "dist: about 3/4 ones");
    dvs_handle_free(h);
}

/* x random below 5, foreach (a[i]) a[i] < x: coupled through x, expanded. */
static void check_coupled(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_add_var(B, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, dvs_builder_expr_var(B, 0), c8(5)));
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, elem(0, idx(0)),
                                      dvs_builder_expr_var(B, 0)));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "coupled: handle");
    if (!h) return;
    CHECK(!dvs_tmpl_array_is_local(dvs_handle_tmpl(h), 0), "coupled: classified expanded");
    uint8_t x, d[10];
    uint32_t n = 10;
    dvs_ref_t ref = {&x, 8, 0, DVS_REF_SCALAR, 0};
    int ok = 1;
    for (uint64_t s = 1; s <= 10; s++) {
        if (randomize(h, &ref, (uint8_t *[]){d}, &n, 1, s) != DVS_RANDOMIZE_OK) ok = 0;
        for (uint32_t k = 0; k < n; k++)
            if (x >= 5 || d[k] >= x) ok = 0;
    }
    CHECK(ok, "coupled: every element below x, x below 5");
    n = 70000;
    uint8_t *big = malloc(n);
    CHECK(randomize(h, &ref, &big, &n, 1, 1) == DVS_RANDOMIZE_TOO_LARGE,
          "coupled: 70000 expanded elements are too large");
    free(big);
    dvs_handle_free(h);
}

/* foreach (a[i]) foreach (b[j]) a[i] != b[j]; b[j] < 4; a[i] < 8. */
static void check_nested(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_add_array(B, 1, 8, 0, DVS_VAR_RAND);
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, elem(0, idx(0)), c8(8)));
    CHECK(dvs_builder_foreach_begin(B, 1, 0) == -1, "nested: a bound level is refused");
    dvs_builder_foreach_begin(B, 1, 1);
    dvs_builder_add_constraint(B, bin(DVS_BIN_NEQ, elem(0, idx(0)), elem(1, idx(1))));
    dvs_builder_foreach_end(B);
    dvs_builder_foreach_end(B);
    dvs_builder_foreach_begin(B, 1, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, elem(1, idx(0)), c8(4)));
    dvs_builder_foreach_end(B);
    CHECK(dvs_builder_foreach_end(B) == -1, "nested: no foreach left to close");
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "nested: handle");
    if (!h) return;
    uint8_t a[5], b[3];
    uint32_t n[2] = {5, 3};
    int ok = 1;
    for (uint64_t s = 1; s <= 10; s++) {
        if (randomize(h, NULL, (uint8_t *[]){a, b}, n, 2, s) != DVS_RANDOMIZE_OK) ok = 0;
        for (uint32_t i = 0; i < 5; i++)
            for (uint32_t j = 0; j < 3; j++)
                if (a[i] >= 8 || b[j] >= 4 || a[i] == b[j]) ok = 0;
    }
    CHECK(ok, "nested: no element of a equals one of b");
    dvs_handle_free(h);
}

/* q.size() == x, x in [2:6], foreach (q[i]) q[i] < 10; p.size() (p STATE)
 * read by the sizes; sum over r, whose size is 4. */
static void check_sizes(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);    /* q: element-local */
    dvs_builder_add_array(B, 1, 8, 0, DVS_VAR_STATE);   /* p */
    dvs_builder_add_array(B, 2, 8, 0, DVS_VAR_RAND);    /* r: expanded */
    CHECK(dvs_builder_set_array_size_random(B, 1) == -1, "sizes: a STATE size is not random");
    dvs_builder_set_array_size_random(B, 0);
    dvs_builder_set_array_size_random(B, 2);
    dvs_builder_add_var(B, 0, 32, 1, INT32_MIN, INT32_MAX);   /* x */
    dvs_builder_add_var(B, 1, 32, 1, INT32_MIN, INT32_MAX);   /* y */
    dvs_expr_t x = dvs_builder_expr_var(B, 0), y = dvs_builder_expr_var(B, 1);
    dvs_builder_add_constraint(B, bin(DVS_BIN_EQ, dvs_builder_expr_array_size(B, 0), x));
    dvs_builder_add_constraint(B, bin(DVS_BIN_AND, bin(DVS_BIN_GTE, x, c32(2)),
                                      bin(DVS_BIN_LTE, x, c32(6))));
    dvs_builder_add_constraint(B, bin(DVS_BIN_EQ, dvs_builder_expr_array_size(B, 1), y));
    dvs_builder_add_constraint(B, bin(DVS_BIN_EQ, dvs_builder_expr_array_size(B, 2), c32(4)));
    dvs_builder_add_constraint(B, bin(DVS_BIN_EQ,
        dvs_builder_expr_reduce(B, DVS_BIN_ADD, 2, 1, to32(elem(2, idx(1)))), c32(100)));
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, elem(0, idx(0)), c8(10)));
    dvs_builder_foreach_end(B);
    dvs_handle_t *h = finish();
    CHECK(h != NULL, "sizes: handle");
    if (!h) return;
    int32_t xv = -1, yv = -1;
    dvs_ref_t refs[2] = {{&xv, 32, 1, DVS_REF_SCALAR, 0}, {&yv, 32, 1, DVS_REF_SCALAR, 0}};
    uint8_t q[8], p[3] = {0}, r[4];
    int ok = 1, seen[7] = {0};
    for (uint64_t s = 1; s <= 40; s++) {
        uint32_t sizes[3] = {0, 3, 0};
        if (dvs_randomize_sizes(h, refs, sizes, s) != DVS_RANDOMIZE_OK) { ok = 0; break; }
        if (xv != -1 || yv != -1) ok = 0;   /* nothing written */
        if (sizes[0] < 2 || sizes[0] > 6 || sizes[1] != 3 || sizes[2] != 4) { ok = 0; break; }
        seen[sizes[0]] = 1;
        uint32_t n[3] = {sizes[0], sizes[1], sizes[2]};
        if (randomize(h, refs, (uint8_t *[]){q, p, r}, n, 3, s) != DVS_RANDOMIZE_OK) ok = 0;
        if ((uint32_t)xv != n[0] || yv != 3 || r[0] + r[1] + r[2] + r[3] != 100) ok = 0;
        for (uint32_t k = 0; k < n[0]; k++) ok &= q[k] < 10;
        xv = yv = -1;
    }
    CHECK(ok, "sizes: solved first, then the elements at those sizes");
    CHECK(seen[2] && seen[3] && seen[4] && seen[5] && seen[6], "sizes: every size drawn");
    CHECK(dvs_tmpl_n_expansions(dvs_handle_tmpl(h)) == 1,
          "sizes: an element-local array's size needs no new expansion");
    dvs_handle_free(h);

    /* A size bounded below only may be drawn over 65535: that fails the call,
     * the sizes unchanged. One below 0 is UNSAT. */
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_set_array_size_random(B, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_GT, dvs_builder_expr_array_size(B, 0), c32(3)));
    h = finish();
    uint32_t size = 0;
    int too_large = 0;
    ok = 1;
    for (uint64_t s = 1; s <= 10; s++) {
        size = 0;
        int rc = dvs_randomize_sizes(h, NULL, &size, s);
        if (rc == DVS_RANDOMIZE_TOO_LARGE) too_large += size == 0;
        else ok &= rc == DVS_RANDOMIZE_OK && size > 3 && size <= 65535;
    }
    CHECK(ok && too_large > 0, "sizes: over 65535 is too large, not capped");
    dvs_handle_free(h);
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_set_array_size_random(B, 0);
    dvs_builder_add_constraint(B, bin(DVS_BIN_LT, dvs_builder_expr_array_size(B, 0), c32(0)));
    h = finish();
    size = 7;
    CHECK(dvs_randomize_sizes(h, NULL, &size, 1) == DVS_RANDOMIZE_UNSAT && size == 7,
          "sizes: a negative size is UNSAT, the size unchanged");
    dvs_handle_free(h);
}

/* The context compiler refuses a problem with arrays. */
static void check_compile_refuses(void) {
    begin();
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_add_var(B, 0, 8, 0, 0, 255);
    size_t sz;
    dvs_problem_t *p = dvs_builder_finalize(B, &sz);
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    CHECK(dvs_solver_compile(ctx, p) == DVS_COMPILE_ARRAYS, "compile: arrays refused");
    dvs_solver_free(ctx);
    CHECK(dvs_problem_validate(p, NULL, 0) == 0, "validate: a template passes");
    dvs_builder_free_problem(B, p, sz);
    dvs_builder_destroy(B);
}

int main(void) {
    check_local();
    check_sorted();
    check_sum();
    check_random_index();
    check_state_array();
    check_unique();
    check_unique_large();
    check_all_different_fixed();
    check_dist_soft();
    check_coupled();
    check_nested();
    check_sizes();
    check_compile_refuses();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}
