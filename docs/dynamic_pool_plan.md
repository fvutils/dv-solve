# Plan: block-grow dynamic allocation for the solver context

Tracking document. Design and rationale: `docs/dynamic_pool_design.md`.

- **Started:** 2026-10-11
- **Status:** P0 implemented, gate running (see Log)
- **Owner:** —

## Goal

Every hosted platform uses one scheme for solver-context memory. The
context grows on demand and never fails for lack of a correctly guessed
buffer size. A caller-supplied fixed buffer stays available as an opt-in
embedded mode, with today's behaviour. All growth goes through the
context's `dvs_alloc_t` and never calls the system `malloc`/`realloc`/`free`
directly.

## Done when

- [ ] No hosted front end (C `dvs_solver_new`, Python, DPI, SMT-LIB2,
      contradiction/MUS) passes a context buffer size, and none contains
      grow-and-retry logic.
- [ ] A context pool overflow is reachable only through true OOM or
      embedded fixed mode.
- [ ] `n_vars_capacity` bail-outs, `VAR_SLACK_*`, `incremental_capacity_hint`
      and `MAX_DECISION_DEPTH` preallocation are gone.
- [ ] Every byte the context owns is allocated through its `dvs_alloc_t`.
- [ ] Push/pop sessions hold pool use flat.
- [ ] Perf history shows no regression on the Verilator-protocol randomize
      bench, the easy-band sweep, or `field_bench.py`.

## Conventions for every phase

- Run tests with `env -u LD_LIBRARY_PATH` and the whole `pytest -q`
  (including `tests/bench`).
- Gates run on Linux and on the Windows/MSVC CI. MSVC has no `__int128`,
  no `__typeof__` and no popcount builtin.
- Any wrong answer found along the way is fixed immediately and logged in
  `docs/solver_bug_backlog.md`.
- Each phase is one or more commits. Record the hashes in the log at the
  bottom.

---

## P0 — Allocator resize hook, inline decode, checkpoint pool rewind

These changes are independent of the redesign and each is useful on its
own.

### P0.1 `dvs_alloc_t` resize hook
- [x] Add `resize(self, ptr, old_size, new_size)` as the last member of
      `dvs_alloc_t` in `dvs_alloc.h`. NULL means use the copy fallback.
- [x] Add `dvs_alloc_resize()` with the `DVS_RESIZE(a, p, old, new)`
      macro: call `resize` if set, otherwise alloc + memcpy + release. On
      failure return NULL and leave `ptr` valid. A NULL allocator means
      `dvs_malloc_alloc`; `new_size` 0 releases.
- [x] `dvs_malloc_alloc.resize` = `realloc`.
- [x] `DVS_DEBUG_ALWAYS_MOVE` (CMake option): `dvs_alloc_resize` always
      copies, poisons the old block (`0xA5`), then releases it.
- [x] Public header: document the new member for custom allocators, and
      that older allocators that leave it NULL still work. Note: this is
      a source-compatible change, not a binary one. An allocator struct
      built against the old two-member layout (a prebuilt binary, or a
      ctypes `Structure`) must add the field; `test_checkpoint_memory.py`'s
      ctypes allocator was updated.
- [x] C test `tests/c/test_dvs_alloc_resize.c`: a hook-less allocator
      takes the fallback, grows, shrinks, keeps data, and returns NULL on
      a failing allocator with the original block intact.

### P0.2 Inline `dvs_pool_ptr`
- [x] Move `dvs_pool_ptr` to a `static inline` in `dvs_pool.h`. The
      out-of-line `(dvs_pool_ptr)` stays exported for ctypes callers.
- [x] Measure against the current baseline. Tool:
      `tests/perf/ab_compare.py` (two builds, interleaved, best of N) over
      the Verilator-protocol randomize benches, API reset+solve, the easy
      fixture sets (tier1, verilator, gaps) and `field_bench.py`. Numbers
      in the log. This is the baseline P1 is compared against.

### P0.3 Checkpoints rewind the pool
- [x] Add a pool mark to `CheckpointMark` and set it in
      `dvs_solver_checkpoint` (new pool API: `dvs_pool_mark` /
      `dvs_pool_release`; the release fills freed bytes with `0xA5` under
      `DVS_DEBUG_ALWAYS_MOVE` and ASan-poisons them until reallocated).
- [x] `dvs_solver_restore` rewinds the pool at its end
      (`_rewind_pool`, `dvs_checkpoint.c`). It never frees below
      `ctx->pool_floor`, the pool position after compile.
- [x] Audit what the pool holds after a checkpoint that must outlive the
      restore, and handle each:
      - watcher chains: the scope's propagators are a prefix of each
        chain (watches are pushed at the head), so they are unlinked;
      - `HoleEntry` (exclusions last across restore, as before): values
        allocated in the scope are carried over and re-inserted;
      - `initial_vars` (reset snapshot, may be regrown in the scope): the
        mark saves pointer, count and capacity, restore puts them back;
      - phase-save and `CostGuidedCtx`: moved off the pool to the
        context's allocator (`ctx->alloc`, freed in `dvs_solver_destroy`);
      - the scope's variables' watcher heads and hole lists are cleared.
- [x] Latent bug found by the poisoned rewind: restore rebuilt
      `unassigned_mask` before rolling `n_vars` back, so the next search
      could decide on a popped variable. Before the rewind that only read
      leaked memory; after it, freed memory. Restore now rebuilds the
      mask over the surviving variables.
- [x] Fix the comment at `smt2_frontend.c:4618` ("snapshot wholesale").
- [x] Test: 10,000 push / assert / check-sat / pop cycles through
      `dv-solve-smt2` (`tests/unit/test_pushpop_pool.py`) and through the
      C API (`tests/c/test_checkpoint_pool.c`: pool use flat after every
      pop, exclusions made in a scope still hold after it, nested scopes).

**Gate P0:** full suite green, normal build and ASan build; perf numbers recorded.

---

## P1 — Segmented pool (`dvs_spool_t`)

- [ ] New `dvs_spool.h/.c`, implementing the design in §4.1:
      - [ ] Segment table `seg[]` grown with `DVS_RESIZE`. Compile-time
            `DVS_SEG_SHIFT`, default 16 (64 KiB slots).
      - [ ] Small-object bump allocation in `cur`. A new slot comes from
            `dvs_block_alloc_get`, with the block size set to the slot size.
      - [ ] Large objects take consecutive slot indices backed by one
            contiguous `DVS_ALLOC`, with `seg[j+i] = base + i*slot`.
      - [ ] Mark/release `(cur, cur_used, next_slot)`. Released slots go
            back to the block allocator's free list. Large objects are
            released with `DVS_RELEASE`.
      - [ ] Fixed mode: `ba == NULL`. The caller's buffer is mapped onto
            slots `0..ceil(size/slot)-1`. Overflow is sticky, as today.
      - [ ] `DVS_POOL_SINGLE_SEGMENT` build option: decode is `base + off`.
- [ ] Switch `dvs_ctx_t.pool` to `dvs_spool_t`. The 33 `dvs_pool_alloc` /
      `dvs_pool_ptr` call sites on `ctx->pool` change only in the type
      or name. `dvs_problem_t` keeps `dvs_pool_t` (§4.5).
- [ ] Wire the P0.3 checkpoint mark onto spool mark/release.
- [ ] Unit tests: decode across slot boundaries, large-object contiguity,
      mark/release slot reuse, fixed-mode overflow, a buffer smaller than
      one slot.

**Gate P1:**
- [ ] Full suite plus the fuzzers (`test_fuzz_cross_check.py`, the array
      fuzzer, the step-check build) at `DVS_SEG_SHIFT=16`.
- [ ] Same again at `DVS_SEG_SHIFT=8`, which forces constant slot turnover
      and the large-object path.
- [ ] Perf within noise of the P0.2 baseline.

---

## P2 — Growable per-variable and per-propagator tables

- [ ] `dvs_ctx_reserve_vars(ctx, n)` grows every per-var table together
      with `DVS_RESIZE`: `vars`, `watcher_heads`, `var_alias`,
      `dist_offsets`, `var_holes_head`, `var_n_values`, `initial_vars`,
      phase-save, `keep_*`, `box_*`, `var_order`.
- [ ] `dvs_ctx_reserve_props(ctx, n)` does the same for `prop_refs`,
      `prop_guard_vars` and `prop_constraint_id`.
- [ ] `level_marks` / `decisions` become growable. `MAX_DECISION_DEPTH`
      changes from a preallocation into an initial size.
- [ ] Replace the roughly 20 `n_vars >= n_vars_capacity → EXPR_NULL` sites
      in `dvs_compile.c` with a reserve. A failed reserve is OOM, reported
      as NOMEM and never as a silent uncompiled constraint.
- [ ] Reserve each constraint's worst-case aux-variable count once per
      `_compile_constraint` where that count can be bounded.
- [ ] Delete `VAR_SLACK_FACTOR`, `VAR_SLACK_MIN` and
      `incremental_capacity_hint`.
- [ ] Move the context's other raw allocations onto the allocator:
      `scope_log_*`, `keep_*`, `box_*`, `value_fails`, `mod_links`,
      `var_order` and `lcg` in `dvs_ctx.c`, plus the context-owned ones in
      `dvs_costguided.c`, `dvs_search.c`, `dvs_compile.c` and
      `dvs_diffcycle.c`.
- [ ] Audit stale pointers: `Variable *` and table pointers held across
      any call that can reserve. Rely on the `DVS_DEBUG_ALWAYS_MOVE`
      gate below rather than review alone.

**Gate P2:**
- [ ] Full suite, the fuzzers and svcorpus under ASan with
      `DVS_DEBUG_ALWAYS_MOVE=ON`, with zero reports.
- [ ] Memory check: peak RSS on a 1,000-variable problem drops (the 33×
      slack is gone). Record it in the log.
- [ ] Perf within noise of the baseline.

---

## P3 — API and front ends

- [ ] `dvs_solver_new(dvs_alloc_t *alloc)` / `dvs_solver_free(ctx)`. The
      header and segment table come from `alloc`.
- [ ] `dvs_solver_create(buf, size, ba)` stays the same. The header is
      carved from `buf` and the rest is fixed-mode segments; it grows
      through `ba` if `ba` is non-NULL.
- [ ] `dv_solve.h`: change the top-of-file example to `dvs_solver_new`
      and describe `dvs_solver_create` as the embedded / fixed option.
- [ ] SMT-LIB2: switch to `dvs_solver_new`. Delete `_ctx_buf_size_for`,
      `CTX_BUF_SIZE`, `ctx_buf_scale`, the 25% headroom recompile and the
      overflow recompile loop. Keep the `pool.overflow` → bitblast
      escalation as a real-OOM safety net.
- [ ] DPI: switch to `dvs_solver_new`; drop `INTERNAL_CTX_SIZE`.
- [ ] Python `SolveContext`: switch to `dvs_solver_new`. `ctx_buf_size`
      is accepted, ignored, and gives a `DeprecationWarning`.
- [ ] Contradiction / MUS: switch to `dvs_solver_new`; drop
      `CONTRA_CTX_BUF_SIZE`.
- [ ] Add an assert that a `dvs_problem_t` is not resized after
      `dvs_solver_compile` has taken it (§4.5).
- [ ] Docs: update backgrounder §4.1 and the Results/API pages.

**Gate P3:**
- [ ] Full suite on Linux and Windows/MSVC CI.
- [ ] Verilator capture fixtures and the Verilator-protocol bench pass.
- [ ] A tiny fixed buffer passed to `dvs_solver_create(buf, small, NULL)`
      still returns `DVS_COMPILE_NOMEM`, so the fixed-mode contract holds.

---

## P4 — Remaining fixed buffers

- [ ] LCG learnt-clause arena (`dvs_lcg.c:77`, fixed 4 MiB) moves onto a
      `dvs_spool_t`, so learning no longer stops when the arena fills.
      Check that `clause_db_gc` still works.
- [ ] Python `SolveProblem` (fixed 64 KiB): make it builder-backed with
      the same method surface. Switch the `ctx.py:426,452` aux-problem
      uses over to it. Deprecate the `buf_size` argument.
- [ ] Gate: perf history, full suite.

## Follow-up (out of scope, tracked here so it isn't lost)

- Raw `malloc`/`realloc`/`free` outside the context: about 490 call sites
  in `src/c/dvs_*.c`. The largest are `dvs_bbsolver.c` (88),
  `dvs_cube.c` (63), `dvs_contradiction.c` (48), `dvs_affine.c` (43),
  `dvs_sv.c` (33) and `dvs_lcg.c` (33). Move them to `dvs_alloc_t` file by
  file once P3 lands. SAT backends (kissat/CaDiCaL) are excluded because
  they own their own memory.
- Other fixed capacities: `MAX_CHECKPOINTS 32` (`dvs_ctx.h:51`),
  `MAX_NOOVERLAP2D_RECTS 64`, and the `Smt2Frontend` inline `funs[8192]`
  (frontend memset hotspot).

---

## Log

| Date | Phase | Commit | Notes |
|---|---|---|---|
| 2026-10-11 | — | — | Plan written; design in `docs/dynamic_pool_design.md`. |
