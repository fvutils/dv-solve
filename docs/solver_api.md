# Solver Runtime C API Reference

Complete reference for the zuspec-solver C runtime API.
Header: `zsp_search.h`, `zsp_ctx.h`.

## Lifecycle

```c
// Create a solver context in a caller-supplied buffer.
SolveCtx *solver_create(void *static_buf, size_t static_size,
                         zsp_block_alloc_t *block_alloc);

// Compile a SolveProblem into the context.
// Returns 0 on success, -1 on pool overflow, -2 on compile-time UNSAT.
int solver_compile(SolveCtx *ctx, SolveProblem *sp);

// Destroy the context (does not free the static buffer).
void solver_destroy(SolveCtx *ctx);
```

## Solving

```c
// Run search. Returns SOLVE_OK, SOLVE_UNSAT, or SOLVE_TIMEOUT.
SolveResult solver_solve(SolveCtx *ctx, const SolveOpts *opts);

// Read the value of a variable after a successful solve.
int64_t solver_get_value(const SolveCtx *ctx, uint32_t var_id);

// Read multiple variable values in one call.
void solver_get_values(const SolveCtx *ctx, uint32_t n,
                       const uint32_t *var_ids, int64_t *out);
```

## Reset and Reuse

```c
// Reset to post-compile state. Restores all variable domains,
// clears trail and decisions, re-enqueues propagators.
// Hole lists (from solver_exclude_value) persist across resets.
void solver_reset(SolveCtx *ctx);

// Set the RNG seed for the next solve.
void solver_set_seed(SolveCtx *ctx, uint64_t seed);
```

## Variable Pinning

```c
// Pin a variable to a specific value. Tightens lb and ub, propagates.
// Returns 0 on success, -1 on conflict.
int solver_pin_var(SolveCtx *ctx, uint32_t var_id, int64_t value);
```

Use cases:
- **rand_mode=0**: pin a variable to its current value before solve.
- **State variables**: pin non-rand members so constraints reference them.
- **Solve-before**: pin a variable after a first-phase solve, then solve remaining.

## Checkpoint and Restore

```c
// Save a checkpoint of the current state.
// Returns checkpoint index (0-based), or -1 if MAX_CHECKPOINTS exceeded.
int solver_checkpoint(SolveCtx *ctx);

// Restore to a previously saved checkpoint.
// Undoes domain changes and deactivates propagators added after checkpoint.
void solver_restore(SolveCtx *ctx, uint32_t cp);
```

## Incremental Constraints

```c
// Add constraints from an auxiliary SolveProblem to a compiled context.
// Returns 0 on success, -1 on capacity error, -2 on UNSAT.
int solver_add_constraint(SolveCtx *ctx, SolveProblem *aux_sp);
```

## Value Exclusion (randc)

```c
// Exclude a value from a variable's domain.
// Persists across solver_reset() for cyclic-random semantics.
// Returns 0 on success, -1 if exclusion would empty the domain.
int solver_exclude_value(SolveCtx *ctx, uint32_t var_id, int64_t value);
```

Typical randc cycle:
1. `solver_solve()` -- obtain value `v`
2. `solver_exclude_value(ctx, var_id, v)` -- exclude it
3. `solver_reset()` -- restore domains (holes persist)
4. Repeat until domain exhausted (`solver_exclude_value` returns -1)

## Soft Constraints

```c
// Query whether a soft constraint assumption is still active after solve.
// Returns 1 (active), 0 (relaxed), or -1 (invalid index).
int solver_soft_active(const SolveCtx *ctx, uint32_t assumption_idx);
```

Soft constraints are added via `problem_add_soft_constraint()` at problem
construction time.  The solver automatically relaxes conflicting soft
constraints (lowest priority first) when hard constraints cannot be
satisfied with all soft constraints active.

## SolveOpts

```c
typedef struct {
    uint64_t seed;              // RNG seed (0 = keep current)
    uint32_t max_conflicts;     // per-restart conflict budget (0 = 100)
    uint32_t max_restarts;      // total restart budget (0 = 10000)
    uint8_t  use_phase_save;    // 1 = remember last tried value
    uint8_t  _pad[3];
    uint32_t max_shave_iters;   // bounds shaving budget (0 = 1000)
} SolveOpts;
```

Pass `NULL` for all-default behaviour.
