/* Parts solved together (dvs_randomize_parts): a tree of objects with
 * imports through nested slots, a cycle, one object reached twice, null and
 * inactive slots, soft priorities across objects, a derived class's part in
 * a slot of its base class, arrays in a part, slot arrays (dynamic arrays of
 * handles), and the shape cache. Problems
 * are EXPLICIT, as Verilator builds them. */
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

static dvs_expr_t c8(int64_t v) { return dvs_builder_expr_const_sized(B, v, 0, 8); }
static dvs_expr_t c1(int64_t v) { return dvs_builder_expr_const_sized(B, v, 0, 1); }
static dvs_expr_t var(uint32_t id) { return dvs_builder_expr_var(B, id); }
static dvs_expr_t bin(dvs_binop_t op, dvs_expr_t a, dvs_expr_t b) {
    return dvs_builder_expr_binary(B, op, a, b);
}
static void hard(dvs_expr_t e) { dvs_builder_add_constraint(B, e); }
static void soft(dvs_expr_t e, uint32_t prio) { dvs_builder_add_soft_constraint(B, e, prio); }

static void begin(uint32_t n_slots) {
    B = dvs_builder_create(0, NULL);
    dvs_builder_set_explicit(B);
    dvs_builder_set_n_slots(B, n_slots);
}

/* An 8-bit unsigned variable; `kind` RAND or STATE. */
static void var8(uint32_t id, dvs_var_kind_t kind) {
    dvs_builder_add_var(B, id, 8, 0, 0, 255);
    dvs_builder_set_var_kind(B, id, kind);
}

/* An 8-bit import of export `k` through `n` slots. */
static void import8(uint32_t id, uint32_t n, const uint32_t *path, uint32_t k) {
    var8(id, DVS_VAR_RAND);
    dvs_builder_add_import(B, id, n, path, k);
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

static dvs_ref_t r8(uint8_t *a) { return (dvs_ref_t){a, 8, 0, DVS_REF_SCALAR, 0}; }
static dvs_ref_t r1(uint8_t *a) { return (dvs_ref_t){a, 1, 0, DVS_REF_SCALAR, 0}; }
static dvs_slot_t slot(uint32_t state, dvs_part_t *child) {
    return (dvs_slot_t){state, 0, child, NULL};
}

static const uint32_t S0[] = {0}, S1[] = {1}, S00[] = {0, 0};

/* A leaf: v < `lim`, v its export 0. */
static dvs_handle_t *leaf(int lim) {
    begin(0);
    var8(0, DVS_VAR_RAND);
    dvs_builder_add_export(B, 0);
    hard(bin(DVS_BIN_LT, var(0), c8(lim)));
    return finish();
}

/* A tree: root.r == mid.m + mid.l.v + l2.v, mid.m == mid.l.v + 1, each
 * leaf v < 10; the imports through one and two slots. */
static void check_tree(void) {
    dvs_handle_t *L = leaf(10);
    begin(1);   /* mid: m (export 0), i = slot 0's v */
    var8(0, DVS_VAR_RAND);
    dvs_builder_add_export(B, 0);
    import8(1, 1, S0, 0);
    hard(bin(DVS_BIN_EQ, var(0), bin(DVS_BIN_ADD, var(1), c8(1))));
    dvs_handle_t *M = finish();
    begin(2);   /* root: r, x = slot 0's m, y = slot 0's slot 0's v, z = slot 1's v */
    var8(0, DVS_VAR_RAND);
    import8(1, 1, S0, 0);
    import8(2, 2, S00, 0);
    import8(3, 1, S1, 0);
    hard(bin(DVS_BIN_EQ, var(0), bin(DVS_BIN_ADD, var(1), bin(DVS_BIN_ADD, var(2), var(3)))));
    dvs_handle_t *R = finish();
    CHECK(L && M && R, "tree: handles");
    if (!L || !M || !R) return;
    CHECK(dvs_handle_n_slots(R) == 2 && dvs_handle_n_slots(L) == 0, "tree: slot counts");

    uint8_t l1v = 0, l2v = 0, mm = 0, rr = 0;
    dvs_ref_t l1refs[] = {r8(&l1v)}, l2refs[] = {r8(&l2v)}, mrefs[] = {r8(&mm), {0}};
    dvs_ref_t rrefs[] = {r8(&rr), {0}, {0}, {0}};
    dvs_part_t l1 = {L, l1refs, NULL, NULL}, l2 = {L, l2refs, NULL, NULL};
    dvs_slot_t mslots[] = {slot(DVS_SLOT_ACTIVE, &l1)};
    dvs_part_t mid = {M, mrefs, NULL, mslots};
    dvs_slot_t rslots[] = {slot(DVS_SLOT_ACTIVE, &mid), slot(DVS_SLOT_ACTIVE, &l2)};
    dvs_part_t root = {R, rrefs, NULL, rslots};
    int ok = 1, seen[10] = {0}, distinct = 0;
    for (uint64_t s = 1; s <= 40; s++) {
        if (dvs_randomize_parts(&root, s) != DVS_RANDOMIZE_OK) { ok = 0; break; }
        if (l1v >= 10 || l2v >= 10 || mm != l1v + 1 || rr != mm + l1v + l2v) ok = 0;
        distinct += !seen[l1v]++;
    }
    CHECK(ok, "tree: every object's constraints, imports through one and two slots");
    CHECK(distinct > 3, "tree: the leaves are randomized");
    CHECK(dvs_parts_n_assembled(R) == 1, "tree: one shape, assembled once");
    CHECK(dvs_randomize(R, rrefs, NULL, 1) == DVS_RANDOMIZE_ERROR,
          "tree: dvs_randomize refuses a part with slots");
    dvs_handle_free(R);
    dvs_handle_free(M);
    dvs_handle_free(L);
}

/* A cycle a -> b -> a: a.a + b.a == 10 (each at most 10), and the node
 * with `lt` set is the smaller. One object through two slots is one
 * object: x != y fails. */
static void check_cycle(void) {
    begin(1);   /* node: a (export 0), nb = the next node's a, lt STATE */
    var8(0, DVS_VAR_RAND);
    dvs_builder_add_export(B, 0);
    import8(1, 1, S0, 0);
    dvs_builder_add_var(B, 2, 1, 0, 0, 1);
    dvs_builder_set_var_kind(B, 2, DVS_VAR_STATE);
    hard(bin(DVS_BIN_LTE, var(0), c8(10)));
    hard(bin(DVS_BIN_EQ, bin(DVS_BIN_ADD, var(0), var(1)), c8(10)));
    hard(bin(DVS_BIN_OR, dvs_builder_expr_unary(B, DVS_UN_NOT, var(2)),
             bin(DVS_BIN_LT, var(0), var(1))));
    dvs_handle_t *N = finish();
    CHECK(N != NULL, "cycle: handle");
    if (!N) return;
    uint8_t aa = 0, ba = 0, alt = 1, blt = 0;
    dvs_ref_t arefs[] = {r8(&aa), {0}, r1(&alt)}, brefs[] = {r8(&ba), {0}, r1(&blt)};
    dvs_part_t a, b;
    dvs_slot_t aslots[] = {slot(DVS_SLOT_ACTIVE, &b)}, bslots[] = {slot(DVS_SLOT_ACTIVE, &a)};
    a = (dvs_part_t){N, arefs, NULL, aslots};
    b = (dvs_part_t){N, brefs, NULL, bslots};
    int ok = 1;
    for (uint64_t s = 1; s <= 20 && ok; s++)
        ok = dvs_randomize_parts(&a, s) == DVS_RANDOMIZE_OK && aa + ba == 10 && aa < ba;
    CHECK(ok, "cycle: solved once each, both nodes' constraints");

    dvs_handle_t *L = leaf(10);
    begin(2);   /* x = slot 0's v, y = slot 1's v, x != y */
    import8(0, 1, S0, 0);
    import8(1, 1, S1, 0);
    hard(bin(DVS_BIN_NEQ, var(0), var(1)));
    dvs_handle_t *P = finish();
    uint8_t v1 = 0, v2 = 0;
    dvs_ref_t l1refs[] = {r8(&v1)}, l2refs[] = {r8(&v2)}, prefs[2] = {{0}};
    dvs_part_t l1 = {L, l1refs, NULL, NULL}, l2 = {L, l2refs, NULL, NULL};
    dvs_slot_t same[] = {slot(DVS_SLOT_ACTIVE, &l1), slot(DVS_SLOT_ACTIVE, &l1)};
    dvs_slot_t two[] = {slot(DVS_SLOT_ACTIVE, &l1), slot(DVS_SLOT_ACTIVE, &l2)};
    dvs_part_t p = {P, prefs, NULL, same};
    CHECK(dvs_randomize_parts(&p, 3) == DVS_RANDOMIZE_UNSAT,
          "alias: one object through two slots is one variable");
    p.slots = two;
    CHECK(dvs_randomize_parts(&p, 3) == DVS_RANDOMIZE_OK && v1 != v2 && v1 < 10 && v2 < 10,
          "alias: two objects are two variables");
    CHECK(dvs_parts_n_assembled(P) == 2, "alias: the two shapes differ");
    dvs_handle_free(P);
    dvs_handle_free(L);
    dvs_handle_free(N);
}

/* r < 50, and `form` (0: g ? r == i + 1 : 1; 1: !g || r == i + 1;
 * 2: r == i + 1, unguarded), i the v of slot 0, g a STATE bit. */
static dvs_handle_t *guarded(int form) {
    begin(1);
    var8(0, DVS_VAR_RAND);
    import8(1, 1, S0, 0);
    dvs_builder_add_var(B, 2, 1, 0, 0, 1);
    dvs_builder_set_var_kind(B, 2, DVS_VAR_STATE);
    hard(bin(DVS_BIN_LT, var(0), c8(50)));
    dvs_expr_t eq = bin(DVS_BIN_EQ, var(0), bin(DVS_BIN_ADD, var(1), c8(1)));
    hard(form == 0 ? dvs_builder_expr_ite(B, var(2), eq, c1(1))
         : form == 1 ? bin(DVS_BIN_OR, dvs_builder_expr_unary(B, DVS_UN_NOT, var(2)), eq)
                     : eq);
    return finish();
}

static void check_null_inactive(void) {
    dvs_handle_t *G = guarded(0), *G2 = guarded(1), *U = guarded(2), *L = leaf(5);
    CHECK(G && G2 && U && L, "null: handles");
    if (!G || !G2 || !U || !L) return;
    uint8_t r = 99, g = 0, v = 7;
    dvs_ref_t refs[] = {r8(&r), {0}, r1(&g)}, lrefs[] = {r8(&v)};
    dvs_part_t l = {L, lrefs, NULL, NULL};
    dvs_slot_t sl[] = {slot(DVS_SLOT_NULL, NULL)};
    dvs_part_t root = {G, refs, NULL, sl};
    CHECK(dvs_randomize_parts(&root, 1) == DVS_RANDOMIZE_OK && r < 50,
          "null: a read under a false guard (if) is no read");
    root.handle = G2;
    r = 99;
    CHECK(dvs_randomize_parts(&root, 1) == DVS_RANDOMIZE_OK && r < 50,
          "null: a read under a false guard (||) is no read");
    root.handle = U;
    r = 99;
    CHECK(dvs_randomize_parts(&root, 1) == DVS_RANDOMIZE_NULL && r == 99,
          "null: an unguarded read fails the call, nothing written");
    root.handle = G;
    g = 1;
    CHECK(dvs_randomize_parts(&root, 1) == DVS_RANDOMIZE_NULL,
          "null: a read under a true guard fails the call");

    sl[0] = slot(DVS_SLOT_ACTIVE, &l);
    CHECK(dvs_randomize_parts(&root, 2) == DVS_RANDOMIZE_OK && v < 5 && r == v + 1,
          "active: the object's constraints, its variable solved");
    v = 7;
    sl[0] = slot(DVS_SLOT_INACTIVE, &l);
    CHECK(dvs_randomize_parts(&root, 2) == DVS_RANDOMIZE_OK && v == 7 && r == 8,
          "inactive: the export read, the object's constraints not added");
    CHECK(dvs_parts_n_assembled(G) == 3, "null, active, inactive: three shapes");
    dvs_handle_free(G);
    dvs_handle_free(G2);
    dvs_handle_free(U);
    dvs_handle_free(L);
}

/* Soft priorities: root R (slots A, B) soft w == 4 on L.v; B soft i == 2;
 * L soft v == 3; A softs i == 1 (priority 5) and i == 7 (priority 9);
 * A and B both hold the same L. */
static void check_soft(void) {
    dvs_handle_t *L[2], *Bh[2], *R[2], *A;
    for (int on = 0; on < 2; on++) {
        begin(0);
        var8(0, DVS_VAR_RAND);
        dvs_builder_add_export(B, 0);
        hard(bin(DVS_BIN_LT, var(0), c8(10)));
        if (on) soft(bin(DVS_BIN_EQ, var(0), c8(3)), 0);
        L[on] = finish();
        begin(1);
        import8(0, 1, S0, 0);
        if (on) soft(bin(DVS_BIN_EQ, var(0), c8(2)), 0);
        Bh[on] = finish();
        begin(2);
        import8(0, 2, S00, 0);
        if (on) soft(bin(DVS_BIN_EQ, var(0), c8(4)), 7);
        R[on] = finish();
    }
    begin(1);
    import8(0, 1, S0, 0);
    soft(bin(DVS_BIN_EQ, var(0), c8(7)), 9);
    soft(bin(DVS_BIN_EQ, var(0), c8(1)), 5);
    A = finish();
    uint8_t v = 0;
    dvs_ref_t lrefs[] = {r8(&v)}, none[1] = {{0}};
    dvs_part_t l = {NULL, lrefs, NULL, NULL};
    dvs_slot_t to_l[] = {slot(DVS_SLOT_ACTIVE, &l)};
    dvs_part_t a = {A, none, NULL, to_l}, b = {NULL, none, NULL, to_l};
    dvs_slot_t rs[] = {slot(DVS_SLOT_ACTIVE, &a), slot(DVS_SLOT_ACTIVE, &b)};
    dvs_part_t root = {NULL, none, NULL, rs};
    static const struct { int r, b, l, want; const char *what; } cases[] = {
        {1, 1, 1, 4, "soft: the container's soft ranks above the contained objects'"},
        {0, 1, 1, 2, "soft: a later handle ranks above an earlier one"},
        {0, 0, 1, 3, "soft: an object reached twice takes its higher rank"},
        {0, 0, 0, 1, "soft: within a part, its own priorities"},
    };
    for (int c = 0; c < 4; c++) {
        root.handle = R[cases[c].r];
        b.handle = Bh[cases[c].b];
        l.handle = L[cases[c].l];
        int ok = 1;
        for (uint64_t s = 1; s <= 5 && ok; s++)
            ok = dvs_randomize_parts(&root, s) == DVS_RANDOMIZE_OK && v == cases[c].want;
        CHECK(ok, cases[c].what);
    }
    for (int on = 0; on < 2; on++) {
        dvs_handle_free(L[on]);
        dvs_handle_free(Bh[on]);
        dvs_handle_free(R[on]);
    }
    dvs_handle_free(A);
}

/* A derived class's part in a slot of its base class: the derived layout
 * extends the base's (export 0 is p in both, though p is var 1 of the
 * derived part), and its own constraints apply. */
static void check_derived(void) {
    dvs_handle_t *Base = leaf(100);
    begin(0);   /* derived: q (var 0), p (var 1); exports p, q; p > 50, q == p + 1 */
    var8(0, DVS_VAR_RAND);
    var8(1, DVS_VAR_RAND);
    dvs_builder_add_export(B, 1);
    dvs_builder_add_export(B, 0);
    hard(bin(DVS_BIN_LT, var(1), c8(100)));
    hard(bin(DVS_BIN_GT, var(1), c8(50)));
    hard(bin(DVS_BIN_EQ, var(0), bin(DVS_BIN_ADD, var(1), c8(1))));
    dvs_handle_t *Der = finish();
    begin(1);   /* r == slot 0's export 0 */
    var8(0, DVS_VAR_RAND);
    import8(1, 1, S0, 0);
    hard(bin(DVS_BIN_EQ, var(0), var(1)));
    dvs_handle_t *R = finish();
    CHECK(Base && Der && R, "derived: handles");
    if (!Base || !Der || !R) return;
    uint8_t p = 0, q = 0, r = 0;
    dvs_ref_t drefs[] = {r8(&q), r8(&p)}, brefs[] = {r8(&p)}, rrefs[] = {r8(&r), {0}};
    dvs_part_t d = {Der, drefs, NULL, NULL}, base = {Base, brefs, NULL, NULL};
    dvs_slot_t sl[] = {slot(DVS_SLOT_ACTIVE, &d)};
    dvs_part_t root = {R, rrefs, NULL, sl};
    int ok = 1;
    for (uint64_t s = 1; s <= 20 && ok; s++)
        ok = dvs_randomize_parts(&root, s) == DVS_RANDOMIZE_OK && p > 50 && p < 100
             && q == p + 1 && r == p;
    CHECK(ok, "derived: the object's own constraints, its export 0 imported");
    sl[0].child = &base;
    int low = 0;
    ok = 1;
    for (uint64_t s = 1; s <= 40 && ok; s++) {
        ok = dvs_randomize_parts(&root, s) == DVS_RANDOMIZE_OK && p < 100 && r == p;
        low |= p <= 50;
    }
    CHECK(ok && low, "derived: a base object in the same slot has the base's constraints");
    dvs_handle_free(R);
    dvs_handle_free(Der);
    dvs_handle_free(Base);
}

/* An array in a part: L has a of random size == n, foreach a[i] < n; the
 * root sets n == r + 2, r < 3. Sizes first, then elements. */
static void check_arrays(void) {
    begin(0);
    var8(0, DVS_VAR_RAND);
    dvs_builder_add_export(B, 0);
    dvs_builder_add_array(B, 0, 8, 0, DVS_VAR_RAND);
    dvs_builder_set_array_size_random(B, 0);
    hard(bin(DVS_BIN_EQ, dvs_builder_expr_array_size(B, 0),
             dvs_builder_expr_sv_cast(B, var(0), 8, 32, 0, 1)));
    dvs_builder_foreach_begin(B, 0, 0);
    hard(bin(DVS_BIN_LT, dvs_builder_expr_elem(B, 0, dvs_builder_expr_loop_index(B, 0)),
             var(0)));
    dvs_builder_foreach_end(B);
    dvs_handle_t *L = finish();
    begin(1);
    var8(0, DVS_VAR_RAND);
    import8(1, 1, S0, 0);
    hard(bin(DVS_BIN_LT, var(0), c8(3)));
    hard(bin(DVS_BIN_EQ, var(1), bin(DVS_BIN_ADD, var(0), c8(2))));
    dvs_handle_t *R = finish();
    CHECK(L && R, "arrays: handles");
    if (!L || !R) return;
    uint8_t n = 0, r = 0, data[8];
    dvs_ref_t lrefs[] = {r8(&n)}, rrefs[] = {r8(&r), {0}};
    dvs_array_ref_t arr[] = {{NULL, 0, 0}};
    dvs_part_t l = {L, lrefs, arr, NULL};
    dvs_slot_t sl[] = {slot(DVS_SLOT_ACTIVE, &l)};
    dvs_part_t root = {R, rrefs, NULL, sl};
    int ok = 1;
    for (uint64_t s = 1; s <= 10 && ok; s++) {
        arr[0].data = NULL;
        arr[0].n_elems = 0;
        ok = dvs_randomize_parts_sizes(&root, s) == DVS_RANDOMIZE_OK && arr[0].n_elems >= 2
             && arr[0].n_elems <= 4;
        arr[0].data = data;
        ok = ok && dvs_randomize_parts(&root, s) == DVS_RANDOMIZE_OK
             && n == arr[0].n_elems && r + 2 == n;
        for (uint32_t k = 0; ok && k < arr[0].n_elems; k++) ok = data[k] < n;
    }
    CHECK(ok, "arrays: a part's random size and elements, with an import");
    dvs_handle_free(R);
    dvs_handle_free(L);
}

/* A part with no slots solves as dvs_randomize_arrays(); the image refuses
 * an import through a slot the part does not have. */
static void check_plain_and_image(void) {
    dvs_handle_t *L = leaf(10);
    uint8_t v = 99;
    dvs_ref_t refs[] = {r8(&v)};
    dvs_part_t l = {L, refs, NULL, NULL};
    CHECK(dvs_randomize_parts(&l, 1) == DVS_RANDOMIZE_OK && v < 10
              && dvs_parts_n_assembled(L) == 0,
          "plain: a part with no slots is solved without assembly");
    dvs_handle_free(L);

    begin(1);
    import8(0, 1, S1, 0);
    CHECK(finish() == NULL, "image: an import through slot 1 of a part with one slot");
    begin(1);
    import8(0, 1, S0, 0);
    CHECK(dvs_builder_add_export(B, 0) == -1, "builder: an import is not exported");
    dvs_builder_destroy(B);
}

/* ------------------------------------------------------------------ */
/* Slot arrays (dynamic arrays and queues of handles)                  */
/* ------------------------------------------------------------------ */

static const uint32_t PL[] = {1, DVS_PATH_LOOP(0)};

static dvs_expr_t c32(int64_t v) { return dvs_builder_expr_const_sized(B, v, 1, 32); }
static dvs_slot_t slots_of(uint32_t n, const dvs_slot_t *elems) {
    return (dvs_slot_t){0, n, NULL, elems};
}

/* A holder: slot 0 a handle s, slot 1 a slot array `items` (array 0);
 * x == s.v, and foreach items[i]: items[i].v == i + 1, under `items[i] !=
 * null` if `guard`; its size random and == `size` if size >= 0. */
static dvs_handle_t *holder(int guard, int size) {
    begin(2);
    dvs_builder_add_slot_array(B, 0, 1);
    import8(0, 2, PL, 0);
    var8(1, DVS_VAR_RAND);
    import8(2, 1, S0, 0);
    hard(bin(DVS_BIN_EQ, var(1), var(2)));
    if (size >= 0) {
        dvs_builder_set_array_size_random(B, 0);
        hard(bin(DVS_BIN_EQ, dvs_builder_expr_array_size(B, 0), c32(size)));
    }
    dvs_builder_foreach_begin(B, 0, 0);
    dvs_expr_t li = dvs_builder_expr_loop_index(B, 0);
    dvs_expr_t eq = bin(DVS_BIN_EQ, var(0),
                        bin(DVS_BIN_ADD, dvs_builder_expr_extract(B, li, 7, 0), c8(1)));
    hard(guard ? bin(DVS_BIN_OR,
                     dvs_builder_expr_unary(B, DVS_UN_NOT, dvs_builder_expr_elem(B, 0, li)), eq)
               : eq);
    dvs_builder_foreach_end(B);
    return finish();
}

/* A leaf whose v has a soft v == `want` */
static dvs_handle_t *soft_leaf(int want) {
    begin(0);
    var8(0, DVS_VAR_RAND);
    dvs_builder_add_export(B, 0);
    hard(bin(DVS_BIN_LT, var(0), c8(10)));
    soft(bin(DVS_BIN_EQ, var(0), c8(want)), 0);
    return finish();
}

static void check_slot_arrays(void) {
    dvs_handle_t *L = leaf(100), *U = holder(0, -1), *G = holder(1, -1);
    begin(0);   /* derived: q (var 0), p (var 1); exports p, q; q == p + 1 */
    var8(0, DVS_VAR_RAND);
    var8(1, DVS_VAR_RAND);
    dvs_builder_add_export(B, 1);
    dvs_builder_add_export(B, 0);
    hard(bin(DVS_BIN_LT, var(1), c8(100)));
    hard(bin(DVS_BIN_EQ, var(0), bin(DVS_BIN_ADD, var(1), c8(1))));
    dvs_handle_t *Der = finish();
    CHECK(L && U && G && Der, "slot arrays: handles");
    if (!L || !U || !G || !Der) return;
    uint8_t v[4] = {0}, x = 0, q = 0, p = 0;
    dvs_ref_t lrefs[4][1], hrefs[] = {{0}, r8(&x), {0}}, drefs[] = {r8(&q), r8(&p)};
    dvs_part_t l[4], d = {Der, drefs, NULL, NULL};
    for (int k = 0; k < 4; k++) {
        lrefs[k][0] = r8(&v[k]);
        l[k] = (dvs_part_t){L, lrefs[k], NULL, NULL};
    }
    dvs_slot_t el[4] = {slot(DVS_SLOT_ACTIVE, &l[0]), slot(DVS_SLOT_ACTIVE, &l[1]),
                        slot(DVS_SLOT_ACTIVE, &l[2])};
    dvs_array_ref_t arr[] = {{NULL, 0, 0}};
    dvs_slot_t sl[] = {slot(DVS_SLOT_ACTIVE, &l[3]), slots_of(0, el)};
    dvs_part_t root = {U, hrefs, arr, sl};

    CHECK(dvs_randomize_parts(&root, 1) == DVS_RANDOMIZE_OK && x == v[3],
          "slot arrays: an empty array");
    arr[0].n_elems = sl[1].n_elems = 3;
    CHECK(dvs_randomize_parts(&root, 2) == DVS_RANDOMIZE_OK && v[0] == 1 && v[1] == 2
              && v[2] == 3 && x == v[3],
          "slot arrays: each element's import bound to its object");
    CHECK(dvs_randomize_parts(&root, 3) == DVS_RANDOMIZE_OK && dvs_parts_n_assembled(U) == 2,
          "slot arrays: the same size, the same shape");
    arr[0].n_elems = sl[1].n_elems = 2;
    v[2] = 77;
    CHECK(dvs_randomize_parts(&root, 4) == DVS_RANDOMIZE_OK && dvs_parts_n_assembled(U) == 3
              && v[2] == 77,
          "slot arrays: a new size, a new shape");
    sl[0] = slot(DVS_SLOT_ACTIVE, &l[1]);
    CHECK(dvs_randomize_parts(&root, 5) == DVS_RANDOMIZE_OK && x == 2 && v[1] == 2,
          "slot arrays: an element also in a scalar slot is one object");
    sl[0] = slot(DVS_SLOT_ACTIVE, &l[3]);
    el[0] = slot(DVS_SLOT_ACTIVE, &d);
    CHECK(dvs_randomize_parts(&root, 6) == DVS_RANDOMIZE_OK && p == 1 && q == 2,
          "slot arrays: a derived part in an element, its export and constraints");

    /* Null elements: a null handle, then one past the handles held */
    el[0] = slot(DVS_SLOT_ACTIVE, &l[0]);
    el[1] = slot(DVS_SLOT_NULL, NULL);
    arr[0].n_elems = sl[1].n_elems = 3;
    v[1] = 55;
    CHECK(dvs_randomize_parts(&root, 7) == DVS_RANDOMIZE_NULL && v[1] == 55,
          "slot arrays: an unguarded read through a null element fails");
    root.handle = G;
    CHECK(dvs_randomize_parts(&root, 7) == DVS_RANDOMIZE_OK && v[0] == 1 && v[2] == 3
              && v[1] == 55,
          "slot arrays: a read through a null element under its guard is no read");
    el[1] = slot(DVS_SLOT_ACTIVE, &l[1]);
    arr[0].n_elems = 4;
    CHECK(dvs_randomize_parts(&root, 8) == DVS_RANDOMIZE_OK && v[1] == 2,
          "slot arrays: past the handles held, the elements are null (guarded)");
    root.handle = U;
    CHECK(dvs_randomize_parts(&root, 8) == DVS_RANDOMIZE_NULL,
          "slot arrays: past the handles held, the elements are null (unguarded)");

    /* Random sizes: grown (new elements null), shrunk */
    dvs_handle_t *GS = holder(1, 3), *US = holder(0, 3), *S2 = holder(0, 2);
    arr[0].n_elems = sl[1].n_elems = 1;
    root.handle = GS;
    int ok = dvs_randomize_parts_sizes(&root, 9) == DVS_RANDOMIZE_OK && arr[0].n_elems == 3;
    CHECK(ok && dvs_randomize_parts(&root, 9) == DVS_RANDOMIZE_OK && v[0] == 1,
          "slot arrays: a random size grown, the new elements null, guarded");
    arr[0].n_elems = 1;
    root.handle = US;
    ok = dvs_randomize_parts_sizes(&root, 9) == DVS_RANDOMIZE_OK && arr[0].n_elems == 3;
    CHECK(ok && dvs_randomize_parts(&root, 9) == DVS_RANDOMIZE_NULL,
          "slot arrays: a random size grown, the new elements read unguarded");
    arr[0].n_elems = sl[1].n_elems = 3;
    el[2] = slot(DVS_SLOT_NULL, NULL);
    root.handle = S2;
    v[2] = 66;
    ok = dvs_randomize_parts_sizes(&root, 10) == DVS_RANDOMIZE_OK && arr[0].n_elems == 2;
    CHECK(ok && dvs_randomize_parts(&root, 10) == DVS_RANDOMIZE_OK && v[0] == 1 && v[1] == 2
              && v[2] == 66,
          "slot arrays: a random size shrunk, a null element dropped, not read");
    dvs_handle_t *S2b = holder(0, 2);
    v[0] = v[1] = 0;
    sl[1].n_elems = 2;
    root.handle = S2b;
    ok = dvs_randomize_parts_sizes(&root, 11) == DVS_RANDOMIZE_OK && arr[0].n_elems == 2;
    CHECK(ok && dvs_randomize_parts(&root, 11) == DVS_RANDOMIZE_OK && v[0] == 1 && v[1] == 2,
          "slot arrays: a size unchanged, the size phase's shape is not the elements'");
    dvs_handle_free(S2b);
    arr[0].n_elems = 70000;
    root.handle = U;
    CHECK(dvs_randomize_parts(&root, 10) == DVS_RANDOMIZE_TOO_LARGE,
          "slot arrays: more than 65536 elements walked");
    dvs_handle_free(GS);
    dvs_handle_free(US);
    dvs_handle_free(S2);

    /* Soft rank: items[0].v == items[1].v, each element's soft differs */
    static const uint32_t E0[] = {0, 0}, E1[] = {0, 1};
    begin(1);
    dvs_builder_add_slot_array(B, 0, 0);
    import8(0, 2, E0, 0);
    import8(1, 2, E1, 0);
    hard(bin(DVS_BIN_EQ, var(0), var(1)));
    dvs_handle_t *R = finish(), *L3 = soft_leaf(3), *L4 = soft_leaf(4);
    dvs_part_t a3 = {L3, lrefs[0], NULL, NULL}, a4 = {L4, lrefs[1], NULL, NULL};
    dvs_slot_t se[2] = {slot(DVS_SLOT_ACTIVE, &a3), slot(DVS_SLOT_ACTIVE, &a4)};
    dvs_array_ref_t sarr[] = {{NULL, 2, 0}};
    dvs_slot_t ss[] = {slots_of(2, se)};
    dvs_ref_t none[1] = {{0}};
    dvs_part_t sroot = {R, none, sarr, ss};
    ok = dvs_randomize_parts(&sroot, 11) == DVS_RANDOMIZE_OK && v[0] == 4 && v[1] == 4;
    se[0] = slot(DVS_SLOT_ACTIVE, &a4);
    se[1] = slot(DVS_SLOT_ACTIVE, &a3);
    CHECK(ok && dvs_randomize_parts(&sroot, 11) == DVS_RANDOMIZE_OK && v[0] == 3 && v[1] == 3,
          "slot arrays: a later element's softs rank above an earlier one's");
    dvs_handle_free(R);
    dvs_handle_free(L3);
    dvs_handle_free(L4);

    begin(1);
    dvs_builder_add_slot_array(B, 0, 1);
    CHECK(finish() == NULL, "image: a slot array on a slot the part does not have");
    dvs_handle_free(U);
    dvs_handle_free(G);
    dvs_handle_free(Der);
    dvs_handle_free(L);
}

int main(void) {
    check_tree();
    check_cycle();
    check_null_inactive();
    check_soft();
    check_derived();
    check_arrays();
    check_plain_and_image();
    check_slot_arrays();
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}
