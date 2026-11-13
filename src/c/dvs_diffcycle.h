/*
 * dvs_diffcycle.h -- negative-cycle detection over difference relations.
 *
 * Bounds propagation proves a cycle of difference constraints infeasible
 * only by walking the bounds round it: `y >= x` and `y + n <= x + m` with
 * n > m move x's and y's bounds n - m closer per round, so a 2^20-wide
 * domain takes ~2^20 / (n - m) rounds -- milliseconds per conflict, and a
 * search with thousands of such conflicts runs for minutes. Read as a graph
 * (an edge u -> v of weight w for `v <= u + w`), that state is a NEGATIVE
 * CYCLE, which Bellman-Ford finds in O(V * E).
 *
 * One propagator per compiled problem covers every variable of every
 * difference relation that lies on a directed cycle through two relations
 * or more (one relation's own edges never sum negative). A negative cycle
 * is what bounds propagation crawls round, so it shows as a propagation
 * that keeps firing, and one that settles first has none: the propagation
 * loop queues the check after DIFFCYCLE_FIRES_FIRST firings and at each
 * doubling, rather than on every bound it covers (one wake per tightening
 * cost more than the propagation, memmap_tight_32). It reads the edge
 * weights from the current bounds -- `r = a + b` gives r <= a + hi(b) and
 * a <= r - lo(b), valid only while the bounds prove the sum cannot wrap --
 * and reports a negative cycle as a conflict, explained by exactly the
 * bounds the cycle's weights and no-wrap conditions read.
 *
 * It adds no pruning beyond that: on a feasible system bounds propagation
 * converges by itself.
 */
#ifndef INCLUDED_DVS_DIFFCYCLE_H
#define INCLUDED_DVS_DIFFCYCLE_H

#include <stdint.h>

struct Propagator;
struct dvs_ctx_s;

/* Firings a propagation spends before the check first runs, at least (twice
 * the propagators, when more). Most settle within a few dozen, and the
 * check, which rebuilds its graph, cost more than they did (shift_width,
 * page_table_entry). */
#define DIFFCYCLE_FIRES_FIRST 64u

typedef enum {
    DREL_LE   = 1,   /* v[0] <= v[1] + c                            */
    DREL_SUM  = 2,   /* v[0] == v[1] + v[2]   (mod 2^width)         */
    DREL_SUB  = 3,   /* v[0] == v[1] - v[2]   (mod 2^width)         */
    DREL_ADDC = 4,   /* v[0] == v[1] + c      (mod 2^width)         */
} DiffRelKind;

typedef struct {
    uint8_t  kind;    /* DiffRelKind */
    uint8_t  width;   /* modulus width for SUM/SUB/ADDC; 0: exact     */
    uint8_t  _pad[2];
    uint32_t v[3];
    int64_t  c;
} DiffRel;

/** The difference relations propagator `p` enforces (at most 2), or 0. */
int prop_difference_relations(const struct Propagator *p,
                              const struct dvs_ctx_s *ctx, DiffRel out[2]);

/** After compile: add the cycle-detection propagator if the problem's
 *  difference relations contain a cycle. Returns 0 (also when none is
 *  needed), -1 on allocation failure. */
int diffcycle_build(struct dvs_ctx_s *ctx);

/** Whether p is the cycle-detection propagator. It adds no pruning of its
 *  own, so it removes no solution the relations' propagators keep. */
int prop_is_diffcycle(const struct Propagator *p);

#endif /* INCLUDED_DVS_DIFFCYCLE_H */
