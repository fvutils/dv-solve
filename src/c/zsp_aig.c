#include "zsp_aig.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

/* ----------------------------- internal types ----------------------------- */

typedef struct {
    /* For AND nodes: ids of the two operands. For inputs / TRUE: 0,0. */
    zsp_aig_node_t left;
    zsp_aig_node_t right;
    /* Next pointer for open-chained unique-table bucket. */
    int32_t        next_in_bucket;
    /* Number of AND nodes that reference this node as a child. Monotonic —
     * we never GC, so it only grows. Used by the CNF encoder for the ITE
     * extraction optimization (avoid extracting ITE if it destroys sharing). */
    uint32_t       parents;
} zsp_aig_data_t;

struct zsp_aig_s {
    zsp_alloc_t *alloc;

    /* Arena: index 0 holds TRUE. Node id n maps to index (n-1). */
    zsp_aig_data_t *nodes;
    uint32_t        nodes_size;
    uint32_t        nodes_cap;

    /* Unique-table for AND hash-consing. Buckets store node-indices (1-based)
     * or 0 for empty. */
    int32_t        *buckets;
    uint32_t        buckets_mask; /* capacity - 1; capacity is power of two */
    uint32_t        num_in_table;

    uint64_t        num_inputs;
    uint64_t        num_ands;
    uint64_t        num_shared;
};

/* ----------------------------- alloc helpers ------------------------------ */

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

/* ----------------------------- arena / table ------------------------------ */

static int ensure_nodes_cap(zsp_aig_t *m, uint32_t want) {
    if (want <= m->nodes_cap) return 0;
    uint32_t cap = m->nodes_cap ? m->nodes_cap : 64;
    while (cap < want) cap *= 2;
    zsp_aig_data_t *nn = (zsp_aig_data_t *)xrealloc(
        m->alloc, m->nodes,
        (size_t)m->nodes_cap * sizeof(zsp_aig_data_t),
        (size_t)cap         * sizeof(zsp_aig_data_t));
    if (!nn) return -1;
    m->nodes = nn;
    m->nodes_cap = cap;
    return 0;
}

static void buckets_init(zsp_aig_t *m, uint32_t cap) {
    /* cap must be power of two. */
    m->buckets = (int32_t *)xalloc(m->alloc, (size_t)cap * sizeof(int32_t));
    memset(m->buckets, 0, (size_t)cap * sizeof(int32_t));
    m->buckets_mask = cap - 1;
    m->num_in_table = 0;
}

static uint32_t bucket_hash(zsp_aig_node_t l, zsp_aig_node_t r, uint32_t mask) {
    uint32_t lhs = (uint32_t)(l < 0 ? -l : l);
    uint32_t rhs = (uint32_t)(r < 0 ? -r : r);
    uint32_t h = 547789289u * lhs + 786695309u * rhs;
    return h & mask;
}

static void buckets_resize(zsp_aig_t *m) {
    uint32_t old_cap = m->buckets_mask + 1;
    uint32_t new_cap = old_cap * 2;
    int32_t *old = m->buckets;
    m->buckets = (int32_t *)xalloc(m->alloc, (size_t)new_cap * sizeof(int32_t));
    memset(m->buckets, 0, (size_t)new_cap * sizeof(int32_t));
    m->buckets_mask = new_cap - 1;
    for (uint32_t b = 0; b < old_cap; b++) {
        int32_t idx = old[b];
        while (idx) {
            zsp_aig_data_t *d = &m->nodes[idx - 1];
            int32_t next = d->next_in_bucket;
            uint32_t h = bucket_hash(d->left, d->right, m->buckets_mask);
            d->next_in_bucket = m->buckets[h];
            m->buckets[h] = idx;
            idx = next;
        }
    }
    xfree(m->alloc, old, (size_t)old_cap * sizeof(int32_t));
}

/* Looks up (left,right) in the unique table; on miss creates a new node and
 * inserts. Returns node-id (always positive on creation). */
static zsp_aig_node_t intern_and(zsp_aig_t *m, zsp_aig_node_t l, zsp_aig_node_t r) {
    uint32_t h = bucket_hash(l, r, m->buckets_mask);
    int32_t idx = m->buckets[h];
    while (idx) {
        zsp_aig_data_t *d = &m->nodes[idx - 1];
        if (d->left == l && d->right == r) {
            m->num_shared++;
            return (zsp_aig_node_t)idx;
        }
        idx = d->next_in_bucket;
    }
    if (m->num_in_table >= (m->buckets_mask + 1)) {
        buckets_resize(m);
        h = bucket_hash(l, r, m->buckets_mask);
    }
    /* New node */
    if (ensure_nodes_cap(m, m->nodes_size + 1) != 0) {
        return ZSP_AIG_NULL;
    }
    uint32_t new_idx = ++m->nodes_size;
    zsp_aig_data_t *d = &m->nodes[new_idx - 1];
    d->left = l;
    d->right = r;
    d->next_in_bucket = m->buckets[h];
    d->parents = 0;
    m->buckets[h] = (int32_t)new_idx;
    m->num_in_table++;
    m->num_ands++;
    /* Bump parent counters on children. */
    {
        uint32_t li = (uint32_t)(l < 0 ? -l : l);
        uint32_t ri = (uint32_t)(r < 0 ? -r : r);
        m->nodes[li - 1].parents++;
        m->nodes[ri - 1].parents++;
    }
    return (zsp_aig_node_t)new_idx;
}

/* ----------------------------- public API --------------------------------- */

zsp_aig_t *zsp_aig_new(zsp_alloc_t *alloc) {
    zsp_aig_t *m = (zsp_aig_t *)xalloc(alloc, sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->alloc = alloc;
    if (ensure_nodes_cap(m, 64) != 0) { xfree(alloc, m, sizeof(*m)); return NULL; }
    buckets_init(m, 64);
    /* Node 1 = TRUE. Leave its left/right as zero so it isn't matched by
     * intern_and (which only looks at AND nodes). */
    m->nodes_size = 1;
    m->nodes[0].left = 0;
    m->nodes[0].right = 0;
    m->nodes[0].next_in_bucket = 0;
    m->nodes[0].parents = 0;
    return m;
}

void zsp_aig_free(zsp_aig_t *m) {
    if (!m) return;
    xfree(m->alloc, m->nodes, (size_t)m->nodes_cap * sizeof(zsp_aig_data_t));
    xfree(m->alloc, m->buckets,
          (size_t)(m->buckets_mask + 1) * sizeof(int32_t));
    xfree(m->alloc, m, sizeof(*m));
}

zsp_aig_node_t zsp_aig_mk_input(zsp_aig_t *m) {
    if (ensure_nodes_cap(m, m->nodes_size + 1) != 0) return ZSP_AIG_NULL;
    uint32_t idx = ++m->nodes_size;
    zsp_aig_data_t *d = &m->nodes[idx - 1];
    d->left = 0;
    d->right = 0;
    d->next_in_bucket = 0;
    d->parents = 0;
    m->num_inputs++;
    return (zsp_aig_node_t)idx;
}

uint32_t zsp_aig_parents(const zsp_aig_t *m, zsp_aig_node_t n) {
    if (n == 0 || n == ZSP_AIG_TRUE || n == ZSP_AIG_FALSE) return 0;
    uint32_t i = (uint32_t)(n < 0 ? -n : n);
    if (i > m->nodes_size) return 0;
    return m->nodes[i - 1].parents;
}

static int is_and_id(const zsp_aig_t *m, zsp_aig_node_t id) {
    if (id == 0 || id == ZSP_AIG_TRUE || id == ZSP_AIG_FALSE) return 0;
    uint32_t i = (uint32_t)(id < 0 ? -id : id);
    if (i > m->nodes_size) return 0;
    return m->nodes[i - 1].left != 0;
}

static void children_of(const zsp_aig_t *m, zsp_aig_node_t id,
                        zsp_aig_node_t *a, zsp_aig_node_t *b) {
    uint32_t i = (uint32_t)(id < 0 ? -id : id);
    *a = m->nodes[i - 1].left;
    *b = m->nodes[i - 1].right;
}

/* Apply the Brummayer/Biere two-level rewriting rules. Faithful port of
 * AigManager::rewrite_and from bitwuzla.
 * Returns the resulting node id (may be a constant or an existing id), or
 * ZSP_AIG_NULL if a new AND must be created (with the final left,right). */
static zsp_aig_node_t rewrite_and(zsp_aig_t *m, zsp_aig_node_t l, zsp_aig_node_t r,
                                  zsp_aig_node_t *out_l, zsp_aig_node_t *out_r) {
    const zsp_aig_node_t TRUE_ID  = ZSP_AIG_TRUE;
    const zsp_aig_node_t FALSE_ID = ZSP_AIG_FALSE;
    zsp_aig_node_t left = l, right = r;

    for (;;) {
        /* Level 1: neutrality / idempotence / boundedness / contradiction */
        if (left == TRUE_ID || left == right) return right;
        if (right == TRUE_ID) return left;
        if (left == FALSE_ID || right == FALSE_ID || left == -right) return FALSE_ID;

        zsp_aig_node_t a = 0, b = 0, c = 0, d = 0;
        int left_is_and  = is_and_id(m, left);
        int right_is_and = is_and_id(m, right);
        if (left_is_and)  children_of(m, left,  &a, &b);
        if (right_is_and) children_of(m, right, &c, &d);
        int left_is_neg  = left  < 0;
        int right_is_neg = right < 0;

        /* If the AND was viewed under negation, the stored children are still
         * the positive form's children. The Brummayer/Biere paper uses the
         * negation explicitly via the left_is_neg flag. */

        /* Level 2 */
        /* Contradiction (asymmetric): (a/\b)/\c, c == ~a or c == ~b */
        if (!left_is_neg && left_is_and && (a == -right || b == -right)) return FALSE_ID;
        if (!right_is_neg && right_is_and && (c == -left || d == -left)) return FALSE_ID;
        /* Contradiction (symmetric) */
        if (!left_is_neg && !right_is_neg && left_is_and && right_is_and
            && (a == -c || a == -d || b == -c || b == -d)) return FALSE_ID;
        /* Subsumption (asymmetric): ~(a/\b) /\ c, c == ~a or c == ~b -> c */
        if (left_is_neg && left_is_and && (a == -right || b == -right)) return right;
        if (right_is_neg && right_is_and && (c == -left || d == -left)) return left;
        /* Subsumption (symmetric): ~(a/\b) /\ (c/\d), ai == ~cj -> (c/\d) */
        if (left_is_neg && !right_is_neg && left_is_and && right_is_and
            && (a == -c || a == -d || b == -c || b == -d)) return right;
        if (right_is_neg && !left_is_neg && right_is_and && left_is_and
            && (c == -a || c == -b || d == -a || d == -b)) return left;
        /* Idempotence: (a/\b) /\ c where c is a or b */
        if (!left_is_neg && left_is_and && (a == right || b == right)) return left;
        if (!right_is_neg && right_is_and && (c == left || d == left)) return right;
        /* Resolution: ~(a/\b) /\ ~(c/\d) with (a=d /\ b=~c) or (a=c /\ b=~d) */
        if (left_is_neg && right_is_neg && left_is_and && right_is_and) {
            if ((a == c && b == -d) || (a == d && b == -c)) return -a;
            if ((d == b && c == -a) || (d == a && c == -b)) return -d;
        }

        /* Level 3: substitution (asymmetric) — these loop and re-run rules */
        if (left_is_neg && left_is_and) {
            if (a == right) { left = -b; continue; }
            if (b == right) { left = -a; continue; }
        }
        if (right_is_neg && right_is_and) {
            if (c == left) { right = -d; continue; }
            if (d == left) { right = -c; continue; }
        }
        /* Substitution (symmetric) */
        if (left_is_neg && !right_is_neg && left_is_and && right_is_and) {
            if (a == c || a == d) { left = -b; continue; }
            if (b == c || b == d) { left = -a; continue; }
        }
        if (right_is_neg && !left_is_neg && right_is_and && left_is_and) {
            if (c == a || c == b) { right = -d; continue; }
            if (d == a || d == b) { right = -c; continue; }
        }

        /* Level 4: idempotence on (a/\b) /\ (c/\d) */
        if (!left_is_neg && !right_is_neg && left_is_and && right_is_and) {
            if (a == c || b == c) { right = d; continue; }
            if (a == d || b == d) { right = c; continue; }
        }

        break;
    }

    /* Normalize: smaller |id| on the left. */
    {
        int32_t al = left  < 0 ? -left  : left;
        int32_t ar = right < 0 ? -right : right;
        if (al > ar) { zsp_aig_node_t t = left; left = right; right = t; }
    }
    *out_l = left;
    *out_r = right;
    return ZSP_AIG_NULL;  /* signal: caller should intern_and(left, right) */
}

zsp_aig_node_t zsp_aig_mk_and(zsp_aig_t *m, zsp_aig_node_t a, zsp_aig_node_t b) {
    zsp_aig_node_t nl = 0, nr = 0;
    zsp_aig_node_t res = rewrite_and(m, a, b, &nl, &nr);
    if (res != ZSP_AIG_NULL) return res;
    return intern_and(m, nl, nr);
}

zsp_aig_node_t zsp_aig_mk_xor(zsp_aig_t *m, zsp_aig_node_t a, zsp_aig_node_t b) {
    /* a XOR b = (a /\ ~b) v (~a /\ b) = ~((~a v b) /\ (a v ~b))
     *        = ~(~(a /\ ~b) /\ ~(~a /\ b)) */
    zsp_aig_node_t l = zsp_aig_mk_and(m, a, -b);
    zsp_aig_node_t r = zsp_aig_mk_and(m, -a, b);
    return zsp_aig_mk_or(m, l, r);
}

zsp_aig_node_t zsp_aig_mk_ite(zsp_aig_t *m,
                              zsp_aig_node_t c,
                              zsp_aig_node_t t,
                              zsp_aig_node_t e) {
    /* ITE(c, t, e) = (c /\ t) v (~c /\ e) */
    if (c == ZSP_AIG_TRUE)  return t;
    if (c == ZSP_AIG_FALSE) return e;
    if (t == e)             return t;
    zsp_aig_node_t lhs = zsp_aig_mk_and(m, c, t);
    zsp_aig_node_t rhs = zsp_aig_mk_and(m, -c, e);
    return zsp_aig_mk_or(m, lhs, rhs);
}

int zsp_aig_is_const(const zsp_aig_t *m, zsp_aig_node_t n) {
    (void)m;
    return n == ZSP_AIG_TRUE || n == ZSP_AIG_FALSE;
}

int zsp_aig_is_and(const zsp_aig_t *m, zsp_aig_node_t n) {
    return is_and_id(m, n);
}

int zsp_aig_is_input(const zsp_aig_t *m, zsp_aig_node_t n) {
    if (n == 0 || n == ZSP_AIG_TRUE || n == ZSP_AIG_FALSE) return 0;
    uint32_t i = (uint32_t)(n < 0 ? -n : n);
    if (i > m->nodes_size) return 0;
    return m->nodes[i - 1].left == 0; /* non-true non-and = input */
}

void zsp_aig_get_children(const zsp_aig_t *m,
                          zsp_aig_node_t n,
                          zsp_aig_node_t *l,
                          zsp_aig_node_t *r) {
    assert(is_and_id(m, n));
    children_of(m, n, l, r);
}

uint64_t zsp_aig_num_nodes(const zsp_aig_t *m)  { return m->nodes_size; }
uint64_t zsp_aig_num_ands(const zsp_aig_t *m)   { return m->num_ands; }
uint64_t zsp_aig_num_inputs(const zsp_aig_t *m) { return m->num_inputs; }
uint64_t zsp_aig_num_shared(const zsp_aig_t *m) { return m->num_shared; }
