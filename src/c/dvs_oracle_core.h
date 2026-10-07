/*
 * dvs_oracle_core -- the parts of an oracle check that do not depend on where
 * the queries come from: the reference solver process, the run directory and
 * its records, and the classification of each answer.
 *
 * Two front ends use it: the SMT-LIB2 frontend's oracle (smt2/smt2_oracle.c,
 * configured by DV_ORACLE*) and the C API's oracle (dvs_oracle.c, configured by
 * dvs_oracle_create). Both write the same run directory, so one report tool
 * (python -m dv_solve.oracle report) reads either. See docs/api_oracle_plan.md.
 *
 * Internal; not installed. POSIX only: on Windows dvs_orc_create returns NULL.
 */
#ifndef DVS_ORACLE_CORE_H
#define DVS_ORACLE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A growable text buffer, always NUL-terminated once non-empty. */
typedef struct { char *p; size_t n, cap; } DvsOBuf;
int  dvs_ob_add(DvsOBuf *b, const char *s, size_t n);
int  dvs_ob_str(DvsOBuf *b, const char *s);
int  dvs_ob_printf(DvsOBuf *b, const char *fmt, ...);
void dvs_ob_clear(DvsOBuf *b);
void dvs_ob_free(DvsOBuf *b);

/* Write `s` as a JSON string literal. */
void dvs_json_str(FILE *f, const char *s, size_t n);
void dvs_json_cstr(FILE *f, const char *s);

/* Verdicts, of dv-solve and of the oracle. */
enum { DVS_ORC_V_NONE, DVS_ORC_V_SAT, DVS_ORC_V_UNSAT, DVS_ORC_V_UNKNOWN,
       DVS_ORC_V_TIMEOUT, DVS_ORC_V_DEAD, DVS_ORC_V_ERROR };

/* Result classes. The values match DVS_ORACLE_RES_* in dv_solve.h. */
enum { DVS_ORC_OK, DVS_ORC_BAD_MODEL, DVS_ORC_BAD_UNSAT, DVS_ORC_GAP,
       DVS_ORC_UNCHECKED, DVS_ORC_ORACLE_ERROR, DVS_ORC_SKIPPED, DVS_ORC__N };

enum { DVS_ORC_KEEP_SUMMARY, DVS_ORC_KEEP_FAIL, DVS_ORC_KEEP_ALL };

const char *dvs_orc_verdict_name(int v);
const char *dvs_orc_result_name(int r);

typedef struct DvsOrc DvsOrc;

typedef struct {
    const char *name;          /* "z3", "bitwuzla" or "cmd": run.json oracle.name */
    const char *cmdline;       /* the reference solver's command line */
    const char *out_pattern;   /* run directory; %t, %p expanded; NULL = default */
    const char *tag;
    double      timeout_s;
    int         chk_model, chk_unsat, chk_unknown;
    double      rate;          /* (0, 1]: fraction of wanted checks done */
    int         keep;          /* DVS_ORC_KEEP_* */
    const char *mode;          /* run.json "mode": "default", "verilator", "api" */
    const char *verilator_hash;/* run.json "verilator_hash", or NULL to omit */
    int         argc;          /* recorded in run.json */
    char      **argv;
} DvsOrcConfig;

/* Start the reference solver and claim the run directory. NULL (reported on
 * `err`) when either fails; the caller then runs without the oracle. */
DvsOrc *dvs_orc_create(const DvsOrcConfig *cfg, FILE *err);

/* Whether a dv-solve verdict `dv` is to be checked: the check class is on and
 * the sampling rate lets it through. Call once per query. */
int dvs_orc_wants(DvsOrc *o, int dv);

typedef struct {
    int         want;          /* from dvs_orc_wants(); 0 records `skipped` */
    const char *cmd;           /* record "cmd" */
    const char *sem;           /* record "sem" */
    const char *engine;        /* record dvs.engine */
    int         dv;            /* dv-solve's verdict, DVS_ORC_V_* */
    double      dvs_ms;
    int         validate;      /* internal validation violations, or -1 */
    uint64_t    s;             /* session (smt2) / context (api) number */
    uint32_t    d;             /* scope depth */
    const char *script;        /* everything before the pins: declarations, */
    size_t      script_n;      /* assertions, options */
    const char *pins;          /* (assert (= x v)) lines; dv == SAT only */
    size_t      pins_n;
    uint32_t    n_pinned;
    const char *partial;       /* declarations left unpinned, or NULL */
    const char *query;         /* the check as sent, e.g. "(check-sat)\n" */
    const char *raw;           /* the check as dv-solve saw it (detail JSON) */
    size_t      raw_n;
    const char *error;         /* set: no check possible, record oracle-error */
    const char *extra_json;    /* more record fields, `"k": v, ...`, or NULL */
} DvsOrcQuery;

/* Check one query and record it. Returns the result class (DVS_ORC_*). */
int dvs_orc_check(DvsOrc *o, const DvsOrcQuery *q);

/* The number of sessions recorded in run.json is max(s) + 1. */
const char *dvs_orc_run_dir(const DvsOrc *o);
uint64_t    dvs_orc_count(const DvsOrc *o, int res);
uint64_t    dvs_orc_queries(const DvsOrc *o);
void        dvs_orc_times(const DvsOrc *o, double *dvs_ms, double *orc_ms);
const char *dvs_orc_version(const DvsOrc *o);

/* Finalise run.json ("complete", or "aborted"), stop the reference solver,
 * print the summary on the creation `err` stream and free the oracle. */
void dvs_orc_finish(DvsOrc *o, int aborted);

#ifdef __cplusplus
}
#endif

#endif /* DVS_ORACLE_CORE_H */
