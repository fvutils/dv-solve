# Propagator Catalog

All propagator types implemented in the solver, their semantics, and
propagation rules.  Source: `zsp_propagator.h`, `zsp_prop_templates.c`.

## Comparison Propagators

| Type | Semantics | Propagation |
|------|-----------|-------------|
| `BoundsLE_{32,64}` | x <= y | `x_hi = min(x_hi, y_hi)`, `y_lo = max(y_lo, x_lo)` |
| `BoundsLT_{32,64}` | x < y | `x_hi = min(x_hi, y_hi - 1)`, `y_lo = max(y_lo, x_lo + 1)` |
| `BoundsEQ_{32,64}` | x == y | Intersect bounds of x and y |
| `BoundsNE_{32,64}` | x != y | When one is singleton, exclude that value from the other |

## Arithmetic Propagators

| Type | Semantics | Forward | Backward |
|------|-----------|---------|----------|
| `BoundsAdd_{32,64}` | r = a + b | `r_lo = a_lo + b_lo`, `r_hi = a_hi + b_hi` | `a_lo = r_lo - b_hi`, etc. |
| `BoundsMul_{32,64}` | r = a * b | Four-corner product bounds | `a = r / b` when b is singleton |
| `BoundsDiv_{32,64}` | r = a / b | `r_lo = a_lo / b_hi` (b > 0) | Guard against div-by-zero |
| `BoundsMod_{32,64}` | r = a % b | `r_hi = min(r_hi, b_hi - 1)` | -- |
| `UnaryNeg_{32,64}` | r = -a | `r_lo = -a_hi`, `r_hi = -a_lo` | Symmetric |

## Bitwise Propagators

| Type | Semantics | Propagation |
|------|-----------|-------------|
| `BoundsBAND_64` | r = a & b | `r_hi = min(a_hi, b_hi)`. Singleton specialization for constant masks. |
| `BoundsBOR_64` | r = a \| b | `r_lo = a_lo \| b_lo`. Coarse upper bound. |
| `BoundsBXOR_64` | r = a ^ b | Singleton specialization only (bijection when one operand is constant). |
| `BoundsBNOT_64` | r = ~a | `r_lo = ~a_hi`, `r_hi = ~a_lo` (unsigned complement reversal). |

## Shift Propagators

| Type | Semantics | Propagation |
|------|-----------|-------------|
| `BoundsSHL_64` | r = a << b | Forward: `r_lo = a_lo << b_lo`. Backward: `a_lo = r_lo >> b_hi`. |
| `BoundsLSHR_64` | r = a >> b | `r_lo = a_lo >> b_hi`, `r_hi = a_hi >> b_lo`. |

## Control Flow Propagators

| Type | Semantics |
|------|-----------|
| `ITEValue_64` | r = cond ? a : b. When cond=1: intersect r and a. When cond=0: intersect r and b. Undecided: `r_lo = min(a_lo, b_lo)`, `r_hi = max(a_hi, b_hi)`. |
| `Implication_32` | guard=1 implies var <= bound (or var >= bound). |
| `Reification_{32,64}` | guard=1 iff x <= y. |
| `DisjClause` | `(v0 op c0) OR ... OR (vN op cN)`. When all but one clause falsified, enforce the survivor. |

## Structural Propagators

| Type | Semantics |
|------|-----------|
| `InSet_{32,64}` | x in {e0, e1, ...}. Tightens bounds to `[min(elems), max(elems)]`. |
| `BitSlice_{32,64}` | r = a[hi_bit:lo_bit]. Extract and equate. |
| `BoundsConcat_64` | r = {hi, lo}. Decomposes into extract + equality. |
| `AllDifferent` | x0, x1, ..., xN all distinct (up to 16 vars). Singleton exclusion. |

## Guard-Gated Propagators

Any propagator can be guard-gated via `prop_set_guard(ctx, prop_ref, guard_var_id)`.

- Guard = 1 (singleton true): propagator fires normally.
- Guard = 0 (singleton false): propagator is marked entailed (never fires).
- Guard undecided (lo != hi): propagator is skipped until the guard is decided.

Used internally by ITE-at-constraint-root and soft constraint compilation.
