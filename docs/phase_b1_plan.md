# Phase B.1 plan: adapt the forked Kissat

Status: in progress
Date: 2026-05-25

## What B.1 is

Phase B.0 linked Kissat 4.0.4 as a vendored static library from
`resources/kissat/`. The SAT layer is correct, non-incremental, and
already drives the 118-fixture cross-check to 0 disagreements vs z3.

Phase B.1 takes ownership of the Kissat sources by **forking** them into
`src/c/sat/kissat/` and then incrementally adapting:

1. Route all allocations through `zsp_alloc_t` so the SAT layer's RSS
   joins the dv-solve allocator vtable (hugepages, NUMA pools, snapshot
   regions become possible).
2. Add **incremental support** — assumptions, push/pop, partial state
   reset between `check-sat` calls.
3. Migrate the clause arena to use `zsp_pool` / `zsp_arena` 32-bit
   reference handles so clauses are compactable and the arena can be
   marked/rolled back to dv-solve checkpoint levels.
4. Integrate with the dv-solve **trail and `LevelMark` checkpoints** so
   the SAT layer participates in the unified backtrack model — see
   [[sat_memory_management_plan]] for the crossover-A/B/C/D ideas.

Ultimate end-state: a dv-solve-native CDCL core that inherits Kissat's
solver heuristics but uses dv-solve's checkpoint / trail / allocator
machinery, suitable for hosting Phase D crossover techniques like
LS-seeded CDCL and CDCL-driven LS restart.

## What B.1 step 1 landed

**Fork the source tree.** `src/c/sat/kissat/` is now the build's source
of Kissat. `resources/kissat/` is preserved as the **upstream baseline**
— we can `diff -r resources/kissat/ src/c/sat/kissat/` to see every
modification.

The fork is byte-identical to upstream Kissat 4.0.4. The only "change"
is `cmake/kissat.cmake` pointing at the new path and the `ID` define
flipping from `"dv-solve-vendored"` to `"dv-solve-forked"`. Cross-check
performance is unchanged (~620ms, same as v2 perf snapshot baseline).

## What B.1 step 2 should do — allocator routing

Kissat funnels all allocations through a small set of wrapper functions
in `src/c/sat/kissat/src/allocate.c`:

```
kissat_malloc(kissat*, size_t)
kissat_calloc(kissat*, size_t, size_t)
kissat_realloc(kissat*, void*, size_t, size_t)
kissat_free(kissat*, void*, size_t)
kissat_dealloc(kissat*, void*, size_t, size_t)
kissat_nalloc(kissat*, size_t, size_t)
```

These currently call libc `malloc`/`realloc`/`free` directly. The
adaptation: add a `zsp_alloc_t *` field to the `kissat` struct
(`src/c/sat/kissat/src/internal.h`), thread it through the wrappers, and
route all backing calls through `ZSP_ALLOC` / `ZSP_RELEASE` when set
(fall back to libc when NULL for upstream compatibility).

zsp_sat.c then plumbs `alloc` through when constructing the kissat
instance.

Validation: all 13 ctest suites, 118-fixture cross-check, identical
results.

## What B.1 step 3 should do — incremental support

Kissat is non-incremental by default — `kissat_solve` consumes the
problem and the subsequent `kissat_value` queries are valid only for
the same call. There's no `kissat_assume` or push/pop API.

Two incremental scenarios we want to support:

- **Push/pop** for SMT2's stack of assertions. Use case: BMC unrolling
  in incremental mode, where each `(check-sat)` adds a frame to an
  already-built base problem.
- **Assumptions** for `(check-sat-assuming ...)` — temporary
  hypotheses dropped after the check.

The smallest-leverage starting point is push/pop, because BMC fixtures
in `tests/formal/smt2/tier3/` are written in this style. CaDiCaL has a
mature push/pop implementation we can reference; the algorithmic core is
"snapshot the trail / clause-DB / unit-clauses at push, roll back on
pop." Aligns naturally with extending dv-solve's `LevelMark` to cover
the SAT-side state.

## What B.1 step 4 should do — clause arena migration

Kissat already keeps clauses in a single word-arena (`src/c/sat/kissat/src/arena.{h,c}`). The
adaptation here is to swap the bespoke arena for a `zsp_arena_t`
instance (already in place per `cmake/kissat.cmake` and tested via
`test_zsp_arena`). This is mostly about replacing the
`STACK(ward)`-macro-driven allocation paths with `zsp_arena_alloc()`
calls.

Open question: keep `STACK` macros for the other Kissat stacks
(trail, queue, etc.) or migrate everything to zsp_stack? Probably keep
the other stacks as-is initially — only the clause arena needs the
zsp_arena treatment to support compaction across dv-solve checkpoints.

## What B.1 step 5 should do — trail / LevelMark integration

This is the crossover work where dv-solve's strengths matter most.
The idea: extend `LevelMark` (currently in `zsp_trail.h`) with a SAT-
arena top, so a single `LevelMark` records the position of all
dv-solve-side trail entries plus the SAT-side clause arena. A
`zsp_levelmark_pop()` then rolls back both layers in one step.

Concretely: when CDCL backtracks, it currently rolls back its own trail
only. With this integration, CDCL backtrack participates in the
dv-solve checkpoint system, and a `solver_restore()` from dv-solve
can reach into the SAT layer's clause DB.

This is the foundation for the Phase D "checkpoint-keyed arena marks"
crossover technique — neither bitwuzla nor CaDiCaL can do this.

## What B.1 step 6 — and 7 — should do

Step 6: integrate the kissat fork with the existing `zsp_sat.{h,c}`
abstraction so callers see the same API but get the forked /
adapted kissat underneath. Probably no API change at all — the
abstraction was designed for this.

Step 7: clean up — remove `resources/kissat/` from the repo (keep
diffs in git history; the upstream URL is in
`src/c/sat/kissat/LICENSE` for traceability). Or keep `resources/`
as the historical-baseline reference; this is a maintenance-policy
choice.

## Order of operations

Each step is its own commit (or small set of commits). Steps 2 and 4
are independent and could be done in either order; steps 3 and 5
depend on having the allocator and arena hooks in place.

Suggested order:
1. ✅ **Step 1**: fork source. [DONE this commit.]
2. Step 2: allocator routing.
3. Step 4: clause arena migration.
4. Step 3: push/pop incremental.
5. Step 5: trail / LevelMark integration.
6. Step 6: zsp_sat.c uses the adapted kissat.
7. Step 7: cleanup.

Each step gates on no cross-check regression and no ctest failure.

## What this does *not* cover

- Heuristic tuning of Kissat (different option defaults, restart
  policies, clause-deletion strategies). Out of scope for B.1 — that
  is part of Phase D / experimental work.
- Replacing Kissat's CDCL core wholesale. That is the eventual end
  state but B.1's purview is "adapt, don't replace."
