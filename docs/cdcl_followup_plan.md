# CDCL soundness & performance — follow-up plan

Status: 2026-05-23. Soundness is restored (0 disagreements across 118
cross-check fixtures, see `tests/formal/results/compare_tier1_v3.csv`).
Cost: 12 fixtures regressed from definitive to skip on the 10 s test
ceiling. This plan tracks what gets us back to definitive on those
plus the still-latent unsoundness around level-0 clause unit prop.

Each phase has explicit **exit criteria** so you can tell from the
outside whether it's done. Each phase that lands updates the status
line at the top of the section.

---

## Phase 1 — Rule-aware bitwise explain (B2 from the prior plan)

**Status:** landed (commit pending). Partial recovery: +1 fixture
(`fsm_onehot_bmc_d32`). The remaining 11 tier2/3 skips don't use
bvand/bor/bxor — they're ITE+equality chains under singleton
decisions, whose perf cost is structural (per-(var,is_lb) seen[]
prevents the unsound shortcut the original variable-dedup gave). See
phase 2 for the deeper lever and phase 6 (new) for the singleton-
decision encoding idea.
**Also folded in:** body-literal double-emission dedup fix in Step 3
(only consume cur_level slots; earlier-level antecedents were already
appended in ADD_EXPL_LIT). That alone removed ~6 redundant body
literals per analysis on the regressed fixtures and is what brought
back fsm_onehot_d32.
**Estimated size:** ~half-session.
**Risk:** low. Local refactor of one function (`_explain_binary_bitwise`),
plus per-op variants in `zsp_explain.c`.

### Problem

`_explain_binary_bitwise` (used by `bvand`, `bvor`, `bvxor`) cites
**all six watched-var bound literals** unconditionally. After the
per-(var, is_lb) `seen[]` fix, these are all recorded distinctly in
the learnt clause — clauses are now ~2× longer than necessary, VSIDS
signal is diluted, and search wanders. The fired propagation rule
typically only needs 1–2 of the six.

### Approach

Split the single helper into per-op explain functions
(`explain_bounds_band`, `explain_bounds_bor`, `explain_bounds_bxor`).
Each inspects `(var_id, is_lb, new_bound)` plus current watched
bounds and selects the rule that produced the tightening, citing
only the antecedents that rule needs. Concretely for `bvand`:

| direction | rule chosen | antecedents to cite |
|---|---|---|
| `r.ub` | `r ≤ min(a.ub, b.ub)` (and any singleton tightening is a subset of this) | `a.ub`, `b.ub` |
| `r.lb = 0` from non-negative inputs | rule 2 | `a.lb ≥ 0`, `b.lb ≥ 0` |
| `r.lb = alo & blo` (both singletons) | full singleton | `a.lb, a.ub, b.lb, b.ub` |
| `a.lb` (backward `a.lb |= r.val`) | rule 5 | `r.lb, r.ub, b.lb, b.ub` |

Soundness is preserved using current-bound semantics: if the
explanation rule held at fire time with current-or-looser bounds, it
still holds with current (tighter) bounds. Verified per-rule in the
plan body of the prior doc (`docs/cdcl_explain_soundness_plan.md`).

### Exit criteria

- [x] `bvand`, `bvor`, `bvxor` explain are split into per-op functions
      in `zsp_explain.c`, each citing 2 or 4 literals (not 6).
- [x] `tests/formal/test_cross_check_tier1.py` and
      `tests/formal/test_cross_check.py` both pass with 0 failures
      (118 total: 104 passed, 14 skipped — was 103/15).
- [ ] ~~At least 4 of the 12 currently-skipped tier2/tier3 fixtures
      return to definitive~~ — only 1 recovered (`fsm_onehot_bmc_d32`).
      Bitwise explain wasn't the bottleneck on the others; see Phase 6.

---

## Phase 2 — Trail-reason for clause unit propagations

**Status:** not started.
**Estimated size:** ~one session.
**Risk:** medium. Touches `TrailEntry` layout, `clause_propagate`,
and `lcg_analyze_conflict`.

### Problem

When a learnt clause unit-propagates at level 0 (or any level), it
calls `ctx_tighten_lb64/ub64` which writes a `TrailEntry` with
`prop_ref = ctx->current_prop_ref` — and `current_prop_ref` is
`EXPR_NULL` outside propagator fire. So the trail entry is
**indistinguishable from a decision**.

This is latent unsoundness: in a follow-on conflict, the analyzer
sees the clause-prop bound, treats it as a decision (no `explain` to
resolve through), and emits a 1-literal learnt clause that forces a
specific bound at level 0 — over-strong relative to what the prior
learnt clauses actually entail. Minimal repro at `/tmp/bor.smt2`
returns the wrong answer in isolation, but the 118-fixture cross-check
happens to dodge it.

### Approach

Two options weighed:

A. **Carry a clause index in the trail entry.** Add a discriminator:
   `prop_ref == EXPR_NULL` either means "decision" or "clause unit
   prop, see clause_idx". Repurpose the existing `_te_pad` field for
   clause_idx, or steal one bit of `kind` for the discriminator.
B. **Side table indexed by trail-entry pointer.** A small hash from
   `TrailEntry*` to clause id, populated by `clause_propagate` before
   it tightens. Slower per-lookup but zero ABI change.

Recommendation: **option A**. Trail entries are 32 bytes today with a
`_te_pad` slot already there; no growth, faster than a hash, and the
discriminator is one bit in `kind` (we currently use `TRAIL_LB`,
`TRAIL_UB`, `TRAIL_HOLE` — room for a flag bit).

`clause_propagate` becomes the only caller that sets the new flag.
`lcg_analyze_conflict` checks `kind & CLAUSE_REASON` and, if set,
walks the clause's other literals to derive antecedents (each "other
literal" of the clause is currently false; its negation is implied,
so the negations of all other literals form the antecedent set for
the unit literal).

### Exit criteria

- [ ] `TrailEntry.kind` carries a `TRAIL_FROM_CLAUSE` flag bit; new
      field `TrailEntry.clause_idx` (or repurposed `_te_pad`).
- [ ] `clause_propagate` sets the flag and clause_idx on the trail
      entry it writes.
- [ ] `lcg_analyze_conflict` resolves through clause-reason entries
      the same way it resolves through propagator-reason entries.
- [ ] `/tmp/bor.smt2` returns `sat` in isolation (currently `unsat`
      from this analyzer corner).
- [ ] Cross-check stays 0 disagreements; skips reduce or hold steady.

---

## Phase 3 — Audit remaining explain callbacks (phase C of prior plan)

**Status:** not started.
**Estimated size:** ~half-session.
**Risk:** low. Read-only audit + targeted patches.

### Problem

The 26 explain callbacks in `zsp_explain.c` were written
incrementally; only those exercised by tier2/3 have been
stress-tested. Now that phase 1 has touched the most-used ones, walk
the rest against:

1. Reason validity — does the cited conjunction imply the tightening?
2. Snapshot vs. current — is "current bound" always a sound antecedent?
3. Rule discrimination — does the callback over-cite for multi-rule
   propagators (`bvmul`, ITE, `sum_eq`, `all_different`)?
4. int32 truncation — flag every `(int32_t)bound` cast.

### Approach

Walk `zsp_explain.c` top to bottom. For each callback, write a one-
line entry in `docs/cdcl_explain_audit.md` with verdict and notes.
Patch the ones flagged.

### Exit criteria

- [ ] `docs/cdcl_explain_audit.md` exists with one row per callback.
- [ ] Every row marked `fix needed: yes` has either a follow-up
      patch landed or a justification why it's actually fine.

---

## Phase 4 — Widen `Literal.bound` toward int64 (phase D of prior plan)

**Status:** not started.
**Estimated size:** ~one session.
**Risk:** medium. Touches `zsp_lcg.h`, every explain.c callback,
clause storage. Possible ABI implication for the DPI shim.

User asked to design for both 32- and 64-bit bounds flexibly. Two
shapes:

A. Unconditional widen `Literal.bound` to `int64_t` (doubles clause
   storage; simplest; one-shot patch).
B. Tagged width — keep an inline `int32_t` with a 1-bit overflow
   flag; rare wide bounds spill to a side table.

Plan: try A first, measure tier2/tier3 timing delta against
`compare_tier23_postfix.csv`. If regression > 10%, prototype B in a
follow-up.

### Exit criteria

- [ ] `Literal.bound` carries an `int64_t` value (either inline or
      via tagged scheme).
- [ ] All `(int32_t)bound` casts in `zsp_lcg.c` and `zsp_explain.c`
      are removed.
- [ ] Cross-check unchanged at 0 disagreements; timing delta
      documented in the plan footer.

---

## Phase 5 — Lock in regression coverage (phase E of prior plan)

**Status:** partial (`test_cross_check_tier1.py` is in).
**Estimated size:** ~quarter session.
**Risk:** low.

### Exit criteria

- [ ] `tests/formal/smt2/regression/bvand_range_mask.smt2` — the
      7-line minimal repro from the phase A trace.
- [ ] `tests/formal/smt2/regression/bvor_max_lb.smt2` — short
      witness for the `bvor` propagator fix.
- [ ] `tests/formal/test_cdcl_explain_soundness.py` — for each
      definitive tier1 fixture, assert `DV_USE_LCG=0` and
      `DV_USE_LCG=1` agree.

---

## Order of execution

1. ~~**Phase 1** (B2)~~ — landed. Only 1/11 perf-regressed fixtures
   recovered; the others don't use bitwise ops.
2. **Phase 6** (singleton decision encoding) — promoted because phase 1
   showed the dominant perf cost is duplicate trail entries for
   singleton decisions, not bitwise-explain over-citation. Do next.
3. **Phase 2** (trail-reason) — fixes the latent unsoundness uncovered
   by the bvor repro. After phase 6.
4. **Phase 3** (audit) — natural next step once the big patches
   are in.
5. **Phase 5** (regression tests) — fold in once phases 1–6 settle.
6. **Phase 4** (int64 widen) — independent; can land any time, but
   easier to measure deltas after timing stabilizes.

## Phase 6 (NEW) — Singleton decision as a single trail event

**Status:** identified during phase 1 analysis. Not started.
**Estimated size:** ~one session.
**Risk:** medium-high. Touches trail layout + propagator queue + analyzer.

### Problem

A search step that pins `v = c` calls `ctx_tighten_lb64(v, c)` then
`ctx_tighten_ub64(v, c)`, producing **two trail entries** at the same
decision level. Conflict analysis treats them as two independent
events: each one explained, resolved, and contributing antecedents
into the working set. For singleton-heavy fixtures (regfile_simple,
cache_direct, anything with ITE chains over decided values) this
doubles resolution-loop depth and the resulting clauses.

The original (unsound) variable-dedup hid this expense — it dropped
the second event silently. The per-(var, is_lb) fix exposes it.

### Approach

A new `TRAIL_SINGLETON` kind on `TrailEntry` (or a "decision marker"
bit) so a pinning decision creates **one** trail entry with `old_lb`
and `old_hi` both stored. `ctx_tighten_singleton(v, c)` helper used
by the search's decision-push path. Conflict analysis recognises
this kind and processes both bounds as one logical antecedent.

For propagator-driven singletons (e.g., bvand exact when both
operands singleton) the optimization is OPTIONAL — propagators can
keep tightening lb then ub separately. The analyzer can later add
"if the trail has back-to-back LB and UB entries on the same var
with the same value and same prop_ref, collapse to one event" as a
cheap coalescing step in ADD_EXPL_LIT.

### Exit criteria

- [ ] `TrailEntry` carries a `TRAIL_SINGLETON` kind (or flag) recording
      both bound changes from a singleton pin.
- [ ] Search decision-push uses the singleton helper.
- [ ] `lcg_analyze_conflict` treats singleton entries as one logical
      antecedent (one slot in seen, one increment of n_at_cur_level,
      learnt clause still emits both bound negations).
- [ ] Aim: most of the 11 remaining tier2/3 skips return to definitive.

## Cross-cutting tracking

After each phase, re-run:
```
python tests/formal/run_compare.py --tier 1 --tier 2 --tier 3 \
  --timeout 30 --out tests/formal/results/compare_phaseN.csv
```
and update the headline numbers below.

| Phase | Disagreements | Definitive | Skip/timeout | Notes |
|------:|--------------:|-----------:|-------------:|-------|
| baseline (pre-fix) | 5 | 109 | 4 | tier1 false-UNSATs |
| after first fix    | 1 | 109 | 8 | memmaptight32 still wrong |
| after multi-UIP + bvor | 0 | 103 | 15 | current commit `605bd61` |
| after phase 1 | 0 | 104 | 14 | only fsm_onehot_d32 recovered |
| after phase 2 | TBD | TBD | TBD | aim: 0 latent unsoundness |
| after phase 3 | TBD | TBD | TBD | audit complete |
| after phase 4 | TBD | TBD | TBD | int64 lifted |
| after phase 5 | TBD | TBD | TBD | regression tests in CI |
