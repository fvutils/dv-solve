#include "zsp_aig_cnf.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------- internal types ----------------------------- */

struct zsp_aig_cnf_s {
    zsp_alloc_t *alloc;
    zsp_aig_t   *aig;
    zsp_sat_t   *sat;

    /* Bitmap: encoded[i] = was AIG node id (i+1) emitted? */
    uint8_t  *encoded;
    uint32_t  encoded_cap;

    /* Generic int32 stack used for visit + work-list traversals. */
    int32_t  *stack;
    uint32_t  stack_size;
    uint32_t  stack_cap;

    /* Visit-mark bitmap used during top-level flattening. */
    uint8_t  *visited;
    uint32_t  visited_cap;

    uint64_t num_vars;
    uint64_t num_clauses;
    uint64_t num_literals;
};

/* ----------------------------- helpers ------------------------------------ */

static void *xalloc(zsp_alloc_t *a, size_t sz) {
    return a ? ZSP_ALLOC(a, sz) : malloc(sz);
}
static void xfree(zsp_alloc_t *a, void *p, size_t sz) {
    if (a) ZSP_RELEASE(a, p, sz); else free(p);
}
static void *xrealloc(zsp_alloc_t *a, void *p, size_t old_sz, size_t new_sz) {
    void *q = xalloc(a, new_sz);
    if (!q) return NULL;
    if (p && old_sz) memcpy(q, p, old_sz < new_sz ? old_sz : new_sz);
    if (p) xfree(a, p, old_sz);
    return q;
}

static void ensure_bitmap(zsp_aig_cnf_t *e, uint8_t **buf, uint32_t *cap,
                          uint32_t need_bits) {
    uint32_t need = (need_bits + 7) / 8;
    if (need <= *cap) return;
    uint32_t nc = *cap ? *cap : 64;
    while (nc < need) nc *= 2;
    uint8_t *nb = (uint8_t *)xrealloc(e->alloc, *buf, *cap, nc);
    if (nb && nc > *cap) memset(nb + *cap, 0, nc - *cap);
    *buf = nb;
    *cap = nc;
}

static int bm_get(const uint8_t *bm, uint32_t bit) {
    return (bm[bit >> 3] >> (bit & 7)) & 1;
}
static void bm_set(uint8_t *bm, uint32_t bit) {
    bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
}

static void ensure_stack(zsp_aig_cnf_t *e, uint32_t need) {
    if (need <= e->stack_cap) return;
    uint32_t nc = e->stack_cap ? e->stack_cap : 64;
    while (nc < need) nc *= 2;
    int32_t *ns = (int32_t *)xrealloc(e->alloc, e->stack,
                                       (size_t)e->stack_cap * sizeof(int32_t),
                                       (size_t)nc * sizeof(int32_t));
    e->stack = ns;
    e->stack_cap = nc;
}

static void stack_push(zsp_aig_cnf_t *e, int32_t v) {
    ensure_stack(e, e->stack_size + 1);
    e->stack[e->stack_size++] = v;
}

static int is_node_encoded(const zsp_aig_cnf_t *e, zsp_aig_node_t n) {
    uint32_t i = (uint32_t)(n < 0 ? -n : n);
    if (i == 0) return 1;
    if (((i - 1) >> 3) >= e->encoded_cap) return 0;
    return bm_get(e->encoded, i - 1);
}

static void mark_encoded(zsp_aig_cnf_t *e, zsp_aig_node_t n) {
    uint32_t i = (uint32_t)(n < 0 ? -n : n);
    bm_set(e->encoded, i - 1);
    e->num_vars++;
}

/* ----------------------------- ITE detection ------------------------------ */

/* Returns 1 if `cur` (an AND node) is the AIG encoding of an ITE *and*
 * extracting it would not destroy sharing. Outputs c, ~a, ~b ids (children
 * may be negated literals — these are the ids as found in the AIG, including
 * sign). */
static int detect_ite(zsp_aig_t *aig, zsp_aig_node_t cur,
                      zsp_aig_node_t *c_out,
                      zsp_aig_node_t *not_a_out,
                      zsp_aig_node_t *not_b_out) {
    if (cur < 0 || !zsp_aig_is_and(aig, cur)) return 0;
    zsp_aig_node_t l, r;
    zsp_aig_get_children(aig, cur, &l, &r);
    /* both must be negated AND nodes with parents == 1 */
    if (l >= 0 || r >= 0) return 0;
    if (!zsp_aig_is_and(aig, l) || !zsp_aig_is_and(aig, r)) return 0;
    if (zsp_aig_parents(aig, l) > 1 || zsp_aig_parents(aig, r) > 1) return 0;
    zsp_aig_node_t ll, lr, rl, rr;
    zsp_aig_get_children(aig, l, &ll, &lr);
    zsp_aig_get_children(aig, r, &rl, &rr);
    /* matches bitwuzla's four commutative cases */
    if (-lr == rl) { *c_out = rl; *not_a_out = rr; *not_b_out = ll; return 1; }
    if (-ll == rl) { *c_out = rl; *not_a_out = rr; *not_b_out = lr; return 1; }
    if (-lr == rr) { *c_out = rr; *not_a_out = rl; *not_b_out = ll; return 1; }
    if (-ll == rr) { *c_out = rr; *not_a_out = rl; *not_b_out = lr; return 1; }
    return 0;
}

/* ----------------------------- core encoder ------------------------------- */

/* Encode a single AND node `cur` (positive id) as Tseitin clauses.
 * `cur` is the SAT variable id; left/right are its child literals. */
static void emit_and_clauses(zsp_aig_cnf_t *e, int32_t x,
                             int32_t a, int32_t b) {
    /*   x <-> a /\ b
     *  (¬x ∨ a)  (¬x ∨ b)  (x ∨ ¬a ∨ ¬b) */
    zsp_sat_add_binary(e->sat, -x, a);
    zsp_sat_add_binary(e->sat, -x, b);
    zsp_sat_add_ternary(e->sat, x, -a, -b);
    e->num_clauses += 3;
    e->num_literals += 7;
}

static void emit_ite_clauses(zsp_aig_cnf_t *e, int32_t x,
                             int32_t c, int32_t a, int32_t b) {
    /* x <-> ite(c, a, b)
     *  (¬x ∨ ¬c ∨ a)  (¬x ∨ c ∨ b)  (x ∨ ¬c ∨ ¬a)  (x ∨ c ∨ ¬b) */
    zsp_sat_add_ternary(e->sat, -x, -c, a);
    zsp_sat_add_ternary(e->sat, -x,  c, b);
    zsp_sat_add_ternary(e->sat,  x, -c, -a);
    zsp_sat_add_ternary(e->sat,  x,  c, -b);
    e->num_clauses += 4;
    e->num_literals += 12;
}

static void encode_subtree(zsp_aig_cnf_t *e, zsp_aig_node_t root) {
    /* DFS using e->stack. Two-phase: push positive ids for "enter", negative
     * for "leave" — we reuse the sign as a phase tag here since the AIG node
     * ids we're traversing already have their natural sign. Switch to a
     * separate phase encoding: pack into the stack as 2*idx for enter and
     * 2*idx+1 for leave. */
    uint32_t saved = e->stack_size;

    stack_push(e, (int32_t)root);

    while (e->stack_size > saved) {
        zsp_aig_node_t cur = e->stack[e->stack_size - 1];
        ensure_bitmap(e, &e->encoded, &e->encoded_cap,
                      (uint32_t)(cur < 0 ? -cur : cur));
        if (is_node_encoded(e, cur)) {
            e->stack_size--;
            continue;
        }

        if (cur == ZSP_AIG_TRUE || cur == ZSP_AIG_FALSE) {
            e->stack_size--;
            mark_encoded(e, cur);
            /* Encode TRUE as the unit clause {1}. (FALSE never appears as a
             * pristine node here — it's id -1, which shares variable 1.) */
            zsp_sat_add_unit(e->sat, ZSP_AIG_TRUE);
            e->num_clauses++;
            e->num_literals++;
            continue;
        }

        if (zsp_aig_is_input(e->aig, cur)) {
            e->stack_size--;
            mark_encoded(e, cur);
            continue;
        }

        assert(zsp_aig_is_and(e->aig, cur));

        zsp_aig_node_t c_id, not_a, not_b;
        int is_ite = detect_ite(e->aig, cur, &c_id, &not_a, &not_b);

        /* Are children already processed? We detect this by checking if both
         * children are encoded. If yes, emit the gate. Otherwise push the
         * children and revisit. */
        zsp_aig_node_t l, r;
        zsp_aig_get_children(e->aig, cur, &l, &r);

        int children_done;
        if (is_ite) {
            children_done = is_node_encoded(e, c_id)
                         && is_node_encoded(e, not_a)
                         && is_node_encoded(e, not_b);
        } else {
            children_done = is_node_encoded(e, l) && is_node_encoded(e, r);
        }

        if (!children_done) {
            if (is_ite) {
                ensure_bitmap(e, &e->encoded, &e->encoded_cap,
                              (uint32_t)(c_id < 0 ? -c_id : c_id));
                ensure_bitmap(e, &e->encoded, &e->encoded_cap,
                              (uint32_t)(not_a < 0 ? -not_a : not_a));
                ensure_bitmap(e, &e->encoded, &e->encoded_cap,
                              (uint32_t)(not_b < 0 ? -not_b : not_b));
                stack_push(e, (int32_t)c_id);
                stack_push(e, (int32_t)not_a);
                stack_push(e, (int32_t)not_b);
            } else {
                ensure_bitmap(e, &e->encoded, &e->encoded_cap,
                              (uint32_t)(l < 0 ? -l : l));
                ensure_bitmap(e, &e->encoded, &e->encoded_cap,
                              (uint32_t)(r < 0 ? -r : r));
                stack_push(e, (int32_t)l);
                stack_push(e, (int32_t)r);
            }
            continue;
        }

        /* All children done — emit clauses and mark cur encoded. The variable
         * id is always positive (= |cur|). */
        int32_t x = cur < 0 ? -cur : cur;
        e->stack_size--;
        mark_encoded(e, x);

        if (is_ite) {
            /* bitwuzla's encoding: ite children are c, ~a, ~b (so negate
             * when feeding to the clauses). */
            emit_ite_clauses(e, x, (int32_t)c_id, (int32_t)(-not_a), (int32_t)(-not_b));
        } else {
            emit_and_clauses(e, x, (int32_t)l, (int32_t)r);
        }
    }
}

/* ----------------------------- public API --------------------------------- */

zsp_aig_cnf_t *zsp_aig_cnf_new(zsp_alloc_t *alloc, zsp_aig_t *aig, zsp_sat_t *sat) {
    zsp_aig_cnf_t *e = (zsp_aig_cnf_t *)xalloc(alloc, sizeof(*e));
    if (!e) return NULL;
    memset(e, 0, sizeof(*e));
    e->alloc = alloc;
    e->aig = aig;
    e->sat = sat;
    return e;
}

void zsp_aig_cnf_free(zsp_aig_cnf_t *e) {
    if (!e) return;
    xfree(e->alloc, e->encoded, e->encoded_cap);
    xfree(e->alloc, e->stack,   (size_t)e->stack_cap * sizeof(int32_t));
    xfree(e->alloc, e->visited, e->visited_cap);
    xfree(e->alloc, e, sizeof(*e));
}

void zsp_aig_cnf_encode(zsp_aig_cnf_t *e, zsp_aig_node_t root, int top_level) {
    if (root == 0) return;

    if (!top_level) {
        encode_subtree(e, root);
        return;
    }

    /* Top-level flatten: walk through positive ANDs collecting leaves, then
     * encode each leaf and assert it as a unit clause. */
    e->visited_cap = 0; /* reset visited bitmap for this flatten pass */
    xfree(e->alloc, e->visited, e->visited_cap);
    e->visited = NULL;

    uint32_t leaf_start = e->stack_size;
    /* phase 1: collect leaves into a temporary buffer past the visit stack */
    /* We'll use the same stack: push visit work onto the back, then collect
     * leaves into a side array. */
    stack_push(e, (int32_t)root);

    /* Use a small dynamic leaves buffer */
    int32_t *leaves = NULL;
    uint32_t leaves_cap = 0, leaves_size = 0;

    /* The visited bitmap tracks (id, sign) pairs separately so positive and
     * negative occurrences of the same AIG id don't dedup each other —
     * critical for soundness: e.g., if a positive AND tree contains both +3
     * and -3 as leaves, both must be asserted (and the conjunction is then
     * unsat). Encoding: bit 2*(id-1) for positive, bit 2*(id-1)+1 for
     * negative. */
    while (e->stack_size > leaf_start) {
        zsp_aig_node_t cur = e->stack[--e->stack_size];
        uint32_t id = (uint32_t)(cur < 0 ? -cur : cur);
        uint32_t bit_idx = 2 * (id - 1) + (cur < 0 ? 1 : 0);
        ensure_bitmap(e, &e->visited, &e->visited_cap, bit_idx + 1);
        if (bm_get(e->visited, bit_idx)) continue;
        bm_set(e->visited, bit_idx);

        if (cur > 0 && zsp_aig_is_and(e->aig, cur)) {
            zsp_aig_node_t l, r;
            zsp_aig_get_children(e->aig, cur, &l, &r);
            stack_push(e, (int32_t)l);
            stack_push(e, (int32_t)r);
        } else {
            if (leaves_size == leaves_cap) {
                uint32_t nc = leaves_cap ? leaves_cap * 2 : 16;
                int32_t *nl = (int32_t *)xrealloc(e->alloc, leaves,
                                                   (size_t)leaves_cap * sizeof(int32_t),
                                                   (size_t)nc * sizeof(int32_t));
                leaves = nl;
                leaves_cap = nc;
            }
            leaves[leaves_size++] = (int32_t)cur;
        }
    }

    /* phase 2: encode each leaf subtree and assert it as a unit clause */
    for (uint32_t i = 0; i < leaves_size; i++) {
        zsp_aig_node_t leaf = leaves[i];
        encode_subtree(e, leaf);
        zsp_sat_add_unit(e->sat, (zsp_sat_lit_t)leaf);
        e->num_clauses++;
        e->num_literals++;
    }

    xfree(e->alloc, leaves, (size_t)leaves_cap * sizeof(int32_t));
}

int zsp_aig_cnf_value(zsp_aig_cnf_t *e, zsp_aig_node_t node) {
    if (node == ZSP_AIG_TRUE)  return 1;
    if (node == ZSP_AIG_FALSE) return -1;
    uint32_t i = (uint32_t)(node < 0 ? -node : node);
    if (!is_node_encoded(e, node)) return 0;
    zsp_sat_lit_t v = zsp_sat_value(e->sat, (zsp_sat_var_t)i);
    int val = v > 0 ? 1 : -1;
    return node < 0 ? -val : val;
}

int zsp_aig_cnf_is_free(const zsp_aig_cnf_t *e, zsp_aig_node_t node) {
    /* A non-constant node that was never encoded into CNF lies outside the cone
     * of every asserted constraint: the SAT model does not determine it, so its
     * value is a don't-care. */
    if (node == ZSP_AIG_TRUE || node == ZSP_AIG_FALSE) return 0;
    return !is_node_encoded(e, node);
}

uint64_t zsp_aig_cnf_num_vars(const zsp_aig_cnf_t *e)     { return e->num_vars; }
uint64_t zsp_aig_cnf_num_clauses(const zsp_aig_cnf_t *e)  { return e->num_clauses; }
uint64_t zsp_aig_cnf_num_literals(const zsp_aig_cnf_t *e) { return e->num_literals; }
