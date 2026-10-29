/**
 * dvs_handle.c — problem handles and dvs_randomize() (V2 of
 * docs/verilator_problem_buffer.md).
 *
 * A handle owns a copy of one problem image and the CDCL context compiled
 * from it on the first call. Every call resets the context, pins the STATE
 * variables to the values read through their refs, runs the seeded
 * fair_pick solve the SMT-LIB front end runs (and bench_vlt_replay's
 * api-cache pass replays), checks the model, and writes RAND and RANDC
 * values back. There is no bitblast route yet: a solve the CDCL search gives
 * up on is DVS_RANDOMIZE_ERROR.
 */
#include <stdlib.h>
#include <string.h>
#include "dv_solve.h"
#include "dvs_problem.h"

struct dvs_handle_s {
    dvs_problem_t *p;          /* the copied image                          */
    dvs_ctx_t     *ctx;        /* compiled on the first call                */
    int            compile_rc; /* dvs_solver_compile() result               */
    uint32_t       n_vars;     /* one past the highest declared var_id      */
    uint8_t       *kind;       /* dvs_var_kind_t per var_id                 */
    uint8_t       *declared;   /* 1 when the problem declares the var_id    */
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
    if (!h->kind || !h->declared) { dvs_handle_free(h); return NULL; }
    for (dvs_expr_t r = h->p->vars_head; r != EXPR_NULL;) {
        VarSpec *vs = POOL_PTR(h->p, r);
        h->kind[vs->var_id] = vs->kind;
        h->declared[vs->var_id] = 1;
        r = vs->next;
    }
    return h;
}

void dvs_handle_free(dvs_handle_t *h) {
    if (!h) return;
    if (h->ctx) dvs_solver_free(h->ctx);
    free(h->kind);
    free(h->declared);
    free(h->p);
    free(h);
}

uint32_t dvs_handle_n_vars(const dvs_handle_t *h) {
    return h ? h->n_vars : 0;
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

int dvs_randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes,
                  uint64_t seed) {
    if (!h || (h->n_vars && !refs) || modes) return DVS_RANDOMIZE_ERROR;
    for (uint32_t v = 0; v < h->n_vars; v++)
        if (refs[v].addr && !ref_bytes(&refs[v])) return DVS_RANDOMIZE_ERROR;

    if (!h->ctx) {
        h->ctx = dvs_solver_new(NULL);
        if (!h->ctx) return DVS_RANDOMIZE_ERROR;
        h->compile_rc = dvs_solver_compile(h->ctx, h->p);
    } else {
        dvs_solver_reset(h->ctx);
    }
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

    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || !refs[v].addr) continue;
        if (h->kind[v] != DVS_VAR_RAND && h->kind[v] != DVS_VAR_RANDC) continue;
        ref_write(&refs[v], ref_bytes(&refs[v]), dvs_solver_get_value(h->ctx, v));
    }
    return DVS_RANDOMIZE_OK;
}
