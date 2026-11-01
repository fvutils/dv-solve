/*
 * dvs_class -- the canonical shape class of a problem
 * (docs/solve_stats_capture_plan.md, S1).
 *
 * Two problems that differ only in variable names or ids, the order their
 * constraints or commutative operands were written in, or the values of
 * their constants get the same class hash; problems that differ in structure
 * get different ones. It is what the solve statistics key their per-class
 * records on, so it holds no names and no constant values: a constant is
 * hashed by its class (zero, one, all-ones, power of two, small, other) and
 * its width, and a variable by its type and by the constraints it appears in.
 */
#ifndef DVS_CLASS_H
#define DVS_CLASS_H

#include <stddef.h>
#include <stdint.h>

#include "dvs_problem.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Motifs (signal 9 of the plan): each top-level constraint is matched against
 * a fixed list of small patterns, one to three operators deep. The list is
 * versioned (DVS_MOTIF_VERSION) and its names are the stats file's keys
 * (dvs_motif_name), so a stats file holds only counts per id. A constraint
 * matching nothing is counted as "other". A guarded constraint (an
 * implication, `if`) counts as `guarded` and as its body's motif.
 * Operands are read through casts and extends; a bit-slice of a variable is a
 * field, and counts as a variable.
 */
#define DVS_MOTIF_VERSION 1
typedef enum {
    DVS_MOTIF_VAR_CMP_CONST = 0, /* x < c, x >= c ...                       */
    DVS_MOTIF_VAR_EQ_CONST,      /* x == c, x != c                          */
    DVS_MOTIF_VAR_CMP_VAR,       /* x < y ...                               */
    DVS_MOTIF_VAR_EQ_VAR,        /* x == y, x != y                          */
    DVS_MOTIF_RANGE,             /* x inside [a:b]                          */
    DVS_MOTIF_SET,               /* x inside {a, b, ...}                    */
    DVS_MOTIF_RANGES,            /* x inside [a:b], [c:d] ...               */
    DVS_MOTIF_MOD_EQ,            /* x % c == k (alignment)                  */
    DVS_MOTIF_MASK_EQ,           /* (x & c) == k                            */
    DVS_MOTIF_SHIFT_EQ,          /* (x >> c) == k, (x << c) == k            */
    DVS_MOTIF_LINEAR,            /* a compare over + and - of vars, consts  */
    DVS_MOTIF_NONLINEAR,         /* a compare with *, / or % in it          */
    DVS_MOTIF_BITWISE,           /* a compare over &, |, ^, ~, shifts       */
    DVS_MOTIF_COUNTONES,         /* $countones / $clog2                     */
    DVS_MOTIF_SUM,               /* array sum                               */
    DVS_MOTIF_ARRAY,             /* an array element select                 */
    DVS_MOTIF_GUARDED,           /* if (g) body / g -> body                 */
    DVS_MOTIF_DISJUNCTION,       /* a || b, not an implication              */
    DVS_MOTIF_BOOL_VAR,          /* a 1-bit variable, or its negation       */
    DVS_MOTIF_ITE_VALUE,         /* a ?: in a value position                */
    DVS_MOTIF_CONSTANT,          /* a constant (a disabled knob: `0 -> c`)  */
    DVS_MOTIF_OTHER,
    DVS_N_MOTIFS
} dvs_motif_t;

/** The name of motif `m` (the stats file key). */
const char *dvs_motif_name(dvs_motif_t m);

/* Width buckets of the feature vector's histogram. */
enum { DVS_WIDTH_1, DVS_WIDTH_8, DVS_WIDTH_16, DVS_WIDTH_32, DVS_WIDTH_64,
       DVS_WIDTH_WIDE, DVS_N_WIDTHS };

/** What a class is made of, without the problem: lets classes be grouped
 * and compared (the plan's feature vector). */
typedef struct {
    uint32_t n_vars;
    uint32_t n_conjuncts;       /* top-level hard constraints              */
    uint32_t n_softs, n_alldiffs, n_dists;
    uint32_t n_ordered;         /* variables with a solve-order stage      */
    uint32_t widths[DVS_N_WIDTHS];  /* variables by width: 1, <=8, <=16,
                                     * <=32, <=64, wider                    */
    uint32_t n_components;      /* groups of variables coupled through
                                 * constraints (singletons included)        */
    uint32_t max_component;     /* variables in the largest group          */
    uint32_t max_depth;         /* deepest expression, in operators        */
    uint32_t guard_depth;       /* deepest nest of guards (if / -> / ?:)   */
    /* operator counts over distinct nodes (as dvs_shape_t) */
    uint32_t n_nodes, n_bit, n_arith, n_mul, n_cmp, n_eq, n_bool, n_other;
    uint32_t motifs[DVS_N_MOTIFS];
} dvs_class_features_t;

/*
 * The variables whose values the statistics follow (signals 4 and 7 of the
 * plan), at most DVS_CLASS_TRACK of them, in canonical order: widest first,
 * then by refined colour, then by id. The order holds no names, and two
 * instances of a class list variables of the same widths in the same order;
 * variables with the same colour are interchangeable in the class, so which
 * of them comes first does not matter. Only rand and randc variables.
 */
#define DVS_CLASS_TRACK 64
typedef struct {
    uint32_t n;
    uint32_t seen;                      /* models offered: for the stats'
                                         * sampling (dvs_stats_take_values) */
    uint32_t var_id[DVS_CLASS_TRACK];
    uint8_t  width[DVS_CLASS_TRACK];    /* capped at 64: the low bits are read */
    uint8_t  free[DVS_CLASS_TRACK];     /* the declared domain has 2+ values */
} dvs_class_track_t;

typedef struct {
    uint64_t hash;       /* the class: names, ids, order and constants ignored */
    uint64_t inst;       /* this instance exactly (dvs_problem_fingerprint)   */
    uint32_t n_nodes;    /* distinct expression nodes hashed                  */
    uint32_t truncated;  /* the walk stopped at its node cap; hash covers what
                          * was seen                                          */
    dvs_class_features_t f;
    dvs_class_track_t track;
} dvs_class_t;

/** Fill `out`. Returns 0, or -1 on allocation failure. */
int dvs_class_compute(const dvs_problem_t *p, dvs_class_t *out);
/** sizeof(dvs_class_t), for bindings that mirror it. */
size_t dvs_class_sizeof(void);

#ifdef __cplusplus
}
#endif

#endif /* DVS_CLASS_H */
