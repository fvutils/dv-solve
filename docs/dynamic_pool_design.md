# Block-grow dynamic allocation for the solver context

Status: design review, 2026-10-11. Nothing here is implemented yet.
Tracking plan: `docs/dynamic_pool_plan.md`.

## 1. Summary

The backgrounder (`docs/dv_solve_internals_backgrounder.md` §4.1) says the
solver context lives in "a single buffer that the caller supplies (1 MiB for
Python, 2 MiB for DPI, grown on demand by the SMT-LIB2 tool)". The code bears
that out, and it is worse than the sentence suggests:

- The context pool (`dvs_pool_t`) **cannot grow**. It is one contiguous,
  caller-owned buffer. When it fills, the allocation fails and sets a
  sticky overflow flag. Constraints are then left uncompiled, and the result
  is an escalation, an `unknown`, or a NOMEM error.
- The SMT-LIB2 tool does not grow the pool. It **throws the context away and
  recompiles** into a buffer 2–4× larger (`smt2_frontend.c:4621-4697`). The
  buffer size comes from a size estimate, a "keep 25% free" heuristic, and a
  64 MiB hard cap (`CTX_BUF_SIZE`).
- Per-variable tables are sized at compile time with **33× slack**
  (`VAR_SLACK_FACTOR 32`, `dvs_compile.c:3952`) because they cannot grow
  either. When an incremental add exceeds that slack, about 20 sites return
  `EXPR_NULL` (`n_vars >= n_vars_capacity`), and the constraint goes
  uncompiled.

**Recommendation.** Every hosted platform gets the same scheme, made of two
parts:

1. **A segmented, block-grow pool** for objects addressed by offset
   (propagators, wide bounds, hole entries, dist metadata, diff-cycle heads).
   Each offset maps to a pointer through a small segment table:
   `seg[off >> S] + (off & M)`. Blocks come from the existing
   `dvs_alloc_t` / `dvs_block_alloc_t`. Offsets stay 32-bit and stable, and
   nothing is ever copied.
2. **Growable vectors** for the per-variable and per-propagator tables
   (`vars`, `watcher_heads`, `prop_refs`, ...). Code already reaches these
   through raw `ctx->` pointers, so they can be resized with geometric
   growth through the context's allocator (`DVS_RESIZE`, §4.0), never the
   raw system `realloc`. That removes the slack heuristics and the `n_vars_capacity` cliff.

The caller-supplied fixed buffer stays, but only as an **opt-in embedded
mode**. It uses the same code: the buffer becomes the only segment, and no
allocator is attached.

## 2. What exists today

| Consumer | Problem buffer | Context buffer | Growth |
|---|---|---|---|
| C API (`dv_solve.h`) | builder → exact-size blob | caller's `static_buf` (doc example: 1 MiB) | none; NOMEM on overflow |
| Python `SolveContext` (`ctx.py:27,127`) | `SolveProblem`: fixed 64 KiB (`problem.py:54`); `SolveProblemBuilder` grows | `ctypes` array, 1 MiB default | none |
| DPI (`dvs_dpi.c:68`) | decoded base64 blob, exact size | `malloc(2 MiB)` | none |
| SMT-LIB2 (`smt2_frontend.c:4558-4697`) | builder → blob | estimate × scale, max 64 MiB | destroy + recompile into 2–4× buffer |
| Contradiction / MUS (`dvs_contradiction.c:18`) | exact-size copy + slack | `malloc(1 MiB)` | none |
| LCG learnt clauses (`dvs_lcg.c:77`) | — | `malloc(4 MiB)` arena | none; learning stops when full |

Parts of the system already grow by blocks, and the context pool can reuse
them:

- `dvs_builder_t` uses linked blocks addressed by "virtual offset", then
  finalizes to one contiguous `dvs_problem_t`.
- `dvs_block_alloc_t` (`dvs_block_alloc.c`) hands out equal-size blocks over
  a pluggable `dvs_alloc_t`, with a free-list cache.
- `dvs_stack_t` (the trail) is block-based, which is why search depth is
  not limited by the buffer.
- `dvs_arena_t` (`dvs_arena.h`) is an offset-keyed arena that grows by
  doubling. It was built for a clause arena but is not used by the LCG.
- The bit-blaster's chunk arena (`dvs_bitblast.c:9`) is a chained bump
  allocator.

So the gap is narrow. The **context pool** and the **LCG clause arena** are
the fixed buffers on hosted platforms, plus the Python `SolveProblem` class
for problems.

## 3. Why the context pool is fixed, and what that rules out

Three properties of the current design constrain the fix.

**(a) Offsets are relative to `&ctx->pool`, and `dvs_pool_ptr` is `base + off`.**
Propagators, watch lists (`watcher_heads[v]` → prop offset → next offset),
`holes_offset`, `dist_offsets` and `var_holes_head` all store 32-bit pool
offsets. This part is relocation-safe: offsets survive a move.

**(b) Many raw pointers point into the pool.** `ctx->vars`,
`watcher_heads`, `prop_refs`, `prop_guard_vars`, `prop_constraint_id`,
`var_alias`, `dist_offsets`, `var_holes_head`, `var_n_values`,
`initial_vars`, `level_marks`, `decisions`, phase-save arrays and
`CostGuidedCtx` are all `T *` into the pool (`dvs_ctx.c:101-118`,
`dvs_compile.c:3963-4490`). In addition, `Variable *v = &ctx->vars[i]` is
held across calls throughout compile and search. Because of these
pointers, a `realloc` of the whole pool is unsafe. That is why SMT2
recompiles rather than moving the pool.

**(c) The context header lives in the caller's buffer**
(`dvs_solver_create` returns `static_buf`). The context's own address is
therefore fixed by the caller.

What this rules out:

- **Whole-pool `realloc`, as in `dvs_arena` or `dvs_builder_finalize_append`.**
  It would invalidate (b). Making it safe means converting every raw pointer
  and every held `Variable *` to an index. That is a large, risky audit for
  no added benefit.
- **The builder's addressing.** `dvs_builder` maps a virtual offset to a
  block by binary search over `blocks[]` (`dvs_builder.c:283`). That is fine
  for finalize, but far too slow for the propagation loop.

### Side finding: checkpoints never rewind the pool

`CheckpointMark` (`dvs_ctx.h:53-70`) records `n_props_at_cp` and a stack
mark, but no pool mark. `dvs_solver_restore` resets `n_props` and never
touches `pool.used` (`dvs_checkpoint.c:119-135`). Every propagator compiled
after a `push` therefore stays allocated after the `pop`. A long
push/assert/pop session (yosys-smtbmc, Verilator) grows the pool without
bound until it overflows and escalates. The SMT2 comment that the pool is
"snapshot wholesale for push/pop checkpoints" (`smt2_frontend.c:4618`) is
not accurate. This can be fixed independently of the redesign (phase P0).

## 4. Proposed design

### 4.0 Allocator-level resize

Everything that grows goes through the context's `dvs_alloc_t`, never the
raw system `malloc`/`realloc`/`free`. Embedded users, custom allocators and
the cube workers then see every byte the solver takes. `dvs_alloc_t`
(`dvs_alloc.h`) gains a third hook:

```c
typedef struct dvs_alloc_s {
    void *(*alloc)  (struct dvs_alloc_s *self, size_t size);
    void  (*release)(struct dvs_alloc_s *self, void *ptr, size_t size);
    /* Resize a block from `alloc`. NULL return = failure, `ptr` still valid.
     * May be NULL: DVS_RESIZE then falls back to alloc + memcpy + release. */
    void *(*resize) (struct dvs_alloc_s *self, void *ptr,
                     size_t old_size, size_t new_size);
} dvs_alloc_t;

void *dvs_alloc_resize(dvs_alloc_t *a, void *ptr, size_t old_sz, size_t new_sz);
#define DVS_RESIZE(a, p, o, n)  dvs_alloc_resize((a), (p), (o), (n))
```

- `dvs_malloc_alloc` implements `resize` with `realloc`, so hosted builds
  keep in-place growth when libc can provide it.
- Adding the field at the end keeps existing allocators source-compatible:
  a designated initializer that omits it leaves it NULL, and NULL selects
  the copy fallback.
- `DVS_DEBUG_ALWAYS_MOVE` (§4.2) is implemented in `dvs_alloc_resize`
  itself: it always takes the copy path and poisons the old block. One
  switch therefore covers every growable structure.
- Growing structures store the allocator they were created with, and the
  old size, because `release` and `resize` both take the size.

### 4.1 Segmented pool for offset-addressed objects

```c
#define DVS_SEG_SHIFT  16u                 /* 64 KiB segments (compile-time) */
#define DVS_SEG_MASK   ((1u << DVS_SEG_SHIFT) - 1)

typedef struct {
    uint8_t  **seg;        /* seg[i] = base of the 64 KiB slot i            */
    uint32_t   n_seg, seg_cap;
    uint32_t   cur;        /* slot small allocations bump in                */
    uint32_t   cur_used;   /* bytes used in `cur`                           */
    uint32_t   next_slot;  /* first never-used slot index                   */
    uint32_t   overflow;   /* sticky, as today                              */
    dvs_block_alloc_t *ba; /* NULL => fixed (embedded) mode: never grows    */
} dvs_spool_t;

static inline void *dvs_spool_ptr(const dvs_spool_t *p, uint32_t off) {
    return p->seg[off >> DVS_SEG_SHIFT] + (off & DVS_SEG_MASK);
}
```

- **Small objects** (≤ one segment) bump-allocate in `cur`. When the next
  object does not fit, a new slot is taken from `next_slot`, backed by
  `dvs_block_alloc_get(ba)` with a 64 KiB block size. The tail of the old
  slot (at most one object's worth) is lost. Objects never straddle a slot
  boundary, so a pointer decoded once per propagator visit stays valid for
  the whole visit, as it does today.
- **Large objects** (> 64 KiB, such as a big `AllDifferent`, `SumEq` or
  `DcHead`) take `k = ceil(size / 64K)` consecutive slot indices backed by
  one contiguous allocation, with `seg[j+i] = base + i*64K`. The same
  one-load decode then returns contiguous memory. Small allocations carry on
  in `cur`, and only the slot indices are consumed.
- **Pointers are stable.** Blocks are never moved or copied, and growth
  never invalidates an earlier pointer. Only the segment table itself is
  resized (via `DVS_RESIZE`), and code always reaches it through `p->seg`.
- **Mark/release** is `(cur, cur_used, next_slot)`. Releasing returns
  slots after the mark to `ba`'s free list, where the next push reuses them.
  The checkpoint fix in §3 uses this.
- **Address space.** 32-bit offsets give 65,536 slots (4 GiB), the same
  ceiling as today. A 16 MiB context needs a 256-entry, 2 KiB table, which
  stays in L1.
- **Fixed / embedded mode.** `dvs_solver_create(buf, size, NULL)` maps the
  caller's buffer onto slots `0..ceil(size/64K)-1` (`seg[i] = buf + i*64K`).
  A buffer smaller than one slot is simply slot 0. With `ba == NULL`, an
  overflow is sticky and returns NOMEM exactly as it does now, so embedded
  callers see no change in behaviour.
  An optional build flag, `-DDVS_POOL_SINGLE_SEGMENT`, compiles
  `dvs_spool_ptr` back to `base + off` for MCU targets where the extra load
  matters.

Why fixed 64 KiB slots rather than geometric segments: the decode stays
shift/mask/load with no `clz`, mark/release is trivial, and the block
allocator's free list recycles slots of one size. Geometric segments would
save table entries, but at these sizes the table is tiny anyway.

### 4.2 Growable vectors for the per-variable and per-propagator tables

Move every table that is indexed by var id or prop id out of the pool
and into separately allocated vectors that grow geometrically
(`DVS_RESIZE`, ×1.5 or ×2): `vars`, `watcher_heads`, `prop_refs`,
`prop_guard_vars`, `prop_constraint_id`, `var_alias`, `dist_offsets`,
`var_holes_head`, `var_n_values`, `initial_vars`, phase-save, and
`level_marks`/`decisions`. The last two also remove the
`MAX_DECISION_DEPTH = 256` preallocation.

- One helper, `dvs_ctx_reserve_vars(ctx, n)`, grows every per-var table
  together. It replaces the roughly 20 `n_vars >= n_vars_capacity → EXPR_NULL`
  bail-outs in `dvs_compile.c` with "reserve, or fail on true OOM".
- `VAR_SLACK_FACTOR`, `VAR_SLACK_MIN` and `incremental_capacity_hint` are
  deleted. Today a 1,000-variable problem reserves 33,000 `Variable` slots
  plus matching watcher, alias and dist tables. After the change it reserves
  what it uses.
- **The hazard is a stale `Variable *` held across a call that can grow
  `vars`**, mainly aux-variable creation during compile. Mitigation:
  - Add a debug build mode, `DVS_DEBUG_ALWAYS_MOVE`, that moves every table
    on every reserve. `dvs_alloc_resize` allocates new, copies, poisons and
    releases the old block (§4.0). Run the
    full suite and the fuzzers under ASan with it on. This turns every stale
    pointer into a deterministic use-after-free report instead of a rare
    wrong model.
  - Where an auxiliary count can be bounded, reserve up front: one reserve
    per `_compile_constraint` for the constraint's worst case, so that most
    of compile runs with no growth.

These vectors go through `dvs_alloc_t` (`DVS_ALLOC` / `DVS_RESIZE` /
`DVS_RELEASE`), so embedded builds can still route them to a static arena. They can also be placed in the fixed buffer, in
which case they cannot grow and keep today's fixed limits.

### 4.3 The context header and API

```c
/* Hosted default: everything heap-allocated, grows on demand. */
dvs_ctx_t *dvs_solver_new(dvs_alloc_t *alloc /* NULL = malloc */);
void       dvs_solver_free(dvs_ctx_t *ctx);

/* Embedded / fixed: unchanged signature and meaning. ba == NULL => never grows. */
dvs_ctx_t *dvs_solver_create(void *static_buf, size_t static_size,
                             dvs_block_alloc_t *ba);
```

`dvs_ctx_t` stops being the head of the buffer: the pool becomes a member
holding the segment table, not a trailing flexible region. In
`dvs_solver_create`, the header is carved from the front of `static_buf` and
the rest becomes the segments, so existing C callers keep working. The doc
comment in `dv_solve.h` changes to recommend `dvs_solver_new`.

### 4.4 Front ends

| Front end | Change |
|---|---|
| SMT-LIB2 | Use `dvs_solver_new`. Delete `_ctx_buf_size_for`, `ctx_buf_scale`, the 25%-headroom recompile, the overflow recompile loop and the 64 MiB cap. `pool.overflow` → bitblast escalation (`smt2_frontend.c:6366`) then fires only on real OOM. |
| DPI | Use `dvs_solver_new`. Drop `INTERNAL_CTX_SIZE`. |
| Python `SolveContext` | Call `dvs_solver_new`. Deprecate `ctx_buf_size` (accept and ignore it for one release). The `ctypes` array goes. |
| Python `SolveProblem` (64 KiB fixed) | Deprecate in favour of `SolveProblemBuilder`, which already grows. Make `SolveProblem()` construct a builder-backed object with the same method surface. `ctx.py:426,452` build aux problems with it today. |
| Contradiction / MUS | Use `dvs_solver_new`. Its problem copies are exact-size plus slack and are fine as they are. |

### 4.5 Problems stay contiguous

A finalized `dvs_problem_t` is a serialization format. DPI sends it as
base64, and the context may keep a pointer to it ("`p` must stay valid
until the context is destroyed"). It should stay one contiguous blob. That
is not a fixed-buffer limitation, because the builder grows and finalize
sizes the blob exactly. The two places that append in place after
finalize, the lazy array refinement via `dvs_builder_finalize_reserve` and
elaboration in `dvs_sv.c:156`, already reallocate with offset-relative
addressing. They are safe as long as they run before `dvs_solver_compile`
takes the pointer. Add an assert to enforce that.

### 4.6 LCG clause arena

`dvs_lcg.c:77` uses a fixed 4 MiB arena holding `Clause *` in
`db->clauses[]`, and stops learning when the arena is full. This is a
performance limit, not a correctness one. Two fixes would work: move it
onto `dvs_arena_t`, storing `cref` offsets in `clauses[]` and in watch
entries, which is the purpose `dvs_arena.h` was written for. Or move it
onto the segmented pool from §4.1, which needs no reference rewrite.
The segmented pool is the smaller change.

## 5. Hot-path cost

Today `dvs_pool_ptr` is an **out-of-line function** in `dvs_pool.c`, and
the build has no LTO. Every decode in the propagation loop (watch-list
walk, propagator fire, trail undo) therefore costs a call. The new decode
is `static inline` and costs a shift, a mask, one load from a table in L1,
and an add. It will most likely be **no slower, and possibly faster**, than
today. This is a guess until measured. P1 must benchmark on:

- the Verilator-protocol randomize bench (`tests/perf`, R1–R3). The
  per-solve ~3 µs is the number to protect;
- the easy-band sweep (`sweep_perf.py`), where CDCL routing applies;
- `field_bench.py`.

A cheap first experiment, which is also an independent win: make the
current `dvs_pool_ptr` `static inline` in `dvs_pool.h` and measure it
alone. That isolates the cost of the call from the cost of the segment load.

## 6. Plan

| Phase | Work | Gate |
|---|---|---|
| P0 | Inline `dvs_pool_ptr`; measure. Add a pool mark to `CheckpointMark` and release it in restore (fixes the push/pop leak). | full `pytest -q`, perf history no regression; new test: 10k push/assert/pop cycles hold pool use flat |
| P1 | `dvs_spool_t` with the `dvs_pool_t` API surface; switch `dvs_ctx_t` to it with all 33 `dvs_pool_alloc` call sites unchanged except the type. Compile-time `DVS_SEG_SHIFT` override. | suite + fuzzers at `DVS_SEG_SHIFT=8` (256-byte slots, which forces constant slot turnover and the large-object path) and at 16 |
| P2 | Per-var / per-prop tables → growable vectors; `dvs_ctx_reserve_vars`; delete slack and capacity bail-outs; `MAX_DECISION_DEPTH` → growable. | suite + fuzzers + svcorpus under ASan with `DVS_DEBUG_ALWAYS_MOVE` |
| P3 | `dvs_solver_new` / `dvs_solver_free`; migrate SMT2 (delete grow-retry), DPI, Python, contradiction; `dv_solve.h` docs. | `pytest -q`, Verilator capture fixtures, Windows/MSVC CI |
| P4 | LCG arena onto `dvs_spool_t`; Python `SolveProblem` deprecation. | perf history |

**Soundness.** Today a pool overflow produces a partial compile, which
validation and escalation then have to catch (see B58 and the
`_flush_aux` comment). After this change, the only way to fail an
allocation is true OOM, so that class of partially compiled contexts
disappears from hosted builds. Fixed mode keeps today's sticky-overflow
contract, and the existing tests for it should run against a
`dvs_solver_create` context of deliberately small size.

## 7. Alternative considered: reserve virtual memory, commit on demand

On 64-bit hosts the context could reserve a large address range (`mmap`
`PROT_NONE` / `VirtualAlloc(MEM_RESERVE)`) and commit pages as the bump
pointer advances. The pool would stay contiguous, `base + off` and every
raw pointer would stay valid, and almost no code would change.

It is not recommended as the primary scheme, for these reasons:

- It needs a separate path for WASM, 32-bit hosts, `RLIMIT_AS`-limited
  sandboxes and sanitizer builds. The block scheme is one path everywhere.
- It does not fix the slack-sized per-var tables, since §4.2 is still
  needed.
- It does not plug into `dvs_alloc_t`, which embedded users and the cube
  workers already rely on.

It remains a possible backend for `dvs_spool_t` later (a single huge
segment) if measurement shows the segment load matters. Measurement does
not suggest that yet.

## Appendix: other fixed capacities found in passing

These are outside this change but are the same kind of limit.

- `MAX_CHECKPOINTS 32` (`dvs_ctx.h:51`): push depth.
- `MAX_NOOVERLAP2D_RECTS 64` (`dvs_placement.h:73`).
- `Smt2Frontend` inline `funs[8192]` (see the frontend memset hotspot note):
  already flagged for heap allocation.
- `ORACLE_SESSION_CAP 512 MiB` (`smt2_oracle.c:58`): a debug oracle cap,
  acceptable.
