# Session status — 2026-05-24

## Headline

Got dv-solve running end-to-end as a yosys-sby backend. counter_assert
verifies through BMC depths 5–25. Cross-check moved 105/13/0 → **108/10/0**
and tier1 is now 100% pass (the three "honest unknown" outliers
— `arrayordering`, `memmaptight32`, `muldivscenario` — recovered as
side effects of the checkpoint-restore fixes).

```
                          definitive  skip  disagreements
start of session             105       13         0
after caps + extend fix      109        9         0
after solver_restore cleanup 108       10         0  (one slow fixture
                                                       flipped to skip,
                                                       three recovered)
```

11 bugs fixed today, every one surfaced by wiring dv-solve into the
real yosys-sby pipeline rather than by browsing code or extending the
synthetic corpus.

## End state

- HEAD: `5338af2`.
- All commits this session land on `main`: `c2e34a0` `6a5c61b` `89e8df4`
  `dc5c4c6` `2b40758` `3e91fbb` `5d59ed3` `e66cbe0` `5338af2`.
- Cross-check (`pytest tests/formal/test_cross_check_tier1.py
  tests/formal/test_cross_check.py`): **108 passed, 10 skipped, 0
  failed**. ~102 s wall.
- yosys-sby on `tests/formal/sv/counter_assert.sv`:

  | Depth | dv-solve | boolector |
  |------:|----------|-----------|
  |   5   | PASS     | PASS      |
  |   7   | PASS     | PASS      |
  |  10   | PASS     | PASS      |
  |  15   | PASS     | PASS      |
  |  20   | PASS     | PASS      |
  |  25   | PASS     | PASS      |
  |  30+  | ERROR    | PASS      |

## What landed (in commit order)

### `c2e34a0` — bvnot bit-width mask + push/pop aux-problem cleanup
1. `_fire_bounds_bnot_64` was using C's `~` on int64, so `~5` produced
   `-6` instead of `250` for an 8-bit operand. The negative result fed
   back into propagation chased an unbounded trail and yosys-sby OOM'd
   at 65 GiB on a 4-bit counter, first BMC step. Fix: mask the result
   by `(1<<width)-1` so the complement stays in the variable's
   unsigned domain.
2. SMT2 frontend push/pop didn't release aux SolveProblems pushed
   between them. `solver_restore` correctly entailed the propagators,
   but the SolveProblem buffers stayed in the validation list — so
   the post-solve validator re-evaluated popped assertions and
   silently downgraded `sat` → `unknown`. Track
   `push_n_aux_problems[]` at push and truncate on pop.

### `6a5c61b` — four incremental-mode bugs from yosys-sby
3. `_bool_to_var` didn't accept EXTRACT shapes on either side of a
   comparison, so an incrementally-added `(or (= ((_ extract H L) v)
   k) ...)` compiled to nothing → spurious unsat. Materialise via
   `_value_to_var`.
4. `solver_add_constraint` gated var init on `id >= ctx->n_vars` but
   the builder's `vars_head` is LIFO — the highest new id bumped
   `n_vars` first, lower new ids fell below the threshold and were
   never initialised (width stayed 0, lo=hi=0, disj_clause saw aux
   as definitely-false). Two-pass: pre-compute max new id, then init
   each var whose id >= the saved pre-call n_vars.
5. `_fresh_aux` used `fe->n_vars` as the next ID, but
   `solver_compile` allocates extra internal aux vars (constant aux,
   ITE result aux) that the frontend never sees. Fresh frontend IDs
   collided with existing backend slots. `_next_var_id` now returns
   `max(fe->n_vars, ctx->n_vars)`, and `_fresh_aux` syncs `fe->n_vars`
   after the alloc.
6. `solver_restore` marked post-checkpoint propagators ENTAILED, but
   the next `solver_reset` cleared the bit on every slot in
   `prop_refs[]`, silently re-activating them. Stale inside-push
   propagators then fired against the new aux vars at the reused IDs.
   Fix: NULL out post-cp `prop_refs[i]` and roll `n_props` back.
7. Learnt CDCL clauses inside a push reference var IDs that get
   reused for new aux vars post-pop. `solver_restore` now NULLs
   `db->clauses[i]` for i >= n_clauses_at_cp and rolls back
   `db->n_clauses`. CheckpointMark gained `n_clauses_at_cp`.

### `89e8df4` — concat with const operand + stderr divert + _add_var grow + SMT2_MAX_FUNS
8. `(concat #b0 v)` (common in yosys output) required both operands
   to be plain VARs — the const hi side dropped the binding
   constraint. Materialise both via `_value_to_var`.
9. yosys-smtbmc runs the solver with `stderr=STDOUT`. Model-validation
   messages on stderr were read as protocol errors. When stdout is a
   pipe, divert `fe->err` to `/tmp/dv-solve.log` (or `$DV_LOG`).
10. `_add_var`'s grow check `n_vars == vars_cap` was too tight after
    `_next_var_id` learned to skip past backend-allocated IDs (could
    jump multiple slots in one allocation). Loop on `while
    (n_vars >= vars_cap)`.
11. `SMT2_MAX_FUNS = 64` capped at ~5 BMC steps. Bumped to 8192.

### `dc5c4c6` — first end-to-end SBY PASS benchmark report

### `2b40758` — incremental_capacity_hint
12. `solver_add_constraint` returned -1 when `id >=
    ctx->n_vars_capacity`. With `VAR_SLACK_FACTOR=32` and `n_initial=15`
    the capacity was 480, blown through in ~7 BMC steps (~48 aux/step).
    Add `ctx->incremental_capacity_hint`. SMT2 frontend sets it to
    8192 in `_ensure_compiled`. Covers depth 150+ on counter-class
    designs. Cost: ~196 KiB of pool memory.
13. `r = extend(a)` compile path required `a` to be a plain VAR.
    Materialise `a` via `_value_to_var(ext->operand, ext->from_bits)`.
    Without this, `(= aux ((_ zero_extend N) something))` silently
    dropped, model validator flagged zext mismatches.

### `3e91fbb` — solver_restore: clear and re-prime queue
14. `solver_restore` was leaving the propagator queue in an
    inconsistent state. Stale prop_refs from inside-push propagators
    (NULL'd in `prop_refs[]` but still chained via `queue_next`)
    could be dequeued in a subsequent `solver_propagate`; their pool
    memory was still resident but state was unreliable. SEGVs in
    `p->fire()` were the observed symptom.
    Fix: clear queue.heads/tails/non_empty_mask at end of
    `solver_restore` and re-enqueue all pre-checkpoint props.
    Mirrors the explicit re-prime that `solver_reset` already does.
    **Side wins:** memmaptight32, arrayordering, muldivscenario all
    pass (was honest-unknown skip); cross-check 105/13/0 → 108/10/0.

### `5d59ed3` — benchmark update (PASS through depth 25)

### `e66cbe0` — solver_restore: dedup redundant queue re-enqueue
After `3e91fbb` the pre-cp prop re-enqueue ran twice (the first add
and the original code at the bottom of `solver_restore`). Drop the
first pass.

### `5338af2` — benchmark report cross-out

## Remaining edges (all reproduce locally)

1. **counter_assert d ≥ 30 ASAN SEGV** at step 25 in
   `solver_propagate`'s `p->fire` dereference. The propagator queue
   clear and `prop_refs` NULL'ing covered the obvious cases. Likely
   another stale pool offset somewhere — watcher chain, queue_next
   tail, or `_register_watcher`'s linkage. Deterministic, reproduces
   under ASAN every time. Foreground-only.

2. **cover mode false-UNSAT** at cover step 1 on counter_assert.
   Minimal repro (cv14): outer `(assert (= (extract u6) #b1))`,
   push, `(assert (and (= (extract u6) #b1) (= (extract u7) #b1)))`,
   pop, push, `(assert (= (extract u6) #b1))`. Third check should
   be sat, dv returns unsat. Likely yet another stale-state edge in
   the checkpoint rollback that the BMC pattern doesn't trigger.

3. **wider counter (8-bit + sub)** triggers a validator complaint
   then ASAN at step 14. Looks like more `(_ zero_extend N)` shapes
   yosys emits that aren't covered by the materialisation fix in
   `2b40758`. Has a real counterexample at step 1 that boolector
   finds — dv-solve doesn't find it either way.

## Diagnostic surface (kept)

- `DV_USE_LCG=0` — disable CDCL.
- `DV_LCG_STATS=1` — per-solve `[lcg]` line on stderr.
- `DV_LCG_TRACE=1` — per-conflict trace (every explain call's
  antecedents, the final learnt clause + backjump level).
- `DV_LOG=path` — when stdout is a pipe, route diagnostics here
  instead of `/tmp/dv-solve.log`.
- `DV_VALIDATE_MODEL={0,1,2}` — model validation mode; 2 (default)
  downgrades sat→unknown silently on violation, 1 keeps sat but
  warns, 0 skips.
- `DV_MAX_RESTARTS=N` — bound the restart count (default 1000).

## Files touched this session

- `src/c/zsp_prop_templates.c` — bvnot width mask
- `src/c/smt2/smt2_frontend.c` — push/pop aux cleanup,
  `_next_var_id` sync, `_fresh_aux` post-sync, `_add_var` grow,
  capacity hint, `_bool_to_var` EXTRACT fallback
- `src/c/smt2/smt2_frontend.h` — `push_n_aux_problems`, bumped
  `SMT2_MAX_FUNS`
- `src/c/smt2/smt2_main.c` — stderr divert when stdout is a pipe
- `src/c/zsp_compile.c` — LIFO var init two-pass, concat & extend
  `_value_to_var` materialisation, `incremental_capacity_hint`
- `src/c/zsp_ctx.h`, `zsp_ctx.c` — `incremental_capacity_hint`,
  `CheckpointMark.n_clauses_at_cp`
- `src/c/zsp_checkpoint.c` — full pop cleanup (NULL post-cp
  prop_refs, roll back `n_props`, clear queue, re-enqueue pre-cp
  props, drop post-cp learnt clauses)

## Regression coverage added

- `tests/formal/regression_bvnot_width.smt2`
- `tests/formal/regression_pop_aux.smt2`
- `tests/formal/regression_incremental_ids.smt2`
- `tests/formal/sby/smtio_dvsolve.patch` (vendored smtio.py patch
  to register dv-solve as a smtbmc engine)
- `tests/formal/results/sby_e2e_2026-05-24.md` (benchmark report)

## Resume here

Highest-leverage next steps, in priority order:

1. **Chase the d ≥ 30 ASAN SEGV.** Reproduces with
   `cd /tmp/sby_test && rm -rf counter_d_bmc && ASAN_OPTIONS=log_path=/tmp/asan.log:abort_on_error=0 sby -f counter_d30.sby bmc`.
   Stack always lands at `solver_propagate:265` `p->fire(p, ctx)`.
   Theory: stale entry in a watcher chain that survives `solver_restore`'s
   queue clear; when a new tightening fires `_wake_var`, the iteration
   trips on the stale next pointer. Test by adding `p->prop_id`
   validation at line 264 (compare against `ctx->n_props` and
   `ctx->n_prop_refs_capacity`).

2. **Fix cover-mode push/pop edge** (cv14 in /tmp). The fact that
   `cv13` (push contradiction without and-of-extracts) works but
   `cv14` (push with and-of-extracts) doesn't suggests the failure
   is in the AND-compile path's aux propagator lifecycle, not the
   bit_slice/EXTRACT path itself. Add a regression fixture in
   `tests/formal/`.

3. **Try a non-counter design** through sby. The
   `wider_counter` repro I built locally found a real bug at step 1
   (per boolector); dv-solve misses it entirely. Likely a different
   compile-path issue from the EXTRACT/concat/extend chain we've
   already extended. Picking a different design class
   (`tests/formal/sv/*.sv` if any exist) and running both
   solvers via sby would expose the next class of bugs.

4. **Recover the 10 remaining cross-check skips.** All in tier2/3:
   `arbiter_fairness d4/d8/d16`, `cache_direct_1way d1/d2/d4/d8`,
   `regfile_simple d1/d4/d8`. After today's restore fixes, three
   recovered for free; the remaining ten are CDCL-search-cost cases
   (per the earlier perf analysis in `docs/cdcl_followup_plan.md`).
   Phase 7+8 from that plan are still applicable.

## Notes / gotchas

- **Two `dv-solve-smt2` symlinks in play**: a real binary symlink at
  `packages/yosys-bin/bin/dv-solve-smt2` for normal use, and a tee
  wrapper at `/tmp/dvtee.sh` for capturing the SBY-driven stdin/stdout.
  If you swap to `/tmp/dvtee.sh` for capture, restore the real
  symlink before running benchmarks — the wrapper adds ~50 ms latency
  per check and makes deeper BMC timeouts look like solver bugs.
  ```
  ln -sf "$(pwd)/build/dv-solve-smt2" packages/yosys-bin/bin/dv-solve-smt2
  ```

- **`fe->err` redirect is gated on `isatty(stdout)`.** Direct CLI
  use keeps stderr on the terminal. Anything that pipes stdout
  (yosys-smtbmc, tee, etc.) silently routes diagnostics to
  `/tmp/dv-solve.log` — check there first when an SBY run gives
  `Unexpected response from solver: unknown` with no obvious cause.

- **`CTX_BUF_SIZE` is 64 MiB.** The 8192-var capacity hint costs
  ~196 KiB. Leave plenty of headroom for the pool's other
  allocations (constraints, prop_refs, watcher_heads, clause arena).

- The `arrayordering` tier1 fixture passing again is purely a
  side effect of clearing stale propagators on pop — not a fix to
  the bvmul-overflow issue noted in the original README. If
  someone resurrects that fixture for stress testing, that overflow
  may bite again.
