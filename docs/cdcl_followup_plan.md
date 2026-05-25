# CDCL soundness & performance — follow-up plan

Status: 2026-05-24. Soundness holds (0 disagreements across 118
cross-check fixtures). Cross-check now **108/10/0** after Phase 9's
yosys-sby integration shook out 14 incremental-mode bugs; three
tier1 "honest unknown" fixtures recovered as a side effect of the
`solver_restore` queue-clear fix. Open work is Phase 10 (sby
end-to-end edges: d≥30 ASAN SEGV, cv14 cover false-UNSAT,
wider_counter extend gap) plus the latent Phase 2 trail-reason
unsoundness still on the books.

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

**Status:** landed 2026-05-24. Soundness fix lands the documented
unsoundness in `/tmp/bor.smt2` (returns `sat` now; was `unsat`).
Perf cost on cross-check: 108/10 → 98/20 (10 fixtures flipped
definitive → skip; no disagreements). Regression fixture:
`tests/formal/regression_clause_reason.smt2`.
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

- [x] `TrailEntry.flags` carries a `TRAIL_FLAG_FROM_CLAUSE` bit;
      `prop_ref` is repurposed as clause_idx when the flag is set.
- [x] `clause_propagate` (full-scan path) and `_check_clause`
      (event-driven path) both stamp the flag + clause_idx via
      `ctx->current_prop_ref` / `current_trail_flags` before
      `ctx_tighten_lb/ub64`.
- [x] `lcg_analyze_conflict` handles `TRAIL_FLAG_FROM_CLAUSE` in
      three places: the main resolution loop, the empty-domain
      "other_entry" branch, and the empty-domain "cur_entry"
      branch. In each case it walks the clause's literals (other
      than the unit literal) and adds their negations as
      antecedents via `ADD_EXPL_LIT(literal_negate(...))`.
- [x] `tests/formal/regression_clause_reason.smt2` (the bvor case)
      returns `sat`.
- [x] Cross-check stays 0 disagreements (98/20 — 10 fixtures
      regressed to skip due to longer learnt clauses; perf
      recovery is follow-up work).

---

## Phase 3 — Audit remaining explain callbacks (phase C of prior plan)

**Status:** landed 2026-05-24. Audit doc:
`docs/cdcl_explain_audit.md`. 33 callbacks documented (28 in
`zsp_explain.c` + 5 `_64` variants in `zsp_prop_templates.c`). No
soundness fix-needed verdicts; the latent bug from `/tmp/bor.smt2`
was about the **analyzer** (Phase 2), not the explain callbacks.
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

- [x] `docs/cdcl_explain_audit.md` exists with one row per callback
      (33 rows).
- [x] No rows marked `fix needed`. Documented two categories of
      follow-up work: int32 bound truncation across all callbacks
      (Phase 4 deliverable) and rule-aware splits for mul/div/mod
      and the shared `_explain_binary_bitwise` helper (half-session
      each, separate from Phase 3 itself).

---

## Phase 4 — Widen `Literal.bound` toward int64 (phase D of prior plan)

**Status:** landed 2026-05-24 (option A). `Literal.bound` is now
`int64_t`; struct grew from 12 → 16 bytes (one extra word).
Cross-check unchanged at 98/20/0; wall time within noise (223s vs
223s on baseline). No regressions; sby BMC d100 and cover both PASS.
Truncation across all 33 explain callbacks removed — the bounds
flagged in the Phase 3 audit no longer collapse to 32-bit.
**Estimated size:** ~one session.
**Risk:** medium. Touches `zsp_lcg.h`, every explain.c callback,
clause storage. Possible ABI implication for the DPI shim.

User asked to design for both 32- and 64-bit bounds flexibly. Two
shapes:

A. Unconditional widen `Literal.bound` to `int64_t` (doubles clause
   storage; simplest; one-shot patch).
B. Tagged width — keep an inline `int32_t` with a 1-bit overflow
   flag; rare wide bounds spill to a side table.

Picked A. Measured no perf regression on cross-check, so the tagged
variant (B) isn't needed.

### Exit criteria

- [x] `Literal.bound` is `int64_t` (struct size 12 → 16 bytes).
- [x] All `(int32_t)bound` casts in `zsp_lcg.c`,
      `zsp_explain.c`, and `zsp_prop_templates.c`'s explain
      callbacks are removed. Trace `printf` widened to `%lld`.
- [x] Cross-check unchanged at 98 passed / 20 skipped / 0
      disagreements. Wall time within noise of the Phase 2
      baseline.

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

1. ~~**Phase 1** (B2)~~ — landed.
2. ~~**Phase 6** (singleton coalescing)~~ — landed (+1 fixture).
3. ~~**Phase 7** (LBD + GC)~~ — infra landed, no fixture moved.
4. ~~**Phase 8** (phase saving opt-in)~~ — landed, wash on default.
5. ~~**Phase 9** (yosys-sby backend)~~ — landed 2026-05-24; 14 bugs
   fixed; cross-check 105/13 → 108/10.
6. **Phase 10** (sby end-to-end edges) — next. d≥30 ASAN SEGV is
   the most likely to expose another stale-pool-offset class bug
   worth fixing for general soundness, not just sby.
7. **Phase 2** (trail-reason) — latent unsoundness uncovered by
   the bvor repro. Defer until Phase 10 settles.
8. **Phase 3** (audit) — natural next step once big patches are in.
9. **Phase 5** (regression tests) — fold in once Phase 10 settles.
10. **Phase 4** (int64 widen) — independent; land any time.

## Phase 6 — Singleton trail-entry coalescing

**Status:** landed. Net +1 fixture recovered (`regfile_simple_bmc_d2`).
Wall time on the full cross-check dropped 112s → 102s.
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

### What was actually built (slightly different shape from the original)

- `TrailEntry._te_pad` repurposed as `flags`; `TRAIL_FLAG_SINGLETON`
  defined in `zsp_trail.h`.
- **Auto-detection** at tighten time: `_mark_singleton_pair` in
  `zsp_propagate.c` watches every `ctx_tighten_lb/ub` and, when the
  variable becomes singleton (`lo == hi`), scans back to find the
  companion bound entry at the same level from the same propagator
  (or same decision) and flags both. Covers decisions, eq, ITE,
  bvand exact, and any future propagator that pins by tightening
  both bounds — no per-propagator code changes needed.
- **Resolution coalescing** in `lcg_analyze_conflict`: when a
  resolution step processes a singleton-flagged entry, it also
  immediately calls the propagator's `explain` for the companion
  bound direction, clears the companion `seen[]`, decrements
  `n_at_cur_level` once more, and skips the companion's own trail
  walk. Halves the resolution-loop iterations for singleton-heavy
  propagation chains (visible in DV_LCG_TRACE as `resolve+` lines).

### Exit criteria

- [x] `TrailEntry` carries `TRAIL_FLAG_SINGLETON` bit (repurposed _te_pad).
- [x] Tighten helpers auto-detect and mark singleton pairs.
- [x] `lcg_analyze_conflict` consumes both halves in one resolve step.
- [ ] Aim: most of the 10 remaining tier2/3 skips return to definitive.
      Only +1 (regfile_simple_d2) on this commit; the others have
      conflict counts so high that even halved resolution-loop work
      isn't enough. Clause minimization / better search heuristics
      are the next levers (new Phase 7).

## Phase 7b — Self-subsumption minimization retry (2026-05-24)

**Status:** opt-in via `DV_LCG_MIN=1`; default off.

After Phase 2 made learnt clauses longer, we retried cheap MiniSAT-
style self-subsumption to recover the 10 fixtures that flipped to
skip. Outcome: sound but doesn't recover anything (96/22 with min on
vs 98/20 with it off) and slow (230s vs 102s wall-time) — the trail
walks per body literal per antecedent are the bottleneck.

Implementation notes (in `lcg_analyze_conflict` after step 3):
- For each body literal L_i, find the trail entry that justified its
  negation (ant = ~L_i is the antecedent currently true).
- Get te's antecedent set: clause-walk for `TRAIL_FLAG_FROM_CLAUSE`
  entries, propagator `explain()` for prop-reason entries.
- For each antecedent `a`, check whether it's implied by `~L_k` for
  some other body literal `L_k`, OR is at decision level 0
  (top-level fact). Implication uses bound comparison:
  - body L = "v <= u" → ~L = "v >= u+1" implies a = "v >= b" iff u >= b-1.
  - body L = "v >= u" → ~L = "v <= u-1" implies a = "v <= b" iff u <= b+1.
- The `_li == read` skip in `IMPLIED_BY_LEARNT` is critical:
  without it, `L_i` can be used to subsume its own antecedent, which
  silently produces unsound learnt clauses (regression_clause_reason
  flipped sat → unsat in initial testing before the skip was added).

Why it doesn't help: the 10 fixtures Phase 2 lost aren't gated on
clause-length per conflict — they hit DV_MAX_RESTARTS because the
search space is huge and propagation is too weak to converge.
Shortening clauses by 1-2 literals doesn't move the needle.

Follow-up directions:
- O(1) "is literal in learnt clause" data structure (per-var slot
  map indexed by (var, is_lb), like the existing seen[] but kept
  across the analyzer call). Would cut min cost from O(n²) → O(n).
- Recursive (expensive) minimization that walks reasons two levels
  deep. Subsumes more but costlier per attempt.
- Decision heuristic improvements (back-jumping further or
  restart-on-VSIDS-stagnation) — likely a bigger win than clause
  shape work on these fixtures (see Phase 8 notes).

## Phase 7 — Clause minimization, real LBD, GC

**Status:** partial. LBD computation + clause-GC infrastructure
landed. Cheap self-subsumption minimization tried and reverted (net
−1 fixture). No fixture recovered from this phase, but the
groundwork is in place for clause-quality-aware GC later.
**Estimated size:** ~one to two sessions.
**Risk:** medium.

### Problem

After phases 1+6, the 10 remaining tier2/3 skips have learnt-clause
counts in the thousands per fixture (`regfile_simple_bmc_d1` hits
the DV_MAX_RESTARTS=1 ceiling of 10000 conflicts well under 1s).
Clauses average 8–10 literals after propagation chains through ITE
+ equality. The clauses are *correct* (negations of valid
conflict cores) but don't propagate well — each one is a long
disjunction over condition variables that doesn't force a single
direction.

The original (pre-fix) variable-dedup produced shorter, more
restrictive clauses by silently dropping antecedents. That was
unsound but heuristically effective. We can't go back to that.
Standard CDCL techniques to recover:

1. **Self-subsumption / clause minimization.** For each learnt
   clause literal, check whether its negation is implied by a
   subset of the other clause literals' negations via an existing
   clause. If so, drop it. MiniSAT-style "expensive" + "cheap"
   minimization passes.
2. **Better restart policy.** Luby restarts + phase saving.
3. **Decision heuristics.** VSIDS already in; tune decay or add
   activity bumping on clause-prop conflicts.

### Approach

Start with cheap minimization in `lcg_analyze_conflict` after Step 3:
for each body literal `l`, walk its reason clause and check whether
all other reasons are already in the learnt clause. If yes, the
literal is redundant.

### What was actually built / what was learned

- `lcg_analyze_conflict` now returns LBD (distinct decision levels
  among the clause's literals, computed via opposite-kind trail
  lookups). LBD is recorded in `Clause.lbd` instead of the prior
  placeholder `lbd = n_lits`.
- Search restart loop calls `clause_db_gc(threshold=4)` once the
  DB exceeds 2048 clauses. GC marks clauses with LBD > 4 as NULL
  (watch lists tolerate NULL via `_check_clause` early-out).
- Cheap self-subsumption minimization (drop a body literal whose
  reason's antecedents are all already covered by other clause
  literals): implemented and tried. Result: **net −1 fixture**
  (`regfile_simple_d2` flipped to skip, no new wins). Reverted.
  Reason in the post-hoc analysis: minimization removes literals
  that were participating in unit-prop chains, weakening clauses
  for search even though the resulting clause is still sound.

**Empirical finding:** the 10 remaining tier2/3 skips produce
clauses with LBD = 2 (glue-quality). GC threshold = 4 leaves them
untouched. Conflict count is the bottleneck — the search makes
thousands of conflicts of essentially the same shape, learning
high-quality but redundant clauses. Standard CDCL self-improvement
doesn't kick in because each conflict already produces a glue
clause; the problem is search heuristic, not clause quality.

### Exit criteria

- [x] Real LBD landed; clause GC infra wired at restart.
- [ ] Most of the 10 remaining skips return to definitive — **not**
      achieved by Phase 7. Next leverage point is decision/restart
      heuristics (see Phase 8 below).

## Phase 8 — Phase saving as opt-in

**Status:** **default-on as of 2026-05-25.** Net +1 fixture on
cross-check after Phase 2 changed clause shapes (98/20 → 99/19):
mempartitionknapsack (tier1, sat) and fsm_onehot_d4/d16 (tier2,
unsat) recover; regfile_addr_alias d1/d4 (tier3) regress. Wall
time also drops 222s → 192s. The tier1 + tier2 recovery is the
more valuable side of the trade; opt out via
`DV_USE_PHASE_SAVE=0` if a specific run regresses.

Phase 8 (original, pre-Phase-2): exposed via `DV_USE_PHASE_SAVE=1`;
empirically a wash on the corpus (5 recover, 5 regress). After
Phase 2 changed clause shapes the balance shifted in favor of
phase-save, so flipped to default-on.

### Problem

Per Phase 7 traces, regfile_simple/cache_direct_1way fixtures
produce thousands of glue-quality (LBD=2) clauses, each forbidding
a slightly different combination of ITE-condition values + a target
value. Search keeps hitting the same conflict pattern because:

1. VSIDS keeps bumping the same conflict-set variables.
2. Decision variables fall in the same range every time.
3. Phase-saving picks the same direction.

Standard CDCL infrastructure (clauses, restarts, GC) is in place
and operating correctly. The bottleneck is search wandering, not
clause management.

### Approach (sketches)

- **Random-walk fallback** every N restarts: ignore VSIDS for K
  decisions to break out of repetitive conflict loops.
- **Polarity flipping at restart**: invert phase_save once every M
  restarts so the search tries the opposite direction.
- **Conflict-set tabu**: temporarily down-weight vars that
  appeared in the last L conflicts.

### Exit criteria

- [x] Phase-saving exposed (default-on after Phase 2; opt-out
      via `DV_USE_PHASE_SAVE=0`).
- [~] At least 5 of the 10 currently-skipped fixtures recover —
      partial: +1 net after Phase 2 re-balance (mempartitionknapsack
      tier1 + fsm_onehot d4/d16 tier2; regfile_addr_alias d1/d4
      tier3 regress).

### What was learned

- Phase saving alone isn't enough; the conflict patterns are
  structural to the problem shape, not search-trajectory noise.
- "Diversification at restart" (resetting phase_save to mid-domain
  every 4 restarts) was also tried — inert on the default config
  since phase_save is off by default. Reverted.
- The 10 remaining skips need something deeper than rearranging
  the order of decisions — possibly: improved propagator
  completeness for ITE chains, or lazy reification of branch
  values to reduce conflict set width.

## Phase 9 (NEW) — yosys-sby end-to-end backend

**Status:** landed. dv-solve registered in
`packages/yosys-bin/share/yosys/python3/smtio.py` as the `dv-solve`
solver. SBY engine line `smtbmc dv-solve` works end-to-end.
counter_assert verifies through BMC depths 5–25 (depth ≥ 30 hits a
separate ASAN SEGV — see Phase 10).

**Bugs found and fixed via this work (all on 2026-05-24):**
1. `_fire_bounds_bnot_64` bit-width mask (`c2e34a0`).
2. SMT2 push/pop aux SolveProblem cleanup (`c2e34a0`).
3. `_bool_to_var` learned EXTRACT shapes — fixes the
   OR-of-extract-equalities blocker that was open at session start
   (`6a5c61b`).
4. LIFO var-init two-pass in `solver_add_constraint` (`6a5c61b`).
5. `_fresh_aux` / `_next_var_id` sync against backend aux allocations
   (`6a5c61b`).
6. `solver_restore` NULLs post-cp `prop_refs` so `solver_reset`
   doesn't re-activate stale inside-push propagators (`6a5c61b`).
7. `solver_restore` rolls back learnt-clause count via new
   `CheckpointMark.n_clauses_at_cp` (`6a5c61b`).
8. `(concat const var)` materialises both sides via `_value_to_var`
   (`89e8df4`).
9. SMT2 frontend diverts `fe->err` to `$DV_LOG` (default
   `/tmp/dv-solve.log`) when stdout is a pipe — yosys-smtbmc runs
   the solver with `stderr=STDOUT` (`89e8df4`).
10. `_add_var` grow loop tolerates multi-slot id jumps from
    `_next_var_id` (`89e8df4`).
11. `SMT2_MAX_FUNS` 64 → 8192 (`89e8df4`).
12. `incremental_capacity_hint` (8192) on the SMT2 frontend so
    `solver_add_constraint` doesn't fail when post-init aux growth
    blows the `VAR_SLACK_FACTOR=32` × `n_initial` capacity (`2b40758`).
13. `r = extend(a)` materialises `a` via `_value_to_var` (`2b40758`).
14. `solver_restore` clears + re-primes the propagator queue so
    stale `queue_next` chains from inside-push propagators can't be
    dequeued post-pop. Side wins: `memmaptight32`, `arrayordering`,
    `muldivscenario` flipped skip → pass; cross-check 105/13/0 →
    108/10/0 (`3e91fbb`, dedup in `e66cbe0`).

### Exit criteria

- [x] OR-of-extract case lands `sat / sat` in dv-solve.
- [x] `sby -f counter_assert_dv.sby bmc` returns PASS with
      `engines: smtbmc dv-solve` (depths 5–25).
- [x] Benchmark report captured:
      `tests/formal/results/sby_e2e_2026-05-24.md`.

## Phase 10 (NEW) — sby end-to-end edges

**Status:** open. Three deterministic edges surfaced once Phase 9
went end-to-end. Documented in `docs/session_status_2026-05-24.md`
section "Remaining edges".

### Open items

1. ~~**counter_assert d ≥ 30 ASAN SEGV**~~ **fixed 2026-05-24.**
   Root cause: `incremental_capacity_hint` sized the var array but
   NOT the `prop_refs` side table. `solver_restore` iterates
   `prop_refs[0..n_props_at_cp)`; once `n_props_at_cp` exceeded
   `n_prop_refs_capacity` (352 vs 727 on the d30 trace), the loop
   read past the end of `prop_refs` into adjacent
   `prop_guard_vars`/`prop_constraint_id` memory. Those slots
   legitimately hold 0 (var id 0, or unset constraint id), which
   slipped past the `!= EXPR_NULL` check; the bogus `prop_ref=0`
   then made `p = pool_base+0` (the pool header), so the fire
   dispatch read a garbage function pointer and SEGV'd. Fix in
   `zsp_compile.c`: apply `incremental_capacity_hint` to `pr_cap`
   (covers `prop_refs`, `prop_guard_vars`, `prop_constraint_id` since
   they all share the same capacity). Defense-in-depth in
   `zsp_checkpoint.c`: `solver_restore` clamps its loop bound to
   `n_prop_refs_capacity`. Regression fixture:
   `tests/formal/regression_prop_refs_capacity.smt2` (captured d30
   trace). Verified: `sby -f counter_assert_d100.sby bmc` PASS.

2. ~~**Cover-mode false-UNSAT**~~ **fixed 2026-05-24.** Two
   interacting bugs in the push/pop state-restore path:

   (a) `solver_solve` at `zsp_search.c:341` overwrites
   `level_marks[0]` to seal "level-0 baseline" for its restarts
   and `bounds_shave` probing. Inside a push scope, this leaves
   `level_marks[m->decision_level]` pointing at a *post-push* trail
   state — so `solver_restore`'s `trail_backtrack` stops there
   instead of walking back to `m->trail_top`. Pre-push trail
   entries (compile-time aux tightenings from inside the push)
   survive the pop, leaving the solver with phantom bounds.

   (b) `trail_backtrack` walks all watcher chains to clear
   `PROP_FLAG_ENTAILED` so post-backtrack propagation can re-fire
   the cleared props. But `solver_restore` only NULLs the
   `prop_refs[]` slots of post-cp propagators; they stay linked
   into watcher chains. After (a) and a subsequent `(declare-fun)`
   that reuses a rolled-back var id, the stale post-cp prop's
   ENTAILED bit gets cleared by the next conflict-driven
   `trail_backtrack`, then fires against the *new* variable that
   now occupies the reused id — propagating wrong bounds and
   producing spurious unsat.

   Fixes in this commit:
   - `solver_restore` restores
     `level_marks[m->decision_level].trail_top`/`.trail_count` from
     the saved `m->trail_top`/`m->trail_count` before
     `trail_backtrack`, so the backtrack stops at the right point.
   - `trail_backtrack`'s watcher-chain ENTAILED-clear pass skips
     dead props (identified by `prop_refs[p->prop_id] != ref`).
     Live props still get cleared; stale post-cp props stay
     entailed across subsequent backtracks.

   Regression fixture:
   `tests/formal/regression_push_unsat_state.smt2`.
   (Cover-mode sby still fails separately on a `(get-value ...)`
   parsing path — that's a different gap in dv-solve's model-output
   support, not related to this bug. Tracked in item 4 below.)

3. **Wider counter (8-bit + sub)** — **diagnosed 2026-05-24,
   fix deferred.** Root cause is not extend/extract shapes but a
   long-standing soundness gap in `_fire_bounds_add_32` /
   `_fire_bounds_add_64`: they do plain integer arithmetic, not
   modular BV. When operand sum overflows the BV width
   (`a=0xFF + b=0x02` on 8-bit), they conclude `r ∈ [0x101, 0x101]`
   which is outside the var's `[0, 0xFF]` domain — spurious UNSAT
   in some shapes, missed CEX in others. Minimal repros captured:
   - `r = bvsub(0, 1)` on 8-bit → returns UNSAT (correct: r=0xFF).
   - `r = bvadd(0xFF, 0x02)` on 8-bit → returns UNSAT (correct: r=1).

   Tried fixes:
   - Wrap-aware "skip tightening when overflow possible" in both
     `bounds_add_32` and `bounds_add_64`. Fixed the minimal
     repros but dropped 4 fixtures from definitive→skip on
     cross-check (108/10 → 104/14). Reverted.
   - Routing BIN_SUB var-const through the wrap-aware
     `bvadd_const_64`. Fixed the bvsub repros but dropped 3 FIFO
     fixtures (108/10 → 105/13) — `bvadd_const_64`'s perf
     characteristics differ from the bounds_add path enough to
     hurt larger BV problems. Reverted.
   - **Attempt 3 (2026-05-25):** per-direction wrap-aware skip
     **plus** singleton-modular exact tightening (computes
     `(a+b) mod 2^w` when both operands are singletons, and
     symmetric backward cases). Fixed the minimal repros
     (wc_min/wc_min2/wc_min4 all sat). But: (a) cross-check
     regressed 98/20 → 93/25 with the singleton case adding
     little vs. its overhead — the per-direction skip is what
     hurts when vars are full-domain; and (b) wider_counter sby
     was **still** false-PASS — the modular fix alone isn't
     enough to find CEXs through the ITE+extract chains yosys
     emits. Reverted; need both a wrap-aware bvadd/bvsub AND
     ITE/extract paths that don't lose modular info downstream.

   Proper fix needs a real modular bvadd/bvsub propagator that
   tightens correctly in both wrap and no-wrap cases (not just
   skipping). That's a substantial design + impl effort, deferred
   beyond Phase 10.

   **End-to-end note:** even with the bvadd/bvsub fix, the
   wider_counter sby case stays false-PASS. Direct probe:
   ```
   (assert (= count1 (bvsub #x00 #x01)))
   (check-sat)  ;; → "unknown" (validator catches plain-int = -1,
                ;;    downgrades sat → unknown; no CEX surfaces)
   ```
   The fix needs to thread modular semantics through ITE selection
   and extract chains too, not just isolated bvadd/bvsub nodes.
   The wider-counter SystemVerilog wraps the bvsub in an ITE
   (`dir ? count-1 : count+1`), and the ITE's bvsub branch loses
   modular semantics on its way to the assertion. So this is at
   minimum a two-step fix: modular bvadd/bvsub + modular ITE/extract
   composition, or a separate bit-blasting fallback for these
   shapes.

   Existing escape hatch: BIN_ADD var-const already routes through
   the wrap-aware `bvadd_const_64`. Same routing for var-var add
   and any sub variant is the natural next step but needs perf
   parity with bounds_add first.

4. ~~**Cover-mode `(get-value)`**~~ **fixed 2026-05-24.**
   yosys-smtbmc cover loop queries cover-firing conditions via
   `(get-value (|UNROLL#N|))` where `UNROLL#N` is a `define-fun`
   macro, not a declared variable. The old handler only matched
   declared variables (and arrays) and silently emitted `()` for
   unknown names, which crashed smtio's response parser. Fix:
   `_cmd_get_value` falls back to `_eval_sexpr` — a small recursive
   evaluator that expands `define-fun` bodies, looks up declared
   vars via `solver_get_value`, and computes BV/Bool ops (`and`,
   `or`, `not`, `=`, `distinct`, `ite`, `bvnot`/`bvand`/`bvor`/
   `bvxor`/`bvadd`/`bvsub`/`bvmul`, `bvult`/`bvule`/`bvugt`/`bvuge`,
   `extract`, `concat`). For unsupported shapes the handler now
   emits a well-formed `(NAME (_ bv0 1))` placeholder + stderr
   warning instead of an empty list. Verified:
   `sby -f counter_cover.sby cover` (depth 10) PASS — cover
   statement reaches at step 1, trace written. Regression fixture:
   `tests/formal/regression_get_value_definefun.smt2`.

### Exit criteria

- [x] d ≥ 30 ASAN SEGV root-caused and fixed; counter_assert
      verifies through depth ≥ 100 (`sby -f counter_assert_d100.sby
      bmc` PASS).
- [x] cv14 push/pop false-UNSAT fixed (root-cause was solver_solve
      + trail_backtrack ENTAILED-clearing interaction with
      solver_restore, not the AND-compile path). Regression
      fixture added: `tests/formal/regression_push_unsat_state.smt2`.
- [ ] wider_counter passes both safety and cover at step 1 — **not
      achieved.** Root cause diagnosed (modular bvadd/bvsub propagator
      missing wrap semantics); two attempted fixes regressed cross-check;
      proper fix deferred. See item 3 above.
- [x] Cover-mode `(get-value)` for define-fun expressions — sexpr
      evaluator added; `sby -f counter_cover.sby cover` PASS,
      reaches cover statement at step 1.

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
| after phase 6 | 0 | 105 | 13 | +regfile_simple_d2; wall 112s→102s |
| after phase 7 | 0 | 105 | 13 | LBD+GC infra in; no fixture moved |
| after phase 8 | 0 | 105 | 13 | phase_save opt-in; wash on default |
| after phase 9 (initial) | 0 | 105 | 13 | +2 bugs fixed (bvnot, pop aux); sby blocked on OR-of-extract |
| after phase 9 (2026-05-24) | 0 | 108 | 10 | 14 bugs fixed end-to-end; sby PASS counter_assert d5–25; restore-queue clear recovered memmaptight32 / arrayordering / muldivscenario |
| phase 10 (mostly closed) | 0 | 108 | 10 | items 1/2/4 fixed; item 3 (wider_counter / modular bvadd/bvsub) diagnosed and deferred |
| after phase 2 | 0 | 98 | 20 | bor.smt2 latent unsoundness fixed; 10 fixtures regressed definitive→skip from longer learnt clauses (no disagreements). Perf recovery is follow-up. |
| phase 7b (opt-in min) | 0 | 96 | 22 | DV_LCG_MIN=1 enables cheap self-subsumption; sound but doesn't recover the 10 lost fixtures and adds 2s overhead. Default off. |
| after phase 3 (audit) | 0 | 98 | 20 | docs/cdcl_explain_audit.md added; no code changes. |
| after phase 4 | 0 | 98 | 20 | Literal.bound widened to int64; no truncation; no perf delta. |
| phase-save default-on (2026-05-25) | 0 | 99 | 19 | mempartitionknapsack + fsm_onehot d4/d16 recover; regfile_addr_alias d1/d4 regress. Wall 222s → 192s. |
| after phase 3 | TBD | TBD | TBD | audit complete |
| after phase 4 | TBD | TBD | TBD | int64 lifted |
| after phase 5 | TBD | TBD | TBD | regression tests in CI |
