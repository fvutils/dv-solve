/**
 * dvs_engine.c — one context, two engines (dvs_engine.h).
 *
 * The CDCL compile always runs. Under AUTO the problem is also copied, so
 * that bit-blast can take it at compile (a problem CDCL left incomplete) or
 * later (a CDCL search that gave up). Bit-blast checks the
 * problem on its first solve and resamples it on every later one, with the
 * front end's one-shot defaults, as dvs_handle did before it moved here.
 *
 * AUTO picks by shape (dvs_shape_route_rand) and gives a CDCL search that
 * could move a work budget (DVS_ENGINE_CDCL_WORK), so a shape the rule
 * misses costs one budgeted search, not a slow one on every call.
 */
#include <stdlib.h>
#include <string.h>
#include "dvs_engine.h"
#include "dvs_ctx.h"
#include "dvs_bbsolver.h"
#include "dvs_shape.h"

typedef struct dvs_engine_s {
    int             eng;       /* DVS_ENGINE_CDCL or DVS_ENGINE_BITBLAST     */
    int             cdcl_ok;   /* the CDCL compile took the problem whole    */
    int             locked;    /* a CDCL-only call ran: CDCL for good        */
    dvs_problem_t  *p;         /* a copy of the compiled problem             */
    uint32_t        n_vars;    /* one past the highest declared var_id       */
    uint8_t        *declared;
    int64_t        *lo, *hi;   /* declared bounds per var_id                 */
    dvs_bbsolver_t *bb;        /* after the first bit-blast solve            */
    int             bb_rc;     /* that check's DVS_BB_* result               */
    int64_t        *x;         /* the bit-blast model, per var_id            */
    int             have_x;
    uint64_t        seq;       /* seeds for opts->seed == 0                  */
} dvs_engine_t;

int dvs_solver_set_engine(dvs_ctx_t *ctx, int engine) {
    if (!ctx || ctx->engine || ctx->n_vars) return -1;    /* compiled already */
    if (engine != DVS_ENGINE_AUTO && engine != DVS_ENGINE_CDCL
            && engine != DVS_ENGINE_BITBLAST)
        return -1;
    ctx->engine_pref = engine;
    return 0;
}

int dvs_solver_engine(const dvs_ctx_t *ctx) {
    if (!ctx) return DVS_ENGINE_AUTO;
    if (ctx->engine) return ctx->engine->eng;
    return ctx->n_vars ? DVS_ENGINE_CDCL : ctx->engine_pref;
}

void dvs_engine_free(dvs_ctx_t *ctx) {
    dvs_engine_t *e = ctx->engine;
    if (!e) return;
    if (e->bb) dvs_bbsolver_free(e->bb);
    free(e->declared);
    free(e->lo);
    free(e->hi);
    free(e->x);
    free(e->p);
    free(e);
    ctx->engine = NULL;
}

/* What bit-blast cannot honour: pins and exclusion the problem asks for
 * through its variable kinds, solve order, soft constraints, dist weights. */
static int _needs_cdcl(const dvs_problem_t *p) {
    if (p->n_softs || p->n_dists) return 1;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(p, r);
        if (vs->kind == DVS_VAR_STATE || vs->kind == DVS_VAR_RANDC || vs->order
                || vs->width > 64)
            return 1;
        r = vs->next;
    }
    return 0;
}

static dvs_engine_t *_engine_new(const dvs_problem_t *p) {
    dvs_engine_t *e = calloc(1, sizeof *e);
    if (!e) return NULL;
    size_t size = sizeof(dvs_problem_t) + p->pool.used;
    e->p = malloc(size);
    if (!e->p) { free(e); return NULL; }
    memcpy(e->p, p, size);
    e->p->pool.capacity = p->pool.used;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(p, r);
        if (vs->var_id >= e->n_vars) e->n_vars = vs->var_id + 1;
        r = vs->next;
    }
    uint32_t n = e->n_vars ? e->n_vars : 1;
    e->declared = calloc(n, 1);
    e->lo = calloc(n, sizeof *e->lo);
    e->hi = calloc(n, sizeof *e->hi);
    e->x = calloc(n, sizeof *e->x);
    if (!e->declared || !e->lo || !e->hi || !e->x) {
        free(e->declared); free(e->lo); free(e->hi); free(e->x); free(e->p); free(e);
        return NULL;
    }
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(p, r);
        e->declared[vs->var_id] = 1;
        e->lo[vs->var_id] = vs->lo;
        e->hi[vs->var_id] = vs->hi;
        r = vs->next;
    }
    return e;
}

int dvs_engine_compile(dvs_ctx_t *ctx, dvs_problem_t *p, int rc) {
    if (!ctx || !p) return rc;
    dvs_engine_free(ctx);
    if (ctx->engine_pref == DVS_ENGINE_CDCL) return rc;
    /* UNSAT from compile-time bounds is sound; the other failures are about
     * the problem itself (memory, ids, widths), not the engine. */
    if (rc < 0) return rc;
    int cdcl_only = _needs_cdcl(p);
    int bitblast;
    if (ctx->engine_pref == DVS_ENGINE_BITBLAST) {
        if (cdcl_only) return rc;
        bitblast = 1;
    } else if (cdcl_only) {
        return rc;
    } else {
        /* CDCL unless the shape says otherwise: it is the engine tuned for
         * randomization (its own product, divisor and dist sampling).
         * Bit-blast takes what CDCL cannot compile whole, the shapes CDCL
         * searches slowly (dvs_shape_route_rand), and (dvs_engine_take_over)
         * what its search gives up on. */
        dvs_shape_t sh;
        bitblast = rc > 0 || (dvs_shape_compute(p, &sh) == 0 && dvs_shape_route_rand(&sh));
    }
    dvs_engine_t *e = _engine_new(p);
    if (!e) return rc;           /* no memory for the copy: CDCL, as before */
    e->cdcl_ok = rc == DVS_COMPILE_OK;
    e->eng = bitblast ? DVS_ENGINE_BITBLAST : DVS_ENGINE_CDCL;
    ctx->engine = e;
    return bitblast ? DVS_COMPILE_OK : rc;
}

static uint64_t _mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static dvs_result_t _bb_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    dvs_engine_t *e = ctx->engine;
    uint64_t seed = opts && opts->seed ? opts->seed : ++e->seq * 0x9e3779b97f4a7c15ull;
    e->have_x = 0;
    if (!e->bb) {
        e->bb = dvs_bbsolver_new(NULL, e->p);
        if (!e->bb) return DVS_SOLVE_TIMEOUT;
        dvs_bbsolver_set_resample(e->bb, 1);
        /* The front end's one-shot default (smt2_frontend.c: DV_SIM). */
        dvs_bbsolver_set_presim(e->bb, 1);
        e->bb_rc = dvs_bbsolver_check(e->bb, seed);
        if (e->bb_rc != DVS_BB_SAT && e->bb_rc != DVS_BB_UNSAT) {
            /* No verdict: the next solve checks afresh. */
            dvs_bbsolver_free(e->bb);
            e->bb = NULL;
            return DVS_SOLVE_TIMEOUT;
        }
    } else if (e->bb_rc == DVS_BB_SAT && dvs_bbsolver_resample(e->bb, seed) != 0) {
        return DVS_SOLVE_TIMEOUT;
    }
    if (e->bb_rc == DVS_BB_UNSAT) return DVS_SOLVE_UNSAT;
    for (uint32_t v = 0; v < e->n_vars; v++) {
        if (!e->declared[v]) continue;
        /* Bit-blast bounds only the variables a constraint references; any
         * other may come back outside its declared bounds, and is then drawn
         * uniformly between them from the seed. lo <= x <= hi modulo 2^64:
         * an unsigned 64-bit variable's bounds are uint64_t patterns. */
        uint64_t span = (uint64_t)e->hi[v] - (uint64_t)e->lo[v] + 1;   /* 0: all 2^64 */
        if (dvs_bbsolver_value(e->bb, v, &e->x[v]) == 0
                && (uint64_t)e->x[v] - (uint64_t)e->lo[v] <= span - 1)
            continue;
        uint64_t r = _mix64(seed * 0x9e3779b97f4a7c15ull + v + 1);
        e->x[v] = (int64_t)((uint64_t)e->lo[v] + (span ? r % span : r));
    }
    e->have_x = 1;
    return DVS_SOLVE_OK;
}

/* Whether a CDCL search that gives up may move the context to bit-blast. A
 * caller's own time budget is kept: bit-blast has none to honour. */
static int _movable(const dvs_ctx_t *ctx, const dvs_solve_opts_t *opts) {
    const dvs_engine_t *e = ctx->engine;
    return e && !e->locked && e->eng == DVS_ENGINE_CDCL && ctx->engine_pref == DVS_ENGINE_AUTO
           && !(opts && opts->time_limit_ms);
}

/* The work (dvs_ctx_work) a movable CDCL search may take before bit-blast
 * takes over for good: a floor of 5x the most one call of a CDCL-routed
 * simbench needs (nonoverlap_regions_8's first, 4.2e5), and more per
 * variable, as a large problem needs (all-different over 1000 variables,
 * test_arrays: under 2.2e7). A count, not time, so the move and the values
 * after it are a function of the seeds. */
#define DVS_ENGINE_CDCL_WORK     2000000ull
#define DVS_ENGINE_CDCL_VAR_WORK 100000ull

int dvs_engine_solve(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts, dvs_result_t *r) {
    dvs_engine_t *e = ctx ? ctx->engine : NULL;
    if (!e || e->eng != DVS_ENGINE_BITBLAST) {
        if (e) {
            e->have_x = 0;
            if (_movable(ctx, opts) && !ctx->work_budget) ctx->work_budget = DVS_ENGINE_CDCL_WORK + DVS_ENGINE_CDCL_VAR_WORK * ctx->n_vars;
        }
        return 0;
    }
    *r = _bb_solve(ctx, opts);
    return 1;
}

int dvs_engine_take_over(dvs_ctx_t *ctx, const dvs_solve_opts_t *opts, dvs_result_t *r) {
    if (!ctx || !_movable(ctx, opts)) return 0;
    ctx->engine->eng = DVS_ENGINE_BITBLAST;
    *r = _bb_solve(ctx, opts);
    return 1;
}

int dvs_engine_value(const dvs_ctx_t *ctx, uint32_t var_id, int64_t *v) {
    const dvs_engine_t *e = ctx ? ctx->engine : NULL;
    if (!e || e->eng != DVS_ENGINE_BITBLAST || !e->have_x) return 0;
    *v = var_id < e->n_vars ? e->x[var_id] : 0;
    return 1;
}

int dvs_engine_need_cdcl(dvs_ctx_t *ctx) {
    dvs_engine_t *e = ctx ? ctx->engine : NULL;
    if (!e) return 0;
    if (e->eng == DVS_ENGINE_BITBLAST) {
        if (!e->cdcl_ok) return -1;
        e->eng = DVS_ENGINE_CDCL;
        e->have_x = 0;
        if (e->bb) { dvs_bbsolver_free(e->bb); e->bb = NULL; }
    }
    e->locked = 1;
    return 0;
}
