/**
 * dvs_tmpl.h -- problems with arrays whose size is given on each call
 * (docs/verilator_problem_buffer.md, "Arrays and queues with variable size").
 *
 * A handle whose image declares arrays solves through a dvs_tmpl_t. It sorts
 * each RAND array by the reach of its constraints:
 *  - element-local: its elements appear only as a[i] in top-level foreachs
 *    over it, whose bodies read nothing random but that element. One small
 *    problem over a single element is compiled once and solved per element,
 *    for any size;
 *  - otherwise expanded: the template is instantiated into a plain problem
 *    for the sizes of the call, compiled, and kept in a small LRU keyed by
 *    the sizes it read.
 */
#ifndef DVS_TMPL_H
#define DVS_TMPL_H

#include "dv_solve.h"
#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dvs_tmpl_s dvs_tmpl_t;

/** Analyse the template `p` (a checked image, which must outlive the result).
 *  NULL if it is malformed (an array id declared twice or not at all) or
 *  memory runs out. */
dvs_tmpl_t *dvs_tmpl_new(const dvs_problem_t *p);

void dvs_tmpl_free(dvs_tmpl_t *t);

/** The engine every problem it builds solves on (dvs_handle_set_engine). */
void dvs_tmpl_set_engine(dvs_tmpl_t *t, int engine);

/** dvs_randomize_arrays() on a template. */
int dvs_tmpl_randomize(dvs_tmpl_t *t, const dvs_ref_t *refs, uint32_t n_vars,
                       const dvs_array_ref_t *arrays, uint64_t seed);

/** dvs_randomize_sizes() on a template. */
int dvs_tmpl_randomize_sizes(dvs_tmpl_t *t, const dvs_ref_t *refs, uint32_t n_vars,
                             uint32_t *sizes, uint64_t seed);

/** The number of arrays the template declares. */
uint32_t dvs_tmpl_n_arrays(const dvs_tmpl_t *t);

/** Whether array `a` is solved element by element (for tests). */
int dvs_tmpl_array_is_local(const dvs_tmpl_t *t, uint32_t a);

/** The template a handle solves through, or NULL (for tests). */
dvs_tmpl_t *dvs_handle_tmpl(dvs_handle_t *h);

/** How many expanded problems were built so far (for tests). */
uint32_t dvs_tmpl_n_expansions(const dvs_tmpl_t *t);

/* ------------------------------------------------------------------ */
/* Parts (dvs_parts.c): problems assembled from several images         */
/* ------------------------------------------------------------------ */

/** The slot arrays of a part being assembled, as the call gives them. */
typedef struct {
    void *ctx;
    /* The number of elements of slot array `a` */
    uint32_t (*length)(void *ctx, uint32_t a);
    /* Whether element `e` of slot array `a` is not null */
    int (*live)(void *ctx, uint32_t a, uint32_t e);
    /* The variable of import `v` (var_map[v] is ASM_PER_ELEM) for the loop
     * indices bound (bit per level), or NO_VAR */
    uint32_t (*var)(void *ctx, uint32_t v, const int32_t *loop, uint32_t bound);
} dvs_asm_slots_t;

/** var_map[] of an import through a DVS_PATH_LOOP element: per element. */
#define ASM_PER_ELEM 0xFFFFFFFEu

/**
 * Add the constraints, arrays, foreachs, distributions, all-differents and
 * sources of the part image `p` to the builder `b` of an assembled problem:
 * its variable v is var_map[v], which the caller declares; its array a is
 * array_base + a; its soft priorities, ranked densely from 0, are added to
 * prio_base, and *n_prios receives how many distinct ones it has. A foreach
 * over a slot array is instantiated per element of `sa`.
 * @return 0, or -1 if building fails.
 */
int dvs_tmpl_assemble(dvs_builder_t *b, const dvs_problem_t *p, const uint32_t *var_map,
                      uint32_t array_base, uint32_t prio_base, uint32_t *n_prios,
                      const dvs_asm_slots_t *sa);

/** The assembled problems kept on a part's handle. */
typedef struct dvs_shapes_s dvs_shapes_t;

void dvs_shapes_free(dvs_shapes_t *s);

/** Where a handle keeps its assembled problems. */
dvs_shapes_t **dvs_handle_shapes(dvs_handle_t *h);

/** A handle's image. */
const dvs_problem_t *dvs_handle_problem(const dvs_handle_t *h);

/** The var_id of export `k` of a handle's part, or NO_EXPORT. */
#define NO_EXPORT 0xFFFFFFFFu
uint32_t dvs_handle_export(const dvs_handle_t *h, uint32_t k);
/* DVS_CAPTURE: the id the handle's calls are recorded with, 0 when not. */
uint32_t dvs_handle_cap_id(const dvs_handle_t *h);

/** How many shapes were assembled on a handle (for tests). */
uint32_t dvs_parts_n_assembled(dvs_handle_t *h);

/** How many learnt clauses a handle's context holds (for tests). */
uint32_t dvs_handle_n_learnt(const dvs_handle_t *h);

/** What a handle carries from one call to the next, for
 *  dvs_handle_import_state() on a fresh handle of the same image, which then
 *  returns the values this one would (an evicted handle's sidecar). Words:
 *  the engine once settled (else -1) and the clause policy (keep, earned,
 *  wait, backoff, conflicts x16); per RANDC variable in var_id order, its
 *  cycle's count of values, then the values; then per kept clause its
 *  literal count, its LBD, and two words per literal (var_id * 2 + 1 for a
 *  lower bound, the bound). Writes at most `cap` words and returns the number
 *  the whole state takes. */
size_t dvs_handle_export_state(const dvs_handle_t *h, int64_t *buf, size_t cap);

/** Load what dvs_handle_export_state() gave into a handle that has not been
 *  called yet, compiling its context if there are clauses.
 *  0, or -1 (malformed input, a handle already called, or out of memory). */
int dvs_handle_import_state(dvs_handle_t *h, const int64_t *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DVS_TMPL_H */
