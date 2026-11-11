/* The problem image: its header and alignment, dvs_problem_check_image()
 * (and so dvs_handle_new()) refusing a damaged image, and
 * dvs_problem_validate() refusing what dvs_solver_compile() refuses. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "dv_solve.h"
#include "dvs_builder.h"
#include "dvs_problem.h"

static int failures = 0;
#define CHECK(c, msg) do { if (!(c)) { fprintf(stderr,"FAIL %s\n",msg); failures++; } else printf("PASS %s\n",msg); } while(0)

static dvs_builder_t *b;
static size_t sz;

static dvs_problem_t *finish(void) {
    dvs_problem_t *p = dvs_builder_finalize(b, &sz);
    dvs_builder_reset(b);
    return p;
}

/* A problem with every node kind and every spec list. */
static dvs_problem_t *rich(void) {
    for (uint32_t i = 0; i < 4; i++) dvs_builder_add_var(b, i, 8, 0, 0, 255);
    dvs_expr_t x = dvs_builder_expr_var(b, 0), y = dvs_builder_expr_var(b, 1);
    dvs_expr_t z = dvs_builder_expr_var(b, 2), w = dvs_builder_expr_var(b, 3);
    dvs_expr_t c3 = dvs_builder_expr_const_sized(b, 3, 0, 8);
    uint64_t limbs[2] = { 5, 0 };
    dvs_expr_t wide = dvs_builder_expr_const_wide(b, 2, limbs, 100, 0);
    dvs_expr_t set[2] = { c3, dvs_builder_expr_const(b, 7, 0) };
    dvs_expr_t los[1] = { c3 }, his[1] = { dvs_builder_expr_const(b, 9, 0) };
    dvs_expr_t sum[2] = { x, y };
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT, x,
        dvs_builder_expr_unary(b, DVS_UN_INVERT, y)));
    dvs_builder_add_constraint(b, dvs_builder_expr_ite(b,
        dvs_builder_expr_in_range(b, x, c3, his[0]),
        dvs_builder_expr_in_set(b, y, 2, set),
        dvs_builder_expr_in_ranges(b, y, 1, los, his)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ,
        dvs_builder_expr_concat(b, dvs_builder_expr_extract(b, x, 3, 0),
            dvs_builder_expr_cast(b, y, 4, 0), 4),
        dvs_builder_expr_extend(b, z, 8, 100, 0)));
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ,
        dvs_builder_expr_extend(b, w, 8, 100, 0), wide));
    dvs_builder_add_constraint(b, dvs_builder_expr_sum(b, w, 2, sum));
    dvs_builder_add_constraint(b, dvs_builder_expr_countones(b, z, x));
    dvs_builder_add_soft_constraint(b, dvs_builder_expr_clog2(b, z, y), 1);
    uint32_t ids[2] = { 0, 1 };
    dvs_builder_add_all_different(b, 2, ids);
    dvs_builder_add_source(b, 2, ids);
    dvs_dist_entry_t d[2] = { { 0, 9, 1, 1, {0} }, { 10, 20, 3, 0, {0} } };
    dvs_builder_add_dist(b, 0, 2, d);
    return finish();
}

static int compile_rc(dvs_problem_t *p) {
    dvs_ctx_t *ctx = dvs_solver_new(NULL);
    int rc = dvs_solver_compile(ctx, p);
    dvs_solver_free(ctx);
    return rc;
}

/* The image check refuses `p` as damaged, and so does dvs_handle_new(). */
static void refused(dvs_problem_t *p, size_t size, const char *msg) {
    char why[160] = "";
    int rc = dvs_problem_check_image(p, size, why, sizeof why);
    dvs_handle_t *h = dvs_handle_new(p, size);
    printf("  (%s)\n", why);
    CHECK(rc == -1 && why[0] && !h, msg);
    dvs_handle_free(h);
}

static void check_layout(void) {
    dvs_problem_t *p = rich();
    CHECK(memcmp(p, "DVSP", 4) == 0, "magic is the bytes DVSP at offset 0");
    uint32_t ver;
    memcpy(&ver, (uint8_t *)p + 4, 4);
    CHECK(ver == DVS_PROBLEM_FORMAT_VERSION && ver == 5, "format version 5 at offset 4");
    CHECK(dvs_problem_check_image(p, sz, NULL, 0) == 0, "every node kind passes the check");
    int aligned = p->dists_head % 8 == 0
        && (uintptr_t)dist_spec_entries(p, p->dists_head) % 8 == 0;
    for (dvs_expr_t r = p->constraints_head; r != EXPR_NULL;) {
        ConstraintSpec *cs = POOL_PTR(p, r);
        aligned &= r % 8 == 0 && cs->root % 8 == 0;
        r = cs->next;
    }
    CHECK(aligned, "nodes, and distribution entries, are 8-aligned");
    char why[160] = "";
    int v = dvs_problem_validate(p, why, sizeof why), rc = compile_rc(p);
    printf("  (validate %d %s, compile %d)\n", v, why, rc);
    CHECK(v == 0 && rc >= 0, "a rich problem validates and compiles");
    dvs_builder_free_problem(b, p, sz);
}

/* The root of rich()'s constraint `i`, in the order they were added. */
static void *root(dvs_problem_t *q, uint32_t i) {
    uint32_t k = q->n_constraints - 1 - i;
    dvs_expr_t r = q->constraints_head;
    while (k--) r = ((ConstraintSpec *)POOL_PTR(q, r))->next;
    return POOL_PTR(q, ((ConstraintSpec *)POOL_PTR(q, r))->root);
}

static void check_damage(void) {
    dvs_problem_t *p = rich();
    dvs_problem_t *q = malloc(sz);
    char *raw = malloc(sz + 8);
#define FRESH() memcpy(q, p, sz)
    FRESH();
    refused(q, sz - 1, "truncated image");
    refused(q, sizeof(dvs_problem_t) - 1, "image shorter than its header");
    memcpy(raw + 4, p, sz);
    char why[160] = "";
    CHECK(dvs_problem_check_image(raw + 4, sz, why, sizeof why) == -1
          && dvs_handle_new(raw + 4, sz) != NULL,
          "a misaligned buffer: refused in place, copied by dvs_handle_new");
    dvs_handle_free(dvs_handle_new(raw + 4, sz));
    q->format_version = 1;
    refused(q, sz, "format version 1");
    FRESH();
    q->magic = 0x44565350u;   /* "PSVD": a big-endian writer's magic */
    refused(q, sz, "big-endian magic");
    FRESH();
    q->pool.capacity = q->pool.used + 8;
    refused(q, sz, "pool capacity past the end");
    FRESH();
    q->pool.used = q->pool.capacity + 1;
    refused(q, sz, "pool used over capacity");
    FRESH();
    ConstraintSpec *cs = POOL_PTR(q, q->constraints_head);
    cs->root = (dvs_expr_t)(sizeof(dvs_pool_t) + q->pool.used);
    refused(q, sz, "root offset out of range");
    FRESH();
    cs->root += 4;
    refused(q, sz, "misaligned root offset");
    FRESH();
    cs->root = EXPR_NULL;
    refused(q, sz, "null root");
    FRESH();
    q->constraints_head += 4;
    refused(q, sz, "misaligned list offset");
    FRESH();
    q->n_constraints++;
    refused(q, sz, "list shorter than its count");
    FRESH();
    cs->next = q->constraints_head;
    refused(q, sz, "list with a cycle");
    FRESH();
    *(uint32_t *)root(q, 5) = 99;
    refused(q, sz, "unknown node kind");
    FRESH();
    ExprBinary *n = root(q, 0);   /* x < ~y */
    n->op = DVS_BIN_ASHR + 1;
    refused(q, sz, "unknown operator");
    FRESH();
    n->lhs = cs->root;   /* a later node: a child above its parent */
    refused(q, sz, "child above its parent");
    FRESH();
    ExprUnary *u = POOL_PTR(q, n->rhs);
    u->operand = (dvs_expr_t)(sizeof(dvs_pool_t) + q->pool.used + 64);
    refused(q, sz, "grandchild out of range");
    FRESH();
    ExprSum *s = root(q, 4);
    s->n_vars = 1u << 30;
    refused(q, sz, "sum terms run past the end");
    FRESH();
    DistSpec *ds = POOL_PTR(q, q->dists_head);
    ds->n_entries = 1000;
    refused(q, sz, "distribution entries run past the end");
    FRESH();
    VarSpec *vs = POOL_PTR(q, q->vars_head);
    vs->kind = 9;
    refused(q, sz, "unknown variable kind");
    FRESH();
    vs->var_id = 0xFFFFFFFFu;
    refused(q, sz, "variable id over n_vars");
    free(raw);
    free(q);
    dvs_builder_free_problem(b, p, sz);
}

/* validate refuses it; when `code` is not 0, compile refuses it with `code`. */
static void invalid(dvs_problem_t *p, int code, const char *msg) {
    char why[160] = "";
    int v = dvs_problem_validate(p, why, sizeof why);
    printf("  (%s)\n", why);
    if (code) {
        int rc = compile_rc(p);
        if (rc != code) printf("  compile returned %d\n", rc);
        CHECK(v == -1 && rc == code, msg);
    } else {
        CHECK(v == -1, msg);
    }
    dvs_builder_free_problem(b, p, sz);
}

static void check_validate(void) {
    dvs_expr_t x;
    dvs_problem_t *p;
    /* Variables */
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    invalid(finish(), DVS_COMPILE_BAD_VAR, "variable declared twice");
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    p = finish();
    ((VarSpec *)POOL_PTR(p, p->vars_head))->width = 0;
    invalid(p, DVS_COMPILE_BAD_VAR, "variable of width 0");
    dvs_builder_add_var(b, 0, 65, 0, 0, 255);
    invalid(finish(), DVS_COMPILE_UNSUPPORTED_WIDTH, "variable over 64 bits");
    /* All-different */
    uint32_t ids[16];
    for (uint32_t i = 0; i < 16; i++) ids[i] = i;
    dvs_builder_add_var(b, 0, 33, 0, 0, 255);
    dvs_builder_add_var(b, 1, 33, 0, 0, 255);
    dvs_builder_add_all_different(b, 2, ids);
    invalid(finish(), DVS_COMPILE_UNSUPPORTED_WIDTH, "all-different over 33 bits");
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_var(b, 1, 8, 0, 0, 255);
    ids[1] = 5;
    dvs_builder_add_all_different(b, 2, ids);
    invalid(finish(), DVS_COMPILE_BAD_VAR, "all-different names an undeclared variable");
    /* Undeclared variables elsewhere */
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_var(b, 3)));
    invalid(finish(), DVS_COMPILE_BAD_VAR, "expression names an undeclared variable");
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_array_select(b, 0, 2,
        dvs_builder_expr_var(b, 0), dvs_builder_expr_const(b, 0, 0)));
    invalid(finish(), 0, "array select over undeclared variables");
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_source(b, 2, ids);
    invalid(finish(), 0, "source group names an undeclared variable");
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_dist_entry_t d = { 0, 1, 1, 1, {0} };
    dvs_builder_add_dist(b, 4, 1, &d);
    invalid(finish(), 0, "distribution names an undeclared variable");
    /* SV elaboration */
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_var(b, 0),
        dvs_builder_expr_extract(b, dvs_builder_expr_var(b, 0), 0, 3)));
    invalid(finish(), DVS_COMPILE_UNSUPPORTED_WIDTH, "reversed slice");
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ,
        dvs_builder_expr_var(b, 0),
        dvs_builder_expr_extend(b, dvs_builder_expr_var(b, 0), 8, DVS_MAX_WIDTH + 1, 0)));
    invalid(finish(), 0, "extend over DVS_MAX_WIDTH");
    dvs_builder_add_var(b, 0, 8, 0, 0, 255);
    x = dvs_builder_expr_var(b, 0);
    for (int i = 0; i < 20001; i++) x = dvs_builder_expr_unary(b, DVS_UN_INVERT, x);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_EQ, x,
        dvs_builder_expr_const(b, 1, 0)));
    invalid(finish(), 0, "expression nested over 20000 deep");
    /* At the limits: accepted by both */
    for (uint32_t i = 0; i < 16; i++) dvs_builder_add_var(b, i, i ? 32 : 64, 0, 0, 255);
    ids[1] = 1;
    dvs_builder_add_all_different(b, 15, ids + 1);
    x = dvs_builder_expr_var(b, 0);
    for (int i = 0; i < 1000; i++) x = dvs_builder_expr_unary(b, DVS_UN_INVERT, x);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_NEQ, x,
        dvs_builder_expr_const(b, 1, 0)));
    p = finish();
    char why[160] = "";
    int v = dvs_problem_validate(p, why, sizeof why), rc = compile_rc(p);
    printf("  (validate %d %s, compile %d)\n", v, why, rc);
    CHECK(v == 0 && rc == 0, "a problem at the limits validates and compiles");
    dvs_builder_free_problem(b, p, sz);
}

/* A problem with arrays: every array node kind and a foreach body. (EXPLICIT,
 * as Verilator writes them: on a builder of its own, as a reset keeps it.) */
static dvs_problem_t *arrays(void) {
    dvs_builder_set_explicit(b);
    dvs_builder_add_array(b, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_add_array(b, 1, 8, 0, DVS_VAR_STATE);
    dvs_builder_foreach_begin(b, 0, 0);
    dvs_expr_t i = dvs_builder_expr_loop_index(b, 0);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_elem(b, 0, i), dvs_builder_expr_elem(b, 1, i)));
    dvs_dist_entry_t de = {0, 3, 1, 0, {0}};
    dvs_builder_add_dist_elem(b, 0, i, 1, &de);
    dvs_builder_foreach_end(b);
    dvs_builder_add_constraint(b, dvs_builder_expr_binary(b, DVS_BIN_LT,
        dvs_builder_expr_reduce(b, DVS_BIN_ADD, 0, 1,
            dvs_builder_expr_elem(b, 0, dvs_builder_expr_loop_index(b, 1))),
        dvs_builder_expr_array_size(b, 1)));
    return finish();
}

/* An array id or a loop level out of range is refused by the image check. */
static void check_array_damage(void) {
    dvs_builder_t *shared = b;
    b = dvs_builder_create(0, NULL);
    dvs_problem_t *p = arrays();
    CHECK(dvs_problem_check_image(p, sz, NULL, 0) == 0, "a problem with arrays passes");
    CHECK(dvs_problem_validate(p, NULL, 0) == 0, "a problem with arrays validates");
    CHECK(compile_rc(p) == DVS_COMPILE_ARRAYS, "compile refuses a problem with arrays");
    dvs_problem_t *q = malloc(sz);
    memcpy(q, p, sz);
    q->n_arrays = 1;
    refused(q, sz, "an element of an undeclared array");
    memcpy(q, p, sz);
    ((ForeachSpec *)POOL_PTR(q, q->foreachs_head))->level = DVS_MAX_LOOP_LEVELS;
    refused(q, sz, "a foreach level out of range");
    memcpy(q, p, sz);
    ((ForeachSpec *)POOL_PTR(q, q->foreachs_head))->n_constraints = 2;
    refused(q, sz, "a foreach body with fewer constraints than it says");
    memcpy(q, p, sz);
    ((ArraySpec *)POOL_PTR(q, q->arrays_head))->elem_width = 65;
    refused(q, sz, "an array wider than 64 bits");
    free(q);
    dvs_builder_free_problem(b, p, sz);
    dvs_builder_destroy(b);
    b = shared;
}

int main(void) {
    b = dvs_builder_create(0, NULL);
    check_layout();
    check_damage();
    check_array_damage();
    check_validate();
    dvs_builder_destroy(b);
    if (failures) { fprintf(stderr, "%d failures\n", failures); return 1; }
    printf("all passed\n");
    return 0;
}
