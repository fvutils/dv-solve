/* dvs_bbword — word-level facts about bit-blasted vectors.
 * See dvs_bbword.h for the facts and why each rule keeps them true. */
#include "dvs_bbword.h"

#include <stdlib.h>
#include <string.h>

#include "dvs_i128.h"

/* A vector as a key: AIG literals, MSB first, in bit-blaster-owned memory. */
typedef struct {
    dvs_aig_node_t *bits;
    uint32_t        size;
} bbw_key_t;

/* ------------------------------------------------------------- hash table */

typedef struct {
    bbw_key_t *keys;
    uint32_t   n, cap;
    uint32_t  *slots;               /* open addressing: key index + 1 */
    uint32_t   cap_slots;
} bbw_tab_t;

static uint32_t _hash(bbw_key_t k) {
    uint64_t h = 1469598103934665603ULL ^ k.size;
    for (uint32_t i = 0; i < k.size; i++) {
        h ^= (uint32_t)k.bits[i];
        h *= 1099511628211ULL;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static int _key_eq(bbw_key_t a, bbw_key_t b) {
    return a.size == b.size &&
        (a.bits == b.bits ||
         memcmp(a.bits, b.bits, a.size * sizeof(dvs_aig_node_t)) == 0);
}

/* Index of `k`, or -1. */
static int64_t _find(const bbw_tab_t *T, bbw_key_t k) {
    if (!T->cap_slots) return -1;
    uint32_t mask = T->cap_slots - 1;
    for (uint32_t i = _hash(k) & mask;; i = (i + 1) & mask) {
        uint32_t s = T->slots[i];
        if (!s) return -1;
        if (_key_eq(T->keys[s - 1], k)) return (int64_t)(s - 1);
    }
}

/* Index of `k`, added if new (*added set); -1 when out of memory. The caller
 * grows its payload array to T->cap. */
static int64_t _insert(bbw_tab_t *T, bbw_key_t k, int *added) {
    *added = 0;
    int64_t f = _find(T, k);
    if (f >= 0) return f;
    if (2 * (T->n + 1) > T->cap_slots) {
        uint32_t cap = T->cap_slots ? 2 * T->cap_slots : 256;
        uint32_t *ns = (uint32_t *)calloc(cap, sizeof(uint32_t));
        if (!ns) return -1;
        for (uint32_t e = 0; e < T->n; e++) {
            uint32_t i = _hash(T->keys[e]) & (cap - 1);
            while (ns[i]) i = (i + 1) & (cap - 1);
            ns[i] = e + 1;
        }
        free(T->slots);
        T->slots = ns;
        T->cap_slots = cap;
    }
    if (T->n == T->cap) {
        uint32_t c = T->cap ? 2 * T->cap : 256;
        bbw_key_t *p = (bbw_key_t *)realloc(T->keys, c * sizeof(*p));
        if (!p) return -1;
        T->keys = p;
        T->cap = c;
    }
    T->keys[T->n] = k;
    uint32_t mask = T->cap_slots - 1;
    uint32_t i = _hash(k) & mask;
    while (T->slots[i]) i = (i + 1) & mask;
    T->slots[i] = ++T->n;
    *added = 1;
    return (int64_t)(T->n - 1);
}

static void _tab_free(bbw_tab_t *T) {
    free(T->keys);
    free(T->slots);
}

/* ------------------------------------------------------------- payloads */

typedef struct {
    uint64_t  poly;   bbw_key_t lbase;  uint8_t has_lin;
    uint64_t  mulc;   bbw_key_t mbase;  uint8_t has_mul;
} bbw_shape_t;

typedef struct {
    uint64_t bound;
    uint64_t mod, k;                 /* value ≡ k + Σ c·t (mod mod); = if 0 */
    uint32_t start, n;               /* terms in W->terms / W->coefs */
    uint8_t  has_form;
} bbw_val_t;

#define BBW_MAX_TERMS 64

typedef struct {
    uint64_t  mod, k;
    uint32_t  n;
    bbw_key_t t[BBW_MAX_TERMS];
    uint64_t  c[BBW_MAX_TERMS];
} bbw_form_t;

struct dvs_bbword_s {
    dvs_alloc_t *alloc;
    bbw_tab_t    shape_tab;
    bbw_shape_t *shapes;
    uint32_t     cap_shapes;
    bbw_tab_t    val_tab;
    bbw_val_t   *vals;
    uint32_t     cap_vals;
    bbw_key_t   *terms;
    uint64_t    *coefs;
    uint32_t     n_terms, cap_terms;
    uint64_t     cancelled, decided, peeled;
    int          off;
};

dvs_bbword_t *dvs_bbword_new(dvs_alloc_t *alloc) {
    dvs_bbword_t *W = (dvs_bbword_t *)calloc(1, sizeof(*W));
    if (!W) return NULL;
    W->alloc = alloc;
    const char *e = getenv("DV_BB_WORD");
    W->off = e && *e == '0';
    return W;
}

void dvs_bbword_free(dvs_bbword_t *W) {
    if (!W) return;
    _tab_free(&W->shape_tab);
    _tab_free(&W->val_tab);
    free(W->shapes);
    free(W->vals);
    free(W->terms);
    free(W->coefs);
    free(W);
}

uint64_t dvs_bbword_num_cancelled(const dvs_bbword_t *W) { return W ? W->cancelled : 0; }
uint64_t dvs_bbword_num_decided(const dvs_bbword_t *W)   { return W ? W->decided : 0; }
uint64_t dvs_bbword_num_peeled(const dvs_bbword_t *W)    { return W ? W->peeled : 0; }

/* The shape entry for `k`, created if absent; NULL when out of memory. */
static bbw_shape_t *_shape_slot(dvs_bbword_t *W, bbw_key_t k) {
    int added;
    int64_t i = _insert(&W->shape_tab, k, &added);
    if (i < 0) return NULL;
    if (W->shape_tab.cap > W->cap_shapes) {
        bbw_shape_t *p = (bbw_shape_t *)realloc(W->shapes,
                             W->shape_tab.cap * sizeof(*p));
        if (!p) return NULL;
        W->shapes = p;
        W->cap_shapes = W->shape_tab.cap;
    }
    if (added) memset(&W->shapes[i], 0, sizeof(bbw_shape_t));
    return &W->shapes[i];
}

static const bbw_shape_t *_shape(const dvs_bbword_t *W, bbw_key_t k) {
    int64_t i = _find(&W->shape_tab, k);
    return i < 0 ? NULL : &W->shapes[i];
}

static const bbw_val_t *_val(const dvs_bbword_t *W, bbw_key_t k) {
    int64_t i = _find(&W->val_tab, k);
    return i < 0 ? NULL : &W->vals[i];
}

/* ------------------------------------------------------------- vectors */

static bbw_key_t _key(dvs_bv_t v) {
    bbw_key_t k = { v.bits, v.size };
    return k;
}

static dvs_bv_t _bv(bbw_key_t k) {
    dvs_bv_t v = { k.bits, k.size };
    return v;
}

static bbw_key_t _strip(bbw_key_t k) {
    while (k.size > 0 && k.bits[0] == DVS_AIG_FALSE) { k.bits++; k.size--; }
    return k;
}

/* The value of a constant vector of at most 64 significant bits. */
static int _const_val(bbw_key_t k, uint64_t *out) {
    k = _strip(k);
    if (k.size > 64) return 0;
    uint64_t v = 0;
    for (uint32_t i = 0; i < k.size; i++) {
        if (k.bits[i] != DVS_AIG_TRUE && k.bits[i] != DVS_AIG_FALSE) return 0;
        v = (v << 1) | (k.bits[i] == DVS_AIG_TRUE);
    }
    *out = v;
    return 1;
}

static uint64_t _mask(uint32_t w) {
    return w >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << w) - 1);
}

static uint64_t _gcd(uint64_t a, uint64_t b) {
    while (b) { uint64_t t = a % b; a = b; b = t; }
    return a;
}

/* ------------------------------------------------------------- value facts */

/* An upper bound on the unsigned value of stripped key `k`. */
static int _bound(const dvs_bbword_t *W, bbw_key_t k, uint64_t *out) {
    uint64_t c;
    if (_const_val(k, &c)) { *out = c; return 1; }
    const bbw_val_t *v = _val(W, k);
    if (v) { *out = v->bound; return 1; }
    if (k.size > 64) return 0;
    *out = _mask(k.size);
    return 1;
}

/* Canonical term order: by literals from the LSB up, then by size. */
static int _key_cmp(bbw_key_t a, bbw_key_t b) {
    uint32_t n = a.size < b.size ? a.size : b.size;
    for (uint32_t i = 1; i <= n; i++) {
        dvs_aig_node_t x = a.bits[a.size - i], y = b.bits[b.size - i];
        if (x != y) return x < y ? -1 : 1;
    }
    return (a.size > b.size) - (a.size < b.size);
}

/* The linear form of stripped key `k`: its remembered form, a constant, or
 * the term k itself. */
static void _form(const dvs_bbword_t *W, bbw_key_t k, bbw_form_t *F) {
    F->mod = 0;
    F->k = 0;
    F->n = 0;
    uint64_t c;
    if (_const_val(k, &c)) { F->k = c; return; }
    const bbw_val_t *v = _val(W, k);
    if (v && v->has_form) {
        F->mod = v->mod;
        F->k = v->k;
        F->n = v->n;
        memcpy(F->t, W->terms + v->start, v->n * sizeof(bbw_key_t));
        memcpy(F->c, W->coefs + v->start, v->n * sizeof(uint64_t));
        return;
    }
    F->n = 1;
    F->t[0] = k;
    F->c[0] = 1;
}

/* a + b, reduced modulo m (no reduction when m = 0); 0 on overflow. */
static int _cadd(uint64_t a, uint64_t b, uint64_t m, uint64_t *out) {
    if (m) {
        a %= m;
        b %= m;
        *out = a >= m - b ? a - (m - b) : a + b;
        return 1;
    }
    return !dvs_add_u64_ovf(a, b, out);
}

static int _cmul(uint64_t a, uint64_t b, uint64_t m, uint64_t *out) {
    if (m) { *out = dvs_mulmod_u64(a % m, b % m, m); return 1; }
    return !dvs_mul_u64_ovf(a, b, out);
}

/* Reduce F modulo m (a divisor of F->mod, or any m when F->mod = 0). */
static void _reduce(bbw_form_t *F, uint64_t m) {
    F->mod = m;
    if (!m) return;
    F->k %= m;
    uint32_t j = 0;
    for (uint32_t i = 0; i < F->n; i++) {
        uint64_t c = F->c[i] % m;
        if (!c) continue;
        F->t[j] = F->t[i];
        F->c[j] = c;
        j++;
    }
    F->n = j;
}

/* H = F + G (H may alias neither). 0 when the terms overflow. */
static int _form_add(const bbw_form_t *F, const bbw_form_t *G, bbw_form_t *H) {
    uint64_t m = _gcd(F->mod, G->mod);
    H->mod = m;
    H->n = 0;
    if (!_cadd(F->k, G->k, m, &H->k)) return 0;
    uint32_t i = 0, j = 0;
    while (i < F->n || j < G->n) {
        int c = i >= F->n ? 1 : j >= G->n ? -1 : _key_cmp(F->t[i], G->t[j]);
        bbw_key_t t;
        uint64_t v;
        if (c < 0)      { t = F->t[i]; v = m ? F->c[i] % m : F->c[i]; i++; }
        else if (c > 0) { t = G->t[j]; v = m ? G->c[j] % m : G->c[j]; j++; }
        else {
            t = F->t[i];
            if (!_cadd(F->c[i], G->c[j], m, &v)) return 0;
            i++; j++;
        }
        if (!v) continue;
        if (H->n == BBW_MAX_TERMS) return 0;
        H->t[H->n] = t;
        H->c[H->n] = v;
        H->n++;
    }
    return 1;
}

static int _form_scale(bbw_form_t *F, uint64_t s) {
    if (!_cmul(F->k, s, F->mod, &F->k)) return 0;
    for (uint32_t i = 0; i < F->n; i++)
        if (!_cmul(F->c[i], s, F->mod, &F->c[i])) return 0;
    _reduce(F, F->mod);
    return 1;
}

static int _form_eq(const bbw_form_t *F, const bbw_form_t *G) {
    if (F->k != G->k || F->n != G->n) return 0;
    for (uint32_t i = 0; i < F->n; i++)
        if (F->c[i] != G->c[i] || !_key_eq(F->t[i], G->t[i])) return 0;
    return 1;
}

/* Remember that stripped key `k` has value at most `bound` and, when F is
 * given, the form F. A key already known keeps its first facts. */
static void _remember_val(dvs_bbword_t *W, bbw_key_t k, uint64_t bound,
                          const bbw_form_t *F) {
    if (k.size == 0) return;
    uint64_t c;
    if (_const_val(k, &c)) return;
    if (F && (F->mod == 1 || (F->mod == 0 && F->n == 1 && F->k == 0 &&
                              F->c[0] == 1 && _key_eq(F->t[0], k))))
        F = NULL;                      /* says nothing */
    if (!F && k.size <= 64 && bound >= _mask(k.size)) return;
    int added;
    int64_t i = _insert(&W->val_tab, k, &added);
    if (i < 0 || !added) return;
    if (W->val_tab.cap > W->cap_vals) {
        bbw_val_t *p = (bbw_val_t *)realloc(W->vals, W->val_tab.cap * sizeof(*p));
        if (!p) { W->off = 1; return; }
        W->vals = p;
        W->cap_vals = W->val_tab.cap;
    }
    bbw_val_t *v = &W->vals[i];
    memset(v, 0, sizeof(*v));
    v->bound = bound;
    if (!F) return;
    if (W->n_terms + F->n > W->cap_terms) {
        uint32_t cap = W->cap_terms ? W->cap_terms : 1024;
        while (cap < W->n_terms + F->n) cap *= 2;
        bbw_key_t *t = (bbw_key_t *)realloc(W->terms, cap * sizeof(*t));
        if (!t) return;
        W->terms = t;
        uint64_t *cf = (uint64_t *)realloc(W->coefs, cap * sizeof(*cf));
        if (!cf) return;
        W->coefs = cf;
        W->cap_terms = cap;
    }
    memcpy(W->terms + W->n_terms, F->t, F->n * sizeof(bbw_key_t));
    memcpy(W->coefs + W->n_terms, F->c, F->n * sizeof(uint64_t));
    v->start = W->n_terms;
    v->n = F->n;
    v->mod = F->mod;
    v->k = F->k;
    v->has_form = 1;
    W->n_terms += F->n;
}

void dvs_bbword_note_add(dvs_bbword_t *W, dvs_bv_t a, dvs_bv_t b, dvs_bv_t r) {
    if (!W || W->off) return;
    bbw_key_t ka = _strip(_key(a)), kb = _strip(_key(b));
    uint64_t ba, bb_;
    if (!_bound(W, ka, &ba) || !_bound(W, kb, &bb_)) return;
    uint64_t s;
    if (dvs_add_u64_ovf(ba, bb_, &s)) return;
    if (s > _mask(r.size)) return;                     /* may wrap */
    bbw_form_t F, G, H;
    _form(W, ka, &F);
    _form(W, kb, &G);
    int ok = _form_add(&F, &G, &H);
    _remember_val(W, _strip(_key(r)), (uint64_t)s, ok ? &H : NULL);
}

void dvs_bbword_note_urem(dvs_bbword_t *W, dvs_bv_t a, dvs_bv_t b, dvs_bv_t r) {
    if (!W || W->off) return;
    uint64_t m;
    if (!_const_val(_key(b), &m) || m == 0) return;   /* x urem 0 = x */
    bbw_key_t ka = _strip(_key(a));
    uint64_t bound = m - 1, ba;
    if (_bound(W, ka, &ba) && ba < bound) bound = ba;
    bbw_form_t F;
    _form(W, ka, &F);
    _reduce(&F, _gcd(F.mod, m));
    _remember_val(W, _strip(_key(r)), bound, &F);
}

void dvs_bbword_note_extract(dvs_bbword_t *W, dvs_bv_t a, uint32_t hi, uint32_t lo,
                             dvs_bv_t r) {
    if (!W || W->off || lo != 0) return;
    bbw_key_t ka = _strip(_key(a));
    uint64_t ba;
    if (!_bound(W, ka, &ba)) return;
    if (hi + 1 < 64 && ba > _mask(hi + 1)) return;     /* drops value bits */
    bbw_form_t F;
    _form(W, ka, &F);
    _remember_val(W, _strip(_key(r)), ba, &F);
}

/* ------------------------------------------------------------- shape facts */

/* v = poly·base: the remembered shape, or 1·v. */
static void _lin_of(const dvs_bbword_t *W, bbw_key_t v, uint64_t *poly,
                    bbw_key_t *base) {
    const bbw_shape_t *s = _shape(W, v);
    if (s && s->has_lin) { *poly = s->poly; *base = s->lbase; return; }
    *poly = 1;
    *base = v;
}

static void _mul_of(const dvs_bbword_t *W, bbw_key_t v, uint64_t *c,
                    bbw_key_t *base) {
    const bbw_shape_t *s = _shape(W, v);
    if (s && s->has_mul) { *c = s->mulc; *base = s->mbase; return; }
    *c = 1;
    *base = v;
}

static void _remember_lin(dvs_bbword_t *W, bbw_key_t v, uint64_t poly, bbw_key_t base) {
    bbw_shape_t *s = _shape_slot(W, v);
    if (!s || s->has_lin) return;
    s->poly = poly;
    s->lbase = base;
    s->has_lin = 1;
}

static void _remember_mul(dvs_bbword_t *W, bbw_key_t v, uint64_t c, bbw_key_t base) {
    bbw_shape_t *s = _shape_slot(W, v);
    if (!s || s->has_mul) return;
    s->mulc = c;
    s->mbase = base;
    s->has_mul = 1;
}

void dvs_bbword_note_shr(dvs_bbword_t *W, dvs_bv_t a, dvs_bv_t b, dvs_bv_t r) {
    if (!W || W->off || a.size > 64 || r.size != a.size) return;
    uint64_t k;
    if (!_const_val(_key(b), &k) || k >= a.size) return;
    uint64_t p;
    bbw_key_t u;
    _lin_of(W, _key(a), &p, &u);
    _remember_lin(W, _key(r), (p << k) & _mask(a.size), u);
}

dvs_bv_t dvs_bbword_xor(dvs_bbword_t *W, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    if (!W || W->off || a.size > 64 || a.size != b.size) return dvs_bb_xor(bb, a, b);
    uint64_t pa, pb;
    bbw_key_t ua, ub;
    _lin_of(W, _key(a), &pa, &ua);
    _lin_of(W, _key(b), &pb, &ub);
    if (!_key_eq(ua, ub)) return dvs_bb_xor(bb, a, b);
    uint64_t p = pa ^ pb;
    if (p == 1) { W->cancelled++; return _bv(ua); }
    if (p == 0) { W->cancelled++; return dvs_bb_value_u64(bb, a.size, 0); }
    dvs_bv_t r = dvs_bb_xor(bb, a, b);
    _remember_lin(W, _key(r), p, ua);
    return r;
}

dvs_bv_t dvs_bbword_mul(dvs_bbword_t *W, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    if (!W || W->off || a.size > 64 || a.size != b.size) return dvs_bb_mul(bb, a, b);
    uint64_t c;
    dvs_bv_t x = a;
    if (!_const_val(_key(b), &c)) {
        if (!_const_val(_key(a), &c)) return dvs_bb_mul(bb, a, b);
        x = b;
    }
    uint32_t w = a.size;
    uint64_t mc;
    bbw_key_t u;
    _mul_of(W, _key(x), &mc, &u);
    uint64_t c2 = (mc * c) & _mask(w);
    if (c2 == 1) { W->cancelled++; return _bv(u); }
    dvs_bv_t r = dvs_bb_mul(bb, a, b);
    if (c2) _remember_mul(W, _key(r), c2, u);
    /* A multiply that cannot wrap scales the value's form. */
    bbw_key_t kx = _strip(_key(x));
    uint64_t bx;
    uint64_t p;
    if (c && _bound(W, kx, &bx) && !dvs_mul_u64_ovf(bx, c, &p) && p <= _mask(w)) {
        bbw_form_t F;
        _form(W, kx, &F);
        _remember_val(W, _strip(_key(r)), bx * c, _form_scale(&F, c) ? &F : NULL);
    }
    return r;
}

/* ------------------------------------------------------------- equality */

dvs_bv_t dvs_bbword_eq(dvs_bbword_t *W, dvs_bitblast_t *bb, dvs_bv_t a, dvs_bv_t b) {
    if (!W || W->off || a.size != b.size) return dvs_bb_eq(bb, a, b);
    bbw_key_t ka = _key(a), kb = _key(b);
    /* Peel invertible shapes common to both sides: f(x) = f(y) iff x = y. */
    for (int it = 0; it < 256 && !_key_eq(ka, kb); it++) {
        const bbw_shape_t *sa = _shape(W, ka), *sb = _shape(W, kb);
        if (!sa || !sb) break;
        if (sa->has_mul && sb->has_mul && sa->mulc == sb->mulc && (sa->mulc & 1)) {
            ka = sa->mbase;
            kb = sb->mbase;
        } else if (sa->has_lin && sb->has_lin && sa->poly == sb->poly &&
                   (sa->poly & 1)) {
            ka = sa->lbase;
            kb = sb->lbase;
        } else {
            break;
        }
        W->peeled++;
    }
    if (_key_eq(ka, kb)) { W->decided++; return dvs_bb_value_u64(bb, 1, 1); }
    /* Two values below a common modulus with congruent forms are equal. */
    bbw_key_t va = _strip(ka), vb = _strip(kb);
    uint64_t ba, bb_;
    if (_bound(W, va, &ba) && _bound(W, vb, &bb_)) {
        bbw_form_t F, G;
        _form(W, va, &F);
        _form(W, vb, &G);
        uint64_t d = _gcd(F.mod, G.mod);
        int same = 0;
        if (d == 0) {
            same = _form_eq(&F, &G);
        } else if (d > 1 && ba < d && bb_ < d) {
            _reduce(&F, d);
            _reduce(&G, d);
            same = _form_eq(&F, &G);
        }
        if (same) { W->decided++; return dvs_bb_value_u64(bb, 1, 1); }
    }
    return dvs_bb_eq(bb, _bv(ka), _bv(kb));
}
