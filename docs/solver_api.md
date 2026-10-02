# Solver Runtime C API Reference

Complete reference for the zuspec-solver C runtime API.
Header: `dvs_search.h`, `dvs_ctx.h`.

## Lifecycle

```c
// Create a solver context in a caller-supplied buffer.
SolveCtx *dvs_solver_create(void *static_buf, size_t static_size,
                         dvs_block_alloc_t *block_alloc);

// Compile a SolveProblem into the context.
// Returns 0 on success, -1 on pool overflow, -2 on compile-time UNSAT.
int dvs_solver_compile(SolveCtx *ctx, SolveProblem *sp);

// Destroy the context (does not free the static buffer).
void dvs_solver_destroy(SolveCtx *ctx);
```

## Solving

```c
// Run search. Returns SOLVE_OK, SOLVE_UNSAT, or SOLVE_TIMEOUT.
SolveResult dvs_solver_solve(SolveCtx *ctx, const SolveOpts *opts);

// Read the value of a variable after a successful solve.
int64_t dvs_solver_get_value(const SolveCtx *ctx, uint32_t var_id);

// Read multiple variable values in one call.
void dvs_solver_get_values(const SolveCtx *ctx, uint32_t n,
                       const uint32_t *var_ids, int64_t *out);
```

## Reset and Reuse

```c
// Reset to post-compile state. Restores all variable domains,
// clears trail and decisions, re-enqueues propagators.
// Hole lists (from dvs_solver_exclude_value) persist across resets.
void dvs_solver_reset(SolveCtx *ctx);

// Set the RNG seed for the next solve.
void dvs_solver_set_seed(SolveCtx *ctx, uint64_t seed);
```

## Variable Pinning

```c
// Pin a variable to a specific value. Tightens lb and ub, propagates.
// Returns 0 on success, -1 on conflict.
int dvs_solver_pin_var(SolveCtx *ctx, uint32_t var_id, int64_t value);
```

Use cases:
- **rand_mode=0**: pin a variable to its current value before solve.
- **State variables**: pin non-rand members so constraints reference them.
- **Solve-before**: pin a variable after a first-phase solve, then solve remaining.

## Checkpoint and Restore

```c
// Save a checkpoint of the current state.
// Returns checkpoint index (0-based), or -1 if MAX_CHECKPOINTS exceeded.
int dvs_solver_checkpoint(SolveCtx *ctx);

// Restore to a previously saved checkpoint.
// Undoes domain changes and deactivates propagators added after checkpoint.
void dvs_solver_restore(SolveCtx *ctx, uint32_t cp);
```

## Incremental Constraints

```c
// Add constraints from an auxiliary SolveProblem to a compiled context.
// Returns 0 on success, -1 on capacity error, -2 on UNSAT.
int dvs_solver_add_constraint(SolveCtx *ctx, SolveProblem *aux_sp);
```

## Value Exclusion (randc)

```c
// Exclude a value from a variable's domain.
// Persists across dvs_solver_reset() for cyclic-random semantics.
// Returns 0 on success, -1 if exclusion would empty the domain.
int dvs_solver_exclude_value(SolveCtx *ctx, uint32_t var_id, int64_t value);
```

Typical randc cycle:
1. `dvs_solver_solve()` -- obtain value `v`
2. `dvs_solver_exclude_value(ctx, var_id, v)` -- exclude it
3. `dvs_solver_reset()` -- restore domains (holes persist)
4. Repeat until domain exhausted (`dvs_solver_exclude_value` returns -1)

## Soft Constraints

```c
// Query whether a soft constraint assumption is still active after solve.
// Returns 1 (active), 0 (relaxed), or -1 (invalid index).
int dvs_solver_soft_active(const SolveCtx *ctx, uint32_t assumption_idx);
```

Soft constraints are added via `problem_add_soft_constraint()` at problem
construction time.  The solver automatically relaxes conflicting soft
constraints (lowest priority first) when hard constraints cannot be
satisfied with all soft constraints active.

## SolveOpts

```c
typedef struct {
    uint64_t seed;              // RNG seed (0 = keep current)
    uint32_t max_conflicts;     // restart unit: restart after luby(i) * this
                                //   many conflicts (0 = 100). Not a total budget.
    uint32_t max_restarts;      // total restart budget (0 = 10000); then SOLVE_TIMEOUT
    uint8_t  use_phase_save;    // 1 = remember last tried value
    uint8_t  use_lcg;           // 1 = lazy clause generation (CDCL)
    uint8_t  fair_pick;         // 1 = random tie-break among smallest domains
    uint8_t  _pad[1];
    uint32_t max_shave_iters;   // bounds shaving budget (0 = 1000)
    uint32_t time_limit_ms;     // wall-clock budget for this solve
                                //   (0 = DV_CDCL_TIME_LIMIT env default, 10 s)
} SolveOpts;
```

Pass `NULL` for all-default behaviour.
