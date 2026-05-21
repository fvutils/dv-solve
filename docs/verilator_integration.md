# Verilator Integration Guide

How to use zuspec-solver as a SystemVerilog constraint backend for
Verilator.  Maps SV constraint constructs to solver C API calls.

## Architecture

```
SV class with rand fields
  |
  v
Verilator codegen --> C++ randomize() method
  |
  v
SolveProblemBuilder API (build problem once per class)
  |
  v
solver_compile() --> SolveCtx (reusable)
  |
  v
solver_reset() + solver_solve() (called per randomize())
```

## Mapping SV Constructs

### Variables

| SV | Solver API |
|----|-----------|
| `rand bit [7:0] x` | `builder_add_var(b, id, 8, 0, 0, 255)` |
| `rand int x` | `builder_add_var(b, id, 32, 1, INT32_MIN, INT32_MAX)` |
| `rand bit [31:0] x` | `builder_add_var(b, id, 32, 0, 0, 0xFFFFFFFF)` (promoted to tier-1) |

### Constraint Operators

| SV | Solver expression |
|----|------------------|
| `x + y` | `expr_binary(BIN_ADD, x, y)` |
| `x < y` | `expr_binary(BIN_LT, x, y)` |
| `x & mask` | `expr_binary(BIN_BAND, x, mask)` |
| `x << n` | `expr_binary(BIN_LSHIFT, x, n)` |
| `cond ? a : b` | `expr_ite(cond, a, b)` |
| `{hi, lo}` | `expr_concat(hi, lo, lo_width)` |

### constraint_mode

`constraint_mode(0)` disables a constraint block.  Model this by not adding
the constraint to the problem, or by using a guard variable:

```c
// enabler var, pinned to 0 to disable
uint32_t en_id = ...; // boolean var, domain [0,1]
solver_pin_var(ctx, en_id, 0);  // disable
```

### rand_mode

`rand_mode(0)` pins a field to its current value:

```c
solver_pin_var(ctx, var_id, current_value);
```

### solve...before

Phased solving:

```c
// Phase 1: solve early variables
int cp = solver_checkpoint(ctx);
solver_pin_var(ctx, late_var, placeholder);
solver_solve(ctx, NULL);
int64_t early_val = solver_get_value(ctx, early_var);

// Phase 2: restore, pin early result, solve all
solver_restore(ctx, cp);
solver_pin_var(ctx, early_var, early_val);
solver_solve(ctx, NULL);
```

### Soft Constraints

```c
// At problem construction:
problem_add_soft_constraint(sp, constraint_expr, priority);

// After solve, check which were relaxed:
solver_soft_active(ctx, assumption_idx);  // 1=kept, 0=relaxed
```

### Distribution Constraints

```c
DistEntry entries[] = {
    {.lo = 0,   .hi = 9,   .weight = 1, .is_per_value = 0},  // :/ 1
    {.lo = 10,  .hi = 19,  .weight = 3, .is_per_value = 0},  // :/ 3
};
problem_add_dist(sp, var_id, 2, entries);
```

### randc (Cyclic Random)

```c
// After each solve, exclude the obtained value:
solver_exclude_value(ctx, var_id, obtained_value);

// On next solve (after reset), excluded values are skipped.
// When all values exhausted (returns -1), start a new cycle.
solver_reset(ctx);  // hole list persists
solver_solve(ctx, NULL);
```

### unique / AllDifferent

```c
uint32_t var_ids[] = {0, 1, 2, 3};
problem_add_all_different(sp, 4, var_ids);
```

## Lifecycle Pattern

The recommended integration pattern for Verilator:

1. **Once per class**: build `SolveProblem` via builder, `solver_compile()`.
2. **Per randomize() call**: `solver_reset()`, apply pins, `solver_solve()`,
   read values with `solver_get_value()`.
3. **Cleanup**: `solver_destroy()`, free buffers.

This avoids recompilation overhead.  `solver_reset()` is O(n_vars),
while `solver_compile()` is O(n_vars + n_constraints).
