/*
 * dvs_linprobe.h -- the weighted value pick's probe, on linear components.
 *
 * A probe (dvs_search.c _wpick_probe) narrows a variable to a bucket,
 * propagates, reads the box left and backtracks: the trail, wakes, queue and
 * explanation hooks it pays for are all undone. When every propagator in the
 * variable's connected component is a linear row, this runs the same
 * propagation on plain bounds instead.
 *
 * Linear rows: an unsigned add or subtract that cannot carry
 * (_bvsum_exact), and `<=`, `<`, `==` between unsigned variables. Each row
 * narrows exactly as its propagator does at a fixpoint, and interval
 * propagation by monotone narrowings reaches one greatest fixpoint in any
 * order, so the bounds, the box and the values drawn are the real probe's.
 * Anything else in the component (a guard, a clause, a non-linear
 * propagator, a signed variable) leaves the probe to the real propagation,
 * as do learnt clauses, a non-empty queue, a sum that could carry, and a
 * propagation that has not settled within its row budget (a crawl).
 *
 * memmap_tight_32's probes went from ~70k instructions to ~14k.
 */
#ifndef INCLUDED_DVS_LINPROBE_H
#define INCLUDED_DVS_LINPROBE_H

#include <stdint.h>

struct Propagator;
struct dvs_ctx_s;

/* Row kinds (prop_linear_row). */
#define LINROW_NONE 0u
#define LINROW_ADD  1u   /* v0 = v1 + v2, without carry when exact */
#define LINROW_LE   2u   /* v0 <= v1 */
#define LINROW_LT   3u   /* v0 <  v1 */
#define LINROW_EQ   4u   /* v0 == v1 */

/* Propagator `p` as a linear row (dvs_prop_templates.c): its kind, its
 * variables in `v` (an add as v0 = v1 + v2, a subtract r = a - b as
 * a = r + b) and an add's width in `*w`. LINROW_NONE for anything else. */
uint8_t prop_linear_row(const struct Propagator *p, const struct dvs_ctx_s *ctx,
                        uint32_t v[3], uint8_t *w);

/* Narrow x to [lo, hi] and propagate x's linear component on plain bounds.
 * -1: not applicable, nothing changed (use the real probe). 0: conflict,
 * nothing changed. 1: the component's bounds are the propagated ones, written
 * in place without the trail; read what is needed, then call
 * dvs_linprobe_undo. `*fires` is the propagation work charged (row
 * evaluations). The caller checks that no clause and no queued propagator
 * could take part. */
int  dvs_linprobe_run(struct dvs_ctx_s *ctx, uint32_t x, int64_t lo, int64_t hi,
                      uint32_t *fires);
void dvs_linprobe_undo(struct dvs_ctx_s *ctx);
void dvs_linprobe_free(struct dvs_ctx_s *ctx);

/* Probes checked against propagation (DVS_LINPROBE=2, dvs_search.c), for
 * tests: note one, and the count so far in this process. */
void     dvs_linprobe_note_check(void);
uint64_t dvs_linprobe_checks(void);

#endif /* INCLUDED_DVS_LINPROBE_H */
