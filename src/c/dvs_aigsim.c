#include "dvs_aigsim.h"

#include <stdlib.h>
#include <string.h>

/* Lane patterns for the first six inputs of the exhaustive sweep: lane k gives
 * input i the value of bit i of k. */
static const uint64_t LANE_PAT[6] = {
    0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
    0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull,
};

/* Words per simulated node: each pass simulates W*64 assignments. Eight words
 * (one cache line) amortize the per-gate index loads and let the compiler
 * vectorize: crc32_affine3's sweep takes 143 ms at two words, 106 at four, 83
 * at eight, 68 at sixteen. */
#define LOG_W 3
#define W (1 << LOG_W)
#define LANES (W * 64)
#define IN_PASS (6 + LOG_W)    /* inputs enumerated within one pass */

/* The cone is renumbered densely: slot 0 is TRUE, then the inputs, then the
 * gates in ascending (topological) id order. An operand is packed as
 * slot << 1 | negated. */
struct dvs_aigsim_s {
    uint32_t        N;          /* AIG nodes                               */
    dvs_aig_node_t *L, *R;      /* children of every AND by id (0 others)  */
    uint32_t       *inputs;     /* cone input ids, ascending               */
    uint32_t        n_inputs;
    uint32_t       *gate_id;    /* cone AND ids, ascending                 */
    uint32_t       *ga, *gb;    /* packed operands of each cone gate       */
    uint32_t        n_gates;
    uint32_t       *asl;        /* packed assertions                       */
    dvs_aig_node_t *asserts;
    uint32_t        n_asserts;
    uint64_t       *val;        /* W words per slot                        */
    uint32_t        hit_lane;   /* a lane of the last pass that satisfies  */
    int             has_hit;
    int             hit_exh;    /* the hit is hit_index, not hit_lane      */
    uint64_t        hit_index;  /* exhaustive: input k is bit k            */
    uint64_t        vectors;
};

static uint64_t _rng(uint64_t *s) {
    uint64_t x = (*s += 0x9E3779B97F4A7C15ull);
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static inline int _bit(const int8_t *nv, dvs_aig_node_t lit) {
    uint32_t i = (uint32_t)(lit < 0 ? -lit : lit);
    return lit < 0 ? !nv[i] : nv[i];
}

void dvs_aigsim_free(dvs_aigsim_t *s) {
    if (!s) return;
    free(s->L); free(s->R); free(s->inputs); free(s->gate_id);
    free(s->ga); free(s->gb); free(s->asl); free(s->asserts); free(s->val);
    free(s);
}

dvs_aigsim_t *dvs_aigsim_new(const dvs_aig_t *m, const dvs_aig_node_t *asserts,
                             uint32_t n_asserts) {
    uint64_t nn = dvs_aig_num_nodes(m);
    if (nn == 0 || nn > 0x3FFFFFF0ull) return NULL;
    dvs_aigsim_t *s = (dvs_aigsim_t *)calloc(1, sizeof *s);
    if (!s) return NULL;
    s->N = (uint32_t)nn;
    s->n_asserts = n_asserts;
    s->asserts = (dvs_aig_node_t *)malloc(((size_t)n_asserts + 1) * sizeof(dvs_aig_node_t));
    s->asl = (uint32_t *)malloc(((size_t)n_asserts + 1) * sizeof(uint32_t));
    s->L = (dvs_aig_node_t *)calloc((size_t)s->N + 1, sizeof(dvs_aig_node_t));
    s->R = (dvs_aig_node_t *)calloc((size_t)s->N + 1, sizeof(dvs_aig_node_t));
    uint32_t *slot = (uint32_t *)calloc((size_t)s->N + 1, sizeof(uint32_t)); /* 0: not in cone */
    uint32_t *stack = (uint32_t *)malloc(((size_t)s->N + 1) * sizeof(uint32_t));
    if (!s->asserts || !s->asl || !s->L || !s->R || !slot || !stack) goto fail;
    if (n_asserts) memcpy(s->asserts, asserts, n_asserts * sizeof(dvs_aig_node_t));

    /* Every AND's children (the full model needs them), then the cone of
     * influence of the assertions (slot = 1 marks membership for now). */
    for (uint32_t id = 2; id <= s->N; id++)
        if (dvs_aig_is_and(m, (dvs_aig_node_t)id))
            dvs_aig_get_children(m, (dvs_aig_node_t)id, &s->L[id], &s->R[id]);
    uint32_t sp = 0;
    for (uint32_t a = 0; a < n_asserts; a++) {
        uint32_t i = (uint32_t)(asserts[a] < 0 ? -asserts[a] : asserts[a]);
        if (i > s->N) goto fail;
        if (i > 1 && !slot[i]) { slot[i] = 1; stack[sp++] = i; }
    }
    while (sp) {
        uint32_t id = stack[--sp];
        if (!s->L[id]) { s->n_inputs++; continue; }
        s->n_gates++;
        dvs_aig_node_t ch[2] = { s->L[id], s->R[id] };
        for (int c = 0; c < 2; c++) {
            uint32_t j = (uint32_t)(ch[c] < 0 ? -ch[c] : ch[c]);
            if (j > 1 && !slot[j]) { slot[j] = 1; stack[sp++] = j; }
        }
    }
    s->inputs  = (uint32_t *)malloc(((size_t)s->n_inputs + 1) * sizeof(uint32_t));
    s->gate_id = (uint32_t *)malloc(((size_t)s->n_gates + 1) * sizeof(uint32_t));
    s->ga      = (uint32_t *)malloc(((size_t)s->n_gates + 1) * sizeof(uint32_t));
    s->gb      = (uint32_t *)malloc(((size_t)s->n_gates + 1) * sizeof(uint32_t));
    s->val     = (uint64_t *)calloc(((size_t)s->n_inputs + s->n_gates + 1) * W, sizeof(uint64_t));
    if (!s->inputs || !s->gate_id || !s->ga || !s->gb || !s->val) goto fail;
    {
        /* Slots: inputs first, then gates; TRUE (id 1) is slot 0. */
        uint32_t ni = 0, ng = 0;
        for (uint32_t id = 2; id <= s->N; id++)
            if (slot[id] && !s->L[id]) { s->inputs[ni] = id; slot[id] = 1 + ni++; }
        for (uint32_t id = 2; id <= s->N; id++)
            if (slot[id] && s->L[id]) { s->gate_id[ng] = id; slot[id] = 1 + s->n_inputs + ng++; }
        slot[1] = 0;
#define PACK(lit) ((slot[(lit) < 0 ? -(lit) : (lit)] << 1) | ((lit) < 0 ? 1u : 0u))
        for (uint32_t k = 0; k < s->n_gates; k++) {
            uint32_t id = s->gate_id[k];
            s->ga[k] = PACK(s->L[id]);
            s->gb[k] = PACK(s->R[id]);
        }
        for (uint32_t a = 0; a < n_asserts; a++) s->asl[a] = PACK(asserts[a]);
#undef PACK
    }
    for (int w = 0; w < W; w++) s->val[w] = ~0ull;              /* TRUE */
    free(slot); free(stack);
    return s;
fail:
    free(slot); free(stack);
    dvs_aigsim_free(s);
    return NULL;
}

uint32_t dvs_aigsim_inputs(const dvs_aigsim_t *s)  { return s->n_inputs; }
uint32_t dvs_aigsim_gates(const dvs_aigsim_t *s)   { return s->n_gates; }
uint64_t dvs_aigsim_vectors(const dvs_aigsim_t *s) { return s->vectors; }

uint64_t dvs_aigsim_exhaustive_work(const dvs_aigsim_t *s) {
    if (s->n_inputs > 32) return UINT64_MAX;
    /* In 64-assignment gate-word evaluations, as decision D6 counts them. */
    uint64_t blocks = s->n_inputs > 6 ? (1ull << (s->n_inputs - 6)) : 1;
    return blocks * ((uint64_t)s->n_gates + s->n_asserts);
}

static inline uint64_t *_v(dvs_aigsim_t *s, uint32_t slot) { return s->val + (size_t)slot * W; }

/* Evaluate the cone for the inputs already in val[]; out[] gets the lanes
 * where every assertion holds. Returns non-zero when any lane does. */
static int _pass(dvs_aigsim_t *s, uint64_t out[W]) {
    uint64_t *restrict val = s->val;
    const uint32_t *ga = s->ga, *gb = s->gb;
    uint64_t *dst = val + (size_t)(1 + s->n_inputs) * W;
    for (uint32_t k = 0; k < s->n_gates; k++, dst += W) {
        uint32_t a = ga[k], b = gb[k];
        const uint64_t *pa = val + (size_t)(a >> 1) * W, *pb = val + (size_t)(b >> 1) * W;
        uint64_t ma = (uint64_t)0 - (a & 1u), mb = (uint64_t)0 - (b & 1u);
        for (int w = 0; w < W; w++) dst[w] = (pa[w] ^ ma) & (pb[w] ^ mb);
    }
    uint64_t any = 0;
    for (int w = 0; w < W; w++) out[w] = ~0ull;
    for (uint32_t i = 0; i < s->n_asserts; i++) {
        uint32_t a = s->asl[i];
        const uint64_t *pa = val + (size_t)(a >> 1) * W;
        uint64_t ma = (uint64_t)0 - (a & 1u);
        for (int w = 0; w < W; w++) out[w] &= pa[w] ^ ma;
    }
    for (int w = 0; w < W; w++) any |= out[w];
    return any != 0;
}

static void _record_hit(dvs_aigsim_t *s, const uint64_t ok[W]) {
    for (int w = 0; w < W; w++) {
        if (ok[w]) {
            s->hit_lane = (uint32_t)(w * 64 + __builtin_ctzll(ok[w]));
            s->has_hit = 1;
            return;
        }
    }
}

int dvs_aigsim_exhaustive(dvs_aigsim_t *s, const volatile int *stop, uint64_t seed) {
    uint32_t n = s->n_inputs;
    if (n > 32) return DVS_AIGSIM_UNDECIDED;
    /* Lane L (word L/64, bit L%64) gives input k bit k of L for k < IN_PASS:
     * the first six vary within a word, the next LOG_W across the words. The
     * rest take their bits from the pass number. */
    for (uint32_t k = 0; k < n && k < IN_PASS; k++) {
        uint64_t *v = _v(s, 1 + k);
        for (int w = 0; w < W; w++)
            v[w] = k < 6 ? LANE_PAT[k] : (((uint32_t)w >> (k - 6)) & 1u) ? ~0ull : 0;
    }
    uint64_t passes = n > IN_PASS ? (1ull << (n - IN_PASS)) : 1;
    uint32_t live = n >= IN_PASS ? LANES : (1u << n);  /* lanes that are distinct */
    uint64_t mask[W];
    for (int w = 0; w < W; w++) {
        uint32_t lo = (uint32_t)w * 64;
        mask[w] = live >= lo + 64 ? ~0ull : live > lo ? ((1ull << (live - lo)) - 1) : 0;
    }
    uint64_t ok[W], st = seed, seen = 0;
    for (uint64_t b = 0; b < passes; b++) {
        if (stop && *stop) return DVS_AIGSIM_UNDECIDED;
        for (uint32_t k = IN_PASS; k < n; k++) {
            uint64_t *v = _v(s, 1 + k), x = ((b >> (k - IN_PASS)) & 1u) ? ~0ull : 0;
            for (int w = 0; w < W; w++) v[w] = x;
        }
        int any = _pass(s, ok);
        s->vectors += live;
        if (any) {
            uint64_t m = 0;
            for (int w = 0; w < W; w++) { ok[w] &= mask[w]; m |= ok[w]; }
            if (!m) continue;
            if (!seed) {
                _record_hit(s, ok);
                s->hit_exh = 1;
                s->hit_index = s->hit_lane | (b << IN_PASS);
                return DVS_AIGSIM_SAT;
            }
            /* Reservoir sampling by pass: this pass's c hits replace the kept
             * one with probability c/seen, then one of them uniformly, so the
             * assignment left is uniform over every satisfying one. */
            uint64_t c = 0;
            for (int w = 0; w < W; w++) c += (uint64_t)__builtin_popcountll(ok[w]);
            seen += c;
            uint64_t r = _rng(&st) % seen;
            if (r >= c) continue;
            for (int w = 0; w < W; w++) {
                uint64_t x = ok[w];
                uint64_t k = (uint64_t)__builtin_popcountll(x);
                if (r >= k) { r -= k; continue; }
                while (r--) x &= x - 1;
                s->has_hit = s->hit_exh = 1;
                s->hit_index = ((uint64_t)w * 64 + (uint64_t)__builtin_ctzll(x)) | (b << IN_PASS);
                break;
            }
        }
    }
    return seen ? DVS_AIGSIM_SAT : DVS_AIGSIM_UNSAT;
}

int dvs_aigsim_random(dvs_aigsim_t *s, uint32_t blocks, uint64_t seed) {
    uint64_t st = seed ^ 0x5DEECE66Dull, ok[W];
    uint32_t passes = (blocks + W - 1) / W;
    for (uint32_t b = 0; b < passes; b++) {
        for (uint32_t k = 0; k < s->n_inputs; k++) {
            uint64_t *v = _v(s, 1 + k);
            for (int w = 0; w < W; w++) v[w] = _rng(&st);
        }
        int any = _pass(s, ok);
        s->vectors += LANES;
        if (any) { _record_hit(s, ok); return DVS_AIGSIM_SAT; }
    }
    return DVS_AIGSIM_UNDECIDED;
}

int8_t *dvs_aigsim_model(const dvs_aigsim_t *s) {
    if (!s->has_hit) return NULL;
    uint32_t w = s->hit_lane / 64, bit = s->hit_lane % 64;
    int8_t *nv = (int8_t *)calloc((size_t)s->N + 1, 1);
    if (!nv) return NULL;
    if (s->N >= 1) nv[1] = 1;                                   /* TRUE */
    for (uint32_t k = 0; k < s->n_inputs; k++)
        nv[s->inputs[k]] = s->hit_exh ? (int8_t)((s->hit_index >> k) & 1u)
                         : (int8_t)((s->val[(size_t)(1 + k) * W + w] >> bit) & 1u);
    for (uint32_t id = 2; id <= s->N; id++)
        if (s->L[id]) nv[id] = (int8_t)(_bit(nv, s->L[id]) & _bit(nv, s->R[id]));
    /* The lane satisfied every assertion; re-check on the scalar valuation. */
    for (uint32_t a = 0; a < s->n_asserts; a++)
        if (!_bit(nv, s->asserts[a])) { free(nv); return NULL; }
    return nv;
}
