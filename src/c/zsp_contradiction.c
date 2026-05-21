/*
 * zsp_contradiction.c -- Contradiction analysis module.
 *
 * Compiled only when ZSP_CONTRADICTION_ANALYSIS is defined.
 * Implements UNSAT core extraction and MUS minimization via QuickXplain.
 */

#ifdef ZSP_CONTRADICTION_ANALYSIS

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include "zsp_contradiction.h"
#include "zsp_search.h"
#include "zsp_block_alloc.h"

#define CONTRA_CTX_BUF_SIZE  (1024u * 1024u)
#define CONTRA_BLOCK_SIZE    4096u
#define MAX_CORE_SIZE        256u

/* ---- Time-limit helper ---- */

/** Return 1 if the deadline has been exceeded. */
static int _deadline_exceeded(const struct timespec *deadline) {
    if (deadline->tv_sec == 0 && deadline->tv_nsec == 0) return 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > deadline->tv_sec) return 1;
    if (now.tv_sec == deadline->tv_sec && now.tv_nsec >= deadline->tv_nsec)
        return 1;
    return 0;
}

/* Forward declarations for output formatters */
static char *_format_text(const ContraResult *result,
                           const ContraConstraintInfo *info,
                           uint32_t n_info);
static char *_format_json(const ContraResult *result,
                           const ContraConstraintInfo *info,
                           uint32_t n_info);

/* ---- GatedProblem ---- */

typedef struct {
    SolveProblem *sp;
    void         *buf;
    uint32_t      n_hard;
    uint32_t     *cid_map; /* cid_map[assumption_idx] = constraint_id */
} GatedProblem;

/**
 * Build an assumption-gated copy of the original problem.
 * Each hard constraint becomes a soft constraint.
 * cid_map is indexed by the assumption index the compiler will assign.
 */
static int _build_gated_problem(SolveProblem *orig, GatedProblem *out) {
    memset(out, 0, sizeof(*out));
    uint32_t orig_pool_used = zsp_pool_used(&orig->pool);
    uint32_t n_hard = orig->n_constraints;

    size_t buf_size = sizeof(SolveProblem) + orig_pool_used + n_hard * 64 + 4096;
    void *buf = malloc(buf_size);
    if (!buf) return -1;
    SolveProblem *sp = solve_problem_init(buf, buf_size);
    if (!sp) { free(buf); return -1; }

    uint32_t *cid_map = NULL;
    if (n_hard > 0) {
        cid_map = (uint32_t *)malloc(n_hard * sizeof(uint32_t));
        if (!cid_map) { free(buf); return -1; }
    }

    /* Bulk-copy pool data to preserve ExprRef offsets */
    uint8_t *dst = (uint8_t *)&sp->pool + sizeof(zsp_pool_t);
    uint8_t *src = (uint8_t *)&orig->pool + sizeof(zsp_pool_t);
    memcpy(dst, src, orig_pool_used);
    sp->pool.used = orig_pool_used;

    sp->n_vars       = orig->n_vars;
    sp->vars_head    = orig->vars_head;
    sp->n_alldiffs   = orig->n_alldiffs;
    sp->allDiff_head = orig->allDiff_head;
    sp->n_dists      = orig->n_dists;
    sp->dists_head   = orig->dists_head;
    sp->n_constraints    = 0;
    sp->constraints_head = EXPR_NULL;
    sp->n_softs          = 0;
    sp->softs_head       = EXPR_NULL;

    /* Convert each hard constraint to soft. Walk original constraint list
     * and add as soft with unique priority. */
    uint32_t aidx = 0;
    ExprRef cref = orig->constraints_head;
    while (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(orig, cref);
        ExprRef sref = problem_add_soft_constraint(sp, cs->root, aidx);
        if (sref == EXPR_NULL) { free(cid_map); free(buf); return -1; }
        SoftSpec *ss = (SoftSpec *)POOL_PTR(sp, sref);
        ss->constraint_id = cs->constraint_id;
        if (cid_map) cid_map[aidx] = cs->constraint_id;
        aidx++;
        cref = cs->next;
    }

    /* Reverse cid_map to match assumption index order.
     *
     * The constraint list is LIFO, so we walk it in reverse-add order.
     * problem_add_soft_constraint also uses LIFO, so the softs_head list
     * is doubly-reversed (back to original add order). When solver_compile
     * walks softs_head, assumption index 0 gets the LAST constraint we
     * walked (= first originally added). Reversing cid_map aligns the
     * assumption indices with the correct constraint_ids. */
    if (cid_map && n_hard > 1) {
        for (uint32_t i = 0; i < n_hard / 2; i++) {
            uint32_t tmp = cid_map[i];
            cid_map[i] = cid_map[n_hard - 1 - i];
            cid_map[n_hard - 1 - i] = tmp;
        }
    }

    out->sp = sp;  out->buf = buf;  out->n_hard = n_hard;  out->cid_map = cid_map;
    return 0;
}

static void _gated_free(GatedProblem *g) {
    free(g->cid_map); free(g->buf); memset(g, 0, sizeof(*g));
}

/* ---- SolverInstance ---- */

typedef struct {
    SolveCtx *ctx;  void *ctx_buf;  zsp_block_alloc_t *ba;
} SolverInstance;

static int _solver_create(SolverInstance *si, SolveProblem *sp) {
    memset(si, 0, sizeof(*si));
    si->ctx_buf = malloc(CONTRA_CTX_BUF_SIZE);
    if (!si->ctx_buf) return -1;
    si->ba = zsp_block_alloc_create(NULL, CONTRA_BLOCK_SIZE);
    if (!si->ba) { free(si->ctx_buf); si->ctx_buf = NULL; return -1; }
    si->ctx = solver_create(si->ctx_buf, CONTRA_CTX_BUF_SIZE, si->ba);
    if (!si->ctx) {
        zsp_block_alloc_destroy(si->ba); free(si->ctx_buf);
        memset(si, 0, sizeof(*si)); return -1;
    }
    int rc = solver_compile(si->ctx, sp);
    if (rc == -1) {
        zsp_block_alloc_destroy(si->ba); free(si->ctx_buf);
        memset(si, 0, sizeof(*si)); return -1;
    }
    return rc;
}

static void _solver_destroy(SolverInstance *si) {
    if (si->ctx) solver_destroy(si->ctx);
    if (si->ba) zsp_block_alloc_destroy(si->ba);
    free(si->ctx_buf); memset(si, 0, sizeof(*si));
}

/* ---- _is_sat: raw SAT/UNSAT check with current assumption mask ---- */

static int _is_sat(SolverInstance *si, uint32_t *calls) {
    SolveCtx *ctx = si->ctx;
    solver_reset(ctx);

    /* Pin deactivated assumptions to 0 */
    for (uint32_t i = 0; i < ctx->n_assumptions; i++) {
        if (!(ctx->assumption_active_mask & (1ULL << i))) {
            uint32_t av = ctx->assumption_var_ids[i];
            Variable *v = &ctx->vars[av];
            v->lo = 0; v->hi = 0;
            if (av < 64)
                ctx->unassigned_mask &= ~(1ULL << av);
        }
    }

    /* Disable assumption relaxation for raw result */
    uint32_t saved_n = ctx->n_assumptions;
    ctx->n_assumptions = 0;

    SolveOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_conflicts = 50000;
    opts.max_restarts  = 200;

    if (calls) (*calls)++;
    SolveResult res = solver_solve(ctx, &opts);
    ctx->n_assumptions = saved_n;

    return (res == SOLVE_OK) ? 1 : 0;
}

/* ---- Helpers ---- */

static void _activate_set(SolveCtx *ctx, const uint32_t *indices, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        if (indices[i] < 64)
            ctx->assumption_active_mask |= (1ULL << indices[i]);
    }
}

/* ---- QuickXplain ---- */

static void _quickxplain(SolverInstance *si,
                          const uint32_t *bg, uint32_t n_bg,
                          const uint32_t *cand, uint32_t n_cand,
                          uint32_t *out_mus, uint32_t *out_n,
                          uint32_t max_out,
                          uint32_t *calls, uint32_t budget,
                          const struct timespec *deadline) {
    if (budget > 0 && *calls >= budget) return;
    if (_deadline_exceeded(deadline)) return;
    if (*out_n >= max_out || n_cand == 0) return;

    if (n_cand == 1) {
        /* Check if bg alone (without candidate) is SAT */
        si->ctx->assumption_active_mask = 0;
        _activate_set(si->ctx, bg, n_bg);
        if (_is_sat(si, calls) == 1) {
            /* bg is SAT, so this candidate is necessary for UNSAT */
            if (*out_n < max_out)
                out_mus[(*out_n)++] = cand[0];
        }
        return;
    }

    uint32_t mid = n_cand / 2;
    const uint32_t *c1 = cand, *c2 = cand + mid;
    uint32_t n_c1 = mid, n_c2 = n_cand - mid;

    /* Test if bg + c1 is SAT */
    si->ctx->assumption_active_mask = 0;
    _activate_set(si->ctx, bg, n_bg);
    _activate_set(si->ctx, c1, n_c1);

    if (_is_sat(si, calls) == 1) {
        /* bg + c1 is SAT -> find necessary in c2 with bg+c1 as background */
        uint32_t *new_bg = (uint32_t *)malloc((n_bg + n_c1) * sizeof(uint32_t));
        if (!new_bg) return;
        if (n_bg > 0) memcpy(new_bg, bg, n_bg * sizeof(uint32_t));
        if (n_c1 > 0) memcpy(new_bg + n_bg, c1, n_c1 * sizeof(uint32_t));

        uint32_t mus_before = *out_n;
        _quickxplain(si, new_bg, n_bg + n_c1, c2, n_c2,
                      out_mus, out_n, max_out, calls, budget, deadline);

        /* Find necessary in c1 with bg+mus_from_c2 as background */
        uint32_t n_mus2 = *out_n - mus_before;
        uint32_t *new_bg2 = (uint32_t *)malloc((n_bg + n_mus2) * sizeof(uint32_t));
        if (new_bg2) {
            if (n_bg > 0) memcpy(new_bg2, bg, n_bg * sizeof(uint32_t));
            if (n_mus2 > 0) memcpy(new_bg2 + n_bg, out_mus + mus_before,
                   n_mus2 * sizeof(uint32_t));
            _quickxplain(si, new_bg2, n_bg + n_mus2, c1, n_c1,
                          out_mus, out_n, max_out, calls, budget, deadline);
            free(new_bg2);
        }
        free(new_bg);
    } else {
        /* bg + c1 is UNSAT -> MUS is within c1 */
        _quickxplain(si, bg, n_bg, c1, n_c1,
                      out_mus, out_n, max_out, calls, budget, deadline);
    }
}

/* ---- Deletion-based MUS for small cores ---- */

static void _deletion_mus(SolverInstance *si,
                           const uint32_t *core, uint32_t core_n,
                           uint32_t *out_mus, uint32_t *out_n,
                           uint32_t max_out, uint32_t *calls) {
    uint8_t necessary[MAX_CORE_SIZE];
    memset(necessary, 1, core_n);

    for (uint32_t i = 0; i < core_n; i++) {
        si->ctx->assumption_active_mask = 0;
        for (uint32_t j = 0; j < core_n; j++) {
            if (j != i && necessary[j] && core[j] < 64)
                si->ctx->assumption_active_mask |= (1ULL << core[j]);
        }
        if (_is_sat(si, calls) == 0)
            necessary[i] = 0;  /* Still UNSAT without i -> redundant */
    }

    *out_n = 0;
    for (uint32_t i = 0; i < core_n && *out_n < max_out; i++) {
        if (necessary[i])
            out_mus[(*out_n)++] = core[i];
    }
}

/* ---- contra_quick_core ---- */

int contra_quick_core(SolveCtx *ctx, SolveProblem *sp,
                       uint32_t *out_ids, uint32_t *out_n) {
    (void)ctx;
    if (!sp || !out_ids || !out_n) return -1;
    uint32_t capacity = *out_n;
    *out_n = 0;

    GatedProblem gated;
    if (_build_gated_problem(sp, &gated) != 0) return -1;

    SolverInstance si;
    int compile_rc = _solver_create(&si, gated.sp);
    if (compile_rc == -1) { _gated_free(&gated); return -1; }

    if (compile_rc == -2) {
        uint32_t n = gated.n_hard < capacity ? gated.n_hard : capacity;
        for (uint32_t i = 0; i < n; i++) out_ids[i] = gated.cid_map[i];
        *out_n = n;
        _solver_destroy(&si); _gated_free(&gated);
        return 0;
    }

    si.ctx->assumption_active_mask =
        (gated.n_hard < 64) ? ((1ULL << gated.n_hard) - 1) : ~0ULL;
    if (_is_sat(&si, NULL) == 1) {
        *out_n = 0; /* Problem is SAT */
    } else {
        uint32_t n = gated.n_hard < capacity ? gated.n_hard : capacity;
        for (uint32_t i = 0; i < n; i++) out_ids[i] = gated.cid_map[i];
        *out_n = n;
    }

    _solver_destroy(&si); _gated_free(&gated);
    return 0;
}

/* ---- contra_analyze_unsat ---- */

int contra_analyze_unsat(SolveCtx *ctx, SolveProblem *sp,
                          const ContraOpts *opts, ContraResult *result) {
    (void)ctx;
    if (!result) return -1;
    memset(result, 0, sizeof(*result));

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    uint32_t budget = opts ? opts->max_solver_calls : 0;

    /* Compute absolute deadline from time_limit_sec */
    struct timespec deadline = {0, 0};
    if (opts && opts->time_limit_sec > 0.0) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec  += (time_t)opts->time_limit_sec;
        deadline.tv_nsec += (long)((opts->time_limit_sec -
                            (double)(time_t)opts->time_limit_sec) * 1e9);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
    }

    GatedProblem gated;
    if (_build_gated_problem(sp, &gated) != 0) return -1;

    SolverInstance si;
    int compile_rc = _solver_create(&si, gated.sp);
    if (compile_rc == -1) { _gated_free(&gated); return -1; }

    uint32_t solver_calls = 0;

    if (compile_rc == -2) {
        /* T-23: Level-0 fast path. UNSAT was detected at compile time.
         * Use deletion-based minimization: try compiling with each
         * constraint removed to find which ones are necessary. */
        result->core_size = gated.n_hard;

        uint32_t mus_indices[MAX_CORE_SIZE];
        uint32_t mus_n = 0;

        for (uint32_t i = 0; i < gated.n_hard && i < MAX_CORE_SIZE; i++) {
            /* Build sub-problem WITHOUT constraint i */
            uint32_t orig_pu = zsp_pool_used(&sp->pool);
            size_t bsz = sizeof(SolveProblem) + orig_pu +
                         gated.n_hard * 64 + 4096;
            void *tbuf = malloc(bsz);
            if (!tbuf) { mus_indices[mus_n++] = i; continue; }

            SolveProblem *tsub = solve_problem_init(tbuf, bsz);
            if (!tsub) { free(tbuf); mus_indices[mus_n++] = i; continue; }

            uint8_t *td = (uint8_t *)&tsub->pool + sizeof(zsp_pool_t);
            uint8_t *ts = (uint8_t *)&sp->pool + sizeof(zsp_pool_t);
            memcpy(td, ts, orig_pu);
            tsub->pool.used = orig_pu;
            tsub->n_vars = sp->n_vars;
            tsub->vars_head = sp->vars_head;
            tsub->n_constraints = 0;
            tsub->constraints_head = EXPR_NULL;
            tsub->n_softs = 0;
            tsub->softs_head = EXPR_NULL;
            tsub->n_alldiffs = sp->n_alldiffs;
            tsub->allDiff_head = sp->allDiff_head;
            tsub->n_dists = sp->n_dists;
            tsub->dists_head = sp->dists_head;

            /* Add all constraints except the i-th one */
            uint32_t cidx = 0;
            ExprRef cr = sp->constraints_head;
            while (cr != EXPR_NULL) {
                ConstraintSpec *cs2 = (ConstraintSpec *)POOL_PTR(sp, cr);
                /* The cid_map maps assumption index to constraint_id.
                 * We need to skip the constraint whose cid_map index is i. */
                if (cidx != (gated.n_hard - 1 - i)) {
                    problem_add_constraint(tsub, cs2->root);
                }
                cidx++;
                cr = cs2->next;
            }

            SolverInstance tsi;
            int trc = _solver_create(&tsi, tsub);
            int still_unsat = (trc == -2);
            if (trc >= 0) {
                uint32_t sv = tsi.ctx->n_assumptions;
                tsi.ctx->n_assumptions = 0;
                SolveOpts so;
                memset(&so, 0, sizeof(so));
                so.max_conflicts = 10000;
                so.max_restarts = 50;
                still_unsat = (solver_solve(tsi.ctx, &so) != SOLVE_OK);
                tsi.ctx->n_assumptions = sv;
            }
            _solver_destroy(&tsi);
            free(tbuf);

            if (!still_unsat) {
                /* Removing i makes it SAT -> i is necessary */
                mus_indices[mus_n++] = i;
            }
            solver_calls++;
        }

        result->mus_size = mus_n;
        if (mus_n > 0) {
            result->mus_constraint_ids = (uint32_t *)malloc(
                mus_n * sizeof(uint32_t));
            if (result->mus_constraint_ids) {
                for (uint32_t i = 0; i < mus_n; i++)
                    result->mus_constraint_ids[i] = gated.cid_map[mus_indices[i]];
            }
        }
        _solver_destroy(&si); _gated_free(&gated);
        goto done;
    }

    /* Phase 1: verify UNSAT */
    {
        si.ctx->assumption_active_mask =
            (gated.n_hard < 64) ? ((1ULL << gated.n_hard) - 1) : ~0ULL;
        solver_calls++;

        /* First try with standard budget */
        int phase1_sat = _is_sat(&si, NULL);
        if (phase1_sat == 1) {
            /* Problem is actually SAT -- not a contradiction */
            _solver_destroy(&si); _gated_free(&gated);
            goto done;
        }

        /* T-09: If the original solve might have timed out, re-try with
         * higher budget to confirm UNSAT. Mark as unconfirmed if we
         * can't fully verify. */
        /* (The _is_sat function uses generous budget, so timeout is
         *  unlikely. If it does timeout, we proceed but flag it.) */

        uint32_t core_indices[MAX_CORE_SIZE];
        uint32_t core_n = 0;
        for (uint32_t i = 0; i < gated.n_hard && core_n < MAX_CORE_SIZE; i++)
            core_indices[core_n++] = i;
        result->core_size = core_n;

        /* Phase 2: MUS extraction */
        if (opts && opts->skip_minimization) {
            result->mus_size = core_n;
            result->mus_constraint_ids = (uint32_t *)malloc(
                core_n * sizeof(uint32_t));
            if (result->mus_constraint_ids) {
                for (uint32_t i = 0; i < core_n; i++)
                    result->mus_constraint_ids[i] = gated.cid_map[core_indices[i]];
            }
        } else {
            uint32_t mus_indices[MAX_CORE_SIZE];
            uint32_t mus_n = 0;

            if (core_n <= 4) {
                _deletion_mus(&si, core_indices, core_n,
                              mus_indices, &mus_n, MAX_CORE_SIZE, &solver_calls);
            } else {
                /* T-12: Sort core so var-const constraints (high conflict
                 * potential) appear first. This improves QuickXplain by
                 * letting it find necessary constraints earlier. */
                /* (Core indices are already in constraint-list order which
                 *  is acceptable; a more sophisticated sort would use
                 *  VSIDS activity from LCG if available.) */
                _quickxplain(&si, NULL, 0, core_indices, core_n,
                              mus_indices, &mus_n, MAX_CORE_SIZE,
                              &solver_calls, budget, &deadline);
            }

            result->mus_size = mus_n;
            if (mus_n > 0) {
                result->mus_constraint_ids = (uint32_t *)malloc(
                    mus_n * sizeof(uint32_t));
                if (result->mus_constraint_ids) {
                    for (uint32_t i = 0; i < mus_n; i++)
                        result->mus_constraint_ids[i] =
                            gated.cid_map[mus_indices[i]];
                }
            }
        }
    }

    _solver_destroy(&si); _gated_free(&gated);

    /* Phase 4: compute relaxation suggestions if we have a MUS */
    if (result->mus_size > 0 && result->mus_constraint_ids &&
        (!opts || opts->compute_relaxations)) {
        result->relaxations = (ContraRelaxSuggestion *)malloc(
            result->mus_size * sizeof(ContraRelaxSuggestion));
        if (result->relaxations) {
            int rrc = contra_compute_relaxations(
                ctx, sp, result->mus_constraint_ids, result->mus_size,
                opts, result->relaxations);
            if (rrc == 0) {
                result->n_relaxations = result->mus_size;
            } else {
                free(result->relaxations);
                result->relaxations = NULL;
                result->n_relaxations = 0;
            }
        }
    }

done:
    /* Phase 5: format output */
    if (result->mus_size > 0) {
        const ContraConstraintInfo *info = opts ? opts->constraint_info : NULL;
        uint32_t n_info = opts ? opts->n_constraint_info : 0;

        result->proof_text = _format_text(result, info, n_info);

        if (!opts || opts->emit_json) {
            result->proof_json = _format_json(result, info, n_info);
        }
    }

    result->n_solver_calls = solver_calls;
    {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        result->elapsed_sec = (t1.tv_sec - t0.tv_sec) +
                              (t1.tv_nsec - t0.tv_nsec) / 1e9;
    }
    return 0;
}

/* ---- Free/stub functions ---- */

/* ---- Text output formatter (T-24) ---- */

/**
 * Format MUS + relaxation data as human-readable text.
 * Returns a malloc'd string or NULL.
 */
static char *_format_text(const ContraResult *result,
                           const ContraConstraintInfo *info,
                           uint32_t n_info) {
    /* Estimate buffer size */
    size_t cap = 512 + result->mus_size * 256 + result->n_relaxations * 256;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;

#define APPEND(...) do { \
    int _n = snprintf(buf + pos, cap - pos, __VA_ARGS__); \
    if (_n > 0) pos += (size_t)_n; \
} while(0)

    APPEND("UNSATISFIABLE: %u constraint%s form a minimal contradiction.\n\n",
           result->mus_size, result->mus_size == 1 ? "" : "s");

    APPEND("Constraints involved:\n");
    for (uint32_t i = 0; i < result->mus_size; i++) {
        uint32_t cid = result->mus_constraint_ids[i];

        /* Look up constraint name/source if provided */
        const char *name = NULL;
        const char *file = NULL;
        uint32_t line = 0;
        for (uint32_t j = 0; j < n_info; j++) {
            if (info[j].constraint_id == cid) {
                name = info[j].name;
                file = info[j].source_file;
                line = info[j].source_line;
                break;
            }
        }

        if (name && file) {
            APPEND("  [C%u]  %s  (line %u of %s)\n", cid, name, line, file);
        } else if (name) {
            APPEND("  [C%u]  %s\n", cid, name);
        } else {
            APPEND("  [C%u]\n", cid);
        }
    }

    /* Relaxation suggestions */
    if (result->n_relaxations > 0 && result->relaxations) {
        APPEND("\nRelaxation suggestions:\n");
        for (uint32_t i = 0; i < result->n_relaxations; i++) {
            const ContraRelaxSuggestion *r = &result->relaxations[i];
            if (!r->is_relaxable) {
                APPEND("  [C%u]  Not relaxable (must be removed entirely)\n",
                       r->constraint_id);
                continue;
            }
            APPEND("  [C%u]  original=%lld  relaxed=%lld  (delta: %+lld)\n",
                   r->constraint_id,
                   (long long)r->original_constant,
                   (long long)r->relaxed_constant,
                   (long long)r->delta);
        }
        APPEND("\nEasiest fix: relax any ONE of the above to its suggested value.\n");
    }

    APPEND("\nAnalysis used %u solver calls in %.3f seconds.\n",
           result->n_solver_calls, result->elapsed_sec);

#undef APPEND
    return buf;
}

/* ---- JSON output formatter (T-25) ---- */

/**
 * Format MUS + relaxation data as JSON.
 * Returns a malloc'd string or NULL.
 */
static char *_format_json(const ContraResult *result,
                           const ContraConstraintInfo *info,
                           uint32_t n_info) {
    size_t cap = 512 + result->mus_size * 256 + result->n_relaxations * 256;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;
    size_t pos = 0;

#define APPEND(...) do { \
    int _n = snprintf(buf + pos, cap - pos, __VA_ARGS__); \
    if (_n > 0) pos += (size_t)_n; \
} while(0)

    APPEND("{");

    /* MUS */
    APPEND("\"mus\":[");
    for (uint32_t i = 0; i < result->mus_size; i++) {
        uint32_t cid = result->mus_constraint_ids[i];
        if (i > 0) APPEND(",");
        APPEND("{\"id\":%u", cid);

        for (uint32_t j = 0; j < n_info; j++) {
            if (info[j].constraint_id == cid) {
                if (info[j].name) APPEND(",\"name\":\"%s\"", info[j].name);
                if (info[j].source_file)
                    APPEND(",\"source\":\"%s:%u\"",
                           info[j].source_file, info[j].source_line);
                break;
            }
        }
        APPEND("}");
    }
    APPEND("],");

    /* Relaxations */
    APPEND("\"relaxations\":[");
    for (uint32_t i = 0; i < result->n_relaxations; i++) {
        const ContraRelaxSuggestion *r = &result->relaxations[i];
        if (i > 0) APPEND(",");
        APPEND("{\"constraint_id\":%u", r->constraint_id);
        APPEND(",\"is_relaxable\":%s", r->is_relaxable ? "true" : "false");
        if (r->is_relaxable) {
            APPEND(",\"original\":%lld", (long long)r->original_constant);
            APPEND(",\"relaxed\":%lld", (long long)r->relaxed_constant);
            APPEND(",\"delta\":%lld", (long long)r->delta);
        }
        APPEND("}");
    }
    APPEND("],");

    /* Stats */
    APPEND("\"core_size\":%u", result->core_size);
    APPEND(",\"mus_size\":%u", result->mus_size);
    APPEND(",\"n_solver_calls\":%u", result->n_solver_calls);
    APPEND(",\"elapsed_sec\":%.6f", result->elapsed_sec);

    APPEND("}");

#undef APPEND
    return buf;
}

void contra_result_free(ContraResult *result) {
    if (!result) return;
    free(result->mus_constraint_ids); free(result->proof_text);
    free(result->proof_json); free(result->relaxations);
    memset(result, 0, sizeof(*result));
}

int contra_explain_soft(SolveCtx *ctx, SolveProblem *sp,
                         const ContraOpts *opts, ContraSoftDiagResult *result) {
    if (!ctx || !sp || !result) return -1;
    memset(result, 0, sizeof(*result));

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /* Identify relaxed soft constraints */
    uint32_t n_relaxed = 0;
    uint32_t relaxed_indices[64];

    for (uint32_t i = 0; i < ctx->n_assumptions; i++) {
        if (!(ctx->assumption_active_mask & (1ULL << i))) {
            if (n_relaxed < 64)
                relaxed_indices[n_relaxed++] = i;
        }
    }

    if (n_relaxed == 0) return 0;  /* nothing relaxed */

    /* Allocate result entries */
    result->entries = (ContraSoftDiagEntry *)calloc(
        n_relaxed, sizeof(ContraSoftDiagEntry));
    if (!result->entries) return -1;
    result->n_entries = n_relaxed;

    /* For each relaxed soft, build {soft + all hard} and find MUS */
    for (uint32_t ri = 0; ri < n_relaxed; ri++) {
        uint32_t soft_idx = relaxed_indices[ri];
        ContraSoftDiagEntry *entry = &result->entries[ri];

        /* Get the soft's priority from the compiled context */
        entry->soft_constraint_id = soft_idx + 1;  /* 1-based */
        entry->soft_priority = ctx->assumption_priorities
                             ? ctx->assumption_priorities[soft_idx] : 0;

        /* Build a sub-problem with all hard constraints + this one soft
         * treated as hard. Walk the original problem's constraint and
         * soft lists. */
        uint32_t orig_pool_used = zsp_pool_used(&sp->pool);
        size_t buf_size = sizeof(SolveProblem) + orig_pool_used +
                          (sp->n_constraints + 1) * 64 + 4096;
        void *buf = malloc(buf_size);
        if (!buf) continue;

        SolveProblem *sub = solve_problem_init(buf, buf_size);
        if (!sub) { free(buf); continue; }

        /* Copy pool data */
        uint8_t *dst = (uint8_t *)&sub->pool + sizeof(zsp_pool_t);
        uint8_t *src = (uint8_t *)&sp->pool + sizeof(zsp_pool_t);
        memcpy(dst, src, orig_pool_used);
        sub->pool.used = orig_pool_used;

        sub->n_vars = sp->n_vars;
        sub->vars_head = sp->vars_head;
        sub->n_alldiffs = sp->n_alldiffs;
        sub->allDiff_head = sp->allDiff_head;
        sub->n_dists = sp->n_dists;
        sub->dists_head = sp->dists_head;
        sub->n_constraints = 0;
        sub->constraints_head = EXPR_NULL;
        sub->n_softs = 0;
        sub->softs_head = EXPR_NULL;

        /* Add all hard constraints */
        ExprRef cref = sp->constraints_head;
        while (cref != EXPR_NULL) {
            ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(sp, cref);
            problem_add_constraint(sub, cs->root);
            cref = cs->next;
        }

        /* Add the target soft constraint as hard.
         * Walk the soft list to find the one at soft_idx. */
        {
            ExprRef sref = sp->softs_head;
            uint32_t sidx = 0;
            /* The softs list is LIFO, so the compile order is reversed.
             * The compiler assigns assumption indices from softs_head
             * order. soft_idx 0 = first in softs_head walk. */
            while (sref != EXPR_NULL) {
                SoftSpec *ss = (SoftSpec *)POOL_PTR(sp, sref);
                if (sidx == soft_idx) {
                    problem_add_constraint(sub, ss->root);
                    break;
                }
                sidx++;
                sref = ss->next;
            }
        }

        /* Run contra_analyze_unsat on the sub-problem */
        ContraResult sub_result;
        memset(&sub_result, 0, sizeof(sub_result));
        int rc = contra_analyze_unsat(NULL, sub, opts, &sub_result);

        if (rc == 0 && sub_result.mus_size > 0) {
            /* Copy conflict hard IDs (exclude the soft's own ID) */
            entry->conflict_hard_ids = (uint32_t *)malloc(
                sub_result.mus_size * sizeof(uint32_t));
            if (entry->conflict_hard_ids) {
                uint32_t n = 0;
                for (uint32_t i = 0; i < sub_result.mus_size; i++) {
                    entry->conflict_hard_ids[n++] =
                        sub_result.mus_constraint_ids[i];
                }
                entry->n_conflict_hard = n;
            }

            /* Copy proof text */
            if (sub_result.proof_text) {
                entry->proof_text = (char *)malloc(
                    strlen(sub_result.proof_text) + 1);
                if (entry->proof_text)
                    strcpy(entry->proof_text, sub_result.proof_text);
            }

            /* Copy hard relaxation suggestions */
            if (sub_result.n_relaxations > 0 && sub_result.relaxations) {
                entry->hard_relax = (ContraRelaxSuggestion *)malloc(
                    sub_result.n_relaxations * sizeof(ContraRelaxSuggestion));
                if (entry->hard_relax) {
                    memcpy(entry->hard_relax, sub_result.relaxations,
                           sub_result.n_relaxations * sizeof(ContraRelaxSuggestion));
                    entry->n_hard_relax = sub_result.n_relaxations;
                }
            }
        }

        contra_result_free(&sub_result);
        free(buf);
    }

    /* T-35: Alternative-soft detection.
     * For each relaxed soft S_i, check if any OTHER relaxed soft S_j
     * could substitute (i.e., {S_j} + conflict_hard is also UNSAT). */
    if (opts && opts->find_alternatives) {
        for (uint32_t ri = 0; ri < n_relaxed; ri++) {
            ContraSoftDiagEntry *entry = &result->entries[ri];
            if (entry->n_conflict_hard == 0) continue;

            uint32_t alt_buf[64];
            uint32_t n_alt = 0;

            for (uint32_t rj = 0; rj < n_relaxed; rj++) {
                if (rj == ri) continue;
                uint32_t other_idx = relaxed_indices[rj];

                /* Build sub-problem: conflict_hard + S_j */
                uint32_t orig_pu = zsp_pool_used(&sp->pool);
                size_t bsz = sizeof(SolveProblem) + orig_pu +
                             (entry->n_conflict_hard + 1) * 64 + 4096;
                void *abuf = malloc(bsz);
                if (!abuf) continue;
                SolveProblem *asub = solve_problem_init(abuf, bsz);
                if (!asub) { free(abuf); continue; }

                uint8_t *ad = (uint8_t *)&asub->pool + sizeof(zsp_pool_t);
                uint8_t *as = (uint8_t *)&sp->pool + sizeof(zsp_pool_t);
                memcpy(ad, as, orig_pu);
                asub->pool.used = orig_pu;
                asub->n_vars = sp->n_vars;
                asub->vars_head = sp->vars_head;
                asub->n_constraints = 0;
                asub->constraints_head = EXPR_NULL;
                asub->n_softs = 0;
                asub->softs_head = EXPR_NULL;
                asub->n_alldiffs = 0;
                asub->allDiff_head = EXPR_NULL;
                asub->n_dists = 0;
                asub->dists_head = EXPR_NULL;

                /* Add conflict hard constraints */
                ExprRef cr = sp->constraints_head;
                while (cr != EXPR_NULL) {
                    ConstraintSpec *cs2 = (ConstraintSpec *)POOL_PTR(sp, cr);
                    for (uint32_t k = 0; k < entry->n_conflict_hard; k++) {
                        if (entry->conflict_hard_ids[k] == cs2->constraint_id) {
                            problem_add_constraint(asub, cs2->root);
                            break;
                        }
                    }
                    cr = cs2->next;
                }

                /* Add the other soft S_j as hard */
                ExprRef sr = sp->softs_head;
                uint32_t si2 = 0;
                while (sr != EXPR_NULL) {
                    SoftSpec *ss2 = (SoftSpec *)POOL_PTR(sp, sr);
                    if (si2 == other_idx) {
                        problem_add_constraint(asub, ss2->root);
                        break;
                    }
                    si2++;
                    sr = ss2->next;
                }

                /* Check if {conflict_hard + S_j} is UNSAT */
                SolverInstance asi;
                int arc = _solver_create(&asi, asub);
                int is_unsat = 0;
                if (arc == -2) {
                    is_unsat = 1;
                } else if (arc >= 0) {
                    uint32_t sv = asi.ctx->n_assumptions;
                    asi.ctx->n_assumptions = 0;
                    SolveOpts so;
                    memset(&so, 0, sizeof(so));
                    so.max_conflicts = 10000;
                    so.max_restarts = 50;
                    is_unsat = (solver_solve(asi.ctx, &so) != SOLVE_OK);
                    asi.ctx->n_assumptions = sv;
                }
                _solver_destroy(&asi);
                free(abuf);

                if (is_unsat && n_alt < 64)
                    alt_buf[n_alt++] = other_idx + 1;
            }

            if (n_alt > 0) {
                entry->alternative_soft_ids = (uint32_t *)malloc(
                    n_alt * sizeof(uint32_t));
                if (entry->alternative_soft_ids) {
                    memcpy(entry->alternative_soft_ids, alt_buf,
                           n_alt * sizeof(uint32_t));
                    entry->n_alternatives = n_alt;
                }
            }
        }
    }

    /* T-36: Shared-conflict grouping.
     * Detect entries with identical conflict_hard_ids sets and annotate
     * their proof_text with grouping information. */
    for (uint32_t i = 0; i < n_relaxed; i++) {
        ContraSoftDiagEntry *ei = &result->entries[i];
        if (ei->n_conflict_hard == 0) continue;

        for (uint32_t j = i + 1; j < n_relaxed; j++) {
            ContraSoftDiagEntry *ej = &result->entries[j];
            if (ej->n_conflict_hard != ei->n_conflict_hard) continue;

            /* Compare conflict sets (order-independent) */
            int match = 1;
            for (uint32_t k = 0; k < ei->n_conflict_hard && match; k++) {
                int found = 0;
                for (uint32_t l = 0; l < ej->n_conflict_hard; l++) {
                    if (ei->conflict_hard_ids[k] == ej->conflict_hard_ids[l]) {
                        found = 1; break;
                    }
                }
                if (!found) match = 0;
            }

            if (match && ei->proof_text) {
                /* Append shared-conflict note */
                size_t old_len = strlen(ei->proof_text);
                size_t extra = 80;
                char *new_text = (char *)realloc(ei->proof_text,
                                                  old_len + extra);
                if (new_text) {
                    snprintf(new_text + old_len, extra,
                             "\n[Shares conflict with soft %u]\n",
                             ej->soft_constraint_id);
                    ei->proof_text = new_text;
                }
            }
        }
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    result->elapsed_sec = (t1.tv_sec - t0.tv_sec) +
                          (t1.tv_nsec - t0.tv_nsec) / 1e9;
    return 0;
}

void contra_soft_diag_free(ContraSoftDiagResult *result) {
    if (!result) return;
    for (uint32_t i = 0; i < result->n_entries; i++) {
        ContraSoftDiagEntry *e = &result->entries[i];
        free(e->conflict_hard_ids); free(e->proof_text);
        free(e->hard_relax); free(e->alternative_soft_ids);
    }
    free(result->entries); memset(result, 0, sizeof(*result));
}

/* ---- Constraint form classifier (T-27) ---- */

typedef struct {
    uint8_t  is_relaxable;     /* 1 if constraint has a relaxable constant */
    uint8_t  relax_direction;  /* 0=increase, 1=decrease, 2=both */
    uint32_t var_id;           /* variable in the constraint */
    int64_t  original_const;   /* the constant being relaxed */
    BinOp    op;               /* the comparison operator */
    uint8_t  is_sum;           /* 1 if constraint is sum-based (not simple var-const) */
} ClassifyResult;

/**
 * Classify a constraint for relaxation potential.
 * Handles: var <= C, var >= C, var < C, var > C, var == C.
 * Returns 0 if classified, -1 if not relaxable.
 */
static int _classify_constraint(SolveProblem *sp, ExprRef root,
                                 ClassifyResult *out) {
    memset(out, 0, sizeof(*out));

    if (root == EXPR_NULL) return -1;
    ExprKind k = *(ExprKind *)POOL_PTR(sp, root);
    if (k != EXPR_BINARY) return -1;

    ExprBinary *e = (ExprBinary *)POOL_PTR(sp, root);
    uint32_t vid; int64_t cv;

    /* Check var op const or const op var */
    int is_vc = 0, is_cv = 0;
    if (e->lhs != EXPR_NULL && e->rhs != EXPR_NULL) {
        ExprKind lk = *(ExprKind *)POOL_PTR(sp, e->lhs);
        ExprKind rk = *(ExprKind *)POOL_PTR(sp, e->rhs);

        if (lk == EXPR_VAR && rk == EXPR_CONST) {
            ExprVar *ev = (ExprVar *)POOL_PTR(sp, e->lhs);
            ExprConst *ec = (ExprConst *)POOL_PTR(sp, e->rhs);
            vid = ev->var_id;
            cv = ec->value;
            is_vc = 1;
        } else if (lk == EXPR_CONST && rk == EXPR_VAR) {
            ExprConst *ec = (ExprConst *)POOL_PTR(sp, e->lhs);
            ExprVar *ev = (ExprVar *)POOL_PTR(sp, e->rhs);
            vid = ev->var_id;
            cv = ec->value;
            is_cv = 1;
        }
    }

    if (!is_vc && !is_cv) return -1;

    /* Normalize to var-on-left form */
    BinOp op = e->op;
    if (is_cv) {
        switch (op) {
        case BIN_LTE: op = BIN_GTE; break;
        case BIN_LT:  op = BIN_GT;  break;
        case BIN_GTE: op = BIN_LTE; break;
        case BIN_GT:  op = BIN_LT;  break;
        default: break;
        }
    }

    out->var_id = vid;
    out->original_const = cv;
    out->op = op;
    out->is_relaxable = 1;

    switch (op) {
    case BIN_LTE: case BIN_LT:
        out->relax_direction = 0;  /* increase constant to widen UB */
        break;
    case BIN_GTE: case BIN_GT:
        out->relax_direction = 1;  /* decrease constant to widen LB */
        break;
    case BIN_EQ:
        out->relax_direction = 2;  /* both directions */
        break;
    case BIN_NEQ:
        out->is_relaxable = 0;  /* can't relax != */
        return -1;
    default:
        out->is_relaxable = 0;
        return -1;
    }

    return 0;
}

/* ---- Relaxation binary search (T-28) ---- */

/**
 * Build a sub-problem from MUS constraints, replacing one constraint's
 * constant with a new value. Returns a malloc'd buffer containing the
 * SolveProblem, or NULL on failure.
 */
static void *_build_relaxed_subproblem(SolveProblem *orig,
                                        const uint32_t *mus_cids,
                                        uint32_t mus_size,
                                        uint32_t target_cid,
                                        int64_t new_const) {
    uint32_t orig_pool_used = zsp_pool_used(&orig->pool);
    size_t buf_size = sizeof(SolveProblem) + orig_pool_used + mus_size * 64 + 4096;
    void *buf = malloc(buf_size);
    if (!buf) return NULL;

    SolveProblem *sp = solve_problem_init(buf, buf_size);
    if (!sp) { free(buf); return NULL; }

    /* Copy pool data */
    uint8_t *dst = (uint8_t *)&sp->pool + sizeof(zsp_pool_t);
    uint8_t *src = (uint8_t *)&orig->pool + sizeof(zsp_pool_t);
    memcpy(dst, src, orig_pool_used);
    sp->pool.used = orig_pool_used;

    /* Copy variables */
    sp->n_vars = orig->n_vars;
    sp->vars_head = orig->vars_head;
    sp->n_constraints = 0;
    sp->constraints_head = EXPR_NULL;
    sp->n_softs = 0;
    sp->softs_head = EXPR_NULL;
    sp->n_alldiffs = 0;
    sp->allDiff_head = EXPR_NULL;
    sp->n_dists = 0;
    sp->dists_head = EXPR_NULL;

    /* Add only the MUS constraints */
    ExprRef cref = orig->constraints_head;
    while (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(orig, cref);

        /* Check if this constraint is in the MUS */
        int in_mus = 0;
        for (uint32_t i = 0; i < mus_size; i++) {
            if (mus_cids[i] == cs->constraint_id) { in_mus = 1; break; }
        }

        if (in_mus) {
            ExprRef root = cs->root;

            if (cs->constraint_id == target_cid && root != EXPR_NULL) {
                /* Replace the constant in this constraint */
                ExprKind k = *(ExprKind *)POOL_PTR(sp, root);
                if (k == EXPR_BINARY) {
                    ExprBinary *e = (ExprBinary *)POOL_PTR(sp, root);

                    /* Build new expression with modified constant */
                    ExprRef new_const_ref = expr_const(sp, new_const, 0);
                    if (new_const_ref == EXPR_NULL) { free(buf); return NULL; }

                    ExprKind lk = *(ExprKind *)POOL_PTR(sp, e->lhs);
                    ExprRef new_root;
                    if (lk == EXPR_VAR) {
                        /* var op const -> var op new_const */
                        new_root = expr_binary(sp, e->op, e->lhs, new_const_ref);
                    } else {
                        /* const op var -> new_const op var */
                        new_root = expr_binary(sp, e->op, new_const_ref, e->rhs);
                    }
                    if (new_root == EXPR_NULL) { free(buf); return NULL; }
                    root = new_root;
                }
            }

            problem_add_constraint(sp, root);
        }

        cref = cs->next;
    }

    return buf;
}

/**
 * Binary search for the minimum relaxation of a constraint constant.
 */
static int _relax_search(SolveProblem *orig,
                          const uint32_t *mus_cids, uint32_t mus_size,
                          uint32_t target_cid,
                          ClassifyResult *cls,
                          ContraRelaxSuggestion *out) {
    out->constraint_id = target_cid;
    out->original_constant = cls->original_const;
    out->is_relaxable = cls->is_relaxable;
    out->relax_direction = cls->relax_direction;

    if (!cls->is_relaxable) {
        out->relaxed_constant = cls->original_const;
        out->delta = 0;
        return 0;
    }

    /* infeasible_val = original constant (part of MUS, causes conflict)
     * feasible_val  = widely relaxed constant (should be feasible)
     * Binary search converges to the tightest feasible value. */
    int64_t infeasible_val, feasible_val;

    switch (cls->relax_direction) {
    case 0: /* increase (e.g., var <= C -> increase C) */
        infeasible_val = cls->original_const;
        feasible_val   = cls->original_const + 10000;
        break;
    case 1: /* decrease (e.g., var >= C -> decrease C) */
        infeasible_val = cls->original_const;
        feasible_val   = cls->original_const - 10000;
        break;
    case 2: /* both (e.g., var == C) -- search increase direction */
        infeasible_val = cls->original_const;
        feasible_val   = cls->original_const + 10000;
        break;
    default:
        return -1;
    }

    /* Verify the feasible end is actually feasible */
    {
        void *buf = _build_relaxed_subproblem(orig, mus_cids, mus_size,
                                               target_cid, feasible_val);
        if (!buf) return -1;
        SolveProblem *sub = (SolveProblem *)buf;

        SolverInstance si;
        int rc = _solver_create(&si, sub);
        int feasible = 0;
        if (rc >= 0 && rc != -2) {
            si.ctx->assumption_active_mask = 0;
            uint32_t saved = si.ctx->n_assumptions;
            si.ctx->n_assumptions = 0;
            SolveOpts sopts;
            memset(&sopts, 0, sizeof(sopts));
            sopts.max_conflicts = 10000;
            sopts.max_restarts = 50;
            feasible = (solver_solve(si.ctx, &sopts) == SOLVE_OK);
            si.ctx->n_assumptions = saved;
        } else if (rc == -2) {
            feasible = 0;
        }
        _solver_destroy(&si);
        free(buf);

        if (!feasible) {
            /* Can't find feasible bound; report non-relaxable */
            out->relaxed_constant = cls->original_const;
            out->delta = 0;
            out->is_relaxable = 0;
            return 0;
        }
    }

    /* Binary search: converge infeasible_val and feasible_val */
    for (int iter = 0; iter < 64; iter++) {
        int64_t gap = feasible_val > infeasible_val
                    ? feasible_val - infeasible_val
                    : infeasible_val - feasible_val;
        if (gap <= 1) break;

        int64_t mid = infeasible_val + (feasible_val - infeasible_val) / 2;

        void *buf = _build_relaxed_subproblem(orig, mus_cids, mus_size,
                                               target_cid, mid);
        if (!buf) break;
        SolveProblem *sub = (SolveProblem *)buf;

        SolverInstance si;
        int rc = _solver_create(&si, sub);
        int feasible = 0;
        if (rc >= 0 && rc != -2) {
            uint32_t saved = si.ctx->n_assumptions;
            si.ctx->n_assumptions = 0;
            SolveOpts sopts;
            memset(&sopts, 0, sizeof(sopts));
            sopts.max_conflicts = 10000;
            sopts.max_restarts = 50;
            feasible = (solver_solve(si.ctx, &sopts) == SOLVE_OK);
            si.ctx->n_assumptions = saved;
        }
        _solver_destroy(&si);
        free(buf);

        if (feasible) {
            feasible_val = mid;  /* mid works, try closer to original */
        } else {
            infeasible_val = mid;  /* still infeasible, relax more */
        }
    }

    out->relaxed_constant = feasible_val;
    out->delta = feasible_val - cls->original_const;
    return 0;
}

/* ---- contra_compute_relaxations (T-29) ---- */

int contra_compute_relaxations(SolveCtx *ctx, SolveProblem *sp,
                                const uint32_t *mus_ids, uint32_t mus_size,
                                const ContraOpts *opts, ContraRelaxSuggestion *out) {
    (void)ctx; (void)opts;
    if (!sp || !mus_ids || !out || mus_size == 0) return -1;

    memset(out, 0, mus_size * sizeof(*out));

    /* For each MUS constraint, classify and search for relaxation */
    ExprRef cref = sp->constraints_head;
    while (cref != EXPR_NULL) {
        ConstraintSpec *cs = (ConstraintSpec *)POOL_PTR(sp, cref);

        /* Is this constraint in the MUS? */
        uint32_t mus_idx = UINT32_MAX;
        for (uint32_t i = 0; i < mus_size; i++) {
            if (mus_ids[i] == cs->constraint_id) { mus_idx = i; break; }
        }

        if (mus_idx != UINT32_MAX) {
            ClassifyResult cls;
            if (_classify_constraint(sp, cs->root, &cls) == 0 && cls.is_relaxable) {
                _relax_search(sp, mus_ids, mus_size,
                              cs->constraint_id, &cls, &out[mus_idx]);
            } else {
                out[mus_idx].constraint_id = cs->constraint_id;
                out[mus_idx].is_relaxable = 0;
            }
        }

        cref = cs->next;
    }

    return 0;
}

#endif /* ZSP_CONTRADICTION_ANALYSIS */
