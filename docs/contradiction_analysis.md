# Contradiction Analysis -- User Guide

**Module: zuspec-solver-lcg contradiction analysis**
**Build flag: `ZSP_CONTRADICTION_ANALYSIS`**

---

## 1. Overview

When `solver_solve()` returns `SOLVE_UNSAT`, the contradiction analysis
module identifies *why* the problem is infeasible:

- **Minimal Unsatisfiable Subset (MUS)**: the smallest set of constraints
  that is itself unsatisfiable. Removing any single MUS constraint makes
  the remainder satisfiable.
- **Relaxation suggestions**: for each MUS constraint, the minimum change
  to its constant that would eliminate the contradiction.
- **Text and JSON output**: formatted reports for humans and tools.

For problems with soft constraints, the module also explains why each
relaxed soft was dropped and which hard constraints forced the relaxation.

## 2. Enabling

### Build flag

```cmake
cmake -DZSP_CONTRADICTION_ANALYSIS=ON ..
```

When OFF (default), no contradiction code is compiled. Zero code-size
overhead on embedded targets.

### Debug library

The build always produces `libdv_solve_debug.so` with the module
enabled, suitable for development and testing.

## 3. API Reference

### `contra_analyze_unsat()`

Analyze why a problem is unsatisfiable. Call after `solver_solve()`
returns `SOLVE_UNSAT`.

```c
int contra_analyze_unsat(SolveCtx *ctx, SolveProblem *sp,
                          const ContraOpts *opts, ContraResult *result);
```

**Parameters:**
- `ctx` -- Solver context (post-solve state).
- `sp` -- The original SolveProblem (must not be reset/freed).
- `opts` -- Analysis options, or NULL for defaults.
- `result` -- Output struct. Caller frees via `contra_result_free()`.

**Returns:** 0 on success, -1 on error.

**Example:**
```c
SolveResult res = solver_solve(ctx, &opts);
if (res == SOLVE_UNSAT) {
    ContraResult result;
    contra_analyze_unsat(ctx, sp, NULL, &result);
    printf("%s\n", result.proof_text);
    contra_result_free(&result);
}
```

### `contra_quick_core()`

Fast UNSAT core extraction without MUS minimization.

```c
int contra_quick_core(SolveCtx *ctx, SolveProblem *sp,
                       uint32_t *out_ids, uint32_t *out_n);
```

**Parameters:**
- `out_ids` -- Caller-allocated array for constraint IDs.
- `out_n` -- On input: capacity. On output: number of IDs written.

### `contra_explain_soft()`

Diagnose why soft constraints were relaxed. Call after `solver_solve()`
returns `SOLVE_OK` with relaxed assumptions.

```c
int contra_explain_soft(SolveCtx *ctx, SolveProblem *sp,
                         const ContraOpts *opts,
                         ContraSoftDiagResult *result);
```

For each relaxed soft, produces:
- The MUS of `{soft + hard constraints}` that forced relaxation.
- Relaxation suggestions for the conflicting hard constraints.
- Alternative soft constraints that could substitute (if `find_alternatives` set).

### `contra_compute_relaxations()`

Compute relaxation suggestions for a set of MUS constraints.

```c
int contra_compute_relaxations(SolveCtx *ctx, SolveProblem *sp,
                                const uint32_t *mus_ids, uint32_t mus_size,
                                const ContraOpts *opts,
                                ContraRelaxSuggestion *out);
```

### `contra_result_free()` / `contra_soft_diag_free()`

Free memory allocated by the analysis functions.

## 4. Output Formats

### Text output

```
UNSATISFIABLE: 2 constraints form a minimal contradiction.

Constraints involved:
  [C1]  x >= 10  (line 42 of input.pss)
  [C2]  x <= 5   (line 43 of input.pss)

Relaxation suggestions:
  [C1]  original=10  relaxed=5  (delta: -5)
  [C2]  original=5   relaxed=10 (delta: +5)

Easiest fix: relax any ONE of the above to its suggested value.

Analysis used 8 solver calls in 0.003 seconds.
```

### JSON output

```json
{
  "mus": [
    {"id": 1, "name": "x >= 10", "source": "input.pss:42"},
    {"id": 2, "name": "x <= 5", "source": "input.pss:43"}
  ],
  "relaxations": [
    {"constraint_id": 1, "is_relaxable": true, "original": 10, "relaxed": 5, "delta": -5},
    {"constraint_id": 2, "is_relaxable": true, "original": 5, "relaxed": 10, "delta": 5}
  ],
  "core_size": 2,
  "mus_size": 2,
  "n_solver_calls": 8,
  "elapsed_sec": 0.003
}
```

## 5. Constraint Identity

Constraints are auto-assigned sequential IDs starting from 1 when added
via `problem_add_constraint()`. To provide human-readable names and
source locations, pass a `ContraConstraintInfo` array in `ContraOpts`:

```c
ContraConstraintInfo info[] = {
    {1, "x >= 10", "input.pss", 42},
    {2, "x <= 5",  "input.pss", 43},
};
ContraOpts opts = {0};
opts.constraint_info = info;
opts.n_constraint_info = 2;
```

## 6. Relaxation Suggestions

For each MUS constraint of the form `var op constant`, the module
computes the minimum change to the constant that would make the MUS
satisfiable:

| Constraint form | Relaxation direction |
|----------------|---------------------|
| `x <= C` | increase C |
| `x >= C` | decrease C |
| `x < C` | increase C |
| `x > C` | decrease C |
| `x == C` | both directions |
| `x != C` | not relaxable |
| `allDifferent` | not relaxable |

Non-relaxable constraints are reported with `is_relaxable = 0`.

## 7. Soft Constraint Diagnostics

When the solver relaxes soft constraints to find a solution,
`contra_explain_soft()` explains why each was dropped:

```c
SolveResult res = solver_solve(ctx, &opts);
if (res == SOLVE_OK) {
    ContraSoftDiagResult diag;
    contra_explain_soft(ctx, sp, NULL, &diag);
    for (uint32_t i = 0; i < diag.n_entries; i++) {
        printf("Soft %u relaxed due to %u hard constraints\n",
               diag.entries[i].soft_constraint_id,
               diag.entries[i].n_conflict_hard);
    }
    contra_soft_diag_free(&diag);
}
```

## 8. Performance

- **MUS extraction**: O(k * log(n/k)) solver calls via QuickXplain,
  where k = MUS size and n = total constraints.
- **Relaxation**: O(k * log(R)) additional calls per MUS constraint,
  where R is the constant range.
- **Budget control**: Set `opts.max_solver_calls` to limit total solver
  invocations. Partial results are returned on budget exhaustion.
- **Zero overhead**: When compiled out (`ZSP_CONTRADICTION_ANALYSIS=OFF`),
  no code is added to the core solver.

## 9. Limitations

- Relaxation only works for var-const comparison constraints. Complex
  expressions (e.g., `x + y <= C`) are not currently classified.
- The module does not produce a full proof DAG (planned for a future
  sprint).
- Timeout vs. UNSAT distinction may be imprecise for very hard problems.
