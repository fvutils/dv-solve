/**
 * dvs_handle.c — problem handles and dvs_randomize() (V2 of
 * docs/verilator_problem_buffer.md).
 *
 * A handle owns a copy of one problem image and the CDCL context compiled
 * from it on the first call. Every call resets the context, pins the STATE
 * variables to the values read through their refs, runs the seeded
 * fair_pick solve the SMT-LIB front end runs (and bench_vlt_replay's
 * api-cache pass replays), checks the model, and writes RAND and RANDC
 * values back.
 *
 * Engine: the first call settles it (dvs_handle_set_engine). AUTO takes the
 * front end's one-shot rule (dvs_shape_route) and moves to bitblast, for good,
 * when CDCL cannot compile the problem whole or its search gives up. On
 * bitblast the first call checks the problem with the clause log kept and
 * every later call resamples it (dvs_bbsolver_resample), as the api-cache
 * pass of bench_vlt_replay does. The bit-blast engine has no pins or value
 * exclusion, so a problem with STATE or RANDC variables stays on CDCL.
 *
 * randc: each RANDC value returned is excluded (dvs_solver_exclude_value,
 * which lasts across resets) and kept in the var's history. The cycle ends
 * when the exclusion would empty the declared range, or when a solve is
 * UNSAT while some history is held; the context has no way to drop an
 * exclusion, so it is then compiled again and the histories that go on are
 * excluded anew. On an UNSAT the call clears every history and solves once
 * more: only then is it UNSAT (a STATE contradiction thus also restarts the
 * cycles).
 */
#include <stdlib.h>
#include <string.h>
#include "dv_solve.h"
#include "dvs_problem.h"
#include "dvs_bbsolver.h"
#include "dvs_shape.h"

struct dvs_handle_s {
    dvs_problem_t *p;          /* the copied image                          */
    dvs_ctx_t     *ctx;        /* compiled on the first call                */
    int            compile_rc; /* dvs_solver_compile() result               */
    uint32_t       n_vars;     /* one past the highest declared var_id      */
    uint8_t       *kind;       /* dvs_var_kind_t per var_id                 */
    uint8_t       *declared;   /* 1 when the problem declares the var_id    */
    int64_t      **hist;       /* per RANDC var: values of this cycle       */
    uint32_t      *n_hist, *cap_hist;
    int            stale;      /* recompile before the next solve           */
    int            engine;     /* DVS_ENGINE_*; AUTO until the first call   */
    int            chosen;     /* dvs_handle_set_engine() named the engine  */
    int            routed;     /* the first call has settled the engine     */
    int            pinned;     /* the problem has STATE or RANDC variables  */
    int64_t       *lo, *hi;    /* declared bounds per var_id                */
    dvs_bbsolver_t *bb;        /* the bitblast engine, after its first check */
    int            bb_rc;      /* that check's DVS_BB_* result              */
};

dvs_handle_t *dvs_handle_new(const void *buf, size_t size) {
    if (!buf || size < sizeof(dvs_problem_t)) return NULL;
    dvs_handle_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->p = malloc(size);
    if (!h->p) { free(h); return NULL; }
    memcpy(h->p, buf, size);
    for (dvs_expr_t r = h->p->vars_head; r != EXPR_NULL;) {
        VarSpec *vs = POOL_PTR(h->p, r);
        if (vs->var_id >= h->n_vars) h->n_vars = vs->var_id + 1;
        r = vs->next;
    }
    h->kind = calloc(h->n_vars ? h->n_vars : 1, 1);
    h->declared = calloc(h->n_vars ? h->n_vars : 1, 1);
    h->hist = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->hist);
    h->n_hist = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->n_hist);
    h->cap_hist = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->cap_hist);
    h->lo = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->lo);
    h->hi = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->hi);
    if (!h->kind || !h->declared || !h->hist || !h->n_hist || !h->cap_hist || !h->lo
            || !h->hi) {
        dvs_handle_free(h);
        return NULL;
    }
    for (dvs_expr_t r = h->p->vars_head; r != EXPR_NULL;) {
        VarSpec *vs = POOL_PTR(h->p, r);
        h->kind[vs->var_id] = vs->kind;
        h->declared[vs->var_id] = 1;
        h->lo[vs->var_id] = vs->lo;
        h->hi[vs->var_id] = vs->hi;
        h->pinned |= vs->kind == DVS_VAR_STATE || vs->kind == DVS_VAR_RANDC;
        r = vs->next;
    }
    return h;
}

void dvs_handle_free(dvs_handle_t *h) {
    if (!h) return;
    if (h->ctx) dvs_solver_free(h->ctx);
    if (h->bb) dvs_bbsolver_free(h->bb);
    for (uint32_t v = 0; h->hist && v < h->n_vars; v++) free(h->hist[v]);
    free(h->hist);
    free(h->n_hist);
    free(h->cap_hist);
    free(h->kind);
    free(h->declared);
    free(h->lo);
    free(h->hi);
    free(h->p);
    free(h);
}

uint32_t dvs_handle_n_vars(const dvs_handle_t *h) {
    return h ? h->n_vars : 0;
}

int dvs_handle_set_engine(dvs_handle_t *h, int engine) {
    if (!h || h->routed) return -1;
    if (engine != DVS_ENGINE_AUTO && engine != DVS_ENGINE_CDCL
            && engine != DVS_ENGINE_BITBLAST)
        return -1;
    if (engine == DVS_ENGINE_BITBLAST && h->pinned) return -1;
    h->engine = engine;
    h->chosen = engine != DVS_ENGINE_AUTO;
    return 0;
}

int dvs_handle_engine(const dvs_handle_t *h) {
    return h ? h->engine : DVS_ENGINE_AUTO;
}

/* Bytes of the scalar word that holds `width` bits, or 0 if unsupported. */
static size_t ref_bytes(const dvs_ref_t *rf) {
    if (rf->layout != DVS_REF_SCALAR || rf->width == 0 || rf->width > 64) return 0;
    return rf->width <= 8 ? 1 : rf->width <= 16 ? 2 : rf->width <= 32 ? 4 : 8;
}

static int64_t ref_read(const dvs_ref_t *rf, size_t n) {
    const uint8_t *a = rf->addr;
    uint64_t u = 0;
    for (size_t i = 0; i < n; i++) u |= (uint64_t)a[i] << (8 * i);
    if (rf->width < 64) {
        u &= (1ull << rf->width) - 1;
        if (rf->is_signed && (u >> (rf->width - 1)) & 1) u |= ~0ull << rf->width;
    }
    return (int64_t)u;
}

static void ref_write(const dvs_ref_t *rf, size_t n, int64_t v) {
    uint64_t u = (uint64_t)v;
    if (rf->width < 64) u &= (1ull << rf->width) - 1;
    uint8_t *a = rf->addr;
    for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(u >> (8 * i));
}

/* Compile the context (again, when stale) and exclude the held randc values. */
static int handle_open(dvs_handle_t *h) {
    if (h->ctx && !h->stale) {
        dvs_solver_reset(h->ctx);
        return 0;
    }
    if (h->ctx) dvs_solver_free(h->ctx);
    h->stale = 0;
    h->ctx = dvs_solver_new(NULL);
    if (!h->ctx) return -1;
    h->compile_rc = dvs_solver_compile(h->ctx, h->p);
    if (h->compile_rc != DVS_COMPILE_OK) return 0;
    for (uint32_t v = 0; v < h->n_vars; v++)
        for (uint32_t k = 0; k < h->n_hist[v]; k++)
            if (dvs_solver_exclude_value(h->ctx, v, h->hist[v][k]) != 0) return -1;
    return 0;
}

static int solve_once(dvs_handle_t *h, const dvs_ref_t *refs, uint64_t seed) {
    if (handle_open(h) != 0) return DVS_RANDOMIZE_ERROR;
    if (h->compile_rc == DVS_COMPILE_UNSAT) return DVS_RANDOMIZE_UNSAT;
    if (h->compile_rc != DVS_COMPILE_OK) return DVS_RANDOMIZE_ERROR;

    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || h->kind[v] != DVS_VAR_STATE || !refs[v].addr) continue;
        if (dvs_solver_pin_var(h->ctx, v, ref_read(&refs[v], ref_bytes(&refs[v]))) != 0)
            return DVS_RANDOMIZE_UNSAT;
    }

    /* The front end's options (smt2_frontend.c, _cmd_check_sat_body). */
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    o.seed = seed;
    o.max_conflicts = 10000;
    o.max_restarts = 1000;
    o.use_lcg = 1;
    o.use_phase_save = 1;
    o.fair_pick = 1;
    dvs_result_t res = dvs_solver_solve(h->ctx, &o);
    if (res == DVS_SOLVE_UNSAT) return DVS_RANDOMIZE_UNSAT;
    if (res != DVS_SOLVE_OK) return DVS_RANDOMIZE_ERROR;
    if (dvs_solver_validate_model(h->ctx, h->p, NULL) != 0) return DVS_RANDOMIZE_ERROR;
    return DVS_RANDOMIZE_OK;
}

/* Write RAND/RANDC values through the refs; exclude each RANDC value. An
 * exclusion discards the search, and with it the model, so every value is
 * read (into the history slot) before the first exclusion. */
static void write_back(dvs_handle_t *h, const dvs_ref_t *refs) {
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v]) continue;
        if (h->kind[v] != DVS_VAR_RAND && h->kind[v] != DVS_VAR_RANDC) continue;
        int64_t x = dvs_solver_get_value(h->ctx, v);
        if (refs[v].addr) ref_write(&refs[v], ref_bytes(&refs[v]), x);
        if (h->kind[v] != DVS_VAR_RANDC) continue;
        if (h->n_hist[v] == h->cap_hist[v]) {
            uint32_t cap = h->cap_hist[v] ? 2 * h->cap_hist[v] : 16;
            int64_t *n = realloc(h->hist[v], cap * sizeof *n);
            if (!n) {
                /* No room: drop the history (pass 2 skips cap 0). */
                free(h->hist[v]);
                h->hist[v] = NULL;
                h->n_hist[v] = h->cap_hist[v] = 0;
                h->stale = 1;
                continue;
            }
            h->hist[v] = n;
            h->cap_hist[v] = cap;
        }
        h->hist[v][h->n_hist[v]] = x;   /* not yet held: n_hist unchanged */
    }
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || h->kind[v] != DVS_VAR_RANDC) continue;
        if (h->n_hist[v] == h->cap_hist[v]) continue;   /* realloc failed */
        int64_t x = h->hist[v][h->n_hist[v]];
        if (dvs_solver_exclude_value(h->ctx, v, x) == 0) {
            h->n_hist[v]++;
        } else {
            /* The cycle is complete: start the next one. */
            h->n_hist[v] = 0;
            h->stale = 1;
        }
    }
}

static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

/* One call on bitblast. Every value is read before the first is written, so
 * a failed read writes nothing. The bit-blast engine applies declared bounds
 * only to the variables a constraint references: any other comes back with
 * some value of its width, possibly outside them, so a value outside its
 * bounds is drawn again, uniformly between them, from the seed. */
static int bb_randomize(dvs_handle_t *h, const dvs_ref_t *refs, uint64_t seed) {
    if (!h->bb) {
        h->bb = dvs_bbsolver_new(NULL, h->p);
        if (!h->bb) return DVS_RANDOMIZE_ERROR;
        dvs_bbsolver_set_resample(h->bb, 1);
        /* The front end's one-shot default (smt2_frontend.c: DV_SIM). */
        dvs_bbsolver_set_presim(h->bb, 1);
        h->bb_rc = dvs_bbsolver_check(h->bb, seed);
        if (h->bb_rc != DVS_BB_SAT && h->bb_rc != DVS_BB_UNSAT) {
            /* No verdict: the next call checks afresh. */
            dvs_bbsolver_free(h->bb);
            h->bb = NULL;
            return DVS_RANDOMIZE_ERROR;
        }
    } else if (h->bb_rc == DVS_BB_SAT && dvs_bbsolver_resample(h->bb, seed) != 0) {
        return DVS_RANDOMIZE_ERROR;
    }
    if (h->bb_rc == DVS_BB_UNSAT) return DVS_RANDOMIZE_UNSAT;
    int64_t *x = malloc((h->n_vars ? h->n_vars : 1) * sizeof *x);
    if (!x) return DVS_RANDOMIZE_ERROR;
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || !refs[v].addr) continue;
        /* lo <= x <= hi modulo 2^64: the bounds of an unsigned 64-bit
         * variable are uint64_t patterns, so a signed compare would redraw
         * every value past INT64_MAX (and all of them when hi is ~0). */
        uint64_t span = (uint64_t)h->hi[v] - (uint64_t)h->lo[v] + 1;   /* 0: all 2^64 */
        if (dvs_bbsolver_value(h->bb, v, &x[v]) == 0
                && (uint64_t)x[v] - (uint64_t)h->lo[v] <= span - 1)
            continue;
        uint64_t r = mix64(seed * 0x9e3779b97f4a7c15ull + v + 1);
        x[v] = (int64_t)((uint64_t)h->lo[v] + (span ? r % span : r));
    }
    for (uint32_t v = 0; v < h->n_vars; v++)
        if (h->declared[v] && refs[v].addr) ref_write(&refs[v], ref_bytes(&refs[v]), x[v]);
    free(x);
    return DVS_RANDOMIZE_OK;
}

/* Settle the engine on the first call. */
static void handle_route(dvs_handle_t *h) {
    if (h->routed) return;
    h->routed = 1;
    if (h->engine != DVS_ENGINE_AUTO) return;
    h->engine = DVS_ENGINE_CDCL;
    if (h->pinned) return;
    dvs_shape_t sh;
    if (dvs_shape_compute(h->p, &sh) == 0 && dvs_shape_route(&sh))
        h->engine = DVS_ENGINE_BITBLAST;
}

int dvs_randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes,
                  uint64_t seed) {
    if (!h || (h->n_vars && !refs) || modes) return DVS_RANDOMIZE_ERROR;
    for (uint32_t v = 0; v < h->n_vars; v++)
        if (refs[v].addr && !ref_bytes(&refs[v])) return DVS_RANDOMIZE_ERROR;

    handle_route(h);
    if (h->engine == DVS_ENGINE_BITBLAST) return bb_randomize(h, refs, seed);

    for (int attempt = 0;; attempt++) {
        int rc = solve_once(h, refs, seed);
        if (rc == DVS_RANDOMIZE_ERROR && !h->chosen && !h->pinned) {
            /* CDCL cannot answer this problem: bitblast from now on. */
            dvs_solver_free(h->ctx);
            h->ctx = NULL;
            h->engine = DVS_ENGINE_BITBLAST;
            return bb_randomize(h, refs, seed);
        }
        if (rc != DVS_RANDOMIZE_UNSAT || attempt) {
            if (rc == DVS_RANDOMIZE_OK) write_back(h, refs);
            return rc;
        }
        int held = 0;
        for (uint32_t v = 0; v < h->n_vars; v++) {
            held |= h->n_hist[v] != 0;
            h->n_hist[v] = 0;
        }
        if (!held) return rc;
        h->stale = 1;
    }
}
