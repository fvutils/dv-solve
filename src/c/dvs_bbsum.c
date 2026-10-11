/* dvs_bbsum — word-level sum normalization during bit-blasting.
 * See dvs_bbsum.h for the rules and why they keep the value. */
#include "dvs_bbsum.h"

#include <stdlib.h>
#include <string.h>

/* A term: an AIG literal vector (MSB first) with its leading FALSE bits
 * stripped, pointing into bit-blaster-owned memory. size 0 is the value 0. */
typedef struct {
    const dvs_aig_node_t *bits;
    uint32_t              size;
} bbsum_term_t;

/* A remembered sum: the stripped result `key` is congruent to the sum of
 * terms[start .. start+n) modulo 2^width, and equal to it when `exact`. */
typedef struct {
    bbsum_term_t key;
    uint32_t     width;
    uint32_t     start, n;
    int          exact;
    int          single;   /* its only user is the next addition */
} bbsum_entry_t;

#define BBSUM_MAX_TERMS 64

/* An unsigned bound below 2^126, as 64-bit halves (MSVC has no __int128).
 * hi >= 2^62 means saturated: "too big to know". */
typedef struct { uint64_t hi, lo; } bbsum_bound_t;
#define BBSUM_SAT_HI ((uint64_t)1 << 62)

struct dvs_bbsum_s {
    dvs_alloc_t   *alloc;
    bbsum_entry_t *ents;
    uint32_t       n_ents, cap_ents;
    bbsum_term_t  *terms;
    uint32_t       n_terms, cap_terms;
    uint32_t      *slots;            /* open addressing: entry index + 1 */
    uint32_t       cap_slots;
    uint64_t       adds, flattened, work;
    int            off;
};

dvs_bbsum_t *dvs_bbsum_new(dvs_alloc_t *alloc) {
    dvs_bbsum_t *T = (dvs_bbsum_t *)calloc(1, sizeof(*T));
    if (!T) return NULL;
    T->alloc = alloc;
    const char *e = getenv("DV_BB_SUM");
    T->off = e && *e == '0';
    return T;
}

void dvs_bbsum_free(dvs_bbsum_t *T) {
    if (!T) return;
    free(T->ents);
    free(T->terms);
    free(T->slots);
    free(T);
}

uint64_t dvs_bbsum_num_adds(const dvs_bbsum_t *T) { return T ? T->adds : 0; }
uint64_t dvs_bbsum_num_flattened(const dvs_bbsum_t *T) { return T ? T->flattened : 0; }

/* ------------------------------------------------------------------ terms */

static bbsum_term_t _strip(const dvs_aig_node_t *bits, uint32_t size) {
    while (size > 0 && bits[0] == DVS_AIG_FALSE) { bits++; size--; }
    bbsum_term_t t = { bits, size };
    return t;
}

/* The low `w` bits of t (t modulo 2^w), stripped. */
static bbsum_term_t _trunc(bbsum_term_t t, uint32_t w) {
    if (t.size <= w) return t;
    return _strip(t.bits + (t.size - w), w);
}

static int _is_const(bbsum_term_t t) {
    for (uint32_t i = 0; i < t.size; i++)
        if (t.bits[i] != DVS_AIG_TRUE && t.bits[i] != DVS_AIG_FALSE) return 0;
    return 1;
}

/* Value of a constant term of at most 64 bits. */
static uint64_t _const_val(bbsum_term_t t) {
    uint64_t v = 0;
    for (uint32_t i = 0; i < t.size; i++)
        v = (v << 1) | (t.bits[i] == DVS_AIG_TRUE);
    return v;
}

static const bbsum_bound_t BBSUM_SAT = { BBSUM_SAT_HI, 0 };

static int _is_sat(bbsum_bound_t b) { return b.hi >= BBSUM_SAT_HI; }

static bbsum_bound_t _from_u64(uint64_t v) {
    bbsum_bound_t b = { 0, v };
    return b;
}

static bbsum_bound_t _sat_add(bbsum_bound_t a, bbsum_bound_t b) {
    if (_is_sat(a) || _is_sat(b)) return BBSUM_SAT;
    bbsum_bound_t r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);   /* each hi < 2^62: no wrap */
    return _is_sat(r) ? BBSUM_SAT : r;
}

/* Largest value t can take. */
static bbsum_bound_t _max_val(bbsum_term_t t) {
    if (t.size <= 64 && _is_const(t)) return _from_u64(_const_val(t));
    if (t.size >= 126) return BBSUM_SAT;
    if (t.size <= 64)
        return _from_u64(t.size == 64 ? ~(uint64_t)0
                                       : (((uint64_t)1 << t.size) - 1));
    bbsum_bound_t b = { ((uint64_t)1 << (t.size - 64)) - 1, ~(uint64_t)0 };
    return b;
}

/* Bits in b's binary form (0 for 0). */
static uint32_t _bitlen(bbsum_bound_t b) {
    uint64_t v = b.hi ? b.hi : b.lo;
    uint32_t n = b.hi ? 64 : 0;
    while (v) { n++; v >>= 1; }
    return n;
}

/* Bits needed to hold every value up to `b`, at most `w`. */
static uint32_t _bits_for(bbsum_bound_t b, uint32_t w) {
    if (_is_sat(b)) return w;
    uint32_t n = _bitlen(b);
    if (n == 0) n = 1;
    return n < w ? n : w;
}

/* b < 2^w. */
static int _fits(bbsum_bound_t b, uint32_t w) {
    if (_is_sat(b)) return 0;
    return _bitlen(b) <= w;
}

static int _term_eq(bbsum_term_t a, bbsum_term_t b) {
    return a.size == b.size &&
        (a.bits == b.bits ||
         memcmp(a.bits, b.bits, a.size * sizeof(dvs_aig_node_t)) == 0);
}

/* Canonical order: by literals from the LSB up, then by size; constants
 * last. Equal terms compare equal wherever they came from. */
static int _term_cmp(const void *pa, const void *pb) {
    const bbsum_term_t *a = (const bbsum_term_t *)pa;
    const bbsum_term_t *b = (const bbsum_term_t *)pb;
    int ca = _is_const(*a), cb = _is_const(*b);
    if (ca != cb) return ca - cb;
    uint32_t n = a->size < b->size ? a->size : b->size;
    for (uint32_t i = 1; i <= n; i++) {
        dvs_aig_node_t x = a->bits[a->size - i], y = b->bits[b->size - i];
        if (x != y) return x < y ? -1 : 1;
    }
    return (a->size > b->size) - (a->size < b->size);
}

/* ------------------------------------------------------------- the table */

static uint32_t _hash(bbsum_term_t t) {
    uint64_t h = 1469598103934665603ULL ^ t.size;
    for (uint32_t i = 0; i < t.size; i++) {
        h ^= (uint32_t)t.bits[i];
        h *= 1099511628211ULL;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static const bbsum_entry_t *_lookup(const dvs_bbsum_t *T, bbsum_term_t key) {
    if (!T->cap_slots || key.size == 0) return NULL;
    uint32_t mask = T->cap_slots - 1;
    for (uint32_t i = _hash(key) & mask;; i = (i + 1) & mask) {
        uint32_t s = T->slots[i];
        if (!s) return NULL;
        if (_term_eq(T->ents[s - 1].key, key)) return &T->ents[s - 1];
    }
}

static int _rehash(dvs_bbsum_t *T, uint32_t cap) {
    uint32_t *ns = (uint32_t *)calloc(cap, sizeof(uint32_t));
    if (!ns) return 0;
    for (uint32_t e = 0; e < T->n_ents; e++) {
        uint32_t i = _hash(T->ents[e].key) & (cap - 1);
        while (ns[i]) i = (i + 1) & (cap - 1);
        ns[i] = e + 1;
    }
    free(T->slots);
    T->slots = ns;
    T->cap_slots = cap;
    return 1;
}

/* Remember that `key` is the sum of `ts` (modulo 2^width; equal if exact).
 * The first claim for a key is kept: any true one serves. */
static void _remember(dvs_bbsum_t *T, bbsum_term_t key, uint32_t width,
                      int exact, int single, const bbsum_term_t *ts, uint32_t n) {
    if (key.size == 0 || _lookup(T, key)) return;
    if (2 * (T->n_ents + 1) > T->cap_slots &&
        !_rehash(T, T->cap_slots ? 2 * T->cap_slots : 256)) return;
    if (T->n_ents == T->cap_ents) {
        uint32_t c = T->cap_ents ? 2 * T->cap_ents : 256;
        bbsum_entry_t *p = (bbsum_entry_t *)realloc(T->ents, c * sizeof(*p));
        if (!p) return;
        T->ents = p; T->cap_ents = c;
    }
    if (T->n_terms + n > T->cap_terms) {
        uint32_t c = T->cap_terms ? T->cap_terms : 1024;
        while (c < T->n_terms + n) c *= 2;
        bbsum_term_t *p = (bbsum_term_t *)realloc(T->terms, c * sizeof(*p));
        if (!p) return;
        T->terms = p; T->cap_terms = c;
    }
    memcpy(T->terms + T->n_terms, ts, n * sizeof(*ts));
    bbsum_entry_t *e = &T->ents[T->n_ents];
    e->key = key; e->width = width; e->exact = exact; e->single = single;
    e->start = T->n_terms; e->n = n;
    T->n_terms += n;
    uint32_t mask = T->cap_slots - 1;
    uint32_t i = _hash(key) & mask;
    while (T->slots[i]) i = (i + 1) & mask;
    T->slots[i] = ++T->n_ents;
}

/* ------------------------------------------------------------- building */

static dvs_bv_t _zext(dvs_bitblast_t *bb, bbsum_term_t t, uint32_t w) {
    if (t.size == 0) return dvs_bb_value_u64(bb, w, 0);
    dvs_bv_t v = { (dvs_aig_node_t *)t.bits, t.size };
    return t.size < w ? dvs_bb_zero_ext(bb, v, w - t.size) : v;
}

dvs_bv_t dvs_bbsum_add(dvs_bbsum_t *T, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b,
                       int single) {
    if (!T || T->off) return dvs_bb_add(bb, a, b);
    uint32_t w = a.size;
    T->adds++;

    bbsum_term_t ops[2] = { _strip(a.bits, a.size), _strip(b.bits, b.size) };
    const bbsum_entry_t *ent[2];
    int splice = 0;
    uint32_t total = 0;
    for (int k = 0; k < 2; k++) {
        ent[k] = _lookup(T, ops[k]);
        if (ent[k] && !(ent[k]->single && (ent[k]->exact || w <= ent[k]->width)))
            ent[k] = NULL;
        total += ent[k] ? ent[k]->n : 1;
        splice |= ent[k] != NULL;
    }
    /* Past the work budget, or too many terms: add as written. */
    if (total > BBSUM_MAX_TERMS || T->work > 4 * T->adds + 4096) splice = 0;

    bbsum_term_t ts[BBSUM_MAX_TERMS + 1];
    uint32_t n = 0;
    uint64_t cval = 0;
    int fold = w <= 64;
    if (splice) {
        /* Flatten, reducing every term modulo 2^w, and fold the constants. */
        for (int k = 0; k < 2; k++) {
            uint32_t m = ent[k] ? ent[k]->n : 1;
            for (uint32_t j = 0; j < m; j++) {
                bbsum_term_t t = _trunc(ent[k] ? T->terms[ent[k]->start + j] : ops[k], w);
                if (t.size == 0) continue;
                if (fold && _is_const(t)) { cval += _const_val(t); continue; }
                ts[n++] = t;
            }
        }
        qsort(ts, n, sizeof(ts[0]), _term_cmp);
        /* A repeated term means the operands share a subsum (a running sum
         * of running sums, as in Fletcher's checksum): the DAG adds it once,
         * the flat form once per occurrence. Keep the DAG. */
        for (uint32_t i = 1; i < n && splice; i++)
            if (_term_eq(ts[i - 1], ts[i])) splice = 0;
    }

    if (!splice) {
        dvs_bv_t r = dvs_bb_add(bb, a, b);
        bbsum_bound_t bound = _sat_add(_max_val(ops[0]), _max_val(ops[1]));
        _remember(T, _strip(r.bits, r.size), w, _fits(bound, w), single, ops, 2);
        return r;
    }
    T->flattened++;
    if (fold) {
        if (w < 64) cval &= (((uint64_t)1 << w) - 1);
        if (cval) {
            dvs_bv_t c = dvs_bb_value_u64(bb, _bits_for(_from_u64(cval), 64), cval);
            ts[n++] = _strip(c.bits, c.size);   /* constants sort last anyway */
        }
    }

    /* Add in that order, each partial sum as wide as its bound needs. */
    dvs_bv_t acc = { NULL, 0 };
    bbsum_bound_t bound = _from_u64(0);
    for (uint32_t i = 0; i < n; i++) {
        bound = _sat_add(bound, _max_val(ts[i]));
        uint32_t wp = _bits_for(bound, w);
        if (i == 0) { acc = _zext(bb, ts[i], wp); continue; }
        bbsum_term_t at = { acc.bits, acc.size };
        acc = dvs_bb_add(bb, _zext(bb, at, wp), _zext(bb, ts[i], wp));
        T->work++;
    }
    bbsum_term_t at = { acc.bits, acc.size };
    dvs_bv_t r = n ? _zext(bb, at, w) : dvs_bb_value_u64(bb, w, 0);
    _remember(T, _strip(r.bits, r.size), w, _fits(bound, w), single, ts, n);
    return r;
}
