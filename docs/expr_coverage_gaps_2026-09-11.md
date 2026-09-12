# The SolveProblem path: expression coverage, and the gaps around it

*(Was "CDCL expression coverage". Broadened 2026-09-11 after a feature-axis sweep
found defects outside expression shape.)*

Measured: 2026-09-11, against `4bbf25e`
**Resolved: 2026-09-12.** All twelve gaps are addressed. This document is kept as
the record of what was wrong and why each fix took the shape it did — the
measurement is now enforced by `tests/unit/test_expr_coverage.py` rather than by
re-running the probes by hand.

Measured with three self-contained probes in this directory, each running
against the installed `libdv_solve.so`:

    python packages/dv-solve/docs/expr_coverage_probe.py       # expression shapes
    python packages/dv-solve/docs/feature_coverage_probe.py    # the other axes
    python packages/dv-solve/docs/aggregate_coverage_probe.py  # SUM/COUNTONES/CLOG2/ARRAY_SELECT

Each prints a two-column table, cdcl against bb. A row is one `SolveProblem`; add
one by adding a `case(...)` line. Every probe checks the returned assignment
against a Python evaluation of the same constraint, so a shape that compiles but
solves *wrongly* is reported as `WRONG` rather than passing — that distinction is
the point, since the failure mode this document is about is a constraint that is
not enforced.

## Status

| id | sev | gap | resolution |
|---|---|---|---|
| **G1** | B | width-64 `ADD`/`SUB` reported unsat on a satisfiable problem | fixed — overflow-safe interval arithmetic |
| **G2** | B | any variable wider than 64 bits made the whole problem unsat | fixed — compile declines loudly (`ZSP_COMPILE_UNSUPPORTED_WIDTH`) |
| **G3** | B | `solve_n` could not relax a soft constraint | fixed — it called the core, bypassing the relaxation wrapper |
| **G4** | A | the bit-blast engine silently ignored `dist` | fixed — `dist` encoded; `AllDifferent` encoded too |
| **G5** | A | `zsp_dpi.c` ignored a positive `solver_compile` return | fixed — count exposed, model validated before reporting OK |
| **G6** | C | arithmetic outside the special-cased `==` shapes | fixed — R1 |
| **G7** | C | a var-var inequality could not be reified | fixed — R2 |
| **G8** | C | `in_range`/`in_set` not usable as a reifiable leaf | fixed — R2 |
| **G9** | D | no unsigned or wide value readback on the cdcl path | **open** — see below |
| **G10** | D | the only post-solve safety net was unreachable | fixed — `SolveCtx.validate_model()` |
| **G11** | D | aggregates unmeasured | measured — cdcl handles all four; bb now *defers* rather than erroring |
| **G12** | D | a stale `BIN_NEQ` workaround, or an unfound bug | stale — workaround removed |

Both the expression probe and the feature probe are green row for row, with one
deliberate exception: W8/W9 (a variable wider than 64 bits) read `UNSUPPORTED` on
cdcl and `ok` on bb. That is the intended end state, not a remaining gap — see
G2.

Severity is about *what the caller sees*, because that decides who gets blamed:

| sev | meaning |
|---|---|
| **A** | silent wrong answer — a returned assignment violates the problem |
| **B** | false unsat — a satisfiable problem reported unsatisfiable, which reads to a user as "my constraints conflict" |
| **C** | loud incomplete — blocks the model, but says so |
| **D** | API/usability gap — correct results, unusable or unreachable |

## What was wrong, and what the fix was

### G6/G7/G8 — the expression-shape gaps (R1, R2)

13 of the expression probe's 27 shapes were reported `INCOMPLETE` by the
propagator engine, and the bit-blaster solved every one of them correctly from
the *same* `SolveProblem`. So this was never a missing solver capability. It was
a coverage gap in one engine's hand-written shape matcher, concentrated in two
functions.

**R1 — `_value_to_var` had no `EXPR_BINARY` arm.** The materialiser handled
`VAR`, `CONST`, `ITE`, `EXTRACT` and `EXTEND`, then fell off the end to
`return EXPR_NULL`. There was no way to reduce an arithmetic subtree to a
variable, so arithmetic compiled only where `_compile_constraint` special-cased
it inline: one operation, with a bare variable or constant on the other side of
an `==`. Everything else — nested arithmetic, arithmetic under `<`, arithmetic in
an OR leaf, constant-only arithmetic — reached no propagator.

This is the same defect class the formal side had already fixed four times
(`cdcl_followup_plan.md` items 8 and 13, `session_status_2026-05-24.md` items 3
and 8: concat, extend and extract operands each "silently dropped" until
`_value_to_var` was threaded in). The arithmetic operand case was the same bug
arriving from the randomization direction.

The arm recursively materialises both operands, allocates an aux result var, and
wires the matching propagator. Two details carried most of the risk:

* **Width.** `_expr_width` computes the width to evaluate the subtree at. A
  default of 32 would make `a + 1` on a `bit[8]` yield 256 instead of 0, so
  `x < a + 1` would be satisfied by assignments the problem forbids — a silent
  wrong answer introduced by a fix for a loud one.
* **Placement.** The generic materialise-both-sides comparison, and the
  reify-the-root fallback that makes a negated membership compile, are at the
  *end* of `_compile_constraint`, after the specialised
  `r == extend/extract/concat/select` handlers. Those encode their shapes far
  more tightly — the zero-extend handler tightens both operands to the source
  width at compile time — and a catch-all placed earlier silently preempted
  every one of them. That showed up as lost propagation, not a wrong answer, so
  no solve-level test caught it; `test_extend.py`'s bound assertions did.

The operator→propagator switch existed in two inline copies and is now
`_emit_binop_prop`.

**R2 — `_bool_to_var` could only reify a comparison against a constant.** A
var-var *inequality* could not become a guard, so it could not be an ITE
condition — which is exactly `(a < b) ? b : a`, the body of the LRM's own
value-yielding `max`, rejected while `(a == 1) ? …` and `(a < 10) ? …` compiled.

`_reify_cmp_var_var` handles all six operators over two materialised values. The
two reification templates speak exactly two relations (`g ↔ a==b`, `g ↔ a≤b`), so
the other four come from swapping operands and negating the guard, never from
offsetting a constant. That matters: the var-const path reifies `a ≥ c` as
`¬(a ≤ c-1)`, and that `c-1` is why it needs a constant in the first place (and
why it needs an unsigned-boundary fold above it to stay sound at `c == 0`).

Membership became a reifiable leaf in the same function — a range is a
conjunction, a set or a union of ranges a disjunction over comparisons.

**R3 (constant folding) turned out not to be a separate fix.** `x == 10 + 10`
and `x < 19 + 1` compile via R1's aux-var materialisation. A recursive constant
evaluator would avoid an aux var per constant subtree; that is a performance
improvement, not a correctness gap, and was not done.

**R4 was a dead block that read as coverage.** `zsp_compile.c` pattern-matched
`BinOp(var, var) op const`, computed the effective operator, and ended at

    /* For now, use the IR translator to handle this by
     * introducing a temp var. See below. */

with no "below" — it emitted no propagator and fell through to uncompiled. R1
subsumes it; it is deleted.

### G1 — width-64 `ADD`/`SUB`

`_fire_bounds_add_64` did plain `int64_t` interval arithmetic — `alo+blo`,
`rlo-bhi`, … — with no overflow guard. Two things go wrong at width 64 that do
not at width 32:

* `a.hi + b.hi` on two int64 bounds is signed overflow, which is undefined
  behaviour and in practice wraps;
* an **unsigned** width-64 variable stores its bounds as uint64 *bit patterns*,
  so `var_b_lt` compares them unsigned — and a negative intermediate like `-1` is
  read as 2^64-1, a bound above every value in the domain, which empties it.

Either fabricates a conflict on a satisfiable problem. `x == a + 1` at width 64
failed at *any* declared bounds, including `[0,1000]`.

The fix is deliberately conservative: bounds are computed with overflow detection
and applied only when the target variable can represent them. **Skipping a
tightening is always sound** — it costs propagation, and the search still has to
satisfy the constraint some other way — so anything the int64 arithmetic cannot
be trusted to have computed is simply not applied.

`bounds_mul_64` already had `_mul64_overflow` guards, which is why `MUL` at 64
worked and `ADD` did not. The add/sub template was the one that never got them.

### G2 — variables wider than 64 bits

One 65-bit variable and *no constraints at all* was enough to make the problem
unsat. Two independent halves were broken, and they fail in opposite directions:

* `var_lo64`/`var_hi64` have no tier-2 arm, so they cast a `WideBoundsN` to a
  `WideBounds64` and read the header `{uint32_t n_limbs; uint32_t _pad;}` as the
  `lo` field. The domain came back `[2, 0]` — empty.
* `trail_record_lb`/`ub` *do* refuse tier-2 (`/* tier-2: Phase 6 */`), but
  `ctx_tighten_lb64`/`ub64` **ignore that failure** and return `PROP_OK` anyway,
  so a tightening silently does not happen.

Fixing only the reads would have traded a false UNSAT for something worse:
constraints on a wide variable would compile, appear to propagate, and be quietly
unenforced — severity B becoming severity A. So `solver_compile` declines the
problem instead, with a distinct code. `SolveCtx` raises
`CompileUnsupportedError`, a subclass of `CompileIncompleteError` so a caller
that already escalates on an incomplete compile keeps working unchanged — and the
engine it escalates to, the bit-blaster, handles these widths correctly.

`_init_tier2` is kept and documented as unreachable. It is the storage half of
the unwritten "Phase 6"; deleting it would lose the limb layout and the
sign-extension rules. `var_lo64`/`var_hi64` gained an assert so a future tier-2
caller cannot reintroduce the header-as-bound read silently — the R4 lesson
applied to a live function rather than a dead one.

### G3 — `solve_n` and soft constraints

A hard `x > 200` with a soft `x < 10` returned 0 solutions via `solve_n` and 228
via `solve`, on the same problem. `solver_solve_n` called `_solver_solve_core`
directly, and the MaxSAT relaxation loop that makes a soft constraint soft lives
in the `solver_solve` wrapper, not the core. `solve_n` is the batch API a
stimulus generator reaches for, so it was the path most likely to hit this.

### G4 — `dist` and `AllDifferent` in the bit-blaster

`_bb_encode` guarded `allDiff_head` and `sources_head` — deferring rather than
dropping — but never read the dist list at all, so a `dist` over `{1,2,3}`
returned 47.

`dist` is a domain restriction on this engine, not only a sampling hint: the
propagator side's value picker draws only from nonzero-weight ranges that
intersect the feasible domain, and the suite locks that (`test_dist_domain_restriction`,
`test_dist_zero_weight_excluded`). So the satisfiability content is
`var ∈ ⋃ {[lo_i, hi_i] : weight_i > 0}`, which is the shape `bb_in_ranges`
already encodes. Weights are deliberately not modelled — a SAT solver returns a
witness, not a sample.

`AllDifferent` is now encoded as pairwise NEQ rather than deferred. Deferral was
sound, but it meant a problem combining `unique` with a shape cdcl could not
compile got `INCOMPLETE` from one engine and `UNKNOWN` from the other — no answer
from either. That combination is now answerable from both sides.

### G5/G10 — the silent path and the unreachable net

`solver_compile` returns the *count* of constraints it could not compile. What
happened next depended entirely on the caller:

| caller | handling of `rc > 0` | consequence |
|---|---|---|
| `dv_solve.ctx.SolveCtx` | raises `CompileIncompleteError` | loud — this is what pssc sees |
| `zsp_dpi.c` | tested `rc < 0` only | **silent** — the SV/DPI consumer solved with those constraints dropped |
| `smt2_frontend.c` | returns `rc`, does not set `compiled` | loud, plus a bit-blast route |

The formal path had both a fallback engine and a post-solve validator; the
randomization path had neither, and one of its two entry points did not check the
return code. A dropped constraint there is an under-constrained stimulus that
looks like a successful solve — worse than the error message, because nothing
reports it.

The DPI handle now remembers the count (`zsp_dpi_n_uncompiled_h`), and when it is
non-zero a successful search is re-checked against the *original* problem before
being reported as OK; a model that violates a dropped constraint returns 3.
`solver_validate_model` — which re-evaluates every constraint against the
assignment, and was called only from the SMT2 frontend — is exposed as
`SolveCtx.validate_model()`.

This is the item that decides whether the *next* gap in this class is found by a
test or by a silently weak stimulus.

### G11 — the aggregates, measured

`SUM`, `COUNTONES`, `CLOG2` and `ARRAY_SELECT` are reachable only through the
growable builder API, which is why neither of the first two probes covers them —
both build through `SolveProblem`. They were the known unknown.

`aggregate_coverage_probe.py` measures them: **cdcl handles all four correctly**,
including under a pinned result (`countones(x) == 3`, `arr[i] == 20` solving the
index backwards), with `solver_validate_model` confirming each model.

The bit-blaster does not bit-blast them, which is fine — but it used to route
them through `err_bv`, so `check()` returned `ZSP_BB_ERROR`. A caller reads that
as "something went wrong" rather than "ask the other engine", and the other
engine is precisely the one that can answer. They now set `had_unsupported` and
return `ZSP_BB_UNKNOWN`, which is the pattern this file already uses for signed
div/mod. `err_bv` is for *malformed* input — a bad `ExprRef`, an unknown
`BinOp` — and these four nodes are well-formed, just not supported.

### G12 — the stale workaround

`problem.py`'s `add_at_least_one` spelled each `v != 0` term as
`(v < 0) OR (v > 0)`, with the comment *"BIN_NEQ has a native solver bug"*.

No such bug is reachable. `BIN_NEQ` against a constant is correct at the
constraint root and as an OR leaf — the only shape this helper builds — verified
exhaustively over 1..4 variables and every subset pinned to zero, **and against
the pre-change library**, so the removal does not rest on a fix made in the same
breath.

The workaround was also only accidentally correct: `v < 0` is vacuously false for
an unsigned variable, which is the only case it was ever exercised on, so the
pair collapsed to the intended `v > 0`. The helper never declared which
signedness it assumed.

## Still open

**G9 — no unsigned or wide value readback on the cdcl path.** `solver_get_value`
returns `int64`, so a `bit[64]` value above 2^63 arrives negative, and there is no
`value_wide` equivalent (bb has `zsp_bbsolver_value_wide`). `add_var`'s `lo`/`hi`
are `int64` too, so a >64-bit domain cannot be expressed at all.

This is severity D — the values are correct, they are just returned in a type
that loses the distinction — and it is a signature change across the C API, the
Python wrapper and the DPI shim rather than a localised fix. It is also partly
moot for widths above 64 now that G2 declines them outright: the domain those
variables would need cannot be expressed through `add_var` anyway, which is the
same constraint from the other end. The feature probe documents the masking a
caller must do today, at the top of the file.

Related, and deliberately not changed: `_fire_bounds_lt_64` computes `hi - 1` and
`lo + 1` without an overflow guard. For an unsigned width-64 variable that
arithmetic is *correct* under the modular reading of the stored bit pattern,
which is why `x < a` at width 64 works; applying G1's conservative skip there
would trade a measured-working path for a weaker one. The unguarded edge is a
bound sitting exactly at the representable extreme.

## Not dv-solve

Recorded because these were measured in the same sitting, against the same PSS
suite, and three of them had previously been attributed to the solver:

* **The ternary is not lowered by be-bc at all** — `ir.ExprIfExp` fails as
  `LoweringError: unsupported constraint expression ExprIfExp`, before any
  `SolveProblem` is built. G7 above meant that *even once be-bc emits it*, cdcl
  would reject the LRM's own `max` shape; that half is now fixed, so the be-bc
  lowering is the only remaining blocker on that example.
* **`foreach` flattening and inline `with {}`** — declared later-phase work by
  be-bc's own error messages.
* **Type inheritance never reaches the solve problem** — `action A : Base {}`
  arrives with no fields at all; a be-bc/`PSSToScenarioPass` flattening gap.
* **Struct sub-fields are not observable** in the pssc test harness (`rand S s;`
  yields one result key `s`) — a harness limitation.

## Probe API notes

Two details the probes document at the top, because getting either wrong
manufactures convincing false failures (both were made, and caught, while writing
them): `expr_in_range`/`expr_in_set` take **ExprRefs** for bounds and elements
rather than Python ints, and values read back through `solve_n`/`get_value`/
`value` are **int64**, so unsigned values above 2^63 arrive negative and must be
masked to the declared width before checking (G9). The first mistake made
`in_range` look broken at the root; the second made every width-64 row look like
a wrong answer. Neither was.

A third, added since: `BVSatCtx` is **not incremental** — kissat rejects a second
`check()` on the same context. A seeded loop must build a fresh `BVSatCtx` per
seed, as `_solve_with_seed` in `tests/unit/test_bvsat.py` does.
