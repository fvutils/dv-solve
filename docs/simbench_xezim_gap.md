# Where dv-solve standalone loses to xezim (2026-11-05)

Data: the three valid runs (2026-11-04 nightly at 1dfa5e8, 11-05 nightly at
9760e23, 11-05 reference at 385c27a), Verilator 5.054, xezim 0.11.3. 93
benches ran on both; dv-solve standalone is slower on 18, about 2x faster on
most of the rest. Geomean per bench over the three runs:

| bench | dvs µs | xezim µs | slower | entropy dvs / xezim |
|---|---:|---:|---:|---|
| arith.mul_div_64 | 301 | 15.0 | 20.1x | 1.00 / 1.00 |
| memmap.memmap_tight_32 | 14,590 | 819 | 17.8x | 0.21 / 1.00 |
| memmap.addr_map_64 | 2,696 | 272 | 9.9x | 0.13 / 1.00 |
| memmap.soc_addr_map_40 | 2,312 | 269 | 8.6x | 0.13 / 1.00 |
| scenario.bus_transaction | 84 | 11.9 | 7.0x | 0.26 / 0.26 |
| memmap.soc_addr_map_32 | 1,686 | 276 | 6.1x | 0.15 / 1.00 |
| dist.dist_range | 37 | 7.6 | 4.9x | 0.79 / 0.90 |
| memmap.soc_mem_map | 520 | 131 | 4.0x | 0.26 / 0.98 |
| dist.dist_mixed / weight_div / weighted | 28–51 | 8–15 | 3.3x | ≈ |
| dist.dist_if | 28 | 12.3 | 2.3x | 0.13 / 0.10 |
| ethernet_hdr, dma_desc, randc_basic, sum_with, wide_add_64, array_sum | | | 1.06–1.6x | |

## 1. What xezim does (from its source)

xezim's source is public, Apache-2.0: github.com/aionhw/xezim (058f2bba) and
aionhw/xezim-core (48820a89), matching the xezim-bin 0.11.3 build info. Its
`Cargo.lock` has no SAT, SMT or BDD crate. The solver is in
`src/compiler/simulator/`:

- **Trial loop** (`constraint_solver.rs`, `exec_randomize_solve`, up to 1000
  trials). Each trial:
  - draws every rand variable uniformly from its `inside`/relational intervals;
  - treats `x == expr` as an assignment, evaluated once `expr`'s inputs are drawn;
  - samples coupled linear constraints one variable at a time
    (`solve_coupled_affine`, `inline_constraints.rs:2953`), each uniformly
    within the bounds implied by the variables already fixed, with i128
    arithmetic and a reshuffled order on a dead end;
  - repairs `dist`, `inside`, `if` and soft constraints, then checks
    everything with the ordinary evaluator. A failure retries.
- **Fallback** (`rand_csp.rs`): interval-list domains, bounds propagation
  to a fixpoint, randomized DFS with a 40k-node budget. It is used first only
  for `solve…before`, `dist` under `if`, or a `dist` variable shared with
  another constraint.
- **`dist`:** the bucket is dealt from a per-object smooth weighted
  round-robin with random swaps, then a value is drawn uniformly in it.

The design is fast because the common case costs one draw, one evaluation and
one check. It is **not** uniform over the joint solution space: sequential
sampling gives early variables wide ranges and squeezes later ones.

## 2. What our "entropy" measures, and what it misses

`metric.py:entropy` is the per-field fraction of distinct values, normalised
by log N, minimised over fields. It catches stuck or sticky fields, which is
dv-solve's real memmap problem (below). It cannot see that xezim's
memory-map samples are skewed. So the memmap 1.0 vs 0.13 overstates xezim's
quality, though not dv-solve's deficit.

→ Add a joint-uniformity check (chi-square over enumerated solutions) on a
few small benches whose solution count is tractable, alongside the existing
metric. This needs a nightly-plan item (S4 or later).

## 3. Why dv-solve loses (profiled)

Setup: Release `build/`, `bench_vlt_replay <session> --passes api-cache
--mode default --calls 210 --warmup 10 --quality` (the CI's arguments),
`perf record --call-graph dwarf`. Scratch captures and logs are not kept.

| bench | default (bitblast) | experiment |
|---|---|---|
| memmap_tight_32 | 14,750 µs / 0.21 | `--engine cdcl`: **6,697 / 0.999** |
| soc_addr_map_40 | 2,419 / 0.13 | cdcl: **928 / 1.00** |
| soc_addr_map_32 | 1,676 / 0.15 | cdcl: **324 / 1.00** |
| soc_mem_map | 881 / 0.26 | cdcl: **193 / 0.99** |
| addr_map_64 | 2,784 / 0.13 | cdcl: unchanged (falls back, "wide-arith") |
| mul_div_64 | 329 / 1.0 | `DV_PINFAST=0`: **175** |
| bus_transaction | 64.6 / 0.259 | `DV_ENUMSAMP=0`: **10.1** (xezim 11.9) |
| dist_range | 40.8 / 0.786 | cdcl: 110 (worse) |

### memmap: routed to SAT search, which is also the entropy bug

- **Routing.** `_engine_is_bitblast` (`src/c/smt2/smt2_frontend.c:6067`) sends
  any `QF_ABV` problem to bitblast. Verilator always declares `QF_ABV`, even
  with no array terms, so the word-level CDCL engine never sees these.
- **Hot path.** `dvs_bbsolver_resample` → `_fresh_base`
  (`src/c/dvs_bbsolver.c:5022`). None of the fast samplers (popsamp,
  uniqsamp, sortsamp, pinfast, divsamp) accepts the shape, so every call
  ends in `_sat_fresh` (:4992): a new kissat instance over about 50k
  clauses, 73% of the time in `kissat_search_propagate`.
- **Entropy.** A kissat model is not a sample; phase heuristics push the
  sizes to their bounds (`r11_size` at its maximum in 163 of 210 models).
  `_diversify_sweeps` (:2794) flips single bits, and any flip of a size bit
  breaks the `end = base + size` / running-sum equalities, so it is
  rejected and the sizes stay put.

### mul_div_64: two whole-AIG passes per call

- `_pinfast` (:8605) runs a three-valued pass over all 61,557 AIG nodes and
  always fails (forced=0). It is 47% of the time, and allocates three
  N-sized arrays per call.
- `_divsamp` (:7235) then succeeds on its first try, but re-evaluates the
  whole AIG (`_eval_ands`) to check it.
- xezim does the obvious thing: draw `a`, `b`, compute `p = a*b`, check
  `p/b == a` by evaluation.

### bus_transaction: a failed table build, rebuilt on every miss

About 10% of calls miss in divsamp and fall to `_enumsamp` (:8532). Each
miss rebuilds the enumeration table, hits `DVS_EN_MAX_NODES` (524k) and
declines, at about 0.5 ms per build. 74% of that is in `bb_eval`
recomputing `bb_self_width` (:466) and `bb_signed` (:414) recursively. The
decline cached in `en_state` (:8543–8552) is not reused; which `_en_key`
(:8015) input changes between calls is not yet pinned down. The entropy is
already at the ceiling for a 2-bit field.

### dist: a new context per call

Verilator implements `dist` itself. It picks the bucket and value, then
asserts `a == const` under `push`, with two check-sats per call. The
constants change every call, so every call compiles a new bbsolver: 49% in
`_bb_encode`, 6% each in context new/free and `_pinfast`, 22% in the
harness's own rebuild, 0.1% in SAT. On a reused context the same call costs
3.8 µs. The dist entropy gap (0.79 vs 0.90) is **Verilator's** bucket
draw, not dv-solve's; the values arrive pinned.

## 4. Improvements, ranked

| # | change | benches | expected | effort |
|---|---|---|---|---|
| 1 | Route by content, not declared logic: `QF_ABV` with no array terms (and no `needs_bitblast`) goes to CDCL. Backlog B130. | 4 memmap | 2.2–5x faster, entropy ≈1.0 (measured with `--engine cdcl`) | low |
| 2 | Make an `_enumsamp` decline stick for the context, or refuse on a cheap size estimate before building; cache self-width/signedness per node. | bus_transaction (and any enumsamp miss) | ~6x, below xezim | low |
| 3 | Remember a `_pinfast` miss per context (it is seed-independent); inline the AIG accessors. | mul_div_64, and all bitblast calls | ~2x, +20–30% | low |
| 4 | Reuse the cached context when the problem differs only in `var == const` pins (apply as assumptions); when everything is pinned, evaluate word-level with no AIG/SAT; answer the duplicate check-sat from the first. | all dist | 3–5 µs, below xezim | medium |
| 5 | Functional dependency: detect `x == f(inputs)` and compute `x`; check divsamp candidates word-level (`a*b`, `p/b`), not over the AIG. | mul_div_64, memmap `end` fields, bus_transaction | toward xezim's 15 µs | medium |
| 6 | Lift the `width >= 64` "wide-arith" restriction in CDCL (`smt2_frontend.c:511`). | addr_map_64 | as #1 | medium |
| 7 | Word-level moves in bitblast diversify (hit-and-run along an equality chain), or a sortsamp/divsamp variant for ordered chains with a fixed sum. | memmap when it stays on bitblast | entropy | medium |
| 8 | Order-then-gap sampler for non-overlap: pick a uniform random order of the regions, split the free space (in alignment units) into gaps by stars-and-bars. Exactly uniform for fixed sizes, no search. | memmap, real address maps | speed and *joint* uniformity, beyond xezim | medium |
| 9 | Interval + congruence + known-bits domains in CDCL propagation, values drawn from the strided set. | memmap alignment, mul/div | speed, uniformity | medium |

Items 1–4 are local fixes to existing paths and should close most of the
measured gap. Items 5 and 8 are where dv-solve could beat xezim on quality
as well as speed: xezim's sequential sampler is skewed on exactly the
memmap shape.

## 5. Literature behind items 5–9

- Naveh et al., IBM Genesys/X-Gen, "Constraint-based random stimuli
  generation", AI Magazine 2007 (interval domains and randomized value
  choice; the xezim fallback design).
- Peled, Rothenberg, Itzhaky, MeGASampler, FM 2023 (arXiv 2212.06472):
  from one model, take the box of intervals around it that still satisfies
  the constraints, and sample inside the box. This is the general form of #7.
- Dutra et al., QuickSampler (ICSE 2018) and SMTSampler (ICCAD 2018):
  combine neighbouring solutions to get many samples per solve.
- Golia et al., CMSGen (FMCAD 2021): randomized CDCL polarity and branching
  for cheap near-uniformity.
- Niemetz, Preiner, Biere: propagation-based local search with inverse values
  (FMSD 2017); Niemetz, Preiner, Zohar: lazy mul/div abstraction in
  bit-blasting (CAV 2024). Both fit mul_div_64 if #5 is not enough.
- Navas et al., wrapped intervals (APLAS 2012); Vishwanathan et al.,
  tristate numbers (CGO 2022): domains for #9.
- Yuan et al., BDD-based uniform sampling (TCAD 2004); WAPS (TACAS 2019).
  Exact, but only for small partitioned subproblems.

## 6. Both front-ends: SMT (Verilator) and the C API

What was measured above is already the API path. `bench_vlt_replay --passes
api-cache` rebuilds each captured check-sat through `dvs_builder_*` and solves
with `dvs_solver_*` or `dvs_bbsolver_*` (`tests/c/bench_vlt_replay.c:17-25`,
`923-950`). It reuses the compiled context when a rebuilt problem is
byte-identical to an earlier one (`:976-988`). It does not choose the engine
itself: it takes the SMT front-end's choice (`cp->bitblast`, `:926`).

API users differ in how they pick an engine:

| caller | engine choice | context reuse |
|---|---|---|
| Python `SolveCtx` (pyvsc/zuspec), DPI | CDCL only; `CompileIncompleteError` if it can't compile (`src/dv_solve/ctx.py:63`) | compile once, reset+solve |
| Python `BVSatCtx` (`bvsat.py`) | bitblast, chosen by the caller | resample |
| `dvs_handle_*` / `dvs_randomize` | AUTO via shared `dvs_shape_route` (`dvs_shape.c:201`): any multiply or bit-level shape → bitblast; CDCL error → bitblast for good (`dvs_handle.c:285`, `:305`). STATE/RANDC stay on CDCL (bitblast has no pins). | resample with presim (`:247-261`) |

Where each fix lives:

| # | SMT front-end | API front-end | put the fix in |
|---|---|---|---|
| 1 routing | declared-logic check `_engine_is_bitblast` (`smt2_frontend.c:6067`); the content classifier `_route_qfbv` (`:6149`) runs only for one-shot QF_BV | SolveCtx: n/a (always CDCL). Handle: already content-routed. | shared `dvs_shape_route`: SMT uses it for QF_ABV without array terms; tune that one rule (orderings and equality chains → CDCL) so the handle gains too |
| 2 enumsamp decline | core (`dvs_bbsolver.c:8532`) | same code via resample | core |
| 3 pinfast miss | core (`:8605`) | same | core |
| 4 per-call pins | SMT-specific trigger: Verilator does `dist` itself and pins under push | the handle can't use bitblast once it has STATE pins | core: pins as assumptions in `dvs_bbsolver` (`dvs_bbsolver_solve_assuming` exists, `dvs_bbsolver.h:286`). That gives SMT context reuse and lets the handle run pinned problems on bitblast. Only the duplicate check-sat dedupe stays in SMT. |
| 5 functional dependency | not in the parser | needed equally | core: extend `run_subst_pass` / `try_record_subst` (`dvs_bbsolver.c:1514`, `:854`) to compute `x` from its inputs; word-level check in `_divsamp` (`:7235`); a CDCL counterpart in `dvs_compile.c` |
| 6 wide arithmetic | SMT flag at `smt2_frontend.c:511` | the same int64-domain hazard in core CDCL (`dvs_compile.c:852`, `:895`, `:2106`): SolveCtx raises, the handle falls to bitblast | core CDCL: an unsigned 64-bit domain; then drop the SMT flag |
| 7–9 | core (`_diversify_sweeps` :2794, `_fresh_base` chain :5022, `dvs_bvdom.c`, `dvs_search.c`) | same | core |

Only #1 and #4 are triggered by the SMT front-end, and both fixes still belong
in shared code.

### dist in the API

- Declared with `dvs_builder_add_dist` (`dv_solve.h:257`), ranges with `:=`
  or `:/` weights.
- **CDCL** picks the bucket at decision time (`_pick_value_dist`,
  `dvs_search.c:539`). It renormalises the weights over the entries still in
  the propagated domain, then draws uniformly inside the chosen bucket. This
  is cheap, but not exact under coupling.
- **Bitblast drops the weights.** Only membership is encoded
  (`_bb_encode_dists`, `dvs_bbsolver.c:2967-2990`), and the fast samplers
  decline any problem with a dist.
- **Probable correctness bug (not yet reproduced):** `dvs_shape_route`
  ignores dists. A handle problem with a multiply and a `dist` goes to
  bitblast and silently loses its weights. Fix: keep dist problems on CDCL in
  the router, or weight the draw in bitblast. Needs a test first.

### Measuring the API path for these shapes

- **`bench_vlt_replay` passes:**
  - `api` and `api-cache` (what CI times);
  - `buffer` is the real handle API. Varying literals are lifted to STATE,
    which forces CDCL; calls with push add-ons (dist) are excluded and
    counted.
  - `--engine` forces an engine.
  - simbench's `replay_pass: buffer` runs the gap benches through handles.
- **Python API arms** (`tests/perf/run_rand.py`, `dv-api` arm,
  `rand_benches.py`): no memmap non-overlap, no 64-bit mul/div, no native
  `dist`.
  - → Add those three shapes to `rand_benches.py`. One definition then gives
    an SMT arm and an API arm, plus exact joint-uniformity checks on small
    instances (§2). Their numbers land in the existing perf history
    (`tests/perf/history/`).

### Order of work, both front-ends in view

1. The dist-under-bitblast test (#4 bug), since it is a correctness issue for
   API users.
2. Shared routing (#1), with the API benches added first, so the router change
   is measured on both paths.
3. Core fixes #2, #3 and pin-assumptions #4.
4. #5 and #6 in the core, then #7–9.
