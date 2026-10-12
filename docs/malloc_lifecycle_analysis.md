# Malloc lifecycle analysis

2026-10-12, at 65acd8d (main) plus the fixes listed under "Bugs found". Covers every raw
`malloc`/`calloc`/`realloc`/`free` in dv-solve's own C sources, about 280 allocation and 390
free call sites. It excludes the bundled SAT solvers (kissat, CaDiCaL), which manage their own
heaps.

Follows [dynamic_pool_plan.md](dynamic_pool_plan.md): the solver context already allocates
through `dvs_alloc_t` and no longer appears in these numbers except at the edges. The
per-file site tables are in the appendices:

- [malloc_survey/smt2.md](malloc_survey/smt2.md): SMT-LIB front end, parser, evaluator, oracle hook.
- [malloc_survey/bitblast.md](malloc_survey/bitblast.md): bit-blaster, AIG, CNF, simulation.
- [malloc_survey/cube.md](malloc_survey/cube.md): cube-and-conquer, costguided, oracle, SAT seam.
- [malloc_survey/core.md](malloc_survey/core.md): compile, SV elaboration, builder, DPI,
  contradiction analysis, the allocators themselves.

## Bottom line

1. **What the allocator costs today.** On the perf surfaces it is about 1–1.5% of solver CPU,
   and 1.6% of wall time was all that removing 72% of the calls bought:
   - On the Verilator-protocol surface, `malloc`/`free` and their internals take 1.3% of
     solver-process CPU. `memset`/`memmove` take another 1.7%, partly zeroing fresh
     allocations.
   - Linking jemalloc instead of glibc malloc made no difference (swizzle 1.010).
   - Cutting the allocations per randomize from 957 to 269 bought 1.6% (swizzle 0.984). That
     change removed one per-constraint malloc that was 72% of all calls.
   - The C/Python API solve loop makes about one allocation per solve and is unaffected.
2. **Where allocation does hurt, it is not the price of a call.** The problems are lifetimes
   that don't match the data:
   - **Leaks and unbounded growth.** The s-expression arena leaked every block past its first
     on each `(reset)`: about 80 KB per Verilator randomize, now fixed (B120). Fixing it
     uncovered a dangling rewrite cache that was giving `unknown` (B121). Separately, a session
     with many pushes and pops keeps its deep copies until reset.
   - **Rebuilt-and-zeroed O(N) structures.** Each Verilator randomize callocs 1.3 MB for a
     bit-blast memo sized at 16 bytes per problem-pool byte. The CNF encoder frees and
     re-zeroes its visited bitmap on every top-level assertion (O(K·N)). The diversity pass
     rebuilds 10 node-sized arrays per SAT result on an AIG that has not changed. Soft reset
     frees and regrows about 17 tables and memsets a 180 KB struct per randomize.
   - **Allocator-contract bypass.** About half the bit-blast engine, the builder's block index,
     `dvs_stack`'s control block and everything in the SMT2 front end use raw libc. A custom
     `dvs_alloc_t` (embedded, accounting, the ASan always-move mode) cannot see those bytes.
   - **Out-of-memory paths.** The survey found two wrong-UNSAT paths, a wrong-model path and
     three memory-safety bugs, all fixed below.
3. **Plan.** Don't migrate all ~490 call sites mechanically. Add three lifecycle-matched
   pieces: a scratch stack, a per-solve arena, and push-frame marks on the existing pools. Use
   them where the measured counts and bytes are, and route the remaining long-lived owners
   through `dvs_alloc_t` for visibility. Proposals P1–P9 below are ranked by measured or
   estimated effect.

## Method

- **Static survey.** Four agents, one per subsystem, read every call site, traced its free,
  and classified its lifecycle (patterns below), frequency and fitting abstraction. The
  appendices hold their tables.
- **Dynamic trace.** `tests/perf/alloc_trace/alloc_trace.c` is an `LD_PRELOAD` shim. It keys
  every allocation by its call stack and records, per site: count, bytes, reallocs, peak
  live blocks, and log2 histograms of block lifetime and request size.
  - `alloc_report.py` symbolizes each stack to its first dv-solve frame. It labels
    allocations made inside a bundled SAT solver `[in sat] <caller>`.
  - `run_workloads.py` runs the four `ab_compare.py` surfaces under the shim.
  - The build needs `-g` and no strip: `cmake -DCMAKE_C_FLAGS=-g -DCMAKE_STRIP=/bin/true`.
- **Time share.** `perf record -g` on each surface (`build-alloc`, Release + `-g`), attributing
  allocator and `mem*` samples to their dv-solve caller. Kernel time was not sampled
  (`perf_event_paranoid` 2); `perf stat` gave sys time and page faults instead.
- **Ceiling test.** The same surfaces, unchanged binary, under `LD_PRELOAD=libjemalloc.so.2`.

## Measurements

### Allocations per operation

| surface (`ab_compare.py`) | operation | allocs/op at 65acd8d | allocs/op now | dominant sources at 65acd8d |
|---|---|---:|---:|---|
| swizzle | one Verilator `randomize()` | 957 | 269 | `collect_substs_from` 684 (one per constraint per solve, 64 ns lifetime); bit-blast chunks 55 (4 KB); kissat clause buffers 57; parser 27; frontend 22 |
| fixtures | one `dv-solve-smt2` run | 279 | (not re-measured) | kissat 80; frontend 38; parser 26; bbsolver 23; affine check 20 (672 KB) |
| api | `reset` + `solve` via `SolveCtx` | 1.2 | — | Python itself (96%); dv-solve 0.05/solve |
| field | one `field_bench` randomize | 0.7 | — | Python itself |

Bytes allocated per Verilator randomize at 65acd8d: 2.4 MB. Of that, 1.34 MB is the bit-blast
memo (`dvs_bbsolver.c:1761`, 16 B per problem-pool byte, calloc'd), 219 KB is bit-blast chunks
and 180 KB is frontend tables.

### Time

| surface | malloc/free CPU (solver process) | memset/memmove | sys time | page faults |
|---|---:|---:|---:|---:|
| swizzle | 1.27% | 1.72% (0.84% one memset in the bit-blast check-sat body) | 1.8% | 17 per randomize |
| fixtures | 1.35% | 2.79% (mostly inside kissat) | — | — |
| api | 0.17% (Python) | — | — | — |

| allocator ceiling (best of 5, interleaved) | glibc | jemalloc | ratio |
|---|---:|---:|---:|
| swizzle total (11 benches × 200 randomizes) | 5372 ms | 5426 ms | 1.010 |
| fixtures (98 one-shot runs) | 144 ms | 184 ms | 1.276 (jemalloc's startup cost) |

| change (A/B vs 65acd8d, best of 5) | swizzle | fixtures |
|---|---:|---:|
| this pass: B120/B121 + `collect_substs_from` stack (957 → 269 allocs/randomize) | 0.984 | 1.009 (noise) |

So malloc calls themselves are cheap here. A faster allocator wins nothing, and removing 72% of
the calls wins 1.6%. The larger lever is bytes: 1.34 MB of zeroed memo and the O(N) rebuilds
listed in P1–P4 are memory traffic on every randomize. That traffic shows up as `memset` and
cache misses rather than in `malloc`.

## Lifecycle patterns

| pattern | what it is | where it shows up (examples) | fits |
|---|---|---|---|
| **SCRATCH** | allocated and freed in one call, LIFO | `collect_substs_from` / `bb_binary` AND-OR stacks; `_try_translate_in_set` 16 KB per assert; deep-copy worklist 1 KB per declaration; `_add_names_known`; `dvs_sv_rewrite_masks` 3 tables + 16 KB memo per add; parser frame stack per command | C-stack buffer with heap fallback for small bounded cases; otherwise a **scratch stack** (mark/release, non-moving, large requests allowed) |
| **PHASE** (aggregate and dump) | many allocations freed together at the end of a phase | bbsolver object graph (~30 allocs per check-sat, n+2 per MaxSAT); affine check (~30 tables freed at one label); evaluator per check-sat; `_cmd_alloc` per command; diffcycle, field split, shape walk per compile | a **per-phase arena** reset at the phase boundary (or one sized block when the sizes are known up front) |
| **SCOPE** | freed in LIFO order on pop/restore | abstract-array nodes (one calloc each, freed one by one on pop); deep copies of assertions (truncated on pop but never reclaimed); `:named` strings; hole carry per restore | `dvs_spool_t` with a **mark per push frame** (stable addresses, one release per pop) |
| **VECTOR** | growable array | nearly all tables. Every SMT2 and core vector doubles. Exceptions: `bb_grow_vars` regrows five arrays to exactly `need` (O(V²) on incremental lemma adds); `dvs_arena`, `dvs_aig`/`aig_cnf` `xrealloc` and the builder index use alloc+copy+free instead of `DVS_RESIZE` | `DVS_RESIZE` with geometric growth; keep capacity across resets |
| **OWNED** | long-lived, freed in destroy | contexts, solvers, handles | `dvs_alloc_t` for visibility; no pooling needed |
| **CHURN** | short-lived allocation inside a hot loop | `collect_substs_from` (72% of Verilator calls, fixed); NoOverlap2D past 64 rectangles on every propagator fire (fixed); aig_cnf visited bitmap per top-level assertion; `_array_refine` per CEGAR round; cube child literal arrays (cross-thread frees) | depends: C-stack, retained buffer, or a fixed-size slot free list |
| **REBUILD** (a CHURN variant worth naming) | an O(N) structure rebuilt and zeroed per call though its input did not change | 1.34 MB bit-blast memo per randomize; `_diversify` L/R/fanout CSR per SAT result; soft reset's 17 tables + 180 KB memset; affine L/R/xa/xb built twice per solve | cache across calls, keyed on what it depends on |

The solver's decision/propagation loop is malloc-free. The one exception, NoOverlap2D's energetic
scratch past 64 rectangles (introduced in 4e89bc6), now lives in the propagator. Trail entries
come from the block-cached `dvs_stack`, value holes from the context spool, and learnt clauses
from the LCG spool. LCG vectors grow geometrically.

## Existing abstractions, and what is missing

| abstraction | lifetime it serves | gaps |
|---|---|---|
| `dvs_alloc_t` (`DVS_ALLOC`/`RESIZE`/`RELEASE`) | any; the visibility layer | about half the engine bypasses it |
| `dvs_spool_t` | SCOPE: segmented, stable addresses, mark/release, objects over 64 KB get their own slot | none for SCOPE use; it is the natural scratch stack too |
| `dvs_stack_t` | SCOPE for small records (the trail) | a request larger than one 64 KB block fails, so not a general scratch stack; its control struct uses raw `malloc` |
| `dvs_arena_t` | PHASE, offsets stable, pointers move | **no production user**; grows by alloc+copy+free, not `DVS_RESIZE`. Adopt it for PHASE work after fixing growth, or delete it |
| `SexprArena` (SMT2 parser) | PHASE per command, plus session copies | reset reuse was broken (B120); no marks, so push/pop cannot reclaim |
| `dvs_bitblast` chunk arena | PHASE per bbsolver | fixed 4 KB chunks: 55 mallocs per randomize; should grow geometrically |

What is missing is small:

1. A **scratch stack** owned by the context and by the SMT2 front end: a `dvs_spool_t` used
   with mark/release around compile, add, check-sat and the translation helpers.
2. A **per-solve arena** for the bbsolver object graph, pointer-stable. The bit-blast chunk
   arena generalized, or a `dvs_spool_t`.
3. **Push-frame marks** on the SMT2 front end's session pools.

## Proposals, ranked

Each lists the lifecycle, the sites (see the appendices), and the expected effect.

| # | change | pattern | effect (measured or estimated) |
|---|---|---|---|
| P1 | **Bit-blast memo sized per node, not per pool byte** (`dvs_bbsolver.c:1761`, plus `uses`/`fo_seen` at 770). A dense node index or hash. | REBUILD / footprint | removes 1.34 MB of calloc+zero per Verilator randomize and ~16× memory on large problems. Largest byte item measured |
| P2 | **Soft reset keeps capacities** (`smt2_frontend.c` soft reset): keep `{ptr,cap}` of the ~17 tables, zero counts and hash tables only; move the cold inline arrays out of the 180 KB struct | REBUILD per randomize | 15–30 malloc/free and 60–250 KB of zero-fill per randomize (survey estimate) |
| P3 | **AIG snapshot on the bbsolver**: L/R children and fanout CSR built once per AIG size and shared by `_diversify` and `dvs_aigsim`; `node_val` kept as one buffer with a valid flag | REBUILD per SAT result | removes about 10 O(N) arrays per SAT result in the cached Verilator, cube and rediversify paths |
| P4 | **aig_cnf visited bitmap persistent**, cleared by touched list or epoch stamps (as `reach_begin` does); `leaves` onto the persistent stack | CHURN, O(K·N) | algorithmic: K top-level assertions × N/4 bytes of memset, e.g. about 50 MB for a 64-var alldiff on a 100k-node AIG |
| P5 | **Context scratch stack** (`dvs_spool_t` + mark/release) for compile/add: `dvs_sv_rewrite_masks` tables and memo, `_add_names_known`, the AND-spine stack, diffcycle, field split, shape walk; skip the mask-rewrite setup when the new roots have no masks | SCRATCH / PHASE per add | 6–8 malloc/free pairs and ≥16 KB zeroing per Verilator add; also avoids a full problem copy per add when a mask rewrite fires (`dvs_sv.c:162`) |
| P6 | **SMT2 front-end scratch and per-command arena**: replace `_cmd_alloc` (512-entry cap, leaks past it) with an arena reset at dispatch; a scratch stack for the 16 KB IN-set accumulator, the deep-copy worklist, let and distinct buffers; one persistent evaluator with reset; a reusable response buffer instead of `open_memstream` | PHASE / SCRATCH per command and per check-sat | 2–3 pairs per assert, 1 per declaration, 4+ pairs and about 85 KB zeroing per check-sat with the model check on |
| P7 | **Per-solve arena for the bbsolver object graph** (~30 allocations per check-sat, n+2 solvers per MaxSAT); geometric bit-blast chunks; route bbword, bbsum and aigsim through `S->alloc`; `bb_grow_vars` doubling with `DVS_RESIZE` | PHASE / VECTOR | fewer calls per solve and full allocator visibility; `bb_grow_vars` stops being O(V²) on lazy-array lemma adds |
| P8 | **Push-frame marks** for abstract-array nodes, assertion deep copies and `:named` strings in the SMT2 front end | SCOPE | bounded memory for push/pop-heavy sessions (today unbounded until reset); one release per pop instead of N frees |
| P9 | **Cube engine**: ring-buffer queue (today it keeps every cube ever pushed, up to about 96 MB, and grows under the shared lock); fixed-size slot free list for child literal arrays (bounded length, cross-thread frees rule out mark/release) | VECTOR / CHURN, threaded | memory, and less time holding the shared lock; per-cube malloc is small next to each CaDiCaL solve |

Also worth doing, smaller: a retained hole-carry buffer per restore (DPI restores on every
solve), builder block recycling across resets, and one allocation for the affine check's ~30
tables shared across its two passes. Contradiction analysis copies the problem once per loop
iteration, but it also builds a solver context each time, which dominates.

**Leave alone:**

- Kissat and CaDiCaL internals: about 6% of Verilator calls, and outside our allocator.
- Worker SAT instances: created on a worker thread, freed on the main one, and CaDiCaL's heap
  can't be redirected.
- Once-per-session tables that already double.
- The oracle and other debug-only paths.

Suggested order:

1. P1 and P2: the largest per-randomize byte savings, and both are local changes.
2. P4: algorithmic.
3. P5 and P6: these introduce the scratch stack, which P7 and P8 then reuse.

Measure each with `ab_compare.py` and `alloc_report.py`, as here.

## Bugs found

The survey turned up these. Fixed ones are in this pass and in `docs/solver_bug_backlog.md`.

| id | bug | kind | status |
|---|---|---|---|
| B120 | `SexprArena` reset leaked every block past the first, re-malloc'ing them each cycle: about 80 KB per Verilator `(reset)`. Over 800 resets peak RSS was 16.9 MB; now flat at 4.6 MB | leak | **fixed** (`smt2_parser.c`) |
| B121 | operator-rewrite cache (rotate, bvnand, bvnor, bvxnor, ...) kept across soft reset but pointing into the reset arena; later declarations overwrote cached templates. 4 of 24 test seeds gave `unknown` at 65acd8d. B120's leak mostly hid it; fixing B120 alone made it frequent | wrong translation (`unknown`, parse errors) | **fixed**: cached templates get their own arena |
| B122 | cube engine: a seed frontier cut short by OOM counted as a complete cover; and if every cube worker failed to start, the undrained queue still returned the provisional UNSAT | wrong UNSAT (OOM / init failure) | **fixed**: frontiers all-or-nothing; undrained queue means unresolved |
| B123 | costguided LNS: under 2 macros the fallback wrote past its buffer, looped forever (1 macro) or divided by zero (0); `new_pos[512]` on the stack overflowed past a neighborhood of 256; designs over 256 macros silently skipped improvement | memory safety / hang | **fixed**: buffers sized from the problem |
| B124 | `dvs_bbsolver_rediversify` rebuilt its base model from the SAT instance even when the model came from simulation, the affine check or a cube/portfolio worker (stale or absent assignment) | wrong model (latent; the cached Verilator path runs with simulation off) | **fixed**: `model_in_sat` flag; diversify from the current model otherwise |
| B125 | contradiction report `APPEND` wrapped `cap - pos` on truncation (heap overflow with a long name or path); NULL dereference when the MUS id array failed to allocate | memory safety | **fixed**: buffer grows; size zeroed on OOM |
| B126 | DPI `dvs_dpi_compile_b64` compiled a decoded blob without checking it holds the header and its pool | out-of-bounds read on malformed input | **fixed** (header check; pool offsets still trusted) |
| — | NoOverlap2D past 64 rectangles allocated on every propagator fire (introduced in 4e89bc6) | perf in the hot loop | **fixed**: scratch in the propagator |

Open, reported by the survey and not yet fixed (all on OOM or diagnostic paths unless noted):

- **Bit-blast**:
  - `check_maxsat` leaves `soft_keep` dangling in the returned solver (latent use-after-free).
  - `aig_cnf` releases its visited bitmap with size 0.
  - Several grow helpers store NULL and leak the old block on OOM (`dvs_aig_cnf.c`, `dvs_aig.c`
    buckets, `dvs_bitblast.c:38`).
  - `bbword` can index past its shapes array after an OOM.
  - Sized-release mismatches in `sess_begin`/`bb_grow_vars`, wrong only for a sized allocator.
- **SMT2**:
  - Model printing for abstract and nested-sparse arrays drops stores silently on OOM, printing
    a wrong array.
  - `_cmd_alloc` past 512 objects leaks permanently.
  - Deep copies are never reclaimed on pop (unbounded growth, P8).
  - The oracle (`smt2_oracle.c:177`) doesn't check a malloc.
- **Core**:
  - `dvs_smt2_emit` ignores map-insert failures, which can change emitted SMT2.
  - `CC_PUSH` OOM in compile reports "uncompiled" rather than NOMEM. This is sound: an
    uncompiled constraint makes the compile incomplete.
  - `dvs_builder_finalize_append` can return a fresh buffer without freeing `prev` (latent).
  - Learnt-clause bytes are not reclaimed within a solve.
- **Cube/oracle**: three unchecked allocations in `dvs_oracle_core.c`; the cube queue's
  unbounded head (P9).

## Tools

```
gcc -O2 -shared -fPIC -o alloc_trace.so tests/perf/alloc_trace/alloc_trace.c -ldl -lpthread
cmake -S . -B build-alloc -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-g -DCMAKE_STRIP=/bin/true
cmake --build build-alloc --target dv-solve-smt2 dv_solve
env -u LD_LIBRARY_PATH python tests/perf/alloc_trace/run_workloads.py \
    --build build-alloc --shim alloc_trace.so --out /tmp/at --n 50
python tests/perf/alloc_trace/alloc_report.py /tmp/at/swizzle --calls 550 --top 40
```

`--calls` is the operation count `run_workloads.py` prints, for per-operation rates. The report
columns are: calls (allocs + reallocs), reallocs, bytes, median request size, median block
lifetime, peak live blocks, then the site and its top caller.
