/*
 * dvs_stats -- solve statistics (docs/solve_stats_capture_plan.md, S0).
 *
 * Counts every user-level solve on every path: the C API, the randomize
 * handle, DPI, SMT2 check-sat (plain and Verilator mode) and the bit-level
 * API. Each path's outermost entry opens a record on the calling thread; calls
 * it makes into other entries see the record already open and only add to it
 * (which engine ran, how much work), so a solve is counted once whatever path
 * it takes. When the record closes it is added to process-global counters
 * under a mutex.
 *
 * Off unless DV_STATS is set (to anything but "" or "0") or dvs_stats_enable()
 * is called; when off, each hook costs one load and one branch. The counters
 * hold no problem content: counts, enum indices and log2 buckets only.
 */
#ifndef DVS_STATS_H
#define DVS_STATS_H

#include <stdint.h>

#include "dv_solve.h"
#include "dvs_class.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The entry a solve came in through. */
typedef enum {
    DVS_STATS_PATH_API = 0,     /* dvs_solver_solve called directly         */
    DVS_STATS_PATH_HANDLE,      /* dvs_randomize                            */
    DVS_STATS_PATH_DPI,         /* dvs_dpi_solve_h                          */
    DVS_STATS_PATH_SMT2,        /* check-sat / check-sat-assuming           */
    DVS_STATS_PATH_VERILATOR,   /* the same, in --mode=verilator            */
    DVS_STATS_PATH_BVSAT,       /* dvs_bbsolver_check/sample/check_maxsat   */
    DVS_STATS_PATH_SMT2_AUX,    /* engine work an SMT2 command other than
                                 * check-sat does (get-unsat-core replays):
                                 * counted as work, not as a solve, and only
                                 * if an engine ran                         */
    DVS_STATS_N_PATHS
} dvs_stats_path_t;

/* An engine that ran during a solve. */
typedef enum {
    DVS_STATS_ENG_CDCL = 0,
    DVS_STATS_ENG_BITBLAST,
    DVS_STATS_ENG_CUBE,
    DVS_STATS_N_ENGINES
} dvs_stats_engine_t;

/* How a solve ended. */
typedef enum {
    DVS_STATS_RES_SAT = 0,
    DVS_STATS_RES_UNSAT,
    DVS_STATS_RES_UNKNOWN,      /* timeout, or the engine could not decide  */
    DVS_STATS_RES_ERROR,
    DVS_STATS_N_RESULTS
} dvs_stats_result_t;

/* What produced the answer (signal 2 of the plan): the last one noted during
 * the solve. An engine scope notes its default (CDCL search, bitblast SAT,
 * cube) when it opens; a fast path that decides notes itself. The names are
 * k_via in dvs_stats.c; the order is the stats file schema. */
typedef enum {
    DVS_STATS_VIA_NONE = 0,         /* nothing noted (no engine ran)        */
    DVS_STATS_VIA_CDCL_ROOT,        /* propagation or shaving before search */
    DVS_STATS_VIA_CDCL_SEARCH,
    DVS_STATS_VIA_CDCL_ENUM,        /* listed solutions (_enum_sample)      */
    DVS_STATS_VIA_CDCL_BOX,         /* box rejection (_box_sample)          */
    DVS_STATS_VIA_BB_SAT,
    DVS_STATS_VIA_BB_POPCOUNT,      /* samplers before SAT: _popsamp ...    */
    DVS_STATS_VIA_BB_UNIQUE,
    DVS_STATS_VIA_BB_SORTED,
    DVS_STATS_VIA_BB_PINFAST,
    DVS_STATS_VIA_BB_DIVMUL,
    DVS_STATS_VIA_BB_ENUM,
    DVS_STATS_VIA_BB_AFFINE,        /* affine / closed form                 */
    DVS_STATS_VIA_BB_SIM_SPLIT,
    DVS_STATS_VIA_BB_SIM_EXHAUSTIVE,
    DVS_STATS_VIA_BB_SIM_RANDOM,
    DVS_STATS_VIA_BB_SIM_RACE,      /* the sweep beat SAT                   */
    DVS_STATS_VIA_BB_SIM_ONCE,      /* resample: one simulation pass       */
    DVS_STATS_VIA_BB_SAT_FRESH,     /* resample: a fresh seeded SAT search  */
    DVS_STATS_VIA_BB_REDIVERSIFY,   /* bit flips of a cached model          */
    DVS_STATS_VIA_CUBE,
    DVS_STATS_VIA_HASH_SKIP,        /* SMT2: only skipped parity asserts    */
    DVS_STATS_VIA_MODEL_REUSE,      /* SMT2: check-sat-assuming reuses model */
    DVS_STATS_VIA_TAINTED,          /* SMT2: unsupported input, `unknown`   */
    DVS_STATS_VIA_CACHE_UNSAT,      /* SMT2: identical instance, was unsat  */
    DVS_STATS_VIA_COMPILE_UNSAT,    /* SMT2: compile proved unsat           */
    DVS_STATS_N_VIA
} dvs_stats_via_t;

/* Why a solve took the route it did: every reason noted during the solve is
 * counted once. The names are k_reason in dvs_stats.c (the SMT2 front end's
 * nb_why / qfbv_why / route strings map onto them, dvs_stats_reason_of). */
typedef enum {
    DVS_STATS_REASON_OTHER = 0,     /* a string not in the list             */
    /* the problem needs bitblast (SMT2 nb_why) */
    DVS_STATS_REASON_WIDE_VAR,
    DVS_STATS_REASON_WIDE_ARITH,
    DVS_STATS_REASON_WIDE_CONST,
    DVS_STATS_REASON_SEXT64,
    DVS_STATS_REASON_SDIV_SREM,
    DVS_STATS_REASON_SMOD,
    DVS_STATS_REASON_FLUSH_FAILED,
    /* operator-mix shape rule (dvs_shape_route, SMT2 qfbv_why) */
    DVS_STATS_REASON_MULTIPLIER,
    DVS_STATS_REASON_BIT_LEVEL,
    DVS_STATS_REASON_SEEDED_SAMPLE,
    /* other routes to bitblast */
    DVS_STATS_REASON_LOGIC,         /* QF_UFBV / QF_ABV / QF_AUFBV          */
    DVS_STATS_REASON_ARRAY,         /* the lazy array engine                */
    DVS_STATS_REASON_ENGINE_FORCED, /* DV_ENGINE                            */
    /* Verilator mode routing */
    DVS_STATS_REASON_VLT_PROBE,
    DVS_STATS_REASON_VLT_STICKY,
    DVS_STATS_REASON_VLT_ENUM,
    DVS_STATS_REASON_VLT_OPT_OUT,
    DVS_STATS_REASON_VLT_CDCL_SLOW,
    DVS_STATS_REASON_VLT_CDCL_DECLINED,
    /* escalations: one engine gave up and another took over */
    DVS_STATS_REASON_COMPILE_FAILED,
    DVS_STATS_REASON_POOL_OVERFLOW,
    DVS_STATS_REASON_CDCL_UNKNOWN,
    DVS_STATS_REASON_CDCL_ERROR,    /* handle: CDCL cannot take the problem */
    DVS_STATS_N_REASONS
} dvs_stats_reason_t;

/* Set from DV_STATS on first use; dvs_stats_enable() overrides. Read on every
 * hook: -1 not yet read, 0 off, 1 on. */
extern volatile int dvs_stats_g_on;
int dvs_stats_init_env(void);

static inline int dvs_stats_on(void) {
    int s = dvs_stats_g_on;
    return s > 0 || (s < 0 && dvs_stats_init_env());
}

/* Open a record if none is open on this thread. Returns 1 if this call opened
 * it (the caller must then call dvs_stats_end), 0 if one was already open or
 * stats are off. */
int  dvs_stats_begin(dvs_stats_path_t path);
/* Close the record dvs_stats_begin opened and add it to the global counters. */
void dvs_stats_end(dvs_stats_result_t res);

/* Engine scopes, called by engines whether or not a record is open (no-ops
 * when none is). An engine running inside another engine's scope (the cube
 * engine's bit-level calls, say) is part of the outer one: only the outermost
 * scope on the thread counts. The engine whose scope opened last is credited
 * with the answer; work done by the others counts as wasted.
 *   dvs_stats_engine_enter returns a token for dvs_stats_engine_leave.
 *   dvs_stats_add_work adds work to the engine whose scope is open: CDCL in
 *   dvs_ctx_work units, bitblast and cube in SAT backend ticks. */
int  dvs_stats_engine_enter(dvs_stats_engine_t eng);
void dvs_stats_engine_leave(int token);
void dvs_stats_add_work(uint64_t work);
/* What produced the answer, if a record is open (see dvs_stats_via_t). */
void dvs_stats_note_via(dvs_stats_via_t via);
/* Why the solve was routed as it was, if a record is open. */
void dvs_stats_note_reason(dvs_stats_reason_t why);
/* The reason a front-end string names; DVS_STATS_REASON_OTHER if none. */
dvs_stats_reason_t dvs_stats_reason_of(const char *why);
/* The name of a reason (the stats file key). */
const char *dvs_stats_reason_name(dvs_stats_reason_t why);

/* The shape class (dvs_class.h) of the problem this solve is for; the first
 * one noted in a solve stands. `inst` is the instance's fingerprint (0 if not
 * known). Engines cache the class and note it on every solve. */
void dvs_stats_note_class(uint64_t cls, uint64_t inst);
/* 1 if a record is open on this thread and has no class yet. */
int dvs_stats_need_class(void);
/* The class of `p`, from a process-wide cache keyed by its fingerprint `fp`
 * (0: compute it here). 0 if it could not be computed. If `track` is not
 * NULL it is filled with the variables whose values the statistics follow
 * (n = 0 if the class could not be computed). */
uint64_t dvs_stats_class_of(const dvs_problem_t *p, uint64_t fp,
                            dvs_class_track_t *track);

/* Value quality (signals 4 and 7): an engine that found a model notes the
 * values of the tracked variables, `v[i]` for `t->var_id[i]` (low 64 bits).
 * The last noted model of a solve stands; it is added to its class when the
 * record closes with sat. dvs_stats_want_values says whether to read them. */
int  dvs_stats_want_values(void);
void dvs_stats_note_values(const dvs_class_track_t *t, const uint64_t *v);
/* Whether to read this model's values: every model for an engine's first
 * DVS_STATS_VALUES_ALL, then one in DVS_STATS_VALUES_STRIDE, by count and
 * never by value, so the sample is unbiased. Reading and adding the values
 * is otherwise 5-13% of a 1 us solve. */
#define DVS_STATS_VALUES_ALL    64
#define DVS_STATS_VALUES_STRIDE 16
static inline int dvs_stats_take_values(dvs_class_track_t *t) {
    uint32_t k = t->seen++;
    return t->n && (k < DVS_STATS_VALUES_ALL || k % DVS_STATS_VALUES_STRIDE == 0);
}

/* For hot paths: the call only when statistics are on. */
#define DVS_STATS_VIA(v) do { if (dvs_stats_on()) dvs_stats_note_via(v); } while (0)

/* A CDCL solve ended with bail reason `bail` (DVS_BAIL_*, 0 = none). */
void dvs_stats_note_bail(unsigned bail);
/* How constrained a problem is at the root (signal 3): over its rand and
 * randc variables, the log2 of the product of their domain sizes (as
 * intervals) as declared and at the first solve's root, after level-0
 * propagation and shaving, in 1/16 bits; how many variables the root fixed
 * to one value, and how many compile tied to another variable. */
typedef struct {
    uint32_t vars, fixed, tied;
    uint64_t bits16, free_bits16;
    /* Signal 5, at compile (CDCL): the problem's constraints, those compiled
     * into no propagator (root bounds, aliases), and those not compiled. */
    uint32_t cons, cons_quiet, cons_dropped;
} dvs_stats_root_t;

/* log2 of the size of [lo, hi] as `width`-bit values (at most `width`), in
 * 1/16 bits. */
uint64_t dvs_stats_bits16(int64_t lo, int64_t hi, uint32_t width);
/* The monotonic clock, in ns, for timing a step outside a record. */
uint64_t dvs_stats_clock(void);
/* A problem of class `cls` (0 if not known) was compiled (CDCL) in `ns`.
 * Counted whether or not a record is open. */
void dvs_stats_note_compile(uint64_t cls, uint64_t ns);
/* The root of the first solve after a compile of class `cls`. */
void dvs_stats_note_root(uint64_t cls, const dvs_stats_root_t *root);
/* Binding constraints (signal 5), from one CDCL solve's per-constraint
 * counts: the constraints, how many narrowed a bound and how many took part
 * in a conflict, and the share of the narrowings (per mille) made by the
 * busiest constraint and by propagators of no one constraint (unique, dist,
 * soft, later adds); the shares only if anything narrowed (`busy`). */
typedef struct {
    uint32_t cons, narrowed, conflicted;
    uint32_t busy, top_pm, other_pm;
} dvs_stats_binding_t;
void dvs_stats_note_binding(const dvs_stats_binding_t *b);
/* Count binding constraints on this solve? The values schedule (every solve
 * of the first DVS_STATS_VALUES_ALL after a compile, then one in
 * DVS_STATS_VALUES_STRIDE) on its own counter. */
static inline int dvs_stats_take_sample(uint32_t *seen) {
    uint32_t k = (*seen)++;
    return k < DVS_STATS_VALUES_ALL || k % DVS_STATS_VALUES_STRIDE == 0;
}

/* Time outside the solves (signal 6, dvs_stats_add_time): build time is
 * charged to the next solve on the thread, read time to class `cls`, or if 0
 * to the class last solved on the thread. */
void dvs_stats_time(dvs_stats_time_t kind, uint64_t cls, uint64_t ns);
/* Timing a library step that is build or read time: dvs_stats_step_begin
 * returns 0 (do not time) when statistics are off, a solve is open on the
 * thread (its time is the solve's), or a step is already being timed (a
 * nested one); else a start for dvs_stats_step_end, which must follow. */
uint64_t dvs_stats_step_begin(void);
void     dvs_stats_step_end(uint64_t t0, dvs_stats_time_t kind, uint64_t cls);
/* The open DVS_STATS_PATH_SMT2_AUX record is a command that reads a model
 * back (get-value): its time is read time. Any other such command states the
 * problem: build time. */
void dvs_stats_note_read(void);

/* Map a CDCL or bit-level verdict to a stats result. */
dvs_stats_result_t dvs_stats_result_of(dvs_result_t r);
dvs_stats_result_t dvs_stats_result_of_bb(int bb_rc);

#ifdef __cplusplus
}
#endif

#endif /* DVS_STATS_H */
