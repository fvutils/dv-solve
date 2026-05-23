# CDCL explain-callback soundness — fix plan

## Status update (after phase A + B1)

The root cause turned out to be in the **analyzer itself**, not in any
explain callback. `lcg_analyze_conflict`'s `seen[]` was indexed
per-variable; an antecedent of "v >= c AND v <= c" (singleton var,
required by non-monotone propagators like `bvand`) silently dropped
the second literal, producing an unsound learnt clause. Fix landed in
`src/c/zsp_lcg.c`: `seen[]` and `seen_lit[]` now indexed per
(var, is_lb) slot.

**Current state:**
- tier1: 4 of 5 fixtures fixed (memmaptight32 still false-unsat, separate cause)
- tier2/tier3: 0 disagreements; **6 fixtures regress to timeout**
  (cache_direct_1way d2/d8, regfile_simple d1/d2/d4/d8). Conflict
  counts up ~1000×. Cause: correct learnt clauses are ~2× larger,
  VSIDS signal worse, search wanders.
- `DV_LCG_TRACE=1` diagnostic tooling landed in zsp_lcg.c +
  `prop_fire_name()` registry in zsp_prop_templates.c.

Remaining work, in priority order:
1. **B2 (rule-aware bvand/bor/bxor explain)** — recover the lost perf
   by citing only the antecedents the actually-fired rule needed
   instead of all six watched-var literals. This was the prime suspect
   in the original plan and is still the right fix for the *quality*
   of explanations even though it wasn't the soundness root cause.
2. **memmaptight32 trace** — get a minimal repro and find the second
   unsound site.
3. C / D / E remain as originally planned.

## Headline

The tier1 cross-check (`tests/formal/test_cross_check_tier1.py`) currently
fails 5 fixtures (`memmaptight32`, `shiftaligned`, `sumpartition`,
`unique16`, `verilatorops`). All 5 are dv-solve false-UNSAT vs. z3 /
boolector sat. With `DV_USE_LCG=0` (CDCL off) every one returns the
correct `sat`, so the bug is in the CDCL **conflict-analysis /
explanation** path, not in any propagator's fire function. This plan
finds the unsound learnt clause, fixes the responsible explain
callback, audits the rest of `zsp_explain.c`, and lifts the int32
bound restriction in a forward-compatible way.

## Background

Lazy clause generation in dv-solve (`src/c/zsp_lcg.c`) builds learnt
clauses by asking each propagator "given that *you* tightened a bound
on var V to value B, what literals over the other watched vars imply
that change?" via the propagator's `explain(self, ctx, V, is_lb, B,
out)` callback in `src/c/zsp_explain.c`.

Today every explain callback reads **current** `var_lo64/var_hi64`
from `ctx` rather than the bounds that held when the propagator fired,
and stores bounds in `Literal.bound` which is `int32_t`. Three failure
modes are possible:

1. **Stale-bound explanation.** Bounds may have tightened further on
   other watched vars between the propagator's fire and the conflict;
   citing the current (tighter) bound is over-approximate but usually
   sound. Citing a current bound that was never simultaneously true
   with the consequent (e.g., one tightening was undone but the
   explanation walks the wrong direction) is unsound.
2. **Coarse but irrelevant antecedents.** `_explain_binary_bitwise`
   blindly returns all six watched literals regardless of *which*
   propagation rule fired. This is sound when all six current bounds
   really do imply the consequent — but the bvand fire function has
   distinct propagation rules (forward upper, forward singleton,
   backward `a |= r`) with different antecedents; an explanation that
   over-cites is usually sound but is suspect under non-monotone
   ops.
3. **int32 truncation.** `(int32_t)var_lo64(...)` silently wraps when
   `|v| > INT32_MAX`. The minimal repro keeps everything in 8/4-bit
   widths so this isn't the trigger there, but `unique16` and
   `wide_datapath_*` push wider, and it's a latent footgun.

We don't yet know which of (1)/(2)/(3) is actually firing — step A
of the plan is to find out.

## Out of scope

- Re-architecting the trail or propagator data layout. We will store
  the captured antecedents inside a propagator-instance side buffer
  if needed, not by changing every fire function's signature.
- Switching to eager clause learning or to a different CDCL algorithm.

---

## A — Pinpoint the unsound learnt clause (diagnose first)

A1. Add a compile-time-gated trace mode `DV_LCG_TRACE=1` in
`zsp_lcg.c::lcg_analyze_conflict`. For each conflict, emit to stderr:
  - decision_level, conflict_var (or conflict prop_ref)
  - every trail entry consulted: `var, kind, decision_level, prop_ref,
    fire_fn_name, old_value → new_value`
  - every explanation: `prop, fire_fn, var_id, is_lb, new_bound,
    {lits}`
  - the final learnt clause + backjump level

  The propagator fire-function name is recoverable today: every
  `_fire_*` is registered through a known table in
  `contra_register_explanations`; build a small `fire_fn → name`
  reverse-lookup so trace output is human-readable.

  Symbol used elsewhere: `lcg_dbg_bail[16]` already counts bail
  reasons. Reuse the `[lcg]` stderr channel; the trace lines must be
  prefixed `[lcg-trace]` and only emitted when `DV_LCG_TRACE` is set.

A2. Run the 7-line minimal repro:

    ```smt2
    (set-logic QF_BV)
    (declare-const a (_ BitVec 8))
    (declare-const masked (_ BitVec 4))
    (assert (bvuge a (_ bv10 8)))
    (assert (bvule a (_ bv200 8)))
    (assert (= ((_ zero_extend 4) masked) (bvand a (_ bv15 8))))
    (assert (bvule masked (_ bv5 4)))
    (check-sat)  ; expected sat — dv-solve currently returns unsat
    ```

  Inspect the first learnt clause the analyzer produces. Either
  (i) some learnt literal is currently true even though it shouldn't
  be (unsoundness ➜ false UNSAT after enough learnts), or
  (ii) the clause is "true" but its consequent is wrong (e.g.,
  drops the bound it was supposed to record).

A3. Bisection ladder if (A2) is ambiguous — drop one input at a time
and rerun the trace. We already know dropping any one of
{`bvuge a 10`, `bvule a 200`, `bvule masked 5`} makes it sat. The
trace from the smallest *failing* configuration is the artifact to
write up.

A4. Repeat for `unique16` — it has no `zero_extend`, just pairwise
`distinct` over 16 vars in a range. That points at
`explain_all_different` or `explain_bounds_ne`. Confirm the suspect
in trace before fixing.

**Exit criterion for phase A:** one named explain callback identified
as the source for each failing fixture, with a saved trace transcript
showing the unsound step.

---

## B — Fix the explain unsoundness

B1. **Decide the fix shape.** Two viable options, decided per
callback:

  - *Snapshot-on-fire:* propagator stores a small per-instance
    "last-fire antecedent" alongside its watch section, written by the
    fire function and read by explain. The watch-section pool already
    backs per-instance state via `PROP_WS`, so adding fields here is
    well-precedented.
  - *Use the trail directly:* the explain callback for var V at level
    L walks `ctx->trail_top` backwards looking for the *first* trail
    entry on V where `prop_ref == self->ref` and uses
    `old_value`/`decision_level` to recover what the propagator
    actually set. The propagator instance still owns "which watched
    vars I read" via `ws->var_ids[]`; the trail gives the per-watch
    bound *as of fire time*.

  We default to **trail-based** for the bitwise/eq/le/lt/add/mul
  callbacks (no per-instance storage growth; matches existing data
  layout) and only fall back to snapshot-on-fire where reading the
  trail is awkward (e.g., propagators that read but don't write some
  watched var). Decision is per-callback during B3.

B2. **`_explain_binary_bitwise` (the prime suspect).** Replace the
six-literal blanket cite with a rule-aware explanation:

  - If `var_id == rid && !is_lb`: cite only `a.ub` and `b.ub` (the
    `r_hi <= min(a_hi, b_hi)` rule).
  - If `var_id == rid && is_lb` and the propagator's lo-set-to-zero
    branch fired: cite only `a.lb >= 0`, `b.lb >= 0`.
  - If `var_id == rid && !is_lb` *and a or b was singleton at fire*:
    cite the singleton (both lb and ub of that var).
  - If `var_id == aid` (backward `a |= r`): cite `b == bval` (both
    lb and ub of b) and `r == rval` (both lb and ub of r). Don't
    cite a's own current bounds — they're not antecedents, they're
    consequents.

  This requires knowing *which* rule fired, which is the snapshot we
  capture per-fire. Concretely: extend the band/bor/bxor propagator
  to record a tiny rule-tag byte in its watch struct (`BoundsBAND_64_t`
  already exists at the bottom of `zsp_prop_templates.c:1571` —
  add `uint8_t last_rule` and the new_bound that was set). Explain
  reads the tag and selects the right antecedent set.

B3. **Other callbacks identified in phase A** get the same treatment:
read fire-time state out of the trail (default) or a small per-instance
snapshot field where simpler. Touch list:

  - `_explain_binary_bitwise` (bvand/bvor/bvxor)
  - whichever of `explain_all_different` / `explain_bounds_ne` the
    `unique16` trace fingers
  - any other callback that phase A's transcript catches

B4. **Verify each fix.** After every callback fix, rerun the affected
fixture(s) and check that the prior trace's unsound learnt clause is
now sound (or no longer emitted), and that `DV_USE_LCG=0` and
`DV_USE_LCG=1` both return the same `sat`.

**Exit criterion for phase B:** `pytest tests/formal/test_cross_check_tier1.py`
fully green (or new failures only reduce to honest `unknown` skips,
not disagreements). `pytest tests/formal/test_cross_check.py` still
green.

---

## C — Audit the remaining explain callbacks

The 26 callbacks in `zsp_explain.c` were written incrementally; only
the few exercised by tier2/tier3 have been stress-tested. Audit each
one against this checklist:

C1. **Reason validity.** Does the explanation say "if these literals
are all true *then* the bound change is implied"? Re-derive each
callback's clause from the corresponding fire function's
propagation rule.

C2. **Snapshot vs. current.** If the callback reads `var_lo64`/
`var_hi64`, prove the current bound is still a valid antecedent for
the recorded `new_bound`. If not, switch to trail-based read.

C3. **Rule discrimination.** If the propagator has multiple
propagation rules with different antecedents (bvand, bvmul,
ITE-value, sum_eq, all_different, …), the callback must select the
rule that fired — either by snapshot-tag (B2) or by reading the
trail.

C4. **int32 truncation.** Every `(int32_t)bound` cast is a latent
correctness bug if any production fixture pushes values outside
INT32. Mark each site; phase D will revisit.

Produce the audit as a table in `docs/cdcl_explain_audit.md`:
  | callback | rules | reads | fix needed? | notes |

**Exit criterion for phase C:** audit document landed; every "fix
needed: yes" row has a follow-on patch or a justification why it's
correct as-is.

---

## D — Lift the int32 bound restriction (forward-compatible)

Today `Literal.bound` is `int32_t`. Long-term, dv-solve will want to
carry full int64 bounds (and beyond, eventually). Goal: design a
path that supports both width regimes without breaking existing tier2/
tier3 timings or memory shapes.

D1. **Survey usage.** Find every `(int32_t)…` cast on a bound, every
read of `Literal.bound`, and every place `Literal` is sized
(`sizeof(Literal)`, packed into clause storage in
`clause_db`, etc.).

D2. **Pick a representation.** Two options to weigh:

  - *Widen unconditionally* to `int64_t`. Simplest; doubles bound
    storage in every Literal and every learnt clause. Clauses on
    tier2 are short; the memory hit is probably negligible.
  - *Tagged width:* `Literal.bound_kind` byte selects between an
    inline 32-bit value and an out-of-line 64-bit slot. Saves memory
    on the 32-bit common case at the cost of more code paths and a
    second allocation pool for wide bounds.

  Recommendation in this plan: start with **unconditional widen**
  to int64 — write a one-shot patch, measure cache miss rate on
  tier2/tier3 via the existing `compare.csv` and `compare_tier1.csv`
  workflow, and only escalate to a tagged scheme if the regression
  measures > 10%. Re-running `python tests/formal/run_compare.py`
  before and after the widen gives an apples-to-apples number.

D3. **Implementation.** Change `Literal.bound` to `int64_t`. Drop
every `(int32_t)` cast in `zsp_explain.c` and `zsp_lcg.c`. Re-run the
build (`cmake --build build`); fix any callers that assumed 32-bit
arithmetic. Rerun the full test suite + tier1 + tier2/3 cross-checks.

D4. **Reserve the tagged path** in `zsp_lcg.h` with a comment
referencing this plan, so a future change can switch representations
without breaking ABI for embedded callers.

**Exit criterion for phase D:** `Literal.bound` is int64, all
cross-checks pass, `run_compare.py` deltas vs. the pre-widen baseline
within noise.

---

## E — Regression coverage

E1. Keep `tests/formal/test_cross_check_tier1.py` in CI; add a
note in the file header pointing to this plan so anyone seeing a
new failure on tier1 knows where the story is.

E2. Add the 7-line minimal repro as a standalone SMT2 fixture
(`tests/formal/smt2/regression/bvand_range_mask.smt2`) plus a unit
test that asserts dv-solve returns `sat` with `DV_USE_LCG=1`. This
fails today, and explicitly guards against the bvand explain bug
coming back if someone refactors `_explain_binary_bitwise` later.

E3. Add `tests/formal/test_cdcl_explain_soundness.py` — a small
property-style test that for each definitive tier1 fixture asserts
the two-mode invariant: `DV_USE_LCG=0` and `DV_USE_LCG=1` agree.
Today this would fail on the 5 fixtures already known; after phase B
it should pass.

**Exit criterion for phase E:** three new tests in place, all
green after phases A–D.

---

## Order of execution (recommended)

1. **A1 + A2** (a few hours) — get the trace, see the actual unsound
   learnt clause. Stop and re-plan if (A2) reveals a different root
   cause than "stale/coarse explanation".
2. **B1 + B2** — fix `_explain_binary_bitwise` first, rerun tier1
   cross-check, expect 4 of 5 fixtures green.
3. **A4 + B3** — diagnose `unique16`, fix the relevant callback.
4. **D1 + D2 + D3** — int64 widen, rerun benchmarks for regression.
5. **C** — full audit (lower urgency, but tracks the
   "fix tier1 + audit all" scope answer).
6. **E** — wire regression tests so the bug is fenced even if the
   fix is later refactored.

Phases B and D are independent; phase D can land first if a hand-
audit shows int32 truncation is the actual root cause of one fixture
(it isn't for the minimal repro, but `unique16`'s width and ranges
haven't been traced yet).

## Risks / things to watch

- **CDCL correctness regressions on tier2/tier3.** The 84-fixture
  cross-check is currently 84/84 green; any explain refactor that
  drops a load-bearing (but technically over-approximate) literal
  could cause those to regress to `unknown` (less efficient analysis,
  more bailout). Rerun `pytest tests/formal/test_cross_check.py` after
  every B-phase change.
- **Trail-walk perf.** Reading antecedent bounds out of the trail
  costs O(trail length on var v). On heavy fixtures this could add
  measurable time. Mitigate by indexing trail entries by var (a
  per-var "most recent entry" pointer) only if profiling shows it
  matters.
- **Literal-widen ABI.** If `libdv_solve.so` is consumed by zuspec
  via the DPI shim, check whether `Literal` crosses the ABI boundary.
  If it does, version the symbol or stage the change.

## Artifacts produced

- `docs/cdcl_explain_audit.md` (phase C output)
- `tests/formal/smt2/regression/bvand_range_mask.smt2`
- `tests/formal/test_cdcl_explain_soundness.py`
- updated `tests/formal/test_cross_check_tier1.py` header
- (optional) trace transcripts captured during phase A,
  saved to `docs/cdcl_explain_traces/` for posterity.
