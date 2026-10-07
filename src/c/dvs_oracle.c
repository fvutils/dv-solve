/*
 * dvs_oracle -- the C API's oracle check: every answer of an attached context
 * checked by a reference SMT solver. See dv_solve.h ("Oracle check") and
 * docs/api_oracle_plan.md.
 *
 * Per context the oracle mirrors what the context holds -- the compiled
 * problem, added constraints, pins and exclusions, scoped by checkpoint depth
 * -- as SMT-LIB2 text (dvs_smt2_emit.c), so it never reads the problem after
 * the call that handed it over. Each check sends the whole script into a reset
 * reference solver (dvs_oracle_core.c).
 */
#if !defined(_WIN32) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "dv_solve.h"
#include "dvs_oracle_hooks.h"
#include "dvs_oracle_core.h"
#include "dvs_smt2_emit.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

struct dvs_oracle_s {
    DvsOrc  *core;
    FILE    *err;
    int      last;
    uint64_t n_ctx;     /* contexts attached so far: each gets a number */
};

enum { OE_ADD, OE_PIN, OE_EXCL };

typedef struct {
    uint8_t  kind;
    uint32_t depth;     /* checkpoint depth it was made at */
    char    *text;
} OEntry;

typedef struct {
    uint8_t  known;
    uint8_t  w;         /* 1..64 (a variable is at most 64 bits) */
    uint8_t  s;
    uint8_t  aux;
    uint32_t depth;     /* checkpoint depth it was declared at */
} OVar;

struct dvs_octx_s {
    dvs_oracle_t *o;
    char     *label;
    uint64_t  id;
    int       compiled;
    int       broken;           /* could not be mirrored: record oracle-error */
    char      why[160];
    unsigned  flags;            /* DVS_EMIT_F_* over everything mirrored */
    uint32_t  n_softs;
    int       uncompiled;       /* compile left constraints out */
    DvsOBuf   base;             /* declarations + compiled constraints */
    OVar     *vars;
    uint32_t  n_vars;
    OEntry   *e;
    uint32_t  n_e, cap_e;
    uint32_t  depth;
    uint32_t  defs;             /* define-fun counter */
};

double dvs_oracle_clock_ms(void) {
#ifdef _WIN32
    return (double)clock() * 1000.0 / CLOCKS_PER_SEC;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
#endif
}

/* ------------------------------------------------------------------ */
/* The oracle                                                          */
/* ------------------------------------------------------------------ */

dvs_oracle_t *dvs_oracle_create(const dvs_oracle_opts_t *opts, FILE *err) {
    dvs_oracle_opts_t dflt;
    memset(&dflt, 0, sizeof(dflt));
    if (!opts) opts = &dflt;
    if (!err) err = stderr;

    DvsOrcConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.timeout_s = opts->timeout_s > 0 ? opts->timeout_s : 10.0;
    cfg.rate = 1.0;
    cfg.chk_model = cfg.chk_unsat = 1;
    cfg.keep = opts->keep_all ? DVS_ORC_KEEP_ALL : DVS_ORC_KEEP_FAIL;
    cfg.mode = "api";
    cfg.tag = opts->tag;
    cfg.out_pattern = opts->out_dir;

    /* The soft per-query limit goes to the solver, so a slow query answers
     * `unknown` and the process lives on; the core's hard limit backs it. */
    char cmd[4096];
    long ms = (long)(cfg.timeout_s * 1000.0);
    if (opts->command && *opts->command) {
        snprintf(cmd, sizeof(cmd), "%s", opts->command);
        cfg.name = "cmd";
    } else if (!opts->solver || !*opts->solver || strcmp(opts->solver, "z3") == 0) {
        snprintf(cmd, sizeof(cmd), "%s -in -smt2 -t:%ld",
                 opts->bin && *opts->bin ? opts->bin : "z3", ms);
        cfg.name = "z3";
    } else if (strcmp(opts->solver, "bitwuzla") == 0) {
        snprintf(cmd, sizeof(cmd), "%s --lang smt2 -m -T %ld",
                 opts->bin && *opts->bin ? opts->bin : "bitwuzla", ms);
        cfg.name = "bitwuzla";
    } else {
        fprintf(err, "dv-solve oracle: solver '%s' not known (z3 | bitwuzla, or "
                     "give a command)\n", opts->solver);
        return NULL;
    }
    cfg.cmdline = cmd;

    dvs_oracle_t *o = (dvs_oracle_t *)calloc(1, sizeof(*o));
    if (!o) return NULL;
    o->err = err;
    o->last = DVS_ORACLE_RES_NONE;
    o->core = dvs_orc_create(&cfg, err);
    if (!o->core) { free(o); return NULL; }
    return o;
}

void dvs_oracle_destroy(dvs_oracle_t *o) {
    if (!o) return;
    dvs_orc_finish(o->core, 0);
    free(o);
}

int dvs_oracle_last_result(const dvs_oracle_t *o) {
    return o ? o->last : DVS_ORACLE_RES_NONE;
}

void dvs_oracle_get_stats(const dvs_oracle_t *o, dvs_oracle_stats_t *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    if (!o) return;
    st->queries   = dvs_orc_queries(o->core);
    st->ok        = dvs_orc_count(o->core, DVS_ORC_OK);
    st->bad_model = dvs_orc_count(o->core, DVS_ORC_BAD_MODEL);
    st->bad_unsat = dvs_orc_count(o->core, DVS_ORC_BAD_UNSAT);
    st->unchecked = dvs_orc_count(o->core, DVS_ORC_UNCHECKED);
    st->error     = dvs_orc_count(o->core, DVS_ORC_ORACLE_ERROR);
    st->skipped   = dvs_orc_count(o->core, DVS_ORC_SKIPPED);
    dvs_orc_times(o->core, &st->dvs_ms, &st->oracle_ms);
}

const char *dvs_oracle_run_dir(const dvs_oracle_t *o) {
    return o ? dvs_orc_run_dir(o->core) : NULL;
}

/* ------------------------------------------------------------------ */
/* Per-context state                                                   */
/* ------------------------------------------------------------------ */

static void _octx_clear(struct dvs_octx_s *c) {
    for (uint32_t i = 0; i < c->n_e; i++) free(c->e[i].text);
    c->n_e = 0;
    c->depth = 0;
    c->defs = 0;
    c->flags = 0;
    c->broken = 0;
    c->why[0] = '\0';
    c->n_softs = 0;
    c->uncompiled = 0;
    c->compiled = 0;
    dvs_ob_clear(&c->base);
    free(c->vars);
    c->vars = NULL;
    c->n_vars = 0;
}

static void _octx_free(struct dvs_octx_s *c) {
    if (!c) return;
    _octx_clear(c);
    free(c->e);
    dvs_ob_free(&c->base);
    free(c->label);
    free(c);
}

int dvs_solver_set_oracle(dvs_ctx_t *ctx, dvs_oracle_t *o, const char *label) {
    if (!ctx) return -1;
    _octx_free(ctx->oracle);
    ctx->oracle = NULL;
    if (!o) return 0;
    struct dvs_octx_s *c = (struct dvs_octx_s *)calloc(1, sizeof(*c));
    if (!c) return -1;
    c->o = o;
    c->id = o->n_ctx++;
    c->label = label ? strdup(label) : NULL;
    ctx->oracle = c;
    return 0;
}

void dvs_oracle_on_destroy(dvs_ctx_t *ctx) {
    if (!ctx || !ctx->oracle) return;
    _octx_free(ctx->oracle);
    ctx->oracle = NULL;
}

static void _break(struct dvs_octx_s *c, const char *why) {
    if (!c->broken) snprintf(c->why, sizeof(c->why), "%s", why);
    c->broken = 1;
}

static int _grow_vars(struct dvs_octx_s *c, uint32_t id) {
    if (id < c->n_vars) return 0;
    uint32_t n = c->n_vars ? c->n_vars : 64;
    while (n <= id) n *= 2;
    OVar *g = (OVar *)realloc(c->vars, n * sizeof(OVar));
    if (!g) return -1;
    memset(g + c->n_vars, 0, (n - c->n_vars) * sizeof(OVar));
    c->vars = g;
    c->n_vars = n;
    return 0;
}

static int _var_type(void *ud, uint32_t id, uint16_t *w, uint8_t *s) {
    struct dvs_octx_s *c = (struct dvs_octx_s *)ud;
    if (id >= c->n_vars || !c->vars[id].known) return -1;
    *w = c->vars[id].w;
    *s = c->vars[id].s;
    return 0;
}

/* Declare `p`'s variables not already known into `out`, at the current
 * depth. */
static void _declare(struct dvs_octx_s *c, const dvs_problem_t *p, DvsOBuf *out) {
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = (const VarSpec *)dvs_pool_ptr(&p->pool, r);
        r = vs->next;
        if (vs->width > 64) { _break(c, "a variable wider than 64 bits"); continue; }
        if (_grow_vars(c, vs->var_id) < 0) { _break(c, "out of memory"); continue; }
        OVar *v = &c->vars[vs->var_id];
        if (v->known) continue;
        v->known = 1;
        v->w = vs->width ? vs->width : 1;
        v->s = vs->is_signed ? 1 : 0;
        v->aux = vs->is_aux ? 1 : 0;
        v->depth = c->depth;
        dvs_smt2_emit_var_decl(out, vs->var_id, v->w, v->s, vs->lo, vs->hi);
    }
}

/* Translate `p`'s hard constraints into `out`. */
static void _emit(struct dvs_octx_s *c, const dvs_problem_t *p, DvsOBuf *out) {
    char why[128];
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "_c%llu_", (unsigned long long)c->id);
    if (dvs_smt2_emit_asserts(p, _var_type, c, prefix, &c->defs, out,
                              &c->flags, why, sizeof(why)) != 0) {
        char msg[160];
        snprintf(msg, sizeof(msg), "not translated: %s", why);
        _break(c, msg);
    }
}

static void _push(struct dvs_octx_s *c, uint8_t kind, char *text) {
    if (!text) { _break(c, "out of memory"); return; }
    if (c->n_e == c->cap_e) {
        uint32_t n = c->cap_e ? c->cap_e * 2 : 16;
        OEntry *g = (OEntry *)realloc(c->e, n * sizeof(OEntry));
        if (!g) { free(text); _break(c, "out of memory"); return; }
        c->e = g;
        c->cap_e = n;
    }
    c->e[c->n_e].kind = kind;
    c->e[c->n_e].depth = c->depth;
    c->e[c->n_e].text = text;
    c->n_e++;
}

static char *_value_assert(const char *rel, uint32_t id, int64_t value, uint16_t w) {
    DvsOBuf b = {0};
    dvs_ob_printf(&b, "(assert (%s v%u ", rel, id);
    dvs_smt2_emit_value(&b, value, w);
    dvs_ob_str(&b, "))\n");
    return b.p;
}

/* ------------------------------------------------------------------ */
/* The check                                                           */
/* ------------------------------------------------------------------ */

static void _check(dvs_ctx_t *ctx, const char *cmd, int dv, uint64_t seed,
                   int has_seed, double ms) {
    struct dvs_octx_s *c = ctx->oracle;
    dvs_oracle_t *o = c->o;

    DvsOrcQuery q;
    memset(&q, 0, sizeof(q));
    q.want = dvs_orc_wants(o->core, dv);

    DvsOBuf script = {0}, pins = {0}, extra = {0};
    if (q.want && !c->broken) {
        dvs_ob_str(&script, "(set-logic QF_BV)\n");
        dvs_ob_add(&script, c->base.p ? c->base.p : "", c->base.n);
        for (uint32_t i = 0; i < c->n_e; i++) dvs_ob_str(&script, c->e[i].text);
        if (dv == DVS_ORC_V_SAT) {
            /* Every variable the caller declared, at dv-solve's value. Aux
             * variables are the compiler's and may be left unfixed by
             * propagation: the reference solver finds their values. */
            for (uint32_t id = 0; id < c->n_vars; id++) {
                OVar *v = &c->vars[id];
                if (!v->known || v->aux) continue;
                dvs_ob_printf(&pins, "(assert (= v%u ", id);
                dvs_smt2_emit_value(&pins, dvs_solver_get_value(ctx, id), v->w);
                dvs_ob_str(&pins, "))\n");
                q.n_pinned++;
            }
        }
    }
    if (c->broken) q.error = c->why;

    char sem[48];
    snprintf(sem, sizeof(sem), "sv%s%s%s",
             (c->flags & DVS_EMIT_F_DIV) ? "+div" : "",
             (c->flags & DVS_EMIT_F_AGG) ? "+agg" : "",
             c->uncompiled ? "+uncompiled" : "");

    dvs_ob_str(&extra, "\"label\": ");
    if (c->label) {
        dvs_ob_add(&extra, "\"", 1);
        for (const char *p = c->label; *p; p++) {
            unsigned char ch = (unsigned char)*p;
            if (ch == '"' || ch == '\\') dvs_ob_printf(&extra, "\\%c", ch);
            else if (ch < 0x20) dvs_ob_printf(&extra, "\\u%04x", ch);
            else dvs_ob_add(&extra, p, 1);
        }
        dvs_ob_add(&extra, "\"", 1);
    } else {
        dvs_ob_str(&extra, "null");
    }
    if (has_seed) dvs_ob_printf(&extra, ", \"seed\": %llu", (unsigned long long)seed);
    dvs_ob_printf(&extra, ", \"softs\": %u", c->n_softs);

    q.cmd = cmd;
    q.sem = sem;
    q.engine = "api";
    q.dv = dv;
    q.dvs_ms = ms;
    q.validate = -1;
    q.s = c->id;
    q.d = c->depth;
    q.script = script.p;
    q.script_n = script.n;
    q.pins = pins.p;
    q.pins_n = pins.n;
    q.query = "(check-sat)\n";
    q.raw = cmd;
    q.raw_n = strlen(cmd);
    q.extra_json = extra.p;
    o->last = dvs_orc_check(o->core, &q);
    dvs_ob_free(&script); dvs_ob_free(&pins); dvs_ob_free(&extra);
}

/* ------------------------------------------------------------------ */
/* Hooks                                                               */
/* ------------------------------------------------------------------ */

void dvs_oracle_on_compile(dvs_ctx_t *ctx, const dvs_problem_t *p, int rc, double ms) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c) return;
    _octx_clear(c);
    c->compiled = 1;
    if (!p) { _break(c, "no problem"); return; }
    c->n_softs = p->n_softs;
    c->uncompiled = rc > 0;
    _declare(c, p, &c->base);
    _emit(c, p, &c->base);
    if (rc == DVS_COMPILE_UNSAT)
        _check(ctx, "api-compile", DVS_ORC_V_UNSAT, 0, 0, ms);
    else if (rc < 0)
        _break(c, "compile failed");
}

void dvs_oracle_on_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts,
                         dvs_result_t r, double ms) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c || !c->compiled) return;
    int dv = r == DVS_SOLVE_OK ? DVS_ORC_V_SAT
           : r == DVS_SOLVE_UNSAT ? DVS_ORC_V_UNSAT : DVS_ORC_V_TIMEOUT;
    _check(ctx, "api-solve", dv, opts ? opts->seed : 0, opts != NULL, ms);
}

void dvs_oracle_on_reset(dvs_ctx_t *ctx) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c) return;
    /* Outside any checkpoint a reset removes the pins. Inside one, dv-solve
     * returns to the checkpoint and re-applies what the scope pinned, so
     * every pin stands. */
    if (c->depth != 0) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < c->n_e; i++) {
        if (c->e[i].kind == OE_PIN) { free(c->e[i].text); continue; }
        c->e[k++] = c->e[i];
    }
    c->n_e = k;
}

void dvs_oracle_on_pin(dvs_ctx_t *ctx, uint32_t var_id, int64_t value, int rc) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c || rc != 0) return;
    if (var_id >= c->n_vars || !c->vars[var_id].known) { _break(c, "pin of an unknown variable"); return; }
    _push(c, OE_PIN, _value_assert("=", var_id, value, c->vars[var_id].w));
}

void dvs_oracle_on_exclude(dvs_ctx_t *ctx, uint32_t var_id, int64_t value, int rc) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c || rc != 0) return;
    if (var_id >= c->n_vars || !c->vars[var_id].known) { _break(c, "exclusion of an unknown variable"); return; }
    /* An exclusion lasts across resets and restores: depth 0. */
    uint32_t d = c->depth;
    c->depth = 0;
    _push(c, OE_EXCL, _value_assert("distinct", var_id, value, c->vars[var_id].w));
    c->depth = d;
}

void dvs_oracle_on_add(dvs_ctx_t *ctx, const dvs_problem_t *p, int rc) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c || !p) return;
    if (rc < 0 && rc != DVS_COMPILE_UNSAT) return;     /* nothing was added */
    if (rc > 0) c->uncompiled = 1;
    c->n_softs += p->n_softs;
    DvsOBuf b = {0};
    _declare(c, p, &b);
    _emit(c, p, &b);
    if (!b.p) dvs_ob_str(&b, "");
    _push(c, OE_ADD, b.p);
}

void dvs_oracle_on_checkpoint(dvs_ctx_t *ctx, int cp) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c || cp < 0) return;
    c->depth = (uint32_t)cp + 1;
}

void dvs_oracle_on_restore(dvs_ctx_t *ctx, uint32_t cp) {
    struct dvs_octx_s *c = ctx ? ctx->oracle : NULL;
    if (!c || cp >= c->depth) return;
    uint32_t k = 0;
    for (uint32_t i = 0; i < c->n_e; i++) {
        if (c->e[i].depth > cp) { free(c->e[i].text); continue; }
        c->e[k++] = c->e[i];
    }
    c->n_e = k;
    for (uint32_t id = 0; id < c->n_vars; id++)
        if (c->vars[id].known && c->vars[id].depth > cp) c->vars[id].known = 0;
    c->depth = cp;
}
