#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include "zsp_costguided.h"
#include "zsp_ctx.h"
#include "zsp_search.h"

/* ================================================================== */
/* CostGuided value selector callback                                  */
/*                                                                     */
/* This is the function installed via solver_set_value_selector().     */
/* It scans the variable's domain, evaluates the cost function at     */
/* each candidate value, and returns the minimum-cost value.          */
/* ================================================================== */

/* Track top-K candidates during cost evaluation */
#define CG_TOP_K 5

typedef struct {
    int64_t value;
    int64_t cost;
} CostCandidate;

static void _insert_candidate(CostCandidate *top, int *n,
                                int64_t val, int64_t cost) {
    /* Maintain sorted top-K (ascending by cost = best first) */
    if (*n < CG_TOP_K) {
        int i = *n;
        while (i > 0 && top[i-1].cost > cost) {
            top[i] = top[i-1]; i--;
        }
        top[i].value = val; top[i].cost = cost;
        (*n)++;
    } else if (cost < top[CG_TOP_K-1].cost) {
        int i = CG_TOP_K - 1;
        while (i > 0 && top[i-1].cost > cost) {
            top[i] = top[i-1]; i--;
        }
        top[i].value = val; top[i].cost = cost;
    }
}

static int64_t _costguided_select(SolveCtx *ctx, uint32_t var_id,
                                   void *user_data) {
    CostGuidedCtx *cg = (CostGuidedCtx *)user_data;
    if (!cg || !cg->cost_fn) return var_lo64(ctx, &ctx->vars[var_id]);

    int64_t lo = var_lo64(ctx, &ctx->vars[var_id]);
    int64_t hi = var_hi64(ctx, &ctx->vars[var_id]);
    int64_t domain_size = hi - lo + 1;

    /* Narrow domains: skip cost evaluation, let systematic handle it */
    if (domain_size <= 10)
        return lo;

    /* Quick check: if cost at both endpoints is 0, no placed neighbors
     * yet -- fall back to default value selection. */
    int64_t c_lo = cg->cost_fn(ctx, var_id, lo, cg->cost_data);
    int64_t c_hi = cg->cost_fn(ctx, var_id, hi, cg->cost_data);
    if (c_lo == 0 && c_hi == 0)
        return -1;

    /* Evaluate sampled positions, keeping top-K candidates */
    int32_t max_s = cg->max_domain_scan > 0 ? cg->max_domain_scan : 100;
    int64_t step = 1;
    if (domain_size > max_s) step = domain_size / max_s;

    CostCandidate top[CG_TOP_K];
    int n_top = 0;
    _insert_candidate(top, &n_top, lo, c_lo);
    _insert_candidate(top, &n_top, hi, c_hi);

    for (int64_t v = lo + step; v < hi; v += step) {
        int64_t c = cg->cost_fn(ctx, var_id, v, cg->cost_data);
        _insert_candidate(top, &n_top, v, c);
    }

    /* Pick from top-K with randomization for search diversity.
     * Use the solver's RNG so different restarts explore different
     * positions among the near-optimal candidates. */
    if (n_top <= 1) return top[0].value;

    /* Weighted selection: favor lower cost but allow diversity */
    uint64_t rng = ctx->rng_state;
    rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
    ctx->rng_state = rng;
    uint32_t pick = (uint32_t)(rng % (uint64_t)n_top);
    /* Bias toward best: 50% chance of picking best, else uniform */
    if (pick == 0 || (rng & 1))
        return top[0].value;
    return top[pick].value;
}

/* ================================================================== */
/* Public API                                                          */
/* ================================================================== */

void solver_set_cost_guided(SolveCtx *ctx, CostFunc cost_fn,
                             void *cost_data, int32_t max_scan) {
    if (!ctx) return;

    /* Allocate the CostGuidedCtx in the solver's pool so it lives
     * as long as the context. */
    uint32_t ref = zsp_pool_alloc(&ctx->pool, (uint32_t)sizeof(CostGuidedCtx),
                                   (uint32_t)_Alignof(CostGuidedCtx));
    if (ref == EXPR_NULL) return;

    CostGuidedCtx *cg = (CostGuidedCtx *)zsp_pool_ptr(&ctx->pool, ref);
    cg->cost_fn = cost_fn;
    cg->cost_data = cost_data;
    cg->max_domain_scan = max_scan;

    solver_set_value_selector(ctx, _costguided_select, cg);
}

/* ================================================================== */
/* HPWL Cost Function                                                  */
/* ================================================================== */

/* Find which macro and axis a variable corresponds to. */
static int _find_macro_axis(const HPWLCostCtx *hctx, uint32_t var_id,
                             uint32_t *out_macro, int *out_is_y) {
    for (uint32_t m = 0; m < hctx->n_macros; m++) {
        if (hctx->macros[m].x_var_id == var_id) {
            *out_macro = m; *out_is_y = 0; return 1;
        }
        if (hctx->macros[m].y_var_id == var_id) {
            *out_macro = m; *out_is_y = 1; return 1;
        }
    }
    return 0;
}

int64_t hpwl_cost_fn(const SolveCtx *ctx, uint32_t var_id,
                      int64_t value, void *user_data) {
    HPWLCostCtx *hctx = (HPWLCostCtx *)user_data;
    if (!hctx) return 0;

    uint32_t macro_idx;
    int is_y;
    if (!_find_macro_axis(hctx, var_id, &macro_idx, &is_y))
        return 0;  /* not a placement variable -- zero cost */

    int64_t total_cost = 0;
    int32_t p = (int32_t)value;

    /* Bounded overlap penalty: check at most a few placed macros
     * to steer away from obvious overlaps without O(N) scan cost. */
    if (hctx->macro_widths) {
        int32_t my_dim = is_y ? hctx->macro_heights[macro_idx]
                              : hctx->macro_widths[macro_idx];
        int checked = 0;
        for (uint32_t m = 0; m < hctx->n_macros && checked < 8; m++) {
            if (m == macro_idx) continue;
            uint32_t ox_var = hctx->macros[m].x_var_id;
            uint32_t oy_var = hctx->macros[m].y_var_id;
            int64_t ox_lo = var_lo64(ctx, &ctx->vars[ox_var]);
            int64_t ox_hi = var_hi64(ctx, &ctx->vars[ox_var]);
            int64_t oy_lo = var_lo64(ctx, &ctx->vars[oy_var]);
            int64_t oy_hi = var_hi64(ctx, &ctx->vars[oy_var]);
            if (ox_lo != ox_hi || oy_lo != oy_hi) continue;
            checked++;
            int32_t opos = (int32_t)(is_y ? oy_lo : ox_lo);
            int32_t odim = is_y ? hctx->macro_heights[m]
                                : hctx->macro_widths[m];
            if (p < opos + odim && opos < p + my_dim)
                total_cost += (int64_t)(my_dim + odim) * 500;
        }
    }

    /* HPWL cost from net bounding boxes */
    uint32_t n_my_nets = hctx->macro_net_count[macro_idx];

    for (uint32_t ni = 0; ni < n_my_nets; ni++) {
        uint32_t net_idx = hctx->macro_net_ids[macro_idx][ni];
        const CostGuidedNet *net = &hctx->nets[net_idx];

        /* Bounding box of already-placed pins (excluding this macro) */
        int32_t net_min = INT32_MAX;
        int32_t net_max = INT32_MIN;
        int32_t my_pin_offset = 0;

        for (uint32_t pi = 0; pi < net->n_pins; pi++) {
            uint32_t mid = net->pins[pi].macro_id;
            if (mid == macro_idx) {
                my_pin_offset = is_y ? net->pins[pi].offset_y
                                     : net->pins[pi].offset_x;
                continue;
            }

            /* Only count placed (singleton) macros */
            uint32_t other_var = is_y ? hctx->macros[mid].y_var_id
                                      : hctx->macros[mid].x_var_id;
            int64_t olo = var_lo64(ctx, &ctx->vars[other_var]);
            int64_t ohi = var_hi64(ctx, &ctx->vars[other_var]);
            if (olo == ohi) {
                int32_t pin_pos = (int32_t)olo
                    + (is_y ? net->pins[pi].offset_y
                            : net->pins[pi].offset_x);
                if (pin_pos < net_min) net_min = pin_pos;
                if (pin_pos > net_max) net_max = pin_pos;
            }
        }

        if (net_min == INT32_MAX) continue;  /* no placed pins in net */

        /* Incremental cost of placing at p + my_pin_offset */
        int32_t my_pin = p + my_pin_offset;
        if (my_pin < net_min)
            total_cost += (int64_t)(net_min - my_pin);
        else if (my_pin > net_max)
            total_cost += (int64_t)(my_pin - net_max);
    }

    return total_cost;
}

/* ================================================================== */
/* HPWL context helpers                                                */
/* ================================================================== */

int hpwl_cost_ctx_build_index(HPWLCostCtx *hctx) {
    if (!hctx) return -1;

    /* Count nets per macro */
    hctx->macro_net_count = (uint32_t *)calloc(hctx->n_macros, sizeof(uint32_t));
    if (!hctx->macro_net_count) return -1;

    for (uint32_t ni = 0; ni < hctx->n_nets; ni++) {
        const CostGuidedNet *net = &hctx->nets[ni];
        for (uint32_t pi = 0; pi < net->n_pins; pi++) {
            uint32_t mid = net->pins[pi].macro_id;
            if (mid < hctx->n_macros)
                hctx->macro_net_count[mid]++;
        }
    }

    /* Allocate per-macro net ID arrays */
    hctx->macro_net_ids = (uint32_t **)calloc(hctx->n_macros, sizeof(uint32_t *));
    if (!hctx->macro_net_ids) { free(hctx->macro_net_count); return -1; }

    for (uint32_t m = 0; m < hctx->n_macros; m++) {
        if (hctx->macro_net_count[m] > 0) {
            hctx->macro_net_ids[m] = (uint32_t *)calloc(
                hctx->macro_net_count[m], sizeof(uint32_t));
            if (!hctx->macro_net_ids[m]) {
                hpwl_cost_ctx_destroy(hctx);
                return -1;
            }
        }
    }

    /* Fill the index (second pass) */
    uint32_t *fill = (uint32_t *)calloc(hctx->n_macros, sizeof(uint32_t));
    if (!fill) { hpwl_cost_ctx_destroy(hctx); return -1; }

    for (uint32_t ni = 0; ni < hctx->n_nets; ni++) {
        const CostGuidedNet *net = &hctx->nets[ni];
        for (uint32_t pi = 0; pi < net->n_pins; pi++) {
            uint32_t mid = net->pins[pi].macro_id;
            if (mid < hctx->n_macros) {
                /* Deduplicate: only add net_idx once per macro */
                int dup = 0;
                for (uint32_t k = 0; k < fill[mid]; k++) {
                    if (hctx->macro_net_ids[mid][k] == ni) { dup = 1; break; }
                }
                if (!dup && fill[mid] < hctx->macro_net_count[mid])
                    hctx->macro_net_ids[mid][fill[mid]++] = ni;
            }
        }
    }

    /* Update counts to actual (deduplicated) counts */
    for (uint32_t m = 0; m < hctx->n_macros; m++)
        hctx->macro_net_count[m] = fill[m];

    free(fill);
    return 0;
}

void hpwl_cost_ctx_destroy(HPWLCostCtx *hctx) {
    if (!hctx) return;
    if (hctx->macro_net_ids) {
        for (uint32_t m = 0; m < hctx->n_macros; m++)
            free(hctx->macro_net_ids[m]);
        free(hctx->macro_net_ids);
    }
    free(hctx->macro_net_count);
    hctx->macro_net_count = NULL;
    hctx->macro_net_ids = NULL;
}

int solver_set_cost_guided_hpwl(SolveCtx *ctx, HPWLCostCtx *hctx,
                                 int32_t max_scan) {
    if (!ctx || !hctx) return -1;
    if (!hctx->macro_net_count) {
        if (hpwl_cost_ctx_build_index(hctx) != 0) return -1;
    }
    solver_set_cost_guided(ctx, hpwl_cost_fn, hctx, max_scan);
    return 0;
}

/* ================================================================== */
/* Greedy placement using CostGuided evaluation                        */
/* ================================================================== */

/* Compare macro indices by net connectivity (descending) */
static int _cmp_by_connectivity(const void *a, const void *b, void *ctx) {
    const uint32_t *ia = (const uint32_t *)a;
    const uint32_t *ib = (const uint32_t *)b;
    const uint32_t *counts = (const uint32_t *)ctx;
    /* Sort descending by net count */
    if (counts[*ib] != counts[*ia])
        return (int)counts[*ib] - (int)counts[*ia];
    return (int)*ia - (int)*ib;  /* tie-break by index */
}

int costguided_greedy_place(SolveCtx *ctx, HPWLCostCtx *hctx,
                             int32_t max_scan, int32_t *out_positions) {
    if (!ctx || !hctx || !out_positions) return -1;
    if (!hctx->macro_net_count) {
        if (hpwl_cost_ctx_build_index(hctx) != 0) return -1;
    }

    uint32_t nm = hctx->n_macros;
    if (nm == 0) return 0;

    int32_t scan = max_scan > 0 ? max_scan : 200;

    /* Build placement order: largest macros first (by area).
     * Large macros are hardest to fit, so place them first when
     * the canvas is empty.  Break ties by connectivity (descending). */
    uint32_t *order = (uint32_t *)malloc(nm * sizeof(uint32_t));
    if (!order) return -1;
    for (uint32_t i = 0; i < nm; i++) order[i] = i;

    /* Compute area for each macro from variable domain spans */
    for (uint32_t i = 1; i < nm; i++) {
        uint32_t key = order[i];
        int32_t key_w = hctx->macro_widths ? hctx->macro_widths[key] : 0;
        int32_t key_h = hctx->macro_heights ? hctx->macro_heights[key] : 0;
        int64_t key_area = (int64_t)key_w * key_h;
        uint32_t key_nets = hctx->macro_net_count[key];
        int j = (int)i - 1;
        while (j >= 0) {
            uint32_t oj = order[j];
            int32_t oj_w = hctx->macro_widths ? hctx->macro_widths[oj] : 0;
            int32_t oj_h = hctx->macro_heights ? hctx->macro_heights[oj] : 0;
            int64_t oj_area = (int64_t)oj_w * oj_h;
            if (oj_area > key_area) break;
            if (oj_area == key_area && hctx->macro_net_count[oj] >= key_nets) break;
            order[j + 1] = order[j]; j--;
        }
        order[j + 1] = key;
    }

    /* Initialize output */
    for (uint32_t i = 0; i < nm * 2; i++) out_positions[i] = -1;

    /* Checkpoint to restore after greedy evaluation */
    int cp = solver_checkpoint(ctx);
    if (cp < 0) { free(order); return -1; }

    /* Place each macro: evaluate HPWL, pin, propagate, continue.
     * Propagation after each pin tightens remaining domains. */
    int placed = 0;
    for (uint32_t step = 0; step < nm; step++) {
        uint32_t m = order[step];
        uint32_t xv = hctx->macros[m].x_var_id;
        uint32_t yv = hctx->macros[m].y_var_id;

        int64_t x_lo = var_lo64(ctx, &ctx->vars[xv]);
        int64_t x_hi = var_hi64(ctx, &ctx->vars[xv]);
        int64_t y_lo = var_lo64(ctx, &ctx->vars[yv]);
        int64_t y_hi = var_hi64(ctx, &ctx->vars[yv]);

        /* Already fixed (preplaced or propagation-fixed) */
        if (x_lo == x_hi && y_lo == y_hi) {
            out_positions[m * 2]     = (int32_t)x_lo;
            out_positions[m * 2 + 1] = (int32_t)y_lo;
            placed++;
            continue;
        }

        /* Domain empty -- can't place */
        if (x_lo > x_hi || y_lo > y_hi) continue;

        /* Find best x (HPWL-optimal) */
        int64_t x_step = (x_hi - x_lo + 1) > scan ? (x_hi - x_lo + 1) / scan : 1;
        int32_t best_x = (int32_t)x_lo;
        int64_t best_cx = hpwl_cost_fn(ctx, xv, x_lo, hctx);
        for (int64_t v = x_lo + x_step; v <= x_hi; v += x_step) {
            int64_t c = hpwl_cost_fn(ctx, xv, v, hctx);
            if (c < best_cx) { best_cx = c; best_x = (int32_t)v; }
        }

        /* Find best y (HPWL-optimal) */
        int64_t y_step = (y_hi - y_lo + 1) > scan ? (y_hi - y_lo + 1) / scan : 1;
        int32_t best_y = (int32_t)y_lo;
        int64_t best_cy = hpwl_cost_fn(ctx, yv, y_lo, hctx);
        for (int64_t v = y_lo + y_step; v <= y_hi; v += y_step) {
            int64_t c = hpwl_cost_fn(ctx, yv, v, hctx);
            if (c < best_cy) { best_cy = c; best_y = (int32_t)v; }
        }

        /* Pin x first, then find feasible y in the propagated domain.
         * This works because NoOverlap2D propagation after pinning x
         * tightens y's domain to exclude overlapping positions. */
        int found = 0;

        /* Candidate x positions: HPWL-optimal, then dense grid */
        int32_t x_cands[130];
        int n_x_cands = 0;
        x_cands[n_x_cands++] = best_x;  /* HPWL-optimal */
        {
            int64_t xl = var_lo64(ctx, &ctx->vars[xv]);
            int64_t xh = var_hi64(ctx, &ctx->vars[xv]);
            /* Add hi and midpoint */
            if ((int32_t)xh != best_x) x_cands[n_x_cands++] = (int32_t)xh;
            if ((int32_t)((xl+xh)/2) != best_x) x_cands[n_x_cands++] = (int32_t)((xl+xh)/2);
            /* Dense grid: ~128 evenly spaced positions */
            int64_t gx = (xh - xl) / 128 + 1;
            for (int64_t v = xl; v <= xh && n_x_cands < 130; v += gx) {
                /* Skip if duplicate of already-added candidates */
                int dup = 0;
                for (int k = 0; k < n_x_cands && k < 3; k++)
                    if (x_cands[k] == (int32_t)v) { dup = 1; break; }
                if (!dup) x_cands[n_x_cands++] = (int32_t)v;
            }
        }

        for (int xi = 0; xi < n_x_cands && !found; xi++) {
            int cp_try = solver_checkpoint(ctx);
            if (cp_try < 0) break;

            if (solver_pin_var(ctx, xv, x_cands[xi]) != 0) {
                solver_restore(ctx, (uint32_t)cp_try);
                continue;
            }

            /* x pinned successfully.  Read propagated y domain and
             * try y positions: HPWL-optimal, then lo, then grid. */
            int64_t yl = var_lo64(ctx, &ctx->vars[yv]);
            int64_t yh = var_hi64(ctx, &ctx->vars[yv]);

            if (yl > yh) {  /* y domain empty after x propagation */
                solver_restore(ctx, (uint32_t)cp_try);
                continue;
            }

            /* Try HPWL-optimal y (clamped to propagated domain) */
            int32_t try_y = best_y;
            if (try_y < (int32_t)yl) try_y = (int32_t)yl;
            if (try_y > (int32_t)yh) try_y = (int32_t)yh;

            if (solver_pin_var(ctx, yv, try_y) == 0) {
                out_positions[m * 2]     = x_cands[xi];
                out_positions[m * 2 + 1] = try_y;
                placed++;
                found = 1;
                continue;
            }

            /* Try y_lo (always valid if domain is non-empty) */
            solver_restore(ctx, (uint32_t)cp_try);
            cp_try = solver_checkpoint(ctx);
            if (cp_try < 0) break;
            if (solver_pin_var(ctx, xv, x_cands[xi]) != 0) {
                solver_restore(ctx, (uint32_t)cp_try);
                continue;
            }

            if (solver_pin_var(ctx, yv, (int32_t)yl) == 0) {
                out_positions[m * 2]     = x_cands[xi];
                out_positions[m * 2 + 1] = (int32_t)yl;
                placed++;
                found = 1;
                continue;
            }
            solver_restore(ctx, (uint32_t)cp_try);
        }
    }

    /* Restore to clean state */
    solver_restore(ctx, (uint32_t)cp);

    free(order);
    return (placed == (int)nm) ? 0 : -1;
}

/* ================================================================== */
/* Phase hint injection                                                */
/* ================================================================== */

int solver_set_phase_hints(SolveCtx *ctx, const uint32_t *var_ids,
                            const int64_t *values, uint32_t n_hints) {
    if (!ctx || !var_ids || !values) return -1;

    /* Ensure phase_save array exists */
    if (!ctx->phase_save && ctx->n_vars > 0) {
        uint32_t ps_size = ctx->n_vars_capacity > 0
                           ? ctx->n_vars_capacity : ctx->n_vars;
        uint32_t ps_ref = zsp_pool_alloc(&ctx->pool,
                                          ps_size * (uint32_t)sizeof(int64_t),
                                          (uint32_t)_Alignof(int64_t));
        if (ps_ref == EXPR_NULL) return -1;
        ctx->phase_save = (int64_t *)zsp_pool_ptr(&ctx->pool, ps_ref);
        for (uint32_t i = 0; i < ctx->n_vars; i++)
            ctx->phase_save[i] = var_lo64(ctx, &ctx->vars[i]);
    }

    if (!ctx->phase_save) return -1;

    for (uint32_t i = 0; i < n_hints; i++) {
        uint32_t vid = var_ids[i];
        if (vid < ctx->n_vars)
            ctx->phase_save[vid] = values[i];
    }

    return 0;
}

/* ================================================================== */
/* LNS optimizer                                                       */
/* ================================================================== */

static double _lns_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Compute total HPWL from placed positions */
static int64_t _compute_hpwl(const HPWLCostCtx *hctx,
                               const int32_t *positions) {
    int64_t total = 0;
    for (uint32_t ni = 0; ni < hctx->n_nets; ni++) {
        const CostGuidedNet *net = &hctx->nets[ni];
        int32_t xmin = INT32_MAX, xmax = INT32_MIN;
        int32_t ymin = INT32_MAX, ymax = INT32_MIN;
        for (uint32_t pi = 0; pi < net->n_pins; pi++) {
            uint32_t mid = net->pins[pi].macro_id;
            int32_t x = positions[mid * 2] + net->pins[pi].offset_x;
            int32_t y = positions[mid * 2 + 1] + net->pins[pi].offset_y;
            if (x < xmin) xmin = x;  if (x > xmax) xmax = x;
            if (y < ymin) ymin = y;  if (y > ymax) ymax = y;
        }
        if (xmin < INT32_MAX)
            total += (int64_t)(xmax - xmin) + (int64_t)(ymax - ymin);
    }
    return total;
}

/* Simple xorshift64 RNG */
static uint64_t _lns_rng(uint64_t *state) {
    uint64_t s = *state;
    s ^= s << 13;  s ^= s >> 7;  s ^= s << 17;
    *state = s;
    return s;
}

int solver_lns_optimize(SolveCtx *ctx, HPWLCostCtx *hctx,
                         const LNSOpts *opts,
                         int32_t *out_positions, LNSResult *result) {
    if (!ctx || !hctx || !out_positions || !result) return -1;
    memset(result, 0, sizeof(LNSResult));

    uint32_t max_iter  = (opts && opts->max_iterations > 0)
                         ? opts->max_iterations : 100;
    double   time_lim  = (opts && opts->time_limit_sec > 0.0)
                         ? opts->time_limit_sec : 10.0;
    uint32_t neigh_sz  = (opts && opts->neighborhood_size > 0)
                         ? opts->neighborhood_size : 2;
    uint32_t sub_conf  = (opts && opts->subproblem_conflicts > 0)
                         ? opts->subproblem_conflicts : 1000;
    uint64_t rng_state = (opts && opts->seed != 0) ? opts->seed : 42;

    uint32_t nm = hctx->n_macros;
    if (neigh_sz > nm) neigh_sz = nm;

    /* Step 1: Get initial solution via greedy placement */
    int grc = costguided_greedy_place(ctx, hctx, 200, out_positions);
    if (grc != 0) return -1;

    result->initial_hpwl = _compute_hpwl(hctx, out_positions);
    result->best_hpwl = result->initial_hpwl;

    /* Working copy of positions */
    int32_t *cur = (int32_t *)malloc(nm * 2 * sizeof(int32_t));
    if (!cur) return -1;
    memcpy(cur, out_positions, nm * 2 * sizeof(int32_t));

    /* Neighborhood selection buffer */
    uint32_t *unfrozen = (uint32_t *)malloc(neigh_sz * sizeof(uint32_t));
    if (!unfrozen) { free(cur); return -1; }

    double t0 = _lns_now();

    for (uint32_t iter = 0; iter < max_iter; iter++) {
        if (_lns_now() - t0 >= time_lim) break;
        result->iterations++;

        /* Alternate between net-based and spatial neighborhoods.
         * Net-based targets HPWL; spatial targets overlap density. */
        uint32_t actual_neigh = 0;

        if ((iter & 1) == 0 && hctx->n_nets > 0) {
            /* Even iterations: net-based neighborhood */
            uint32_t net_idx = (uint32_t)(_lns_rng(&rng_state) % hctx->n_nets);
            const CostGuidedNet *net = &hctx->nets[net_idx];
            for (uint32_t pi = 0; pi < net->n_pins && actual_neigh < neigh_sz; pi++) {
                uint32_t mid = net->pins[pi].macro_id;
                if (mid >= nm) continue;
                int dup = 0;
                for (uint32_t k = 0; k < actual_neigh; k++)
                    if (unfrozen[k] == mid) { dup = 1; break; }
                if (!dup) unfrozen[actual_neigh++] = mid;
            }
        } else {
            /* Odd iterations: spatial neighborhood.
             * Pick a random anchor macro and find its nearest neighbors
             * by Manhattan distance from current positions. */
            uint32_t anchor = (uint32_t)(_lns_rng(&rng_state) % nm);
            unfrozen[actual_neigh++] = anchor;
            int32_t ax = cur[anchor * 2], ay = cur[anchor * 2 + 1];

            /* Find nearest neigh_sz-1 macros by distance */
            for (uint32_t need = actual_neigh; need < neigh_sz && need < nm; need++) {
                int32_t best_dist = INT32_MAX;
                uint32_t best_m = anchor;
                for (uint32_t m = 0; m < nm; m++) {
                    int already = 0;
                    for (uint32_t k = 0; k < actual_neigh; k++)
                        if (unfrozen[k] == m) { already = 1; break; }
                    if (already) continue;
                    int32_t dx = cur[m * 2] - ax;
                    int32_t dy = cur[m * 2 + 1] - ay;
                    int32_t dist = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
                    if (dist < best_dist) { best_dist = dist; best_m = m; }
                }
                if (best_m != anchor || actual_neigh == 0)
                    unfrozen[actual_neigh++] = best_m;
            }
        }

        if (actual_neigh < 2) {
            /* Fallback: random pair */
            actual_neigh = 0;
            for (uint32_t k = 0; k < 2; k++) {
                uint32_t idx;
                int unique;
                do { idx = (uint32_t)(_lns_rng(&rng_state) % nm); unique = 1;
                    for (uint32_t j = 0; j < actual_neigh; j++)
                        if (unfrozen[j] == idx) { unique = 0; break; }
                } while (!unique);
                unfrozen[actual_neigh++] = idx;
            }
        }

        /* Checkpoint and freeze all macros at current positions,
         * except the unfrozen ones */
        int cp = solver_checkpoint(ctx);
        if (cp < 0) break;

        int freeze_ok = 1;
        for (uint32_t m = 0; m < nm; m++) {
            /* Skip unfrozen macros */
            int is_unfrozen = 0;
            for (uint32_t k = 0; k < actual_neigh; k++)
                if (unfrozen[k] == m) { is_unfrozen = 1; break; }
            if (is_unfrozen) continue;

            /* Pin this macro at its current position */
            uint32_t xv = hctx->macros[m].x_var_id;
            uint32_t yv = hctx->macros[m].y_var_id;
            if (solver_pin_var(ctx, xv, cur[m * 2]) != 0 ||
                solver_pin_var(ctx, yv, cur[m * 2 + 1]) != 0) {
                freeze_ok = 0;
                break;
            }
        }

        if (!freeze_ok) {
            solver_restore(ctx, (uint32_t)cp);
            continue;
        }

        /* Solve the subproblem: only unfrozen macros can move */
        SolveOpts sopts;
        memset(&sopts, 0, sizeof(sopts));
        sopts.seed = _lns_rng(&rng_state);
        sopts.max_conflicts = sub_conf;
        sopts.max_restarts = 50;
        sopts.use_phase_save = 0;

        SolveResult sr = solver_solve(ctx, &sopts);

        if (sr == SOLVE_OK) {
            /* Read new positions for unfrozen macros */
            int32_t new_pos[2 * 256];  /* enough for neigh_sz <= 128 */
            for (uint32_t k = 0; k < actual_neigh; k++) {
                uint32_t m = unfrozen[k];
                new_pos[k * 2]     = (int32_t)solver_get_value(ctx, hctx->macros[m].x_var_id);
                new_pos[k * 2 + 1] = (int32_t)solver_get_value(ctx, hctx->macros[m].y_var_id);
            }

            /* Compute new HPWL with the updated positions */
            int32_t trial[512];
            if (nm * 2 <= 512) {
                memcpy(trial, cur, nm * 2 * sizeof(int32_t));
                for (uint32_t k = 0; k < actual_neigh; k++) {
                    uint32_t m = unfrozen[k];
                    trial[m * 2]     = new_pos[k * 2];
                    trial[m * 2 + 1] = new_pos[k * 2 + 1];
                }
                int64_t new_hpwl = _compute_hpwl(hctx, trial);

                if (new_hpwl < result->best_hpwl) {
                    /* Validate: re-pin all macros at trial positions
                     * to verify no overlaps via NoOverlap2D */
                    int cp_val = solver_checkpoint(ctx);
                    int valid = 1;
                    if (cp_val >= 0) {
                        for (uint32_t vm = 0; vm < nm && valid; vm++) {
                            if (solver_pin_var(ctx, hctx->macros[vm].x_var_id,
                                               trial[vm * 2]) != 0 ||
                                solver_pin_var(ctx, hctx->macros[vm].y_var_id,
                                               trial[vm * 2 + 1]) != 0)
                                valid = 0;
                        }
                        solver_restore(ctx, (uint32_t)cp_val);
                    }

                    if (valid) {
                        memcpy(cur, trial, nm * 2 * sizeof(int32_t));
                        result->best_hpwl = new_hpwl;
                        result->improvements++;
                    }
                }
            }
        }

        solver_restore(ctx, (uint32_t)cp);
    }

    result->improved = (result->best_hpwl < result->initial_hpwl) ? 1 : 0;
    result->elapsed_sec = _lns_now() - t0;

    /* Copy best solution to output */
    memcpy(out_positions, cur, nm * 2 * sizeof(int32_t));

    free(unfrozen);
    free(cur);
    return 0;
}
