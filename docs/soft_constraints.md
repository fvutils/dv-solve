# Soft Constraints

Soft (relaxable) constraints allow the solver to drop lower-priority
requirements when they conflict with hard constraints.

## API

### Problem Construction

```c
// Add a soft constraint with a priority.
// priority: 0 = highest (kept first), larger = relaxed first.
ExprRef problem_add_soft_constraint(SolveProblem *sp, ExprRef root,
                                    uint32_t priority);

// Builder equivalent:
ExprRef builder_add_soft_constraint(SolveProblemBuilder *b, ExprRef root,
                                    uint32_t priority);
```

### After Solve

```c
// Returns 1 if the soft constraint was satisfied, 0 if relaxed, -1 on error.
int solver_soft_active(const SolveCtx *ctx, uint32_t assumption_idx);
```

Note: assumption indices correspond to the order soft constraints appear
in the linked list (reverse of addition order due to prepend semantics).

## Semantics

1. Each soft constraint generates a boolean assumption variable pinned to 1.
2. The constraint body is compiled with the assumption as a guard.
3. On conflict, the solver relaxes the lowest-priority active assumption
   (highest priority number) by pinning it to 0.
4. The solver resets and retries with the relaxed assumption.
5. If hard constraints alone are UNSAT, the solver returns SOLVE_UNSAT
   (soft relaxation cannot help).

## Priority Model

- Priority 0 is the highest (most important to keep).
- Higher numeric values are relaxed first.
- When multiple soft constraints conflict, they are relaxed one at a time
  in decreasing priority order until a solution is found.

## Example

```c
// Hard: x >= 10
// Soft (pri 0): x == 5   -- will be relaxed
// Soft (pri 1): y == 7   -- compatible, will be kept

problem_add_soft_constraint(sp, eq_x_5, 0);
problem_add_soft_constraint(sp, eq_y_7, 1);

solver_solve(ctx, NULL);
// solver_soft_active(ctx, 0) == 0  (x==5 relaxed)
// solver_soft_active(ctx, 1) == 1  (y==7 kept)
```
