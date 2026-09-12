# The SolveProblem path: expression coverage, and the gaps around it

*(Was "CDCL expression coverage". Broadened 2026-09-11 after a feature-axis sweep
found defects outside expression shape — see §Consolidated gap list.)*

Date: 2026-09-11
Status: measured findings + proposed fix order
Measured with: `docs/expr_coverage_probe.py` (in this directory; self-contained,
runs against the installed `libdv_solve.so`)

**Reassessed 2026-09-11 against `4bbf25e`** (the "SMT solver updates" merge —
CaDiCaL backend, incremental bbsolver, +272 lines in `zsp_compile.c`). Rebuilt
with `ZSP_WITH_CADICAL=ON`; 17/17 ctest and 623 pytest green.

* **Coverage: identical, row for row.** Every shape in the table below lands in
  the same column as before. All four root causes hold verbatim in the merged
  source (line numbers below are `4bbf25e`).
* **What changed is the cost of the fallback.** The merge's size-gated "light
  search" in `zsp_bbsolver.c` (kissat probing off below 2M clauses) made the
  bit-blast engine **~30× faster on randomization-sized problems** — it is no
  longer 1–2 orders of magnitude behind the propagator engine but within 0.3–5×,
  and it is *faster* at 8-bit widths. That moves the fallback from "last resort"
  to a genuine option, and it is why the recommended order below was revised.
* **One cause got cheaper to fix.** The merge factored aux-var allocation into
  `_init_aux_tiered` (tier-aware, handles width > 32), which is most of the
  boilerplate R1 needs. R1 is a smaller edit now than when this was written.

## Consolidated gap list

Added 2026-09-11 after a second sweep over the axes the expression probe never
varied — signedness, width tier, membership, bitwise/divide, AllDifferent, soft,
`dist`, and the two solve entry points (`docs/feature_coverage_probe.py`). The
expression findings alone were a statement about one PSS construct; these two
probes together are a statement about the `SolveProblem` path.

Severity is about *what the caller sees*, because that decides who gets blamed:

| sev | meaning |
|---|---|
| **A** | silent wrong answer — a returned assignment violates the problem |
| **B** | false unsat — a satisfiable problem reported unsatisfiable, which reads to a user as "my constraints conflict" |
| **C** | loud incomplete — blocks the model, but says so |
| **D** | API/usability gap — correct results, unusable or unreachable |

| id | sev | gap | repro | cause |
|---|---|---|---|---|
| **G1** | **B** | **width-64 `ADD`/`SUB` reports unsat on a satisfiable problem.** `x == a + 1` at width 64. Unsigned: fails at *any* declared bounds, including `[0,1000]`. Signed: fails when the domain reaches the int64 extremes, works at `±1000`. Width ≤ 63 fine; `MUL` at 64 fine. | `feature_coverage_probe.py` W4/W5/W6 vs W3/W7 | `_fire_bounds_add_64` (`zsp_prop_templates.c:1148`) does plain `int64_t` interval arithmetic — `alo+blo`, `rlo-bhi`, … — with no overflow guard and no modular semantics. A negative or wrapped intermediate becomes a bound that empties the domain. |
| **G2** | **B** | **any variable wider than 64 bits makes the whole problem unsat** — one 65-bit variable and *no constraints at all* is enough. | W8/W9 | tier2 (`_init_tier2`, wide-limb bounds) is initialised but the search/bound path does not honour it. Not an artifact of the int64 `lo`/`hi` API (G9): it fails with int64-expressible bounds too. |
| **G3** | **B** | **`solve_n` cannot relax a soft constraint.** A hard `x > 200` with a soft `x < 10` returns 0 solutions via `solve_n`; the *same problem* via `solve` returns 228. A soft constraint that doesn't need relaxing is fine. | D2 vs D3, control D4/D5 | `solve_n`'s reset-between-solves loop does not restore the assumption/priority state that soft relaxation depends on. `solve_n` is the batch API a stimulus generator uses. |
| **G4** | **A** | **the bit-blast engine silently ignores `dist`.** With a `dist` over `{1,2,3}` it returns 47. It has no `dist_head` handling and — unlike `AllDifferent`/sources — no guard that defers instead. | D6 | `zsp_bbsolver.c` `_bb_encode` guards `allDiff_head` and `sources_head` only. |
| **G5** | **A** | **`zsp_dpi.c` ignores a positive `solver_compile` return**, so the SV/DPI consumer solves with uncompiled constraints dropped. | code reading (`zsp_dpi.c:107` tests `rc < 0`) | — |
| **G6** | **C** | **arithmetic outside the special-cased `==` shapes** — under `<`/`>`/`!=`, nested, on both sides, in an OR/implication leaf, constant-only. The original finding; see R1/R3 below. | `expr_coverage_probe.py` B/C/D/E/F rows | `_value_to_var` has no `EXPR_BINARY` arm. |
| **G7** | **C** | **a var-var inequality cannot be reified**, so `(a < b) ? … : …` — the LRM's own `max` — is rejected while `(a == 1) ? …` and `(a < 10) ? …` compile. | G1 row of the expression probe | `_bool_to_var`'s comparison arm requires a constant on one side. |
| **G8** | **C** | **`in_range`/`in_set` work at the constraint root but not as a reifiable leaf** — under an OR, or negated. Same shape of gap as G6/G7, different node kind, and `x in [a..b]` inside an implication is a common PSS idiom. | R3/R4 vs R1/R2 | the OR/negation paths route through `_bool_to_var`, which has no membership arm. |
| **G9** | **D** | **no unsigned or wide value readback on the cdcl path.** `solver_get_value` returns `int64`, so a `bit[64]` value above 2^63 arrives negative, and there is no `value_wide` equivalent (bb has `zsp_bbsolver_value_wide`). `add_var`'s `lo`/`hi` are `int64` too, so a >64-bit domain cannot be expressed. | measured: `get_value` → -5587729586572015559 for a 64-bit var constrained `> 2^62` | — |
| **G10** | **D** | **the only post-solve safety net is unreachable from this path.** `solver_validate_model` re-evaluates every constraint against the assignment and is called *only* from the SMT2 frontend; it is not exposed in the Python wrapper or called by the DPI shim. | code reading | — |
| **G11** | **D** | **`SUM`/`COUNTONES`/`CLOG2`/`ARRAY_SELECT` are reachable only through the growable builder API**, not `SolveProblem`; bb rejects all four outright. cdcl's support for them is **unmeasured** — a hole in this assessment, not a known defect. | — | — |
| **G12** | **D** | **a stale workaround, or an unfound bug.** `problem.py`'s `add_at_least_one` avoids `BIN_NEQ` with the comment *"BIN_NEQ has a native solver bug"*, but `x != 5`, `x != a` and `x != 0` all solve correctly. Either the bug is fixed and the workaround should go, or it is shape-specific and undocumented. | N1/N2/N3 | — |

**Measured working**, so the list is not read as "everything is broken": signed
32-bit including negatives (S1–S4); all of bitwise/shift/divide under `==`,
including reachable divide-by-zero (B1–B7); `in_range`/`in_set` at the root;
`AllDifferent`; `dist` on the cdcl path; soft constraints via `solve`; comparisons
at every width ≤ 64; `!=` in all three shapes.

**Where to start.** G1–G3 are the ones to fix first, and not because they are the
largest: they are the three that produce a *wrong* answer rather than a refusal.
G1 and G2 make a satisfiable model look self-contradictory, which sends the user
hunting a modelling error that does not exist; G3 does the same to anyone using
the batch API with soft constraints. G4 and G5 are the two places where something
is dropped with no report at all, and G5 is the one that reaches a real
SystemVerilog flow. G6–G8 are the coverage work already detailed below — they
block, but they block honestly. G11 is the known unknown: aggregates were not
measured, and the list should not be read as covering them.

## Summary

dv-solve contains two engines that both accept a `SolveProblem`:

* **cdcl** — `solver_compile` + `solver_solve` (`zsp_compile.c`), the
  bounds/propagator engine. This is what the builder API, the DPI shim and
  therefore every constrained-random consumer reaches.
* **bb** — `zsp_bbsolver` (`zsp_bbsolver.c`, via `zsp_bitblast` + `zsp_aig_cnf`
  + kissat), the bit-blasting completeness engine, wrapped for Python as
  `dv_solve.bvsat.BVSatCtx`.

**Every expression shape measured below that cdcl rejects, bb solves correctly
from the same `SolveProblem`.** So this is not a missing solver capability. It is
a coverage gap in one engine's hand-written shape matcher, with no fallback to
the engine that does not have the gap.

The gap is concentrated: `_value_to_var` — the function that materialises a value
expression as a solver variable — has arms for `VAR`, `CONST`, `ITE`, `EXTRACT`
and `EXTEND`, and **no arm for `EXPR_BINARY`**. Arithmetic therefore compiles only
where `_compile_constraint` happens to special-case it inline: one operation, with
a bare variable or a constant on the other side of an `==`. Everything else —
nested arithmetic, arithmetic under `<`, arithmetic in an OR leaf, constant-only
arithmetic — reaches no propagator.

This is the same defect class the formal side has fixed four times already
(`cdcl_followup_plan.md` items 8 and 13, `session_status_2026-05-24.md` items 3
and 8: concat, extend and extract operands each "silently dropped" until
`_value_to_var` was threaded in). The arithmetic operand case is the same bug
arriving from the randomization direction.

## Measured

8-bit unsigned vars, one constraint per problem, built through
`dv_solve.problem.SolveProblem` directly — no pssc, no be-bc, no IR in the
picture. Arithmetic is checked modulo 2^8, which is what an 8-bit bit-vector
means. `ok` means compiled *and* the returned assignment satisfies the
constraint.

| # | shape | cdcl | bb |
|---|---|---|---|
| A1 | `j == k + 1` | ok | ok |
| A2 | `j == k * 2` | ok | ok |
| A3 | `j == k + l` | ok | ok |
| A4 | `k + 1 == 200` | ok | ok |
| A5 | `x % 4 == 0` | ok | ok |
| B1 | `x < a + 1` | **INCOMPLETE** | ok |
| B2 | `x <= a + b` | **INCOMPLETE** | ok |
| B3 | `x > a - 1` | **INCOMPLETE** | ok |
| B4 | `x != a + 1` | **INCOMPLETE** | ok |
| B5 | `a + 1 < x` | **INCOMPLETE** | ok |
| C1 | `j == (k + 1) + 1` | **INCOMPLETE** | ok |
| C2 | `j == k * 2 + 1` | **INCOMPLETE** | ok |
| C3 | `j == (k + l) * 2` | **INCOMPLETE** | ok |
| D1 | `j + 1 == k + 2` | **INCOMPLETE** | ok |
| E1 | `x == 10 + 10` | **INCOMPLETE** | ok |
| E2 | `x < 19 + 1` | **INCOMPLETE** | ok |
| F1 | `(j == k + 1) \|\| (x > 200)` | **INCOMPLETE** | ok |
| F2 | `(x < 10) && (j == k + 1)` | ok | ok |
| F3 | `(a != 1) \|\| (j == k + 1)` (an implication) | **INCOMPLETE** | ok |
| F4 | `(x < 10) \|\| (x > 200)` — control, no arithmetic | ok | ok |
| F5 | `(k < l) \|\| (x > 200)` — var-var inequality as an OR leaf | ok | ok |
| G1 | `j == ((k < l) ? l : k)` | **INCOMPLETE** | ok |
| G2 | `j == ((k == 1) ? l : k)` | ok | ok |
| G3 | `j == ((k < 10) ? l : k)` | ok | ok |
| G4 | `j == ((k < l) ? l + 1 : k)` | **INCOMPLETE** | ok |
| H1 | `x < a` — control | ok | ok |
| H2 | `x == a` — control | ok | ok |

`INCOMPLETE` = `CompileIncompleteError: 1 constraint(s) could not be compiled
natively`, i.e. `solver_compile` returned a positive count.

Four things in that table are worth reading twice:

1. **The boundary is not "arithmetic is unsupported"** — A1–A5 show one
   arithmetic operation working under `==` with either a variable or a constant
   on the other side, including `%`. It is *narrower* than "arithmetic works" and
   *wider* than "only `var == var op const`".
2. **`&&` works where `||` does not** (F2 vs F1) for the identical leaf. `BIN_AND`
   recurses into `_compile_constraint`, which has the inline arithmetic case;
   `BIN_OR` goes through `_flatten_or`/`_bool_to_var`, which does not.
3. **The one ternary shape the PSS LRM itself uses is the one that fails** (G1).
   `(a < b) ? b : a` — the body of the LRM's own value-yielding `max` example.
   G2/G3 show ITE working; what G1 adds is only that the condition is a *var-var
   inequality*, and that is the part `_bool_to_var` cannot reify.
4. **Constant-only arithmetic is not folded** (E1, E2). `x == 10 + 10` is not
   turned into `x == 20` anywhere on this path.

## Root causes

### R1 — `_value_to_var` has no `EXPR_BINARY` arm (`zsp_compile.c:1086`)

The materialiser handles `EXPR_VAR`, `EXPR_CONST`, `EXPR_ITE`, `EXPR_EXTRACT` and
`EXPR_EXTEND`, then falls off the end to a bare `return EXPR_NULL`. There is no
way to reduce an arithmetic subtree to a variable — and every propagator that
would consume one already exists in exactly the ternary-relation form
materialisation needs:

    prop_add_bounds_add_32/64(ctx, r, a, b)      /* r = a + b   */
    prop_add_bounds_mul_32/64, ..._div_, ..._mod_
    prop_add_bounds_band_64, ..._bor_, ..._bxor_, ..._shl_, ..._lshr_

`_compile_constraint` uses them in its `var == BinOp(a, b)` case, but only for
operands that are *already* a plain var or const. Adding the recursive arm makes
that same switch reachable for any depth. Aux-var allocation is now factored into
`_init_aux_tiered` (added by `4bbf25e`), which is most of what the new arm needs.

Accounts for: B1–B5, C1–C3, D1, G4, and the arithmetic half of F1/F3.

### R2 — `_bool_to_var` can only reify a comparison against a constant (`zsp_compile.c:928–938`)

The comparison arm computes `is_vc` / `is_cv` and requires one side to be an
`EXPR_CONST`; the only non-constant case is a var-var `BIN_EQ` special case
further down. Two consequences:

* A var-var **inequality** cannot become a guard, so it cannot be an ITE
  condition — that is exactly G1, and `prop_add_reification_32(ctx, g, a, b)`
  already means `g ↔ (a ≤ b)` over two var ids, with the invert-via-add trick for
  `>`/`>=` already written a few lines below (999–1057).
* A comparison whose operand is an arithmetic expression cannot become a guard
  even with R1 fixed, because the `!is_vc && !is_cv` recovery block (930–938)
  *also* insists the other side is a constant. F1/F3 need "materialise both sides,
  then reify var-var" — a generalisation of what is already there for one side.

Accounts for: G1, and the reification half of F1/F3.

### R3 — no constant folding below the root (`_is_const`, `zsp_compile.c:120`)

`_is_const` matches `EXPR_CONST` only. The one fold in `_compile_constraint`
(`zsp_compile.c:1457`) handles `CONST cmp CONST` *at the constraint root*. A constant
subtree one level down — `x == 10 + 10` — matches nothing. R1 would make these
compile via an aux var, which is correct but wasteful: a recursive constant
evaluator in `_is_const` is both cheaper and a prerequisite for good bounds.

Accounts for: E1, E2.

### R4 — a dead branch that reads as coverage (`zsp_compile.c:1848–1885`)

`/* BinOp(var, var) op const — handle the pattern where an arithmetic expression
is compared to a constant */` pattern-matches the shape, computes `eff`, and then
ends at

    /* For now, use the IR translator to handle this by
     * introducing a temp var. See below. */

with no "below". The block emits no propagator and falls through to uncompiled.
This is the B-row gap, already localised in the source. It should be deleted when
R1 lands, not extended — R1 subsumes it.

## Severity: where this is loud and where it is silent

`solver_compile` returns the *count* of constraints it could not compile. What
happens next depends entirely on the caller:

| caller | handling of `rc > 0` | consequence |
|---|---|---|
| `dv_solve.ctx.SolveCtx` (`ctx.py:118`) | raises `CompileIncompleteError` | loud — this is what pssc sees |
| `zsp_dpi.c:106` | tests `rc < 0` only | **silent** — the SV/DPI consumer solves with those constraints dropped |
| `smt2_frontend.c:2790` | returns `rc`, does not set `compiled` | loud, plus a bit-blast route |

`solver_validate_model` (`zsp_ctx.h:196`) — which re-evaluates every constraint
against the returned assignment and is precisely the net that catches a dropped
constraint — is called **only** from `smt2_frontend.c:3896`. It is not exposed in
the Python wrapper and not called by the DPI shim.

So the formal path has both a fallback engine and a post-solve validator; the
randomization path has neither, and one of its two entry points does not even
check the return code. A dropped constraint there is an under-constrained
stimulus that looks like a successful solve — which is worse than the error
message, because nothing reports it.

## Recommended fix order

Timings on this machine, chained `<` constraints, 30 solves each, **remeasured on
`4bbf25e`**:

| shape | cdcl | bb | ratio | bb on `cfa92c3` |
|---|---|---|---|---|
| w=8, 8 vars | 0.230 ms | 0.076 ms | **0.3×** | 1.42 ms |
| w=16, 8 vars | 0.165 ms | 0.118 ms | 0.7× | 2.92 ms |
| w=32, 8 vars | 0.163 ms | 0.194 ms | 1.2× | 5.80 ms |
| w=64, 8 vars | 0.161 ms | 0.370 ms | 2.3× | — |
| w=32, 24 vars | 0.215 ms | 0.644 ms | 3.0× | — |
| w=32, 48 vars | 0.337 ms | 1.767 ms | 5.2× | — |

The last column is the same measurement before the merge. The bit-blast engine is
**~30× faster than it was** on problems this size, because `4bbf25e` gates
kissat's failed-literal probing off below 2M clauses
(`zsp_bbsolver.c`, `DV_KISSAT_LIGHT_MAXCLAUSES`) — pure overhead on small
instances, and everything a stimulus problem produces is a small instance. It is
now *faster* than the propagator engine at 8 bits and within ~5× at 48 vars,
still growing with width × var count as bit-blasting must.

This revises the earlier recommendation. "Route everything to bb" is still the
wrong answer — the cost grows the wrong way for wide problems, and the propagator
engine is what supports bound-nudging and distribution control. But **bb as a
fallback is now cheap enough to land first**, ahead of the compiler work: it
converts every `INCOMPLETE` row into a correct answer in one wiring change, and
then R1–R3 become a *performance* improvement on shapes that already work rather
than the only thing standing between a model and a solve.

0. **Wire bb as a fallback** — *but fix G4 first.* The feature sweep found that bb
   silently ignores `dist` (returns 47 for a `dist` over `{1,2,3}`), and a
   fallback that drops a constraint without saying so converts a severity-C
   refusal into a severity-A wrong answer. The guard in `_bb_encode` that defers
   on `AllDifferent`/sources is the right pattern; `dist_head` needs the same
   treatment, and then encoding rather than deferral for both.

   Keyed on `solver_compile` returning `rc > 0` rather than on a flag. The
   problem object is already the right type
   (`zsp_bbsolver_new` takes a `SolveProblem`) and `BVSatCtx.check(seed=...)`
   varies with the seed — 12 distinct assignments over 12 seeds on B1 — so it is
   usable for stimulus. Promoted to first place by the timings above; it was item
   6 when the ratio was 5–142×. Its *uniformity* is still unmeasured, and that
   matters more now that it would be on the default path: if the fallback becomes
   load-bearing for constrained-random, measure the distribution before trusting
   it for coverage-driven work.

   Two things the wiring must respect, both already explicit in the source:
   the fallback is **whole-problem, not per-constraint** — you cannot solve some
   constraints with propagators and the rest by bit-blasting, so `rc > 0` means
   re-submitting the entire `SolveProblem` to bb; and bb **defers rather than
   drops** on `AllDifferent` and source groups (`_bb_encode`, the deliberate
   `ZSP_BB_UNKNOWN` return) and rejects `SUM`/`COUNTONES`/`CLOG2`/`ARRAY_SELECT`.
   That second point has teeth here: be-bc lowers PSS `unique` to
   `add_all_different` when it can (`lower/constraints.py:589`), so a model with
   both `unique` *and* an uncompilable arithmetic shape would get `INCOMPLETE`
   from one engine and `UNKNOWN` from the other. That combination is the case to
   test first, not last — measured on `4bbf25e`, three 8-bit vars:

   | problem | cdcl | bb |
   |---|---|---|
   | `x < a` | ok | SAT |
   | `x < a + 1` | INCOMPLETE | SAT |
   | `all_different` + `x < a` | ok | **UNKNOWN** |
   | `all_different` + `x < a + 1` | **INCOMPLETE** | **UNKNOWN** |

   So the fallback covers the gap everywhere except where the problem also uses
   `unique`, and there neither engine can answer. Encoding `AllDifferent` in the
   bit-blaster (pairwise NEQ is the obvious lowering, which is what be-bc falls
   back to already when the var count is too high) closes that corner and is the
   one addition item 0 needs to be complete rather than partial.
1. **R1 — `EXPR_BINARY` arm in `_value_to_var`.** Recursively materialise both
   operands, allocate an aux result var of the operand width, add the matching
   `prop_add_bounds_*` propagator. ~40 lines, no new propagators, and it is the
   same edit shape as the concat/extend/extract fixes already in the log. Then
   make `_compile_constraint`'s comparison path fall back to
   materialise-both-sides-and-compare-var-var, which is what turns B1–B5 and D1
   into the existing `prop_add_bounds_lt_32` etc.
2. **R2 — generalise `_bool_to_var`'s comparison arm** to materialise both sides
   via `_value_to_var` and reify var-var for all six operators, reusing
   `prop_add_reification_32` and the existing invert trick. Unblocks G1 and the
   OR/implication leaves.
3. **R3 — recursive constant folding**, so a constant subtree becomes a value
   rather than an aux var.
4. **Delete R4's dead block.**
5. **Fail loudly on the paths that currently do not.** `zsp_dpi.c` should treat
   `rc > 0` as an error (or at minimum report it), and `solver_validate_model`
   should be exposed in the Python wrapper so a consumer can assert the
   assignment actually satisfies the problem it submitted. This is the item that
   decides whether the *next* gap in this class is found by a test or by a
   silently weak stimulus.

Items 1–3 are still the ones that pay for the long run: they widen the fast path,
and each is a local edit to a function that already does the analogous thing for a
neighbouring node kind. Item 0 is what makes the gap stop blocking anyone while
that work happens, and item 5 is what stops the next gap in this class from being
found the hard way.

## What is *not* dv-solve

Recorded because these were measured in the same sitting, against the same PSS
suite, and three of them have previously been attributed to the solver:

* **The ternary is not lowered by be-bc at all** — `ir.ExprIfExp` fails as
  `LoweringError: unsupported constraint expression ExprIfExp`, before any
  `SolveProblem` is built. G1 above says that *even once be-bc emits it*, cdcl
  will reject the LRM's own `max` shape until R2 lands. Two independent blockers
  stacked under one example.
* **`foreach` flattening and inline `with {}`** — declared later-phase work by
  be-bc's own error messages.
* **Type inheritance never reaches the solve problem** — `action A : Base {}`
  arrives with no fields at all; a be-bc/`PSSToScenarioPass` flattening gap.
* **Struct sub-fields are not observable** in the pssc test harness (`rand S s;`
  yields one result key `s`) — a harness limitation.

## Reproducing

    python packages/dv-solve/docs/expr_coverage_probe.py      # expression shapes
    python packages/dv-solve/docs/feature_coverage_probe.py   # the other axes

Each prints a two-column table, cdcl against bb. A row is one `SolveProblem`; add
one by adding a `case(...)` line. Both probes check the returned assignment
against a Python evaluation of the same constraint, so a shape that compiles but
solves *wrongly* is reported as `WRONG` rather than passing — that distinction is
the point, since the failure mode this document is about is a constraint that is
not enforced.

Two API details the feature probe documents at the top, because getting either
wrong manufactures convincing false failures (both were made, and caught, while
writing it): `expr_in_range`/`expr_in_set` take **ExprRefs** for bounds and
elements rather than Python ints, and values read back through
`solve_n`/`get_value`/`value` are **int64**, so unsigned values above 2^63 arrive
negative and must be masked to the declared width before checking. The first
mistake made `in_range` look broken at the root; the second made every width-64
row look like a wrong answer. Neither was.
