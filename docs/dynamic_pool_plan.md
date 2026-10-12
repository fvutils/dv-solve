# Plan: block-grow dynamic allocation for the solver context

Tracking document. Design and rationale: `docs/dynamic_pool_design.md`.

- **Started:** 2026-10-11
- **Status:** P0–P4 implemented and gated, 2026-10-11 (see Log). Follow-ups
  2026-10-12: six of nine done (see Follow-ups). Not merged to `main`.
- **Owner:** —

## Goal

Every hosted platform uses one scheme for solver-context memory. The
context grows on demand and never fails for lack of a correctly guessed
buffer size. A caller-supplied fixed buffer stays available as an opt-in
embedded mode, with today's behaviour. All growth goes through the
context's `dvs_alloc_t` and never calls the system `malloc`/`realloc`/`free`
directly.

## Done when

- [x] No hosted front end (C `dvs_solver_new`, Python, DPI, SMT-LIB2,
      contradiction/MUS) passes a context buffer size, and none contains
      grow-and-retry logic.
- [x] A context pool overflow is reachable only through true OOM, embedded
      fixed mode, or more than 65,535 propagators (all reported as NOMEM).
- [x] `n_vars_capacity` bail-outs, `VAR_SLACK_*` and
      `incremental_capacity_hint` are gone. `MAX_DECISION_DEPTH` stays,
      deliberately (P2).
- [x] Every byte the context owns is allocated through its `dvs_alloc_t`,
      except the LCG's clause-pointer array and analysis scratch, which
      are in the follow-up list.
- [x] Push/pop sessions hold pool use flat.
- [x] No perf regression: see the Log (faster than the baseline on all
      four surfaces).

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

**Gate P0:** ✅ full suite: the same 30 failures as the baseline (f04af10),
no new ones. ASan covered by the P3 run below. Perf recorded in the Log.

---

## P1 — Segmented pool (`dvs_spool_t`)

- [x] New `dvs_spool.h/.c`, implementing the design in §4.1:
      - [x] Segment table `seg[]` grown with `DVS_RESIZE`. Compile-time
            `DVS_SEG_SHIFT`, default 16 (64 KiB slots).
      - [x] Small-object bump allocation in the current slot. *Changed
            from the design:* a new slot comes from the context's
            `dvs_alloc_t` (`ctx->alloc`), with a cache of
            `DVS_SPOOL_SPARES` (4) freed slots, not from the block
            allocator. The block allocator's block size is the caller's
            choice and is shared with the trail stack, so it cannot double
            as the slot size.
      - [x] Large objects take consecutive slot indices backed by one
            contiguous `DVS_ALLOC` of exactly their size, with
            `seg[j+i] = base + i*slot`.
      - [x] Mark/release `(top, end, overflow, n_back)`. Every backing is
            recorded in allocation order; release pops the ones opened
            after the mark (one-slot backings to the spare cache, others
            `DVS_RELEASE`). Offsets stay monotonic in allocation order, so
            "offset >= mark" still means "allocated after the mark".
      - [x] Fixed mode: no allocator. The caller's buffer is mapped onto
            slots `0..ceil(size/slot)-1` (slot table carved from its
            front) and behaves as one contiguous block. Overflow is
            sticky, as before.
      - [x] `DVS_POOL_SINGLE_SEGMENT` build option: decode is
            `fixed + off`, no growth.
- [x] Switch `dvs_ctx_t.pool` to `dvs_spool_t` (no longer the last field;
      the fixed region is the buffer after the header). `dvs_problem_t`
      keeps `dvs_pool_t` (§4.5). The build used
      `-Werror=incompatible-pointer-types` to catch every missed call.
- [x] Wire the P0.3 checkpoint mark onto spool mark/release. The compile
      floor is a mark too.
- [x] Unit tests `tests/c/test_dvs_spool.c`, built twice (default and
      `DVS_SEG_SHIFT=8`): decode across slot boundaries, large-object
      contiguity, mark/release and slot reuse from the spare cache,
      fixed-mode overflow and its release, a buffer smaller than one slot,
      a pool with no buffer at all.

**Gate P1:**
- [x] Full suite (fuzzers included, at their default sizes) at
      `DVS_SEG_SHIFT=16`. The step-check build was not rebuilt for this
      work; its test skips in a worktree.
- [x] Same again at `DVS_SEG_SHIFT=8` (final code): results identical to
      SHIFT=16.
- [x] Perf within noise of the P0.2 baseline (after the hole-list fix
      below).

---

## P2 — Growable per-variable and per-propagator tables

- [x] `dvs_ctx_reserve_vars(ctx, n)` grows the per-var tables together.
      *Changed from the plan:* they share one block (`var_block`:
      `var_n_values`, `vars`, `watcher_heads`, `var_alias`,
      `dist_offsets`, `var_holes_head`), so a resize allocates the new
      block, copies each array and frees the old one: all-or-nothing, one
      allocation instead of six. Phase-save and the LCG's per-variable
      arrays grow in the same call. `initial_vars` keeps its own growth
      in the pool (`_snapshot_added_vars`); `keep_*`, `box_val` and
      `value_fails` are per-solve and sized at solve time, as before.
- [x] `dvs_ctx_reserve_props(ctx, n)` does the same for `prop_refs`,
      `prop_guard_vars` and `prop_constraint_id` (`prop_block`).
- [x] Tables come from `ctx->alloc` when the pool can grow. In fixed mode
      they are carved from the pool and cannot grow inside a checkpoint
      scope (the restore would free them): such a reserve fails, so the
      add is NOMEM and the context is whole again after the restore.
- [ ] ~~`level_marks` / `decisions` become growable.~~ *Not done, by
      decision:* `MAX_DECISION_DEPTH` (256) is a search policy as much as
      a memory size. A solve that needs more decision levels gives up
      (`DVS_BAIL_MAX_DEPTH`) and the front ends hand it to bitblast. Making
      the arrays growable would change when that happens, which is an
      engine-routing change with its own perf risk. Track separately if
      wanted.
- [x] Replace the `n_vars >= n_vars_capacity → decline` sites in
      `dvs_compile.c` (about 40, not 20) with a reserve (`dvs_ctx_room`).
      A failed reserve sets the pool's overflow flag, and compile/add
      now return `DVS_COMPILE_NOMEM` whenever that flag is set.
- [x] *Found and fixed while doing this:* compile never checked the
      overflow flag. A failed allocation whose caller ignored it (several
      `prop_add_*` call sites do) dropped the constraint and compile still
      returned OK. Only the SMT-LIB2 front end checked the flag; Python,
      DPI and the C API did not.
- [x] *Found and fixed:* `Propagator.prop_id` is 16 bits, so a context
      with more than 65,535 propagators gave ids that wrapped and shared
      `prop_refs` / `prop_guard_vars` / `prop_constraint_id` entries with
      another propagator. The context now declines past `DVS_MAX_PROPS`
      (NOMEM). Widening `prop_id` is a separate decision (it grows every
      propagator by 8 bytes).
- [ ] ~~Reserve each constraint's worst-case aux-variable count once per
      `_compile_constraint`.~~ Not needed: growth is geometric, so the
      per-variable reserve is amortised O(1).
- [x] Delete `VAR_SLACK_FACTOR`, `VAR_SLACK_MIN`, `PROP_SLACK_*` and
      `incremental_capacity_hint` (and its two setters).
- [x] Move the context's other raw allocations onto the allocator:
      scope log (one block), kept draws (one block), `box_val`,
      `box_skip`, `var_order`, `value_fails`, `mod_links`, the `LCGCtx`
      struct and its per-variable arrays. Per-call scratch in
      `dvs_diffcycle.c` is not context-owned and stays on malloc; the
      HPWL cost context in `dvs_costguided.c` belongs to the caller.
- [x] Audit stale pointers. Review found two (`_bool_to_var`'s `v` and
      the bvnot path's `av`, both read after a reserve); fixed. Under
      `DVS_DEBUG_ALWAYS_MOVE` small tables grow one entry at a time, so
      every new variable moves them and ASan catches the rest.
- [x] *Found and fixed:* `dvs_solver_create(buf, size, NULL)` never had a
      trail stack, so compile crashed on its first bound tightening. That
      is now the fixed-pool mode; the context makes a small block
      allocator of its own for the trail.
- [x] C test `tests/c/test_growable_tables.c`: growth from a 64 KiB
      buffer, 12,000 incremental variables (past the old 8,192 cap),
      fixed-mode NOMEM and in-scope NOMEM with a clean restore, and the
      65,535-propagator limit.

**Gate P2:**
- [x] Full suite under ASan with `DVS_DEBUG_ALWAYS_MOVE=ON` (P3 code):
      zero memory-error reports. It found and drove the fixes for the
      stale `unassigned_mask` (P0.3) and the spool debug fill. Its 25 extra
      failures are environmental: tests that cap the address space
      (`RLIMIT_AS`: `test_propagation_guard`, the Verilator-protocol tests),
      where ASan's shadow memory cannot be reserved; `LD_PRELOAD` reaching
      Verilator in `test_doc_pages`; and a timing threshold under -O0.
      Those capped tests therefore had no ASan coverage.
- [x] Memory check, recorded in the Log.
- [x] Perf within noise of the baseline.

---

## P3 — API and front ends

- [x] `dvs_solver_new(dvs_alloc_t *alloc)` / `dvs_solver_free(ctx)`. The
      header, pool and trail block allocator all come from `alloc` (NULL:
      malloc); the pool has no fixed region.
- [x] `dvs_solver_create(buf, size, ba)` keeps its signature; it and
      `dvs_solver_new` share `_ctx_init`. With `ba` the pool grows past the
      buffer through `ba`'s backing allocator; with NULL it is fixed (see
      P2b for the trail allocator that mode now gets).
- [x] `dv_solve.h`: the top-of-file example uses `dvs_solver_new`;
      `dvs_solver_create` is documented as the embedded option.
- [x] SMT-LIB2: `dvs_solver_new`. Deleted `_ctx_buf_size_for`,
      `CTX_BUF_SIZE` (64 MiB cap), `BA_BLOCK_SIZE`, `ctx_buf_scale`
      (and its carry across `(reset)`), the 25%-headroom recompile and the
      overflow recompile loop. The `pool.overflow` → bitblast escalation
      stays as the real-OOM safety net.
- [x] DPI: `dvs_solver_new`; `INTERNAL_CTX_SIZE` gone.
- [x] Python `SolveCtx`: `dvs_solver_new`. `ctx_buf_size` is accepted,
      ignored, and warns `DeprecationWarning`. Its compile-error paths now
      free the context (`dvs_solver_free`) as well as the allocator.
- [x] Contradiction / MUS: `dvs_solver_new`; `CONTRA_CTX_BUF_SIZE` gone.
      Its NOMEM path used to leak the context; fixed.
- [x] `bench_vlt_replay.c` mirrored the SMT2 sizing loop; it uses
      `dvs_solver_new` too.
- [x] §4.5 guard: compile and add set `DVS_PROBLEM_HELD` in the problem's
      pool header (its spare `_pad` word, so the blob layout is
      unchanged). `dvs_builder_finalize_append` asserts on, and in release
      builds refuses (returns NULL), a move of a held problem.
- [x] Docs: `docs/site/reference/c-api.md` and `docs/solver_api.md`
      document `dvs_solver_new`/`dvs_solver_free` and the embedded
      `dvs_solver_create`; `DVS_COMPILE_NOMEM` now means memory ran out.
      Backgrounder §4.1 updated in the working tree (it is untracked).

**Gate P3:**
- [x] Full suite on Linux. **Windows/MSVC CI not run** (needs a push).
      The new code uses no GCC builtins; the parenthesised
      `(dvs_pool_ptr)` definition and the `<sanitizer/asan_interface.h>`
      include (guarded by `__SANITIZE_ADDRESS__`) are the parts to watch.
- [x] Verilator capture fixtures and the Verilator-protocol tests pass
      (in the suite).
- [x] `dvs_solver_create(buf, small, NULL)` returns `DVS_COMPILE_NOMEM`
      for a problem that does not fit (`test_growable_tables.c`).

---

## P4 — Remaining fixed buffers

- [x] LCG learnt-clause arena (fixed 4 MiB malloc) moved onto a
      `dvs_spool_t` on the context's allocator, so learning no longer
      stops when it fills. Clauses and watch entries are held by pointer;
      spool segments never move, so they stay valid. `lcg_reset` releases
      the arena to empty. `clause_db_gc` only marks clauses inactive (no
      compaction), so it is unaffected. Test: `tests/c/test_clause_arena.c`
      (120,000 clauses, ~16 MiB of arena).
- [x] Python `SolveProblem` (fixed 64 KiB): *changed from the plan.* It
      stays on the raw problem API and grows instead: a call that fails
      because the buffer is full copies the problem (offset-addressed, so
      relocatable) into a buffer twice the size and retries. Rebuilding it
      on `SolveProblemBuilder` would have switched it to the builder's
      SystemVerilog sizing (B38) and changed what existing problems mean,
      `optimize`'s bound constraints included. `buf_size` is now just the
      initial size (default 4 KiB). It refuses to grow once a context has
      compiled the problem. Test: `tests/unit/test_problem_grows.py`.
- [x] Gate: full suite and perf (Log).
- [x] *Found by the perf gate and fixed:* with the segmented pool, every
      hop of a hole-list walk decoded an offset through the slot table,
      doubling the cost of exclusion-heavy work (`mempartitionknapsack`
      3.7 → 6.7 ms; `x inside {...}` holes on the randomize path). The
      links are now pointers (pool memory never moves): 3.1 ms.

## Follow-ups (started 2026-10-12)

Worked in this order: correctness first, then fixed limits a hosted user
can hit, then memory, then performance.

- [x] **B116** (logged in the bug backlog): a duplicate `declare-fun` of a
      sort-typed constant was accepted. Pre-existing; it showed as
      `test_parser_fuzz[21]` failing because the B83 corpus change shifted
      that seed's mutation. Fixed: sort-typed declarations are checked for
      redeclaration and scoped by push/pop.
- [x] **Checkpoint depth.** `MAX_CHECKPOINTS 32` was a fixed array in the
      context header, and the SMT2 front end kept 17 parallel `[32]` push
      arrays, so a 33rd nested `(push)` was an error. Now:
  - [x] `ctx->checkpoints` is an array from `ctx->alloc`, doubled on
        demand, up to `DVS_MAX_CHECKPOINTS` (trail entries record their
        level in 16 bits). A fixed-pool context carves 32 at create and
        keeps that limit (`DVS_FIXED_CHECKPOINTS`).
  - [x] `level_marks` and `decisions` are one block of `level_cap`
        entries. A checkpoint reserves its own level plus a full search
        above it (`dvs_ctx_reserve_levels`).
  - [x] `MAX_DECISION_DEPTH` is now counted from the search's root
        (`max_depth = search_base + MAX_DECISION_DEPTH`), not from level 0.
        Before, each open checkpoint took a level from the search. A solve
        with no checkpoints is unchanged.
  - [x] `scope_log_depth` widened from 8 to 32 bits.
  - [x] SMT2: one growable `Smt2PushFrame` array replaces the parallel
        arrays. A `(push N)` count can no longer wrap the depth check.
  - [x] Tests: `tests/c/test_deep_checkpoints.c` (1,000 nested checkpoints
        with pins; 240 decisions under 32 fixed-pool checkpoints, which the
        old absolute limit refused), conformance `ok_push_deep.smt2`, and
        `test_dpi_shim.py::test_deep_checkpoints_then_solve`.
- [x] **B117:** incremental SMT2 sessions held O(hand-offs × problem
      size) memory. `_flush_aux` copied the whole retained builder for each
      hand-off and kept every copy for model validation. Now:
  - [x] A context reads a problem only during `dvs_solver_compile` /
        `dvs_solver_add_constraint`. Verified before relying on it: a full
        suite run with every problem copied, compiled from the copy, and
        the copy poisoned on return showed no failure;
        `tests/c/test_problem_lifetime.c` frees each problem right after the
        call and runs under ASan. `DVS_PROBLEM_HELD` is gone, the API docs
        say "read only during the call", and a Python `SolveProblem` may
        grow after it was compiled.
  - [x] While retained, the front end keeps one finalized copy of the
        builder (`vproblem`), brought up to date with
        `dvs_builder_finalize_append` (new bytes only). A hand-off gives the
        context the items since `aux_mark` by cutting its lists in place
        (`dvs_problem_cut_since` / `dvs_problem_uncut`), so it costs
        O(new items), not a full copy. The `aux_problems` entry is NULL;
        `n_aux_problems` keeps its meaning for the routing checks.
  - [x] Validation checks `vproblem`'s items since the compile. When
        retention ends (the 16 MB cap), `vproblem` is frozen as `vbase`, and
        each push frame records its constraint count so a pop trims it.
  - [x] 4,000 rounds of `assert; check-sat`: 447 MB → 6.6 MB, 1.02 s →
        0.74 s. 10,000 nested pushes with an assert each: 2.76 GB → 13 MB,
        1.14 s → 0.09 s. Same answers.
- [x] **B118** (found writing the lifetime test): an incremental add read
      a "new" variable whose id the compile had taken for an internal one
      as that variable — a wrong `unsat`, also on f04af10. Variables a
      problem declares are now marked (`VAR_DECLARED`); an add re-declaring
      any other id is `DVS_COMPILE_BAD_VAR`. New `dvs_solver_n_vars()` /
      `SolveCtx.n_vars`: number new variables from here.
- [x] **B119** (found by fuzzing the end of retention with the new
      `DV_RETAIN_MAX_BYTES` override): a refused hand-off (a variable wider
      than 64 bits) still ended retention and reset the builder, and the
      refused add had half set up its variables; later adds over them read
      zeroed slots. Wrong `unsat`s, also on the old code. A refused hand-off
      now keeps the builder; an add checks widths and that every name
      exists before changing anything.
- [x] `MAX_NOOVERLAP2D_RECTS 64`: the propagator is now sized to its
      rectangle count (`_no_size`), up to 65,535 rectangles (16-bit pair
      tracking). Up to 64 rectangles the fire's scratch stays on the stack.
- [x] The LCG's `clauses` pointer array and its analysis scratch
      (`mk_ent`, `undo_*`, `learnt_buf`) come from its `dvs_alloc_t`; the
      undo log is one block.
- [ ] Propagator watcher chains are walked by offset, so each hop pays the
      slot decode. That is the likely source of the ~1.5% left on the
      Verilator-protocol bench versus P0. Linking by pointer, as was done
      for holes, would need the chain unlink in restore to keep each
      propagator's offset.
- [ ] `Propagator.prop_id` is 16 bits; contexts are capped at 65,535
      propagators. Widening it costs 8 bytes per propagator.
- [ ] Raw `malloc`/`realloc`/`free` outside the context: about 490 call
      sites in `src/c/dvs_*.c`. The largest are `dvs_bbsolver.c` (88),
      `dvs_cube.c` (63), `dvs_contradiction.c` (48), `dvs_affine.c` (43),
      `dvs_sv.c` (33) and `dvs_lcg.c` (33). Move them to `dvs_alloc_t` file
      by file. SAT backends (kissat/CaDiCaL) are excluded because they own
      their own memory.
- Not a limit any more: the `Smt2Frontend` `funs[]` table is already
  heap-grown (`SMT2_MAX_FUNS` only caps it).

---

## Log

| Date | Phase | Commit | Notes |
|---|---|---|---|
| 2026-10-11 | — | — | Plan written; design in `docs/dynamic_pool_design.md`. |
| 2026-10-11 | P0 | fda520f | Resize hook, inline decode, pop frees pool. 10,000-scope push/pop through dv-solve-smt2: 55 s, 102 MB, 5,125 bitblast escalations before; 0.54 s, 22 MB, none after. Perf vs f04af10 (`ab_compare.py`, best of 7, N=300): Verilator-protocol 0.922, API 0.854, fixtures 1.000, field 0.747. A/A noise 1–5%. Suite: 30 failures, all also on f04af10. |
| 2026-10-11 | P1 | cbf67fc | `dvs_spool_t`; ctx pool switched over. |
| 2026-10-11 | P2a | 21fdc95 | Growable tables; NOMEM on overflow; 65,535-prop cap. Fixed B83 as a side effect. |
| 2026-10-11 | P2b | f830dda | Context-owned memory through ctx->alloc; NULL-ba fixed mode works. Memory, 1,000-var sum problem via Python: f04af10 cannot compile it in the default 1 MiB context (NOMEM) and needs ≥2 MiB (+3.8 MB peak RSS); P2 compiles from a 64 KiB buffer (+2.0 MB). dv-solve-smt2 peak RSS 93.4 → 84.2 MB. |
| 2026-10-11 | P3 | a84ee8f, 9eb8649 | `dvs_solver_new`; front ends switched. ASan + always-move full suite: zero memory-error reports. |
| 2026-10-11 | P4 | 6d1caa1, c6fb271 | Clause arena on a spool; SolveProblem grows; hole lists by pointer. Final perf vs f04af10: Verilator-protocol 0.935, API 0.876, fixtures 0.946, field 0.766. vs P0: 1.015 / 0.997 / 0.988 / 1.024 (noise). Final suite, SHIFT=16 and SHIFT=8 identical: 9,934 passed, 31 failed = the 30 baseline failures + `test_parser_fuzz[21]` (B116, pre-existing). |
| 2026-10-12 | F | de3a3f8 | B116: sort-typed redeclaration rejected; sort tables scoped by push/pop. `test_parser_fuzz[21]` passes again. |
| 2026-10-12 | F | f977c28 | Checkpoints and SMT2 pushes nest past 32 (growable stack and level arrays; search depth counted from its root). |
| 2026-10-12 | F | 4e89bc6 | NoOverlap2D sized to its rectangle count (was fixed at 64). |
| 2026-10-12 | F | de07a9c | B117 (incremental SMT2 memory: 447 MB → 6.6 MB on 4,000 rounds; 2.76 GB → 13 MB on 10,000 nested pushes), B118 and B119 (wrong `unsat`s in incremental adds, pre-existing). Problems are read only during compile/add (poisoned-problem suite run: no failures). Perf vs de3a3f8: Verilator-protocol 0.992, API 1.000, fixtures 0.96–1.02 (A/A 0.967), field 1.004. Full suite: 26 failures, all in the P4 baseline list; 5 baseline failures now pass. 2,000 session-fuzz seeds clean at retention caps 1 KB, 4 KB and default. |
| 2026-10-12 | F | ee92fb4 | LCG scratch from its allocator; ASan + leak check clean. |
