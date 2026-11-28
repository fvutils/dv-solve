/**
 * dvs_handle.c — problem handles and dvs_randomize() (V2 of
 * docs/verilator_problem_buffer.md).
 *
 * A handle owns a copy of one problem image and the CDCL context compiled
 * from it on the first call. Every call resets the context, pins the STATE
 * variables to the values read through their refs,
 * runs the seeded fair_pick solve the SMT-LIB front end runs (and
 * bench_vlt_replay's api-cache pass replays), checks the model, and writes
 * RAND and RANDC values back, and then drops its learnt clauses or keeps
 * the ones it used (handle_settle).
 *
 * Engine: the context chooses it (dvs_solver_set_engine, dvs_engine.c); the
 * handle only passes on what dvs_handle_set_engine() asked for. The
 * bit-blast engine has no pins or value exclusion, and its resample ignores
 * soft constraints, dist weights and solve-order, so a problem with any of
 * them stays on CDCL (cdcl_only).
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
#include "dvs_capture.h"
#include "dvs_problem.h"
#include "dvs_presolve.h"
#include "dvs_ctx.h"
#include "dvs_lcg.h"
#include "dvs_search.h"
#include "dvs_tmpl.h"
#include "dvs_bbsolver.h"
#include "dvs_narrow.h"

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
    int            engine;     /* DVS_ENGINE_* asked for (set_engine)       */
    int            called;     /* a call has run: the engine is settled     */
    int            cdcl_only;  /* STATE or RANDC variables, soft constraints,
                                * dist or solve-order: bitblast cannot solve
                                * the problem as asked                      */
    dvs_tmpl_t    *tmpl;       /* a problem with arrays: it solves the call */
    uint32_t      *exports;    /* var_id per export index                   */
    dvs_shapes_t  *shapes;     /* a part's assembled problems (dvs_parts.c) */
    int            canonical;  /* DVS_CANONICAL=1: dvs_solve_opts_t.canonical */
    int            validate;   /* DVS_VALIDATE=1: re-check each model      */
    /* The clause policy (handle_settle). With the randc histories and the
     * kept clauses it is all a call carries to the next, so a fresh handle
     * that imports them (dvs_handle_import_state) goes on as this one. */
    uint8_t        keep;       /* calls start with the clauses the last one
                                * used; 0: with none                       */
    uint8_t        earned;     /* a period of keeping has paid since the
                                * last drop                                 */
    uint8_t        fired;      /* a kept clause fired in this period        */
    uint32_t       calls;      /* calls so far in this period               */
    uint64_t       sum;        /* their conflicts                           */
    uint64_t       ref;        /* the conflicts of the last dropping period */
    uint32_t       wait;       /* dropping periods before the next trial    */
    uint32_t       backoff;    /* the wait after the next failed trial      */
    uint32_t       n_kept;     /* clauses this call started with            */
    uint32_t       cap_id;     /* DVS_CAPTURE: the id its calls are recorded
                                * with; 0 when not recorded                 */
    uint32_t      *width;      /* declared width per var_id                 */
    int64_t       *lo, *hi;    /* declared bounds per var_id                */
    uint8_t       *sgn;        /* 1: declared signed                        */
    /* Wide problems (a variable over 64 bits, or a constraint the CDCL
     * compile left out; wide_solve). The CDCL engine does not search such
     * variables, so the handle solves them on bit-blast, the STATE values
     * pinned by equalities added to a copy of the problem (wp). The
     * instance is kept while the STATE values stay the same, and then
     * resampled. */
    int             wide;      /* solved by wide_solve: a declared variable
                                * is over 64 bits, or (wide_open) CDCL left
                                * a constraint out of a problem bit-blast
                                * can solve with the STATE values pinned   */
    int             wide_bad;  /* and the problem has what bit-blast cannot
                                * honour: randc, soft constraints, dist     */
    uint32_t       *woff;      /* [n_vars + 1]: each var's first limb       */
    uint64_t       *wkey;      /* the pins `wbb` was built for: per var, a
                                * pinned flag limb, then its limbs          */
    uint64_t       *wcur;      /* this call's pins, as wkey                 */
    dvs_problem_t  *wp;        /* the pinned copy wbb blasts                */
    dvs_bbsolver_t *wbb;
    int             wrc;       /* its check's DVS_BB_* verdict              */
};

/* 64-bit limbs of a `width`-bit value. */
static uint32_t n_limbs(uint32_t width) { return (width + 63u) / 64u; }

/* The tables of wide_solve: each var's pins (woff, wkey, wcur). */
static int wide_setup(dvs_handle_t *h) {
    free(h->woff);
    free(h->wkey);
    free(h->wcur);
    h->wkey = h->wcur = NULL;
    h->woff = malloc((h->n_vars + 1) * sizeof *h->woff);
    if (!h->woff) return -1;
    uint64_t nl = 0;
    for (uint32_t v = 0; v < h->n_vars; v++) {
        h->woff[v] = (uint32_t)nl;
        nl += h->declared[v] ? 1 + n_limbs(h->width[v]) : 0;
    }
    h->woff[h->n_vars] = (uint32_t)nl;
    h->wkey = calloc(nl ? nl : 1, sizeof *h->wkey);
    h->wcur = calloc(nl ? nl : 1, sizeof *h->wcur);
    return nl > UINT32_MAX || !h->wkey || !h->wcur ? -1 : 0;
}

/* Whether the problem stays on CDCL whatever it reads: STATE or RANDC
 * variables, solve order, soft constraints or dists (cdcl_only), or arrays,
 * whose expansions have STATE variables of their own (a local array's size,
 * an element-local problem's index). Its expressions over 64 bits are
 * narrowed (dvs_narrow.c), the CDCL engine compiling nothing wider. */
static int stays_on_cdcl(const dvs_problem_t *p) {
    if (p->n_arrays || p->n_foreachs || p->n_softs || p->n_dists) return 1;
    for (dvs_expr_t r = p->vars_head; r != EXPR_NULL;) {
        const VarSpec *vs = POOL_PTR(p, r);
        if (vs->kind == DVS_VAR_STATE || vs->kind == DVS_VAR_RANDC || vs->order) return 1;
        r = vs->next;
    }
    return 0;
}

/* `top`: not made inside another API call (dvs_capture_enter). */
static dvs_handle_t *handle_new(const void *buf, size_t size, int top) {
    if (!buf) return NULL;
    dvs_handle_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->backoff = 1;
    /* `buf` need not be aligned (an embedded byte array): check the copy. */
    h->p = malloc(size ? size : 1);
    if (!h->p) { free(h); return NULL; }
    memcpy(h->p, buf, size);
    if (dvs_problem_check_image(h->p, size, NULL, 0) != 0) {
        dvs_handle_free(h);
        return NULL;
    }
    size_t psize = size;
    /* What it cannot narrow stays as it is: dvs_problem_validate() refuses
     * it when soft constraints, a dist or randc keep it from bit-blast, and
     * otherwise solve_once hands the problem to wide_solve. */
    if (stays_on_cdcl(h->p) && !dvs_narrow_has_wide_decl(h->p)
            && dvs_narrow_problem(&h->p, &psize, NULL, 0) < 0) {
        dvs_handle_free(h);
        return NULL;
    }
    dvs_presolve_linear(&h->p, &psize);
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
    if (!h->kind || !h->declared || !h->hist || !h->n_hist || !h->cap_hist) {
        dvs_handle_free(h);
        return NULL;
    }
    h->width = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->width);
    h->sgn = calloc(h->n_vars ? h->n_vars : 1, 1);
    h->lo = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->lo);
    h->hi = calloc(h->n_vars ? h->n_vars : 1, sizeof *h->hi);
    if (!h->width || !h->sgn || !h->lo || !h->hi) {
        dvs_handle_free(h);
        return NULL;
    }
    int randc = 0;
    for (dvs_expr_t r = h->p->vars_head; r != EXPR_NULL;) {
        VarSpec *vs = POOL_PTR(h->p, r);
        h->kind[vs->var_id] = vs->kind;
        h->declared[vs->var_id] = 1;
        h->width[vs->var_id] = vs->width;
        h->sgn[vs->var_id] = vs->is_signed ? 1 : 0;
        h->lo[vs->var_id] = vs->lo;
        h->hi[vs->var_id] = vs->hi;
        h->wide |= vs->width > 64;
        randc |= vs->kind == DVS_VAR_RANDC;
        h->cdcl_only |= vs->kind == DVS_VAR_STATE || vs->kind == DVS_VAR_RANDC
                        || vs->order != 0;
        r = vs->next;
    }
    h->cdcl_only |= h->p->n_softs != 0 || h->p->n_dists != 0;
    /* Solve order changes only which solution is drawn, never the set, so
     * the wide path solves an ordered problem (without the order). */
    h->wide_bad = randc || h->p->n_softs != 0 || h->p->n_dists != 0;
    if (h->wide && wide_setup(h)) {
        dvs_handle_free(h);
        return NULL;
    }
    const char *canon = getenv("DVS_CANONICAL");
    h->canonical = canon && *canon == '1';
    const char *val = getenv("DVS_VALIDATE");
    h->validate = val && *val == '1';
    /* An export is a declared variable of this part, not an import. */
    h->exports = malloc((h->p->n_exports ? h->p->n_exports : 1) * sizeof *h->exports);
    if (!h->exports) {
        dvs_handle_free(h);
        return NULL;
    }
    uint32_t k = h->p->n_exports;
    for (dvs_expr_t r = k ? h->p->exports_head : EXPR_NULL; r != EXPR_NULL;) {
        const ExportSpec *e = POOL_PTR(h->p, r);
        if (e->var_id >= h->n_vars || !h->declared[e->var_id]
                || h->kind[e->var_id] == DVS_VAR_IMPORT) {
            dvs_handle_free(h);
            return NULL;
        }
        h->exports[--k] = e->var_id;
        r = e->next;
    }
    if ((h->p->n_arrays || h->p->n_foreachs) && !(h->tmpl = dvs_tmpl_new(h->p))) {
        dvs_handle_free(h);
        return NULL;
    }
    if (top) h->cap_id = dvs_capture_handle(buf, size, h->canonical);
    /* DV_ENGINE=cdcl|bitblast, as for dv-solve-smt2: force an engine where
     * the problem allows it. */
    const char *eng = getenv("DV_ENGINE");
    if (eng && (!strcmp(eng, "cdcl") || !strcmp(eng, "bitblast")))
        dvs_handle_set_engine(h, eng[0] == 'c' ? DVS_ENGINE_CDCL : DVS_ENGINE_BITBLAST);
    return h;
}

dvs_handle_t *dvs_handle_new(const void *buf, size_t size) {
    int top = dvs_capture_enter();
    dvs_handle_t *h = handle_new(buf, size, top);
    dvs_capture_leave();
    return h;
}

void dvs_handle_free(dvs_handle_t *h) {
    if (!h) return;
    if (h->ctx) dvs_solver_free(h->ctx);
    dvs_tmpl_free(h->tmpl);
    dvs_shapes_free(h->shapes);
    free(h->exports);
    for (uint32_t v = 0; h->hist && v < h->n_vars; v++) free(h->hist[v]);
    free(h->hist);
    free(h->n_hist);
    free(h->cap_hist);
    free(h->kind);
    free(h->declared);
    free(h->width);
    free(h->sgn);
    free(h->lo);
    free(h->hi);
    if (h->wbb) dvs_bbsolver_free(h->wbb);
    free(h->wp);
    free(h->woff);
    free(h->wkey);
    free(h->wcur);
    free(h->p);
    free(h);
}

uint32_t dvs_handle_n_vars(const dvs_handle_t *h) {
    return h ? h->n_vars : 0;
}

int dvs_handle_set_engine(dvs_handle_t *h, int engine) {
    if (!h || h->ctx || h->called) return -1;
    if (engine != DVS_ENGINE_AUTO && engine != DVS_ENGINE_CDCL
            && engine != DVS_ENGINE_BITBLAST)
        return -1;
    if (engine == DVS_ENGINE_BITBLAST && h->cdcl_only) return -1;
    if (h->tmpl) dvs_tmpl_set_engine(h->tmpl, engine);
    h->engine = engine;
    if (h->cap_id) dvs_capture_engine(h->cap_id, engine);
    return 0;
}

int dvs_handle_engine(const dvs_handle_t *h) {
    if (!h) return DVS_ENGINE_AUTO;
    return h->ctx ? dvs_solver_engine(h->ctx) : h->engine;
}

/* Bytes a ref reads and writes, or 0 if it is not a supported ref. */
static size_t ref_bytes(const dvs_ref_t *rf) {
    if (rf->width == 0) return 0;
    if (rf->layout == DVS_REF_WORDS)
        return rf->width > DVS_MAX_WIDTH ? 0 : 4 * (size_t)((rf->width + 31u) / 32u);
    if (rf->layout != DVS_REF_SCALAR || rf->width > 64) return 0;
    return rf->width <= 8 ? 1 : rf->width <= 16 ? 2 : rf->width <= 32 ? 4 : 8;
}

/* Word i of a DVS_REF_WORDS ref (the words need not be aligned). */
static uint32_t ref_word(const dvs_ref_t *rf, size_t i) {
    uint32_t w;
    memcpy(&w, (const uint8_t *)rf->addr + 4 * i, 4);
    return w;
}

static void ref_set_word(const dvs_ref_t *rf, size_t i, uint32_t w) {
    memcpy((uint8_t *)rf->addr + 4 * i, &w, 4);
}

/* The low 64 bits of the value at `rf`, sign-extended from its width when
 * it is signed. `n`: ref_bytes(rf). */
static int64_t ref_read(const dvs_ref_t *rf, size_t n) {
    uint64_t u = 0;
    if (rf->layout == DVS_REF_WORDS) {
        u = ref_word(rf, 0);
        if (n > 4) u |= (uint64_t)ref_word(rf, 1) << 32;
    } else {
        const uint8_t *a = rf->addr;
        for (size_t i = 0; i < n; i++) u |= (uint64_t)a[i] << (8 * i);
    }
    if (rf->width < 64) {
        u &= (1ull << rf->width) - 1;
        if (rf->is_signed && (u >> (rf->width - 1)) & 1) u |= ~0ull << rf->width;
    }
    return (int64_t)u;
}

/* Write `v` through a DVS_REF_SCALAR ref, masked to its width. */
static void ref_write(const dvs_ref_t *rf, size_t n, int64_t v) {
    uint64_t u = (uint64_t)v;
    if (rf->width < 64) u &= (1ull << rf->width) - 1;
    uint8_t *a = rf->addr;
    for (size_t i = 0; i < n; i++) a[i] = (uint8_t)(u >> (8 * i));
}

/* Bit b of little-endian limbs. */
static int limb_bit(const uint64_t *l, uint32_t b) { return (int)((l[b / 64] >> (b % 64)) & 1); }

/* Keep bits [0, from) of `nl` limbs; set those of [from, nl * 64) to `one`. */
static void limbs_fill(uint64_t *l, uint32_t nl, uint32_t from, int one) {
    for (uint32_t i = from / 64; i < nl; i++) {
        uint64_t keep = i == from / 64 && from % 64 ? (1ull << (from % 64)) - 1
                        : i * 64 < from ? ~0ull : 0;
        l[i] = (l[i] & keep) | (one ? ~keep : 0);
    }
}

/* The value at `rf` as the n_limbs(width) limbs of a `width`-bit variable:
 * read at the ref's width, sign-extended (is_signed) or zero-extended to
 * `width`, the bits above it 0. */
static void ref_read_limbs(const dvs_ref_t *rf, uint64_t *l, uint32_t width) {
    const uint32_t nl = n_limbs(width);
    memset(l, 0, nl * sizeof *l);
    const uint32_t rw = rf->width < width ? rf->width : width;
    if (rf->layout == DVS_REF_WORDS) {
        for (uint32_t i = 0; 32 * i < rw; i++)
            l[i / 2] |= (uint64_t)ref_word(rf, i) << (32 * (i % 2));
    } else {
        l[0] = (uint64_t)ref_read(rf, ref_bytes(rf));
    }
    limbs_fill(l, nl, rw, rf->is_signed && rf->width < width && limb_bit(l, rw - 1));
    limbs_fill(l, nl, width, 0);
}

/* Write the `width`-bit value `l` (n_limbs(width) limbs) through `rf`,
 * masked to the ref's width; a wider ref is filled with the sign of a
 * signed value. */
static void ref_write_limbs(const dvs_ref_t *rf, const uint64_t *l, uint32_t width,
                            int is_signed) {
    if (rf->layout != DVS_REF_WORDS) {
        int64_t v = (int64_t)l[0];
        if (width < 64 && is_signed && limb_bit(l, width - 1)) v |= (int64_t)(~0ull << width);
        ref_write(rf, ref_bytes(rf), v);
        return;
    }
    const uint32_t nw = (rf->width + 31u) / 32u;
    const uint32_t fill = is_signed && limb_bit(l, width - 1) ? ~0u : 0u;
    for (uint32_t i = 0; i < nw; i++) {
        uint32_t w;
        if (32 * i >= width) {
            w = fill;
        } else {
            w = (uint32_t)(l[i / 2] >> (32 * (i % 2)));
            if (32 * (i + 1) > width)
                w = (w & ((1u << (width % 32)) - 1)) | (fill & ~((1u << (width % 32)) - 1));
        }
        if (i == nw - 1 && rf->width % 32) w &= (1u << (rf->width % 32)) - 1;
        ref_set_word(rf, i, w);
    }
}

/* Compile the context (again, when stale) and exclude the held randc values. */
static int handle_open(dvs_handle_t *h) {
    if (h->ctx && !h->stale) {
        dvs_solver_reset(h->ctx);   /* the clauses were settled after the last call */
        return 0;
    }
    if (h->ctx) dvs_solver_free(h->ctx);
    h->stale = 0;
    h->ctx = dvs_solver_new(NULL);
    if (!h->ctx) return -1;
    dvs_solver_set_engine(h->ctx, h->engine);
    h->compile_rc = dvs_solver_compile(h->ctx, h->p);
    if (h->compile_rc != DVS_COMPILE_OK) return 0;
    int held = 0;
    for (uint32_t v = 0; v < h->n_vars; v++)
        for (uint32_t k = 0; k < h->n_hist[v]; k++, held = 1)
            if (dvs_solver_exclude_value(h->ctx, v, h->hist[v][k]) != 0) return -1;
    /* An exclusion at a bound also tightens it, until the next reset: start
     * from the root every later call starts from (the holes alone). */
    if (held) dvs_solver_reset(h->ctx);
    return 0;
}

/* solve_once: CDCL cannot solve the problem whole; wide_solve can (not a
 * DVS_RANDOMIZE_* code). */
#define RANDOMIZE_PINNED 0x7FFF

static int solve_once(dvs_handle_t *h, const dvs_ref_t *refs, uint64_t seed) {
    if (handle_open(h) != 0) return DVS_RANDOMIZE_ERROR;
    if (h->compile_rc == DVS_COMPILE_UNSAT) return DVS_RANDOMIZE_UNSAT;
    /* CDCL left a constraint out (one over 64 bits it could not narrow) of a
     * problem it kept because of its STATE variables or solve order: solved
     * on bit-blast with the STATE values pinned, as a wide problem is. Not
     * with randc, soft constraints or dists, which bit-blast cannot honour,
     * nor when the caller asked for CDCL. */
    if (h->compile_rc > 0 && !h->wide_bad && h->engine != DVS_ENGINE_CDCL)
        return RANDOMIZE_PINNED;
    if (h->compile_rc != DVS_COMPILE_OK) return DVS_RANDOMIZE_ERROR;

    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || h->kind[v] != DVS_VAR_STATE || !refs[v].addr) continue;
        if (dvs_solver_pin_var(h->ctx, v, ref_read(&refs[v], ref_bytes(&refs[v]))) != 0)
            return DVS_RANDOMIZE_UNSAT;
    }
    /* The compile's problem keeps the offsets of h->p (dvs_sv.c copies). */
    dvs_solver_eval_dists(h->ctx, h->p);

    /* The front end's options (smt2_frontend.c, _cmd_check_sat_body), but
     * the search takes max_conflicts as its restart unit: at 10000 a solve
     * never restarted, and one stuck under a bad early decision ran to the
     * trail cap (simbench sum_partition on CDCL: 434 ms -> 7.3 ms per call
     * at 100). The conflict budget, the product, is kept. */
    dvs_solve_opts_t o;
    memset(&o, 0, sizeof o);
    o.seed = seed;
    o.max_conflicts = 100;
    o.max_restarts = 100000;
    o.use_lcg = 1;
    o.use_phase_save = 1;
    o.fair_pick = 1;
    o.canonical = (uint8_t)h->canonical;
    dvs_result_t res = dvs_solver_solve(h->ctx, &o);
    if (res == DVS_SOLVE_UNSAT) return DVS_RANDOMIZE_UNSAT;
    if (res != DVS_SOLVE_OK) return DVS_RANDOMIZE_ERROR;
    /* Compile took every constraint (compile_rc), so a model the propagators
     * accept satisfies the problem: re-checking it only finds solver bugs.
     * It cost 7-26% of a riscv-dv run, so tests turn it on (ctest does). */
    if (h->validate && dvs_solver_validate_model(h->ctx, h->p, NULL) != 0)
        return DVS_RANDOMIZE_ERROR;
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
        if (refs[v].addr && refs[v].layout == DVS_REF_WORDS) {
            uint64_t l = (uint64_t)x;
            ref_write_limbs(&refs[v], &l, h->width[v], h->sgn[v]);
        } else if (refs[v].addr) {
            ref_write(&refs[v], ref_bytes(&refs[v]), x);
        }
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

/* ------------------------------------------------------------------ */
/* Wide problems: bit-blast with the STATE values pinned                */
/* ------------------------------------------------------------------ */

static uint64_t mix64(uint64_t z) {
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    return z ^ (z >> 31);
}

static void wide_drop(dvs_handle_t *h) {
    if (h->wbb) dvs_bbsolver_free(h->wbb);
    h->wbb = NULL;
    free(h->wp);
    h->wp = NULL;
}

/* A copy of the problem with `var == value` added for each pinned var of
 * h->wcur; NULL when memory runs out. */
static dvs_problem_t *wide_pinned(const dvs_handle_t *h) {
    const dvs_problem_t *p = h->p;
    uint64_t extra = 0;
    for (uint32_t v = 0; v < h->n_vars; v++)
        if (h->declared[v] && h->wcur[h->woff[v]])
            extra += 256 + 8 * (uint64_t)n_limbs(h->width[v]);
    const uint64_t cap = (uint64_t)p->pool.used + extra;
    if (cap > UINT32_MAX - 64) return NULL;
    dvs_problem_t *q = malloc(sizeof *q + (size_t)cap);
    if (!q) return NULL;
    memcpy(q, p, sizeof *q + p->pool.used);
    q->pool.capacity = (uint32_t)cap;
    for (uint32_t v = 0; v < h->n_vars; v++) {
        const uint64_t *k = &h->wcur[h->woff[v]];
        if (!h->declared[v] || !k[0]) continue;
        dvs_expr_t c = expr_const_wide(q, n_limbs(h->width[v]), k + 1, h->width[v], h->sgn[v]);
        dvs_expr_t x = expr_var(q, v);
        dvs_expr_t eq = c == EXPR_NULL || x == EXPR_NULL ? EXPR_NULL
                                                        : expr_binary(q, DVS_BIN_EQ, x, c);
        if (eq == EXPR_NULL || problem_add_constraint(q, eq) == EXPR_NULL) {
            free(q);
            return NULL;
        }
    }
    return q;
}

/* Solve a problem with a variable over 64 bits once, and write the RAND
 * values on OK. A new instance is built when the STATE values differ from
 * those of the last one; otherwise the last one is resampled. */
static int wide_solve(dvs_handle_t *h, const dvs_ref_t *refs, uint64_t seed) {
    if (h->wide_bad) return DVS_RANDOMIZE_ERROR;
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v]) continue;
        uint64_t *k = &h->wcur[h->woff[v]];
        memset(k, 0, (h->woff[v + 1] - h->woff[v]) * sizeof *k);
        if (h->kind[v] != DVS_VAR_STATE || !refs[v].addr) continue;
        k[0] = 1;
        ref_read_limbs(&refs[v], k + 1, h->width[v]);
    }
    const size_t nk = h->woff[h->n_vars];
    if (!seed) seed = 0x9e3779b97f4a7c15ull;
    if (h->wbb && memcmp(h->wkey, h->wcur, nk * sizeof *h->wkey) == 0) {
        if (h->wrc == DVS_BB_UNSAT) return DVS_RANDOMIZE_UNSAT;
        if (dvs_bbsolver_resample(h->wbb, seed) != 0) return DVS_RANDOMIZE_ERROR;
    } else {
        wide_drop(h);
        h->wp = wide_pinned(h);
        h->wbb = h->wp ? dvs_bbsolver_new(NULL, h->wp) : NULL;
        if (!h->wbb) {
            wide_drop(h);
            return DVS_RANDOMIZE_ERROR;
        }
        dvs_bbsolver_set_resample(h->wbb, 1);
        h->wrc = dvs_bbsolver_check(h->wbb, seed);
        if (h->wrc != DVS_BB_SAT && h->wrc != DVS_BB_UNSAT) {
            wide_drop(h);
            return DVS_RANDOMIZE_ERROR;
        }
        memcpy(h->wkey, h->wcur, nk * sizeof *h->wkey);
        if (h->wrc == DVS_BB_UNSAT) return DVS_RANDOMIZE_UNSAT;
    }
    /* Read every value before writing any: a ref may alias a STATE input. */
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || !refs[v].addr) continue;
        if (h->kind[v] != DVS_VAR_RAND) continue;
        uint64_t *l = &h->wcur[h->woff[v] + 1];
        const uint32_t w = h->width[v];
        if (w > 64) {
            if (dvs_bbsolver_value_wide(h->wbb, v, l, n_limbs(w)) != 0)
                return DVS_RANDOMIZE_ERROR;
            continue;
        }
        /* As the context's bit-blast engine does (dvs_engine.c): a variable
         * no constraint reads may come back outside its declared bounds, and
         * is then drawn between them from the seed. */
        int64_t x = 0;
        uint64_t span = (uint64_t)h->hi[v] - (uint64_t)h->lo[v] + 1;   /* 0: all 2^64 */
        if (dvs_bbsolver_value(h->wbb, v, &x) != 0
                || (uint64_t)x - (uint64_t)h->lo[v] > span - 1) {
            uint64_t r = mix64(seed * 0x9e3779b97f4a7c15ull + v + 1);
            x = (int64_t)((uint64_t)h->lo[v] + (span ? r % span : r));
        }
        l[0] = (uint64_t)x;
        limbs_fill(l, 1, w, 0);
    }
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || !refs[v].addr || h->kind[v] != DVS_VAR_RAND) continue;
        ref_write_limbs(&refs[v], &h->wcur[h->woff[v] + 1], h->width[v], h->sgn[v]);
    }
    return DVS_RANDOMIZE_OK;
}

/* After a CDCL call: keep the clauses it used for the next call, or drop
 * them all. Kept whole they pile up with no bound, each call costing more
 * than the last; most sites learn only what one seed needs (`x != k`), and
 * keeping even the used ones gains nothing (scenario.dma_desc: they fire,
 * and the conflicts do not fall). A few learn a fact every call needs, and
 * dropped it is learnt again every call (memmap.nonoverlap_regions_8: 262
 * conflicts a call dropping, 1.4 keeping). So a site drops all by default
 * and, after a period of POLICY_CALLS calls with POLICY_MIN_CONFLICTS a
 * call or more, keeps for a period; it goes on keeping while the kept
 * clauses fire and a period has at most half the conflicts of the last
 * dropping one. A trial that fails waits twice as many periods for the
 * next. The decision reads only the calls so far, so the values stay a
 * function of the seeds. */
#define POLICY_CALLS         8u
#define POLICY_MIN_CONFLICTS 4u
#define POLICY_MAX_WAIT      64u
static void handle_settle(dvs_handle_t *h) {
    dvs_ctx_t *c = h->ctx;
    LCGCtx *lcg = c && !h->stale && dvs_solver_engine(c) == DVS_ENGINE_CDCL
                      ? (LCGCtx *)c->lcg : NULL;
    uint64_t conf = lcg ? lcg->n_analyses : 0;
    for (uint32_t i = 0; lcg && i < h->n_kept && i < lcg->clause_db.n_clauses; i++) {
        const Clause *cl = lcg->clause_db.clauses[i];
        h->fired |= cl && cl->last_used == lcg->clause_db.epoch;
    }
    h->sum += conf;
    if (++h->calls == POLICY_CALLS) {
        if (!h->keep) {
            h->ref = h->sum;
            if (h->wait) h->wait--;
            else h->keep = h->sum >= POLICY_CALLS * POLICY_MIN_CONFLICTS;
        } else if (h->fired && 2 * h->sum <= h->ref) {
            h->earned = 1;
        } else {
            h->backoff = h->earned ? 1 : h->backoff < POLICY_MAX_WAIT ? 2 * h->backoff : h->backoff;
            h->wait = h->backoff;
            h->keep = h->earned = 0;
        }
        h->calls = 0;
        h->sum = 0;
        h->fired = 0;
    }
    h->n_kept = 0;
    /* Pins: dvs_solver_reset drops what was learnt under them. A relaxed
     * soft constraint is a root bound (its assumption at 0) that the next
     * call, starting from every soft, does not have. */
    if (!lcg || c->n_root_pins) return;
    uint32_t na = c->n_assumptions;
    int relaxed = na && c->assumption_active_mask != (na >= 64 ? ~0ULL : (1ULL << na) - 1);
    if (h->keep && !relaxed) {
        /* The trail still holds this call's clause reasons; the next call's
         * reset drops them before any analysis reads one. */
        lcg_retain_used(lcg);
        h->n_kept = lcg->clause_db.n_clauses;
    } else {
        lcg_reset(lcg);
    }
}

dvs_tmpl_t *dvs_handle_tmpl(dvs_handle_t *h) {
    return h ? h->tmpl : NULL;
}

const dvs_problem_t *dvs_handle_problem(const dvs_handle_t *h) { return h->p; }

uint32_t dvs_handle_n_learnt(const dvs_handle_t *h) {
    return h->ctx ? lcg_n_clauses((const LCGCtx *)h->ctx->lcg) : 0;
}

uint64_t dvs_handle_n_conflicts(const dvs_handle_t *h) {
    return h->ctx ? h->ctx->conflict_count : 0;
}

/* Words of the policy at the head of a state export. */
#define STATE_HEAD 9

size_t dvs_handle_export_state(const dvs_handle_t *h, int64_t *buf, size_t cap) {
    int64_t head[STATE_HEAD] = { h->called ? dvs_handle_engine(h) : -1, h->keep, h->earned, h->fired,
                                 h->calls, (int64_t)h->sum, (int64_t)h->ref, h->wait,
                                 h->backoff };
    size_t n = 0;
#define PUT(x) do { if (n < cap) buf[n] = (x); n++; } while (0)
    for (uint32_t i = 0; i < STATE_HEAD; i++) PUT(head[i]);
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || h->kind[v] != DVS_VAR_RANDC) continue;
        PUT(h->n_hist[v]);
        for (uint32_t k = 0; k < h->n_hist[v]; k++) PUT(h->hist[v][k]);
    }
    /* The context's tier verdicts, its length first; a stale context goes. */
    const dvs_ctx_t *c = h->ctx && !h->stale && h->compile_rc == DVS_COMPILE_OK ? h->ctx : NULL;
    size_t nt = c ? dvs_search_export_tiers(c, NULL, 0) : 0;
    PUT((int64_t)nt);
    if (nt && n + nt <= cap) dvs_search_export_tiers(c, buf + n, nt);
    n += nt;
    /* Clauses learnt under pins assume them. */
    const LCGCtx *lcg = c && !c->n_root_pins ? (const LCGCtx *)c->lcg : NULL;
    for (uint32_t i = 0; lcg && i < lcg->clause_db.n_clauses; i++) {
        const Clause *cl = lcg->clause_db.clauses[i];
        if (!cl) continue;
        const Literal *l = (const Literal *)(cl + 1);
        PUT(cl->n_lits);
        PUT(cl->lbd);
        for (uint32_t j = 0; j < cl->n_lits; j++) {
            PUT((int64_t)l[j].var_id * 2 + l[j].is_lb);
            PUT(l[j].bound);
        }
    }
#undef PUT
    return n;
}

int dvs_handle_import_state(dvs_handle_t *h, const int64_t *buf, size_t n) {
    if (h->ctx || h->called || n < STATE_HEAD) return -1;
    /* Recompiled, the problem goes back to the engine its compile chose,
     * unless a search had given up and moved it to bitblast. */
    if (buf[0] == DVS_ENGINE_BITBLAST) h->engine = DVS_ENGINE_BITBLAST;
    h->called = buf[0] >= 0;
    h->keep = (uint8_t)buf[1];
    h->earned = (uint8_t)buf[2];
    h->fired = (uint8_t)buf[3];
    h->calls = (uint32_t)buf[4];
    h->sum = (uint64_t)buf[5];
    h->ref = (uint64_t)buf[6];
    h->wait = (uint32_t)buf[7];
    h->backoff = (uint32_t)buf[8];
    size_t i = STATE_HEAD;
    for (uint32_t v = 0; v < h->n_vars; v++) {
        if (!h->declared[v] || h->kind[v] != DVS_VAR_RANDC) continue;
        if (i >= n || (uint64_t)buf[i] > n - i - 1) return -1;
        uint32_t nh = (uint32_t)buf[i++];
        int64_t *x = malloc((nh ? nh : 1) * sizeof *x);
        if (!x) return -1;
        memcpy(x, buf + i, nh * sizeof *x);
        free(h->hist[v]);
        h->hist[v] = x;
        h->n_hist[v] = h->cap_hist[v] = nh;
        i += nh;
    }
    if (i >= n || (uint64_t)buf[i] > n - i - 1) return -1;
    size_t nt = (size_t)buf[i++];
    if (!nt) return i == n ? 0 : -1;
    /* Compile now (excluding the randc histories), load the tier verdicts,
     * and add the kept clauses as lcg_retain_used left them, from an epoch
     * now ended. */
    if (handle_open(h) != 0 || h->compile_rc != DVS_COMPILE_OK
            || dvs_search_import_tiers(h->ctx, buf + i, nt) != 0
            || dvs_search_lcg_open(h->ctx) != 0)
        return -1;
    i += nt;
    LCGCtx *lcg = (LCGCtx *)h->ctx->lcg;
    Literal lits[MAX_CLAUSE_LITS];
    while (i < n) {
        uint64_t nl = (uint64_t)buf[i++];
        if (nl == 0 || nl > MAX_CLAUSE_LITS || n - i < 2 * nl + 1) return -1;
        uint32_t lbd = (uint32_t)buf[i++];
        for (uint32_t j = 0; j < nl; j++, i += 2) {
            uint64_t vk = (uint64_t)buf[i];
            if (vk / 2 >= h->ctx->n_vars) return -1;
            lits[j] = (Literal){ .var_id = (uint32_t)(vk / 2), .is_lb = (uint8_t)(vk & 1u),
                                 .bound = buf[i + 1] };
        }
        if (clause_db_add(&lcg->clause_db, (uint32_t)nl, lits, lbd) == UINT32_MAX) return -1;
    }
    lcg->clause_db.epoch++;
    h->n_kept = lcg->clause_db.n_clauses;
    return 0;
}

dvs_shapes_t **dvs_handle_shapes(dvs_handle_t *h) { return &h->shapes; }

uint32_t dvs_handle_export(const dvs_handle_t *h, uint32_t k) {
    return k < h->p->n_exports ? h->exports[k] : NO_EXPORT;
}

uint32_t dvs_handle_n_slots(const dvs_handle_t *h) {
    return h ? h->p->n_slots : 0;
}

uint32_t dvs_handle_n_arrays(const dvs_handle_t *h) {
    return h && h->tmpl ? dvs_tmpl_n_arrays(h->tmpl) : 0;
}

static int randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes,
                     uint64_t seed);

static int randomize_arrays(dvs_handle_t *h, const dvs_ref_t *refs,
                            const dvs_array_ref_t *arrays, const uint32_t *modes,
                            uint64_t seed) {
    if (!h || !h->tmpl) return randomize(h, refs, modes, seed);
    if ((h->n_vars && !refs) || modes) return DVS_RANDOMIZE_ERROR;
    for (uint32_t v = 0; v < h->n_vars; v++)
        if (refs[v].addr && !ref_bytes(&refs[v])) return DVS_RANDOMIZE_ERROR;
    h->called = 1;
    return dvs_tmpl_randomize(h->tmpl, refs, h->n_vars, arrays, seed);
}

static int randomize_sizes(dvs_handle_t *h, const dvs_ref_t *refs, uint32_t *sizes,
                           uint64_t seed) {
    if (!h || (h->n_vars && !refs)) return DVS_RANDOMIZE_ERROR;
    if (!h->tmpl) return DVS_RANDOMIZE_OK;
    for (uint32_t v = 0; v < h->n_vars; v++)
        if (refs[v].addr && !ref_bytes(&refs[v])) return DVS_RANDOMIZE_ERROR;
    h->called = 1;
    return dvs_tmpl_randomize_sizes(h->tmpl, refs, h->n_vars, sizes, seed);
}

static int randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes,
                     uint64_t seed) {
    /* A part with slots is solved by dvs_randomize_parts() alone. */
    if (!h || h->tmpl || h->p->n_slots || (h->n_vars && !refs) || modes)
        return DVS_RANDOMIZE_ERROR;
    for (uint32_t v = 0; v < h->n_vars; v++)
        if (refs[v].addr && !ref_bytes(&refs[v])) return DVS_RANDOMIZE_ERROR;
    h->called = 1;
    if (h->wide) return wide_solve(h, refs, seed);

    for (int attempt = 0;; attempt++) {
        int rc = solve_once(h, refs, seed);
        if (rc == RANDOMIZE_PINNED) {
            /* From now on every call: the context goes. */
            dvs_solver_free(h->ctx);
            h->ctx = NULL;
            if (wide_setup(h)) return DVS_RANDOMIZE_ERROR;
            h->wide = 1;
            return wide_solve(h, refs, seed);
        }
        if (rc != DVS_RANDOMIZE_UNSAT || attempt) {
            if (rc == DVS_RANDOMIZE_OK) write_back(h, refs);
            handle_settle(h);
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

static int run(int kind, dvs_handle_t *h, const dvs_ref_t *refs, uint32_t *sizes,
               const dvs_array_ref_t *arrays, const uint32_t *modes, uint64_t seed) {
    return kind == DVS_CAP_SIZES    ? randomize_sizes(h, refs, sizes, seed)
           : kind == DVS_CAP_ARRAYS ? randomize_arrays(h, refs, arrays, modes, seed)
                                    : randomize(h, refs, modes, seed);
}

/* A call of `kind` on a handle, recorded when the handle is and the call is
 * not made by another (DVS_CAPTURE). */
static int recorded(int kind, dvs_handle_t *h, const dvs_ref_t *refs, uint32_t *sizes,
                    const dvs_array_ref_t *arrays, const uint32_t *modes, uint64_t seed) {
    uint32_t n_arrays = 0;
    uint8_t *elem_bytes = NULL;
    if ((kind == DVS_CAP_SIZES && sizes) || (kind == DVS_CAP_ARRAYS && arrays)) {
        n_arrays = dvs_handle_n_arrays(h);
        elem_bytes = dvs_capture_elem_bytes(h);
    }
    dvs_capbuf_t b;
    dvs_capture_begin(&b, h->cap_id, kind, modes != NULL, seed, refs, h->n_vars, sizes, arrays, elem_bytes,
                      n_arrays);
    int rc = run(kind, h, refs, sizes, arrays, modes, seed);
    dvs_capture_end(&b, kind, rc, refs, h->n_vars, sizes, arrays, elem_bytes, n_arrays);
    free(elem_bytes);
    return rc;
}

static int call(int kind, dvs_handle_t *h, const dvs_ref_t *refs, uint32_t *sizes,
                const dvs_array_ref_t *arrays, const uint32_t *modes, uint64_t seed) {
    int rc = dvs_capture_enter() && h && h->cap_id
                 ? recorded(kind, h, refs, sizes, arrays, modes, seed)
                 : run(kind, h, refs, sizes, arrays, modes, seed);
    dvs_capture_leave();
    return rc;
}

int dvs_randomize(dvs_handle_t *h, const dvs_ref_t *refs, const uint32_t *modes,
                  uint64_t seed) {
    return call(DVS_CAP_RANDOMIZE, h, refs, NULL, NULL, modes, seed);
}

int dvs_randomize_sizes(dvs_handle_t *h, const dvs_ref_t *refs, uint32_t *sizes,
                        uint64_t seed) {
    return call(DVS_CAP_SIZES, h, refs, sizes, NULL, NULL, seed);
}

int dvs_randomize_arrays(dvs_handle_t *h, const dvs_ref_t *refs,
                         const dvs_array_ref_t *arrays, const uint32_t *modes,
                         uint64_t seed) {
    return call(DVS_CAP_ARRAYS, h, refs, NULL, arrays, modes, seed);
}

uint32_t dvs_handle_cap_id(const dvs_handle_t *h) { return h ? h->cap_id : 0; }
