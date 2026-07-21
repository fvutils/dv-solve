/*
 * zsp_cube — cube-and-conquer driver over the bit-blast engine.
 *
 * Phases (docs/cube_and_conquer_design.md):
 *   P0/P1 — sequential search-space partitioning: bit-blast once, split the
 *           space into cubes (structural k-way `or` split, else a high-fanout
 *           bit), solve each cube over the shared instance under a conflict
 *           budget, dynamically re-splitting budget-hit cubes.
 *   P2 (this file) — PARALLEL conquer. Bit-blast once, clone the CNF into one
 *           private solver per worker thread, and fan the cube frontier across
 *           cores with first-SAT-wins. Threads (not fork) so it ports to
 *           native Windows via the zsp_thread layer; each worker owns its own
 *           CaDiCaL instance (instances are not thread-safe to share) built by
 *           replaying the recorded clause DB, so the expensive encode is paid
 *           once while every thread solves independently.
 *
 * Soundness contract (never a wrong verdict), identical across sequential and
 * parallel paths:
 *   - SAT   as soon as any cube is SAT (its model models the original). The
 *           winning worker's assignment is installed as this problem's model.
 *   - UNSAT only if the queue drains with EVERY cube proved UNSAT — the
 *           exhaustive partition is fully refuted and no region was abandoned.
 *   - UNKNOWN if any region was abandoned (max depth / cube cap / no split
 *           literal / resource failure) — never a claimed UNSAT we did not
 *           prove. The parallel path degrades to the sequential/single-shot
 *           path whenever cloning or threading is unavailable, so `cube` is
 *           never worse than plain bitblast.
 */

#include "zsp_cube.h"
#include "zsp_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tunables (env-overridable). Conservative defaults: a cheap first pass over
 * the initial partition, then geometric budget growth on the survivors. */
#define CUBE_BUDGET0_DEFAULT   8000u    /* conflicts for a depth-0 cube        */
#define CUBE_GROWTH_DEFAULT     6u      /* budget ×= growth per re-split depth */
#define CUBE_MAXDEPTH_DEFAULT   24u     /* max extra bit-splits per cube       */
/* Cumulative cube backstop. Must exceed CUBE_CART_CAP_DEFAULT with headroom, or
 * a full cartesian seed would leave no budget for adaptive k-way deepening. */
#define CUBE_MAXCUBES_DEFAULT   4000000u /* total-cubes backstop (termination) */
#define CUBE_ORSPLIT_CAP        4096u   /* max disjuncts we form cubes from    */
#define CUBE_BITS_CAP           8192u   /* candidate re-split bit literals     */
#define CUBE_MAX_WORKERS        256u    /* hard cap on the worker pool         */

/* P2.5 cartesian-`or` split. Fold several narrow top-level `or`s into a product
 * frontier so each cube fixes MANY disjuncts (small residual), capping the cube
 * count so the product cannot blow up. The default folds ~5 narrow ors on the
 * mcm targets (e.g. 68 → 7x11x11x11x11 = 102487 seed cubes): enough constraint
 * that each cube solves in ~1s (vs ~15s for a 3-or cube), so the pool sweeps
 * and prunes fast enough to hit a SAT cube — this is what cracks mcm/68. */
#define CUBE_CART_CAP_DEFAULT   200000u /* max product cubes to enumerate      */
#define CUBE_CART_MAXORS_DEF    64u     /* max `or` groups to consider folding */
#define CUBE_CART_PEROR_CAP     1024u   /* max arity of a foldable `or`        */
#define CUBE_CART_LITS_CAP     32768u   /* flat disjunct-literal buffer size   */

/* P3 lookahead deepening. When a cube hits its budget, before blindly pinning
 * the next held `or` k-way, PROBE each disjunct with a tiny conflict budget:
 * drop the ones that are locally UNSAT under the cube (sound — that sub-region
 * is empty), catch a SAT probe as a win, and if EVERY disjunct is refuted prove
 * the whole cube UNSAT. This collapses a k-way pin (arity 16/34 on mcm/140/176)
 * to only the live disjuncts — often a 2–3-way split or a forced unit — which
 * is the granularity lever the arity-heavy targets need (their narrowest `or`
 * is too wide for the P2.5 cartesian fold alone). See the design doc §12 P3. */
#define CUBE_LABUDGET_DEFAULT   2000u   /* conflict budget for one probe        */
#define CUBE_LAMAX_DEFAULT      64u     /* max disjuncts probed per deepening   */

/* P3b diversified portfolio. The cube pool is complete for UNSAT (exhaustive
 * drain) but finds SAT only when a worker reaches a satisfying cube — and on the
 * hard mcm targets (all SAT) the satisfying cube sits far down a huge FIFO
 * partition, so the pool grinds UNSAT cubes without reaching it. A portfolio
 * side-channel attacks the FULL instance directly: each portfolio worker gets a
 * distinct RNG seed + initial phase (via zsp_sat_set_seed) and solves with no
 * cube assumptions. SAT runtimes are heavy-tailed, so the MINIMUM over several
 * diversified full-instance solves often lands far below the median — one lucky
 * worker finds a model the cube pool would take ages to reach. It composes with
 * the pool and preserves the contract: a full-instance SAT is a global model, a
 * full-instance UNSAT is an authoritative global UNSAT (independent of the cube
 * partition), and a chunk-exhausted UNKNOWN just loops. See the design doc §12
 * P3b. Portfolio workers are carved out of the worker pool (always leaving >=1
 * cube worker so the exhaustive-UNSAT contract and queue servicing hold). */
#define CUBE_PORT_CHUNK_DEFAULT 200000u /* conflicts per portfolio solve chunk  */

static uint32_t env_u32(const char *name, uint32_t def) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end = NULL;
    unsigned long x = strtoul(v, &end, 10);
    if (end == v) return def;
    return (uint32_t)x;
}

/* Worker count: DV_PARALLEL if set, else cores-2 (matching the workflow cap
 * convention), clamped to [1, CUBE_MAX_WORKERS]. */
static uint32_t worker_count(void) {
    const char *v = getenv("DV_PARALLEL");
    uint32_t w;
    if (v && *v) {
        w = env_u32("DV_PARALLEL", 1);
    } else {
        unsigned cpu = zsp_cpu_count();
        w = cpu > 2 ? cpu - 2 : 1;
    }
    if (w < 1) w = 1;
    if (w > CUBE_MAX_WORKERS) w = CUBE_MAX_WORKERS;
    return w;
}

/* Read-only set of top-level `or` groups (P2.5). `lits` is a flat buffer of all
 * disjunct assumption literals; group g occupies lits[off[g] .. off[g]+size[g]).
 * Groups are ordered narrowest-first. The conquer loop deepens a budget-hit cube
 * by pinning the next unpinned group k-way (exhaustive), so granularity grows
 * only where cubes are hard while the UNSAT contract is preserved. */
typedef struct {
    int32_t  *lits;
    uint32_t *off;
    uint32_t *size;
    uint32_t  ng;
    uint32_t  max_size;   /* widest group — sizes the re-split child buffer */
} or_set;

static void or_set_free(or_set *o) {
    if (!o) return;
    free(o->lits); free(o->off); free(o->size);
    o->lits = NULL; o->off = o->size = NULL; o->ng = o->max_size = 0;
}

/* One cube = a conjunction of assumption literals, plus its solve budget and
 * re-split depth (drives budget growth and the depth cap). `or_depth` counts how
 * many top-level `or` groups it has pinned (in or_set order); the next re-split
 * pins group `or_depth` k-way, falling back to a bit split once all are pinned. */
typedef struct {
    int32_t *lits;
    uint32_t n;
    uint32_t budget;
    uint32_t depth;
    uint32_t or_depth;
} cube_t;

/* A minimal FIFO queue of cubes (breadth-first, so the whole cheap initial
 * partition is tried before any expensive re-split escalation). */
typedef struct {
    cube_t  *v;
    uint32_t head, size, cap;
} cube_q;

static int q_push(cube_q *q, cube_t c) {
    if (q->size == q->cap) {
        uint32_t nc = q->cap ? q->cap * 2 : 64;
        cube_t *nv = (cube_t *)realloc(q->v, (size_t)nc * sizeof(cube_t));
        if (!nv) return -1;
        q->v = nv;
        q->cap = nc;
    }
    q->v[q->size++] = c;
    return 0;
}
static int q_empty(const cube_q *q) { return q->head >= q->size; }
static cube_t q_pop(cube_q *q) { return q->v[q->head++]; }

/* Build a child cube = parent.lits ∪ {extra}, one deeper, with a grown budget. */
static int make_child(cube_t parent, int32_t extra, uint32_t growth,
                      cube_t *out) {
    int32_t *lits = (int32_t *)malloc((size_t)(parent.n + 1) * sizeof(int32_t));
    if (!lits) return -1;
    if (parent.n) memcpy(lits, parent.lits, parent.n * sizeof(int32_t));
    lits[parent.n] = extra;
    uint64_t b = (uint64_t)parent.budget * growth;
    out->lits = lits;
    out->n = parent.n + 1;
    out->budget = b > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)b;
    out->depth = parent.depth + 1;
    return 0;
}

/* Pick a re-split bit literal not already pinned in `c` (by SAT variable).
 * Returns the literal (positive) or 0 if the candidate pool is exhausted. */
static int32_t pick_split_bit(const cube_t *c, const int32_t *bits,
                              uint32_t n_bits) {
    for (uint32_t i = 0; i < n_bits; i++) {
        int32_t var = bits[i];               /* candidates are positive vars */
        int used = 0;
        for (uint32_t j = 0; j < c->n; j++) {
            int32_t v = c->lits[j] < 0 ? -c->lits[j] : c->lits[j];
            if (v == var) { used = 1; break; }
        }
        if (!used) return var;
    }
    return 0;
}

/* Push a one-literal cube {lit} at depth 0 with the base budget. */
static int push_seed_cube(cube_q *q, int32_t lit, uint32_t budget) {
    int32_t *l = (int32_t *)malloc(sizeof(int32_t));
    if (!l) return -1;
    l[0] = lit;
    cube_t c = { l, 1, budget, 0, 0 };
    if (q_push(q, c) != 0) { free(l); return -1; }
    return 0;
}

/* Produce the re-split children of a budget-hit cube `c` into `out` (capacity
 * >= max(ors->max_size, 2)). Prefers an EXHAUSTIVE k-way expansion on the next
 * unpinned top-level `or` (each child pins one more disjunct → smaller residual,
 * partition stays exhaustive); falls back to a 2-way high-fanout bit split once
 * every `or` is pinned. Returns the child count, or 0 if the cube cannot be
 * split further (depth/cube caps, no split literal, or OOM) — caller then marks
 * the region unresolved. Does not free `c`; caller frees it and enqueues out[]. */
static uint32_t split_children(const cube_t *c, const or_set *ors,
                               const int32_t *bits, uint32_t n_bits,
                               uint32_t GROWTH, uint32_t MAXDEPTH,
                               uint32_t total_cubes, uint32_t MAXCUBES,
                               cube_t *out) {
    if (c->depth >= MAXDEPTH) return 0;

    /* k-way expansion on the next unpinned `or` (exhaustive cover). */
    if (ors && ors->off && c->or_depth < ors->ng) {
        uint32_t g = c->or_depth;
        uint32_t k = ors->size[g];
        if (k >= 2 && (uint64_t)total_cubes + k <= MAXCUBES) {
            uint32_t base = ors->off[g];
            for (uint32_t i = 0; i < k; i++) {
                if (make_child(*c, ors->lits[base + i], GROWTH, &out[i]) != 0) {
                    for (uint32_t j = 0; j < i; j++) free(out[j].lits);
                    return 0;
                }
                out[i].or_depth = c->or_depth + 1;
            }
            return k;
        }
    }

    /* Fallback: 2-way bit split on a not-yet-pinned high-fanout bit. */
    if ((uint64_t)total_cubes + 2 > MAXCUBES) return 0;
    int32_t sb = pick_split_bit(c, bits, n_bits);
    if (sb == 0) return 0;
    if (make_child(*c, sb, GROWTH, &out[0]) != 0) return 0;
    if (make_child(*c, -sb, GROWTH, &out[1]) != 0) { free(out[0].lits); return 0; }
    out[0].or_depth = out[1].or_depth = c->or_depth;
    return 2;
}

/* ------------------------------------------------------------------------- *
 * P3 lookahead deepening. A probe solves `cube ∧ extra` under a tiny conflict
 * budget; the two contexts (sequential primary instance / parallel worker
 * instance) supply it via a callback. Return codes are the shared
 * SAT/UNSAT/UNKNOWN triple (ZSP_BB_* == ZSP_SAT_* numerically).
 * ------------------------------------------------------------------------- */
typedef int (*probe_fn)(void *ctx, const int32_t *lits, uint32_t n,
                        uint32_t budget);

/* Sequential probe: reuse the primary bbsolver (assumptions retract, so the
 * instance stays reusable; a SAT probe leaves its model live in `bb`). */
static int probe_bb(void *ctx, const int32_t *lits, uint32_t n, uint32_t budget) {
    return zsp_bbsolver_solve_assuming((zsp_bbsolver_t *)ctx, lits, n, budget);
}

/* Parallel probe: reuse this worker's private CaDiCaL instance (the same one it
 * runs full cube solves on; assumptions retract per solve, learned clauses
 * carry over — beneficial and sound). A SAT probe leaves the model in `sat`,
 * exactly where install_worker_model reads a normal parallel win from. */
static int probe_sat(void *ctx, const int32_t *lits, uint32_t n, uint32_t budget) {
    zsp_sat_t *s = (zsp_sat_t *)ctx;
    zsp_sat_set_conflict_limit(s, budget);
    for (uint32_t i = 0; i < n; i++)
        zsp_sat_assume(s, (zsp_sat_lit_t)lits[i]);
    return zsp_sat_solve(s);
}

enum { LA_FALLBACK = 0, LA_SAT, LA_DEAD, LA_SPLIT };

/* Lookahead deepening of a budget-hit cube `c` on its next unpinned `or` group.
 * Probes each disjunct (a bounded prefix of at most `la_max`) under `la_budget`:
 *   - a SAT probe  → LA_SAT   (model is live in the probe context, `c` wins);
 *   - an UNSAT probe → the disjunct's sub-region is empty; DROP it (sound: the
 *     `or` is asserted, so surviving disjuncts still cover every model of `c`);
 *   - an UNKNOWN probe (or an un-probed tail disjunct) → keep it as a child.
 * If every disjunct is refuted → LA_DEAD: `c ∧ (or g)` is UNSAT and `(or g)` is
 * asserted, so `c` itself is UNSAT (a PROVEN prune, not an abandoned region).
 * Otherwise fills `out` (capacity >= ors->max_size) with the surviving children
 * (each pinning one disjunct, or_depth+1) and returns LA_SPLIT with `*nout` set.
 * LA_FALLBACK means "couldn't apply — caller should use split_children" (no
 * held `or`, depth/cube cap, or OOM); no verdict is implied. Never frees `c`. */
static int lookahead_children(const cube_t *c, const or_set *ors,
                              uint32_t GROWTH, uint32_t MAXDEPTH,
                              uint32_t total_cubes, uint32_t MAXCUBES,
                              uint32_t la_budget, uint32_t la_max,
                              probe_fn probe, void *ctx,
                              cube_t *out, uint32_t *nout) {
    *nout = 0;
    if (c->depth >= MAXDEPTH) return LA_FALLBACK;
    if (!(ors && ors->off && c->or_depth < ors->ng)) return LA_FALLBACK;
    uint32_t g = c->or_depth, k = ors->size[g];
    if (k < 2) return LA_FALLBACK;
    uint32_t base = ors->off[g];

    /* Probe buffer = c->lits ∪ {disjunct}. */
    int32_t *pl = (int32_t *)malloc((size_t)(c->n + 1) * sizeof(int32_t));
    if (!pl) return LA_FALLBACK;
    if (c->n) memcpy(pl, c->lits, c->n * sizeof(int32_t));

    uint32_t nprobe = k < la_max ? k : la_max;   /* probe a bounded prefix */
    uint32_t nsurv = 0;
    for (uint32_t i = 0; i < k; i++) {
        int32_t d = ors->lits[base + i];
        if (i < nprobe) {
            pl[c->n] = d;
            int r = probe(ctx, pl, c->n + 1, la_budget);
            if (r == ZSP_SAT_SAT)   { free(pl); return LA_SAT; }
            if (r == ZSP_SAT_UNSAT) continue;        /* dead disjunct: drop it */
        }
        /* UNKNOWN, or an un-probed tail disjunct: keep it live as a child. */
        if ((uint64_t)total_cubes + nsurv + 1 > MAXCUBES ||
            make_child(*c, d, GROWTH, &out[nsurv]) != 0) {
            for (uint32_t j = 0; j < nsurv; j++) free(out[j].lits);
            free(pl);
            return LA_FALLBACK;
        }
        out[nsurv].or_depth = c->or_depth + 1;
        nsurv++;
    }
    free(pl);
    if (nsurv == 0) return LA_DEAD;   /* every disjunct refuted → c is UNSAT */
    *nout = nsurv;
    return LA_SPLIT;
}

/* Build the initial partition into `q`. Prefers the widest top-level `or`
 * (k-way, exhaustive and SAT-directed); else a single high-fanout bit (2-way).
 * Returns the number of seed cubes pushed (>=1), or 0 if none could be formed
 * (caller falls back to a single unbounded solve). */
static uint32_t build_frontier(cube_q *q, const int32_t *orlits, uint32_t n_or,
                               const int32_t *bits, uint32_t n_bits,
                               uint32_t B0, int verbose) {
    uint32_t pushed = 0;
    if (n_or >= 2) {
        if (verbose) fprintf(stderr, "[cube] structural split: %u disjuncts\n", n_or);
        for (uint32_t i = 0; i < n_or; i++) {
            if (push_seed_cube(q, orlits[i], B0) != 0) break;
            pushed++;
        }
    } else if (n_bits >= 1) {
        if (verbose) fprintf(stderr, "[cube] bit split on var %d\n", bits[0]);
        for (int s = 0; s < 2; s++) {
            if (push_seed_cube(q, s ? -bits[0] : bits[0], B0) != 0) break;
            pushed++;
        }
    }
    return pushed;
}

/* One folded `or` group: where its disjunct literals live in the flat buffer,
 * and how many there are. */
typedef struct { uint32_t off, size; } or_group;

/* Compare groups by arity ascending (narrowest first): folding narrow ors first
 * maximizes how many ors (K) fit under the cube-count cap, so each product cube
 * fixes the most disjuncts and leaves the smallest residual. */
static int grp_cmp(const void *a, const void *b) {
    uint32_t sa = ((const or_group *)a)->size, sb = ((const or_group *)b)->size;
    return (sa > sb) - (sa < sb);
}

/* Build the P2.5 cartesian frontier + the shared `or_set` used for adaptive
 * deepening. Collects every usable top-level `or`, orders them narrowest-first
 * into `ors`, and SEEDS `q` with the cartesian product of the leading groups
 * that fit under `cube_cap` (one disjunct chosen per seeded group). Each seed
 * cube pins those groups (`or_depth = nfold`); a budget-hit cube later deepens
 * onto the remaining groups k-way (see split_children). Because every group is
 * folded IN FULL and each `or` is asserted, both the seed product and every
 * deepening step are EXHAUSTIVE covers → all-UNSAT still soundly implies UNSAT.
 *
 * Returns the number of seed cubes pushed (>=2), and fills `*ors` (caller frees
 * via or_set_free) and `*total_out`. Returns 0 (and leaves `*ors` empty) when
 * fewer than two groups exist — caller falls back to the single widest-`or`/bit
 * frontier, whose seeds have no or_set so deepening uses bit splits as before. */
static uint32_t build_cartesian_frontier(zsp_bbsolver_t *bb, cube_q *q,
                                         uint32_t cube_cap, uint32_t max_ors,
                                         uint32_t per_or_cap, uint32_t B0,
                                         or_set *ors, uint32_t *total_out,
                                         int verbose) {
    memset(ors, 0, sizeof(*ors));
    int32_t  *lits  = (int32_t *)malloc((size_t)CUBE_CART_LITS_CAP * sizeof(int32_t));
    uint32_t *sizes = (uint32_t *)malloc((size_t)max_ors * sizeof(uint32_t));
    if (!lits || !sizes) { free(lits); free(sizes); return 0; }

    uint32_t ng = zsp_bbsolver_or_groups(bb, lits, CUBE_CART_LITS_CAP,
                                         sizes, max_ors, per_or_cap);
    if (ng < 2) { free(lits); free(sizes); return 0; }

    /* Record each group's offset in the flat buffer, then order narrowest-first. */
    or_group *grp = (or_group *)malloc((size_t)ng * sizeof(or_group));
    if (!grp) { free(lits); free(sizes); return 0; }
    for (uint32_t g = 0, off = 0; g < ng; g++) {
        grp[g].off = off; grp[g].size = sizes[g]; off += sizes[g];
    }
    qsort(grp, ng, sizeof(or_group), grp_cmp);

    /* Materialize the sorted, compacted or_set (owned; drives deepening). */
    uint32_t total_lits = 0;
    for (uint32_t g = 0; g < ng; g++) total_lits += grp[g].size;
    ors->lits = (int32_t *)malloc((size_t)total_lits * sizeof(int32_t));
    ors->off  = (uint32_t *)malloc((size_t)ng * sizeof(uint32_t));
    ors->size = (uint32_t *)malloc((size_t)ng * sizeof(uint32_t));
    if (!ors->lits || !ors->off || !ors->size) {
        or_set_free(ors); free(grp); free(lits); free(sizes); return 0;
    }
    ors->ng = ng;
    for (uint32_t g = 0, w = 0; g < ng; g++) {
        ors->off[g] = w; ors->size[g] = grp[g].size;
        if (grp[g].size > ors->max_size) ors->max_size = grp[g].size;
        memcpy(&ors->lits[w], &lits[grp[g].off], grp[g].size * sizeof(int32_t));
        w += grp[g].size;
    }
    free(grp); free(lits); free(sizes);

    if (verbose) {
        fprintf(stderr, "[cube] or arities (%u):", ng);
        for (uint32_t g = 0; g < ng; g++) fprintf(stderr, " %u", ors->size[g]);
        fprintf(stderr, "\n");
    }

    /* Choose how many leading groups to fold into the SEED product (the rest are
     * held for adaptive deepening). Fold narrowest-first while product <= cap. */
    uint64_t product = 1;
    uint32_t nfold = 0;
    for (uint32_t g = 0; g < ng; g++) {
        uint32_t s = ors->size[g];
        if (s < 2) continue;
        if (product > (uint64_t)cube_cap / s) break;   /* product*s would exceed cap */
        nfold++;
        product *= s;
    }
    if (nfold < 1) { or_set_free(ors); return 0; }

    if (verbose) {
        fprintf(stderr, "[cube] cartesian seed: %u/%u ors folded -> %llu cubes, "
                "%u ors held for deepening\n",
                nfold, ng, (unsigned long long)product, ng - nfold);
    }

    /* Enumerate the mixed-radix seed product over groups [0, nfold): cube j
     * selects disjunct (j / place_g) % size_g from each. or_depth = nfold. */
    uint32_t pushed = 0;
    for (uint64_t j = 0; j < product; j++) {
        int32_t *cl = (int32_t *)malloc((size_t)nfold * sizeof(int32_t));
        if (!cl) break;
        uint64_t rem = j;
        for (uint32_t g = 0; g < nfold; g++) {
            uint32_t s = ors->size[g];
            cl[g] = ors->lits[ors->off[g] + (uint32_t)(rem % s)];
            rem /= s;
        }
        cube_t c = { cl, nfold, B0, 0, nfold };
        if (q_push(q, c) != 0) { free(cl); break; }
        pushed++;
    }

    if (pushed < 2) {          /* degenerate (single narrow or, or OOM): fall back */
        or_set_free(ors);
        if (total_out) *total_out = 0;
        return 0;
    }
    if (total_out) *total_out = pushed;
    return pushed;
}

/* ------------------------------------------------------------------------- *
 * Sequential conquer (P0/P1). Solves the frontier over the primary bbsolver
 * instance; validated path, used when there is nothing to gain from threads
 * (single worker, non-incremental backend, or the parallel setup failed).
 * Consumes `q` (frees every cube). Returns ZSP_BB_SAT/UNSAT/UNKNOWN/ERROR;
 * on SAT the model is live in `bb` (solve_assuming re-diversified it).
 * ------------------------------------------------------------------------- */
static int run_sequential(zsp_bbsolver_t *bb, cube_q *q, uint32_t total_cubes,
                          const or_set *ors, const int32_t *bits, uint32_t n_bits,
                          uint32_t GROWTH, uint32_t MAXDEPTH, uint32_t MAXCUBES,
                          int lookahead, uint32_t la_budget, uint32_t la_max,
                          int verbose) {
    int result = ZSP_BB_UNSAT;   /* provisional: promoted to UNKNOWN if abandoned */
    int unresolved = 0;
    uint64_t n_solves = 0;
    uint32_t kids_cap = (ors && ors->max_size > 2) ? ors->max_size : 2;
    cube_t  *kids = (cube_t *)malloc((size_t)kids_cap * sizeof(cube_t));
    if (!kids) return ZSP_BB_ERROR;

    while (!q_empty(q)) {
        cube_t c = q_pop(q);
        int rc = zsp_bbsolver_solve_assuming(bb, c.lits, c.n, c.budget);
        n_solves++;
        if (verbose)
            fprintf(stderr, "[cube] solve depth=%u ord=%u n=%u budget=%u -> %d\n",
                    c.depth, c.or_depth, c.n, c.budget, rc);

        if (rc == ZSP_BB_SAT)   { free(c.lits); result = ZSP_BB_SAT;   goto done; }
        if (rc == ZSP_BB_ERROR) { free(c.lits); result = ZSP_BB_ERROR; goto done; }
        if (rc == ZSP_BB_UNSAT) { free(c.lits); continue; }

        /* UNKNOWN (budget hit). First try lookahead deepening: probe the next
         * `or`'s disjuncts under a tiny budget so dead ones are pruned, a SAT
         * probe wins, and an all-dead `or` proves this cube UNSAT. Fall back to
         * the plain k-way/bit split when lookahead can't apply. All paths keep
         * the cover exhaustive. */
        uint32_t nk = 0;
        int la = LA_FALLBACK;
        if (lookahead)
            la = lookahead_children(&c, ors, GROWTH, MAXDEPTH, total_cubes,
                                    MAXCUBES, la_budget, la_max, probe_bb, bb,
                                    kids, &nk);
        if (la == LA_SAT)  { free(c.lits); result = ZSP_BB_SAT; goto done; }
        if (la == LA_DEAD) { free(c.lits); continue; }  /* proven UNSAT: prune */
        if (la != LA_SPLIT)                             /* FALLBACK: plain split */
            nk = split_children(&c, ors, bits, n_bits, GROWTH, MAXDEPTH,
                                total_cubes, MAXCUBES, kids);
        free(c.lits);
        if (nk == 0) { unresolved = 1; continue; }
        for (uint32_t i = 0; i < nk; i++) {
            if (q_push(q, kids[i]) != 0) {
                unresolved = 1;
                for (uint32_t j = i; j < nk; j++) free(kids[j].lits);
                break;
            }
            total_cubes++;
        }
    }

    if (result == ZSP_BB_UNSAT && unresolved) result = ZSP_BB_UNKNOWN;
done:
    while (!q_empty(q)) free(q_pop(q).lits);
    free(kids);
    if (verbose)
        fprintf(stderr, "[cube] seq done: result=%d solves=%llu cubes=%u\n",
                result, (unsigned long long)n_solves, total_cubes);
    return result;
}

/* ------------------------------------------------------------------------- *
 * Parallel conquer (P2). A shared work queue fanned across N worker threads;
 * each worker owns a private CaDiCaL instance cloned from the recorded clause
 * DB. First SAT wins and stops the pool; UNSAT prunes; budget-hit re-splits
 * (under lock) and re-enqueues. Sound termination: the pool finishes only when
 * the queue is empty AND no worker is mid-solve (so no re-split is pending).
 * ------------------------------------------------------------------------- */
typedef struct {
    zsp_mutex_t mtx;
    zsp_cond_t  cv;

    cube_q      q;             /* shared frontier (all access under mtx)       */
    uint32_t    active;        /* workers currently solving a popped cube      */
    uint32_t    total_cubes;   /* termination backstop                          */
    int         stop;          /* set on first SAT: workers drain and exit      */
    int         result;        /* ZSP_BB_SAT/UNSAT/UNKNOWN (final aggregate)    */
    int         unresolved;    /* a region abandoned → UNSAT becomes UNKNOWN    */
    int         port_unsat;    /* a portfolio worker proved the FULL instance   */
                               /* UNSAT — authoritative, overrides `unresolved` */
    zsp_sat_t  *winner;        /* winning worker's instance (model source)      */
    uint64_t    n_solves;      /* diagnostics                                   */

    /* read-only shared config */
    const or_set  *ors;        /* top-level `or` groups for k-way deepening    */
    const int32_t *bits;       uint32_t n_bits;
    uint32_t GROWTH, MAXDEPTH, MAXCUBES;
    int lookahead; uint32_t la_budget, la_max;   /* P3 lookahead deepening      */
    uint64_t seed_base;        /* portfolio worker i seeds off this (P3b)       */
    uint32_t port_chunk;       /* conflicts per portfolio solve chunk (P3b)     */
    const int32_t *db;         size_t db_n;   int32_t max_var;
    int verbose;
} cube_ctx;

typedef struct {
    cube_ctx  *ctx;
    zsp_sat_t *sat;            /* this worker's private instance (freed by main) */
} worker_arg;

static void *cube_worker(void *arg) {
    worker_arg *wa = (worker_arg *)arg;
    cube_ctx *C = wa->ctx;

    /* Build this worker's private instance by replaying the recorded clause DB.
     * Var ids are preserved, so cube literals computed on the primary instance
     * are valid here. A worker that can't build simply does no work (it never
     * touches `active`, so termination is unaffected). */
    zsp_sat_t *sat = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    if (!sat || !zsp_sat_is_incremental(sat)) {
        if (sat) zsp_sat_free(sat);
        wa->sat = NULL;
        return NULL;
    }
    zsp_sat_reserve(sat, (zsp_sat_var_t)C->max_var);
    for (size_t i = 0; i < C->db_n; i++)
        zsp_sat_add(sat, (zsp_sat_lit_t)C->db[i]);
    wa->sat = sat;

    /* Per-worker re-split child buffer (sized to the widest `or`). */
    uint32_t kids_cap = (C->ors && C->ors->max_size > 2) ? C->ors->max_size : 2;
    cube_t *kids = (cube_t *)malloc((size_t)kids_cap * sizeof(cube_t));
    if (!kids) { wa->sat = NULL; zsp_sat_free(sat); return NULL; }

    zsp_mutex_lock(&C->mtx);
    for (;;) {
        while (!C->stop && q_empty(&C->q) && C->active > 0)
            zsp_cond_wait(&C->cv, &C->mtx);
        if (C->stop) break;
        if (q_empty(&C->q) && C->active == 0) {
            /* Frontier fully explored and no solve outstanding: we're done. If it
             * drained with no region abandoned, the exhaustive partition is fully
             * refuted → global UNSAT; set stop so any portfolio workers wind down
             * too (the instance is proven unsat, they cannot find a model). If a
             * region was abandoned, leave stop clear so portfolio keeps hunting. */
            if (!C->unresolved && !C->stop) C->stop = 1;
            zsp_cond_broadcast(&C->cv);
            break;
        }
        if (q_empty(&C->q)) continue;   /* spurious/raced wake: re-check */

        cube_t c = q_pop(&C->q);
        C->active++;
        C->n_solves++;
        uint32_t tc_snap = C->total_cubes;   /* snapshot for the lookahead cap */
        zsp_mutex_unlock(&C->mtx);

        /* Solve original ∧ cube under the cube's conflict budget. Assumptions
         * retract after the solve, so the instance is reusable for the next. */
        zsp_sat_set_conflict_limit(sat, c.budget);
        for (uint32_t i = 0; i < c.n; i++)
            zsp_sat_assume(sat, (zsp_sat_lit_t)c.lits[i]);
        int rc = zsp_sat_solve(sat);

        /* On UNKNOWN, compute the deepening WHILE UNLOCKED — lookahead probes
         * are full solves, and holding the global lock across them would
         * serialize every worker. `active` stays incremented across this window
         * (bumped at pop above), so no other worker can observe an idle/empty
         * "done" state before the children land. */
        uint32_t nk = 0;
        int la = LA_FALLBACK;
        if (rc == ZSP_SAT_UNKNOWN && C->lookahead)
            la = lookahead_children(&c, C->ors, C->GROWTH, C->MAXDEPTH, tc_snap,
                                    C->MAXCUBES, C->la_budget, C->la_max,
                                    probe_sat, sat, kids, &nk);

        zsp_mutex_lock(&C->mtx);
        if (C->verbose)
            fprintf(stderr, "[cube] w solve depth=%u ord=%u n=%u budget=%u -> %d la=%d\n",
                    c.depth, c.or_depth, c.n, c.budget, rc, la);

        if (rc == ZSP_SAT_SAT || la == LA_SAT) {
            if (!C->stop) { C->result = ZSP_BB_SAT; C->winner = sat; C->stop = 1; }
            free(c.lits);
            C->active--;
            zsp_cond_broadcast(&C->cv);
            break;
        }
        if (rc == ZSP_SAT_UNSAT || la == LA_DEAD) {
            free(c.lits);            /* UNSAT, or lookahead proved the region empty */
            C->active--;
            zsp_cond_broadcast(&C->cv);   /* active changed: maybe now all-done */
            continue;
        }

        /* UNKNOWN and not resolved by lookahead: enqueue the deepened children.
         * lookahead already filled `kids` on LA_SPLIT; otherwise fall back to a
         * plain k-way/bit split (cheap, no solves — safe under the lock). */
        if (la != LA_SPLIT)
            nk = split_children(&c, C->ors, C->bits, C->n_bits, C->GROWTH,
                                C->MAXDEPTH, C->total_cubes, C->MAXCUBES, kids);
        free(c.lits);
        if (nk == 0) {
            C->unresolved = 1;
            C->active--;
            zsp_cond_broadcast(&C->cv);
            continue;
        }
        for (uint32_t i = 0; i < nk; i++) {
            if (q_push(&C->q, kids[i]) != 0) {
                C->unresolved = 1;
                for (uint32_t j = i; j < nk; j++) free(kids[j].lits);
                break;
            }
            C->total_cubes++;
        }
        C->active--;
        zsp_cond_broadcast(&C->cv);
    }
    zsp_mutex_unlock(&C->mtx);
    free(kids);
    return NULL;   /* keep `sat` alive; main reads the winner then frees all. */
}

/* ------------------------------------------------------------------------- *
 * P3b portfolio worker. Attacks the FULL instance (no cube assumptions) with a
 * distinct RNG seed + phase, in interruptible conflict-limited chunks so it
 * cooperates with the shared stop flag. A full-instance SAT is a global model
 * (winner installed exactly like a cube win); a full-instance UNSAT is an
 * authoritative global UNSAT regardless of the cube partition (port_unsat).
 * ------------------------------------------------------------------------- */
typedef struct {
    cube_ctx  *ctx;
    zsp_sat_t *sat;            /* this worker's private instance (freed by main) */
    uint64_t   seed;           /* distinct per worker — the diversity source     */
} port_arg;

static void *cube_portfolio_worker(void *arg) {
    port_arg *pa = (port_arg *)arg;
    cube_ctx *C = pa->ctx;

    /* Private instance from the recorded clause DB, exactly like a cube worker. */
    zsp_sat_t *sat = zsp_sat_new_backend(NULL, ZSP_SAT_BACKEND_CADICAL);
    if (!sat || !zsp_sat_is_incremental(sat)) {
        if (sat) zsp_sat_free(sat);
        pa->sat = NULL;
        return NULL;
    }
    /* CaDiCaL only accepts the "seed"/"phase" options right after init, before
     * any clause is added — so diversify FIRST, then replay the clause DB. */
    zsp_sat_set_seed(sat, pa->seed);   /* diversify decisions + initial phase */
    zsp_sat_reserve(sat, (zsp_sat_var_t)C->max_var);
    for (size_t i = 0; i < C->db_n; i++)
        zsp_sat_add(sat, (zsp_sat_lit_t)C->db[i]);
    pa->sat = sat;

    for (;;) {
        zsp_mutex_lock(&C->mtx);
        int stop = C->stop;
        zsp_mutex_unlock(&C->mtx);
        if (stop) break;

        /* One chunk over the full instance (no assumptions). Learned clauses
         * carry across chunks, so this resumes progress rather than restarting. */
        zsp_sat_set_conflict_limit(sat, C->port_chunk);
        int rc = zsp_sat_solve(sat);

        zsp_mutex_lock(&C->mtx);
        C->n_solves++;
        if (rc == ZSP_SAT_SAT) {
            if (!C->stop) { C->result = ZSP_BB_SAT; C->winner = sat; C->stop = 1; }
            if (C->verbose)
                fprintf(stderr, "[cube] portfolio SAT seed=%llu\n",
                        (unsigned long long)pa->seed);
            zsp_cond_broadcast(&C->cv);
            zsp_mutex_unlock(&C->mtx);
            break;
        }
        if (rc == ZSP_SAT_UNSAT) {
            /* Full instance is unsat — authoritative, overrides any abandoned
             * cube region. Record it so the aggregate is not downgraded. */
            if (!C->stop) {
                C->result = ZSP_BB_UNSAT; C->port_unsat = 1; C->stop = 1;
            }
            if (C->verbose)
                fprintf(stderr, "[cube] portfolio UNSAT seed=%llu\n",
                        (unsigned long long)pa->seed);
            zsp_cond_broadcast(&C->cv);
            zsp_mutex_unlock(&C->mtx);
            break;
        }
        /* UNKNOWN: chunk exhausted. Loop to keep grinding unless someone won. */
        int again = !C->stop;
        zsp_mutex_unlock(&C->mtx);
        if (!again) break;
    }
    return NULL;   /* keep `sat` alive; main reads the winner then frees all. */
}

/* Run the frontier in `q0` across `n_workers` threads. Consumes q0 (its cubes
 * are moved into the shared queue and freed by the pool). On SAT, installs the
 * winner's model into `bb`. Returns ZSP_BB_SAT/UNSAT/UNKNOWN/ERROR. */
static int run_parallel(zsp_bbsolver_t *bb, cube_q *q0, uint32_t total_cubes,
                        const or_set *ors, const int32_t *bits, uint32_t n_bits,
                        uint32_t GROWTH, uint32_t MAXDEPTH, uint32_t MAXCUBES,
                        int lookahead, uint32_t la_budget, uint32_t la_max,
                        uint32_t n_port, uint64_t seed_base, uint32_t port_chunk,
                        const int32_t *db, size_t db_n, int32_t max_var,
                        uint32_t n_workers, int verbose) {
    cube_ctx C;
    memset(&C, 0, sizeof(C));
    C.q          = *q0;          /* take ownership of the seeded frontier */
    C.total_cubes = total_cubes;
    C.result     = ZSP_BB_UNSAT; /* provisional; promoted to UNKNOWN if abandoned */
    C.ors = ors;
    C.bits = bits; C.n_bits = n_bits;
    C.GROWTH = GROWTH; C.MAXDEPTH = MAXDEPTH; C.MAXCUBES = MAXCUBES;
    C.lookahead = lookahead; C.la_budget = la_budget; C.la_max = la_max;
    C.seed_base = seed_base; C.port_chunk = port_chunk;
    C.db = db; C.db_n = db_n; C.max_var = max_var;
    C.verbose = verbose;
    memset(q0, 0, sizeof(*q0));   /* frontier now owned by C.q */

    /* Carve portfolio workers out of the pool, always leaving >=1 cube worker so
     * the exhaustive-UNSAT contract and queue servicing still hold (P3b). */
    if (n_port >= n_workers) n_port = n_workers - 1;
    uint32_t n_cube = n_workers - n_port;

    if (zsp_mutex_init(&C.mtx) != 0) { /* fall back handled by caller via ERROR */
        while (!q_empty(&C.q)) free(q_pop(&C.q).lits);
        free(C.q.v);
        return ZSP_BB_ERROR;
    }
    if (zsp_cond_init(&C.cv) != 0) {
        zsp_mutex_destroy(&C.mtx);
        while (!q_empty(&C.q)) free(q_pop(&C.q).lits);
        free(C.q.v);
        return ZSP_BB_ERROR;
    }

    worker_arg *wa = (worker_arg *)calloc(n_cube ? n_cube : 1, sizeof(worker_arg));
    port_arg   *pa = (port_arg *)calloc(n_port ? n_port : 1, sizeof(port_arg));
    zsp_thread_t *th  = (zsp_thread_t *)calloc(n_workers, sizeof(zsp_thread_t));
    if (!wa || !pa || !th) {
        free(wa); free(pa); free(th);
        zsp_cond_destroy(&C.cv); zsp_mutex_destroy(&C.mtx);
        while (!q_empty(&C.q)) free(q_pop(&C.q).lits);
        free(C.q.v);
        return ZSP_BB_ERROR;
    }

    if (verbose)
        fprintf(stderr, "[cube] parallel: %u cube + %u portfolio workers, "
                "%u seed cubes, %zu db lits\n", n_cube, n_port, C.q.size, db_n);

    /* Spawn cube workers first (th[0..n_cube)), then portfolio workers
     * (th[n_cube..n_cube+n_port)). Each portfolio worker gets a distinct seed
     * spread by the golden-ratio constant so their low bits (hence phases)
     * differ. On thread exhaustion we run with however many started. */
    uint32_t sp_cube = 0, sp_port = 0;
    for (uint32_t i = 0; i < n_cube; i++) {
        wa[i].ctx = &C;
        if (zsp_thread_create(&th[i], cube_worker, &wa[i]) == 0) sp_cube++;
        else break;
    }
    for (uint32_t i = 0; i < n_port; i++) {
        pa[i].ctx = &C;
        pa[i].seed = seed_base ^ ((uint64_t)(i + 1) * 0x9E3779B97F4A7C15ull);
        if (zsp_thread_create(&th[sp_cube + i], cube_portfolio_worker, &pa[i]) == 0)
            sp_port++;
        else break;
    }
    uint32_t spawned = sp_cube + sp_port;
    if (spawned == 0) {
        /* Could not start any worker: run the frontier sequentially instead so
         * we still return a sound verdict (never worse than bitblast). */
        cube_q q = C.q; memset(&C.q, 0, sizeof(C.q));
        free(wa); free(pa); free(th);
        zsp_cond_destroy(&C.cv); zsp_mutex_destroy(&C.mtx);
        int r = run_sequential(bb, &q, total_cubes, ors, bits, n_bits,
                               GROWTH, MAXDEPTH, MAXCUBES,
                               lookahead, la_budget, la_max, verbose);
        free(q.v);
        return r;
    }
    if (sp_cube == 0) {
        /* All cube spawns failed but portfolio started: without a cube worker the
         * queue never drains, so signal stop-on-idle is moot — portfolio alone is
         * still sound (each full solve is authoritative). Mark unresolved so a
         * provisional UNSAT can never be claimed from the undrained partition. */
        zsp_mutex_lock(&C.mtx);
        C.unresolved = 1;
        zsp_mutex_unlock(&C.mtx);
    }

    for (uint32_t i = 0; i < spawned; i++)
        zsp_thread_join(&th[i], NULL);

    int result = C.result;
    /* Downgrade a provisional (cube-drain) UNSAT to UNKNOWN only if a region was
     * abandoned AND no portfolio worker proved the full instance unsat. A
     * portfolio UNSAT is authoritative and stands regardless of the partition. */
    if (result == ZSP_BB_UNSAT && C.unresolved && !C.port_unsat)
        result = ZSP_BB_UNKNOWN;

    /* On SAT, snapshot the winner's model before any worker instance is freed.
     * The winner may be a cube worker or a portfolio worker; both leave a full
     * model in their instance (a portfolio solve has no assumptions to retract). */
    if (result == ZSP_BB_SAT && C.winner) {
        if (zsp_bbsolver_install_worker_model(bb, C.winner) != 0)
            result = ZSP_BB_UNKNOWN;   /* couldn't materialize the model: sound */
    }

    for (uint32_t i = 0; i < sp_cube; i++)
        if (wa[i].sat) zsp_sat_free(wa[i].sat);
    for (uint32_t i = 0; i < sp_port; i++)
        if (pa[i].sat) zsp_sat_free(pa[i].sat);

    if (verbose)
        fprintf(stderr, "[cube] par done: result=%d solves=%llu cubes=%u\n",
                result, (unsigned long long)C.n_solves, C.total_cubes);

    while (!q_empty(&C.q)) free(q_pop(&C.q).lits);
    free(C.q.v);
    free(wa); free(pa); free(th);
    zsp_cond_destroy(&C.cv);
    zsp_mutex_destroy(&C.mtx);
    return result;
}

int zsp_cube_check(zsp_bbsolver_t *bb, uint64_t seed) {
    if (!bb) return ZSP_BB_ERROR;

    int verbose = getenv("DV_CUBE_VERBOSE") != NULL;
    const uint32_t B0        = env_u32("DV_CUBE_BUDGET0",  CUBE_BUDGET0_DEFAULT);
    const uint32_t GROWTH    = env_u32("DV_CUBE_GROWTH",   CUBE_GROWTH_DEFAULT);
    const uint32_t MAXDEPTH  = env_u32("DV_CUBE_MAXDEPTH", CUBE_MAXDEPTH_DEFAULT);
    const uint32_t MAXCUBES  = env_u32("DV_CUBE_MAXCUBES", CUBE_MAXCUBES_DEFAULT);
    /* Lookahead deepening is OPT-IN (DV_CUBE_LOOKAHEAD=1): it only pays off when
     * an `or`'s disjuncts refute cheaply under the probe budget. On the mcm SAT
     * targets (arity-16/34 ors, ~8000 conflicts to refute) probes resolve
     * nothing and are pure overhead, so it stays off by default. See §12 P3. */
    const int      LOOKAHEAD = env_u32("DV_CUBE_LOOKAHEAD", 0) != 0;
    const uint32_t LABUDGET  = env_u32("DV_CUBE_LABUDGET", CUBE_LABUDGET_DEFAULT);
    const uint32_t LAMAX     = env_u32("DV_CUBE_LAMAX",    CUBE_LAMAX_DEFAULT);
    /* P3b diversified portfolio: DV_CUBE_PORTFOLIO workers attack the full
     * instance with distinct seeds instead of pulling cubes (default 0 = pure
     * cube pool). Carved from the pool, always leaving >=1 cube worker. */
    const uint32_t PORTFOLIO = env_u32("DV_CUBE_PORTFOLIO", 0);
    const uint32_t PORTCHUNK = env_u32("DV_CUBE_PORTCHUNK", CUBE_PORT_CHUNK_DEFAULT);
    const uint32_t NWORKERS  = worker_count();

    /* If we may go parallel, record the CNF now so the whole encode (and the
     * split-literal encoding done just after prepare) is captured for cloning
     * into per-worker instances. Harmless if we later fall back to sequential. */
    if (NWORKERS >= 2) zsp_bbsolver_record_clauses(bb);

    /* Bit-blast + CNF-encode once (the expensive step, shared across cubes). */
    int enc = zsp_bbsolver_prepare(bb, seed);
    if (enc != ZSP_BB_ENCODE_READY) {
        if (verbose) fprintf(stderr, "[cube] encode -> %d (no solve)\n", enc);
        return enc;   /* UNKNOWN / ERROR — verdict identical to plain bitblast */
    }

    /* Cube-and-conquer needs retractable per-solve assumptions (CaDiCaL).
     * Without it, one plain solve — identical to bitblast, never worse. */
    if (!zsp_bbsolver_is_incremental(bb)) {
        if (verbose)
            fprintf(stderr, "[cube] non-incremental backend; single-shot solve\n");
        return zsp_bbsolver_solve_assuming(bb, NULL, 0, /*conflict_limit=*/0);
    }

    /* Candidate re-split bit literals (also the fallback initial split). */
    int32_t *bits = (int32_t *)malloc((size_t)CUBE_BITS_CAP * sizeof(int32_t));
    uint32_t n_bits = bits ? zsp_bbsolver_split_lits(bb, bits, CUBE_BITS_CAP) : 0;

    /* Initial partition. Prefer the P2.5 cartesian-`or` product (fold several
     * narrow top-level `or`s so each cube fixes many disjuncts → small residual
     * → cubes that actually resolve). Fall back to the single widest-`or` k-way
     * split, else a high-fanout bit (2-way). All three are exhaustive, so the
     * UNSAT contract holds regardless of which fires. */
    const uint32_t CARTCAP  = env_u32("DV_CUBE_CARTCAP", CUBE_CART_CAP_DEFAULT);
    const uint32_t CARTORS  = env_u32("DV_CUBE_CARTORS", CUBE_CART_MAXORS_DEF);
    const int      use_cart = env_u32("DV_CUBE_CART", 1) != 0;

    cube_q q = {0};
    or_set ors; memset(&ors, 0, sizeof(ors));
    uint32_t pushed = 0, total_cubes = 0;
    if (use_cart && CARTCAP >= 2 && CARTORS >= 2)
        pushed = build_cartesian_frontier(bb, &q, CARTCAP, CARTORS,
                                          CUBE_CART_PEROR_CAP, B0,
                                          &ors, &total_cubes, verbose);
    if (pushed == 0) {
        /* No multi-or product available: single widest-`or` / bit frontier. */
        int32_t *orlits = (int32_t *)malloc((size_t)CUBE_ORSPLIT_CAP * sizeof(int32_t));
        uint32_t n_or = orlits ? zsp_bbsolver_or_split_lits(bb, orlits, CUBE_ORSPLIT_CAP) : 0;
        pushed = build_frontier(&q, orlits, n_or, bits, n_bits, B0, verbose);
        total_cubes = pushed;
        free(orlits);   /* seed cubes copied into the queue; orlits no longer needed */
    }

    if (pushed == 0) {
        /* No split literal at all: a single unbounded solve is still sound. */
        if (verbose) fprintf(stderr, "[cube] no split literal; single-shot\n");
        or_set_free(&ors);
        free(bits); free(q.v);
        return zsp_bbsolver_solve_assuming(bb, NULL, 0, /*conflict_limit=*/0);
    }

    int result;
    if (NWORKERS < 2) {
        /* Single worker: no thread overhead, use the validated sequential loop. */
        result = run_sequential(bb, &q, total_cubes, &ors, bits, n_bits,
                                GROWTH, MAXDEPTH, MAXCUBES,
                                LOOKAHEAD, LABUDGET, LAMAX, verbose);
        free(q.v);
    } else {
        /* Parallel: clone the recorded CNF into per-worker CaDiCaL instances. */
        size_t db_n = 0; int32_t max_var = 0;
        const int32_t *db = zsp_bbsolver_clause_db(bb, &db_n, &max_var);
        if (!db || db_n == 0) {
            /* Recording failed (OOM): fall back to sequential, never wrong. */
            if (verbose) fprintf(stderr, "[cube] no clause DB; sequential fallback\n");
            result = run_sequential(bb, &q, total_cubes, &ors, bits, n_bits,
                                    GROWTH, MAXDEPTH, MAXCUBES,
                                    LOOKAHEAD, LABUDGET, LAMAX, verbose);
            free(q.v);
        } else {
            result = run_parallel(bb, &q, total_cubes, &ors, bits, n_bits,
                                  GROWTH, MAXDEPTH, MAXCUBES,
                                  LOOKAHEAD, LABUDGET, LAMAX,
                                  PORTFOLIO, seed, PORTCHUNK,
                                  db, db_n, max_var, NWORKERS, verbose);
            /* run_parallel took ownership of q's buffer (freed it). A rare
             * threading-primitive setup failure returns ERROR: fall back to a
             * single unbounded solve over the primary instance — sound and
             * never worse than plain bitblast. */
            if (result == ZSP_BB_ERROR)
                result = zsp_bbsolver_solve_assuming(bb, NULL, 0, /*conflict_limit=*/0);
        }
    }

    or_set_free(&ors);
    free(bits);
    if (verbose)
        fprintf(stderr, "[cube] done: result=%d\n", result);
    return result;
}
