# Would an ILP solver or a BDD engine help dv-solve? (2026-11-05)

Desk study. It is based on reading the code and the existing docs
(`docs/simbench_xezim_gap.md`, `docs/solver_gap_plan.md`, `docs/soft_constraints.md`).
Nothing was built or measured for it. Section 4 lists the measurement that would
settle the BDD question.

## Short answer

- **ILP: no.** It solves the wrong problem (finding an optimum, not drawing a
  sample). It doesn't match bit-vector semantics, and its per-call overhead is
  milliseconds against a 10–300 µs budget. The useful parts of linear
  reasoning are better added as small in-core pieces (section 1.3).
- **BDD: maybe, as one more exact sampler, not as an engine.** It fits the
  compile-once, sample-many pattern well. It gives exactly uniform samples, and
  exact `dist` weights even when variables are coupled. Nothing here does the
  coupled-`dist` case today. It blows up on the shapes that are slow now
  (64-bit mul/div, wide memory maps), so it does not help close the xezim gap.
  Its value is sample quality on medium-sized components. Whether enough
  components are medium-sized and stay compact is an empirical question
  (section 4).

## 0. What matters here

These are the facts that decide both questions:

- **The job is sampling.** `randomize()` needs a random solution, ideally close
  to uniform, with `dist` weights honoured. Optimisation only appears in soft
  constraints, and SV soft semantics are priority-ordered. The existing greedy
  relaxation already implements that (`docs/soft_constraints.md`).
- **The workload is small and repeated.** simbench is mostly 2–8 variables of
  1–32 bits, solved many times per compiled problem. The largest benches:
  - `memmap_tight_32`: 46 variables of 33 bits;
  - `addr_map_64` and `soc_addr_map_*`: 24 variables;
  - `mem_partition_knapsack`: 17 variables of 13 bits.

  xezim takes 8–15 µs a call on most benches.
- **Bit-vector semantics.** Arithmetic wraps; there are bitwise operators,
  shifts, variable×variable products, and div/mod. Widths of 64 bits and more
  are in scope (plan steps 5 and 5b).
- **Exact samplers already exist.** They cover the small end:
  - CDCL: box rejection, and an enumeration tier capped at 2^18 values;
  - bit-blast: `_enumsamp`, a flat table of at most 65,536 solutions;
  - popsamp, uniqsamp, sortsamp, and an exhaustive simulation sweep.

  Beyond those limits, sampling is heuristic. CDCL draws a random value at each
  decision, which gives measurable joint skew (`memmap3` JSD 0.085–0.12 against
  a 0.006 floor). Bit-blast diversifies a kissat model, which gives low entropy
  on memmap.

## 1. ILP

### 1.1 Why not as an engine

1. **Wrong objective.** Branch-and-cut returns vertices of the LP relaxation,
   or points near them. A random objective just gives a random vertex. This is
   the memmap entropy bug in a worse form: kissat already pushes `r11_size` to
   its bound in 163 of 210 models, and an LP optimum does that by construction.
   Drawing uniformly from the integer points of a polytope needs counting
   (Barvinok) or MCMC (hit-and-run plus rounding and rejection). An ILP solver
   does neither.
2. **Semantics mismatch.** Wrap-around needs an extra integer per operation
   (`x + y = r + 2^w·k`). Bitwise operators and shifts need one 0/1 variable per
   bit. A product of two variables is nonlinear: it needs McCormick envelopes
   plus branching, or bit-level expansion. div/mod needs both. By then the
   model is a bit-blast with worse propagation than SAT.
3. **Numerics.** Mainstream MIP solvers (HiGHS, CBC, SCIP's default LP) use
   doubles, which are exact only to 2^53. With 33–64-bit addresses and the big-M
   terms from item 2, tolerances let wrong answers through. Exact-rational
   solvers (SoPlex exact, QSopt_ex) are orders of magnitude slower.
4. **Latency.** Setting up and presolving a MIP model takes milliseconds,
   against a per-call budget of tens of µs. The model could be built once per
   context, but each solve still re-runs simplex and branching.
5. **Soft constraints don't need it.** Priority order is lexicographic, and
   greedy relaxation in priority order is the intended semantics. A weighted
   MaxSAT/ILP optimum would change the answers.

### 1.2 Where linear reasoning really is short

The memmap family is linear: `end = base + size`, non-overlap disjunctions,
running sums, alignment. So is the knapsack bench. The current gaps there:

- **Equalities only adjust weights.** `_lin_collect` finds the linear equality
  groups (`dvs_compile.c:295`) but does not eliminate them.
- **Propagation is bounds-only.** Bounds propagation is weak on chains of
  equalities and sums. `dvs_diffcycle` only detects negative cycles; it does no
  pruning.
- **Bit-blast cannot follow the equalities.** Its diversify step flips single
  bits, and any such flip breaks `end = base + size`.

### 1.3 Better ways to get the same benefit

In order:

1. **Exact integer elimination of linear equalities.** Hermite or Gaussian
   elimination over ℤ, modulo 2^w where it wraps. This removes the dependent
   variables (`end`, running sums) so they are computed rather than searched.
   It is the "functional dependency" part of plan step 5 and gap item 5, and it
   also fixes the bit-blast diversify problem for these fields.
2. **Order-then-gap sampler** (gap item 8). It is exactly uniform for
   non-overlap with fixed sizes and needs no search. This is a counting
   argument (stars and bars), which is the thing ILP cannot do.
3. **Optionally, a simplex-based LIA propagator inside CDCL.** This would be
   dual simplex in the Dutertre–de Moura style, exact rationals, incremental,
   with bounds as the interface. It is the SMT "theory of linear integer
   arithmetic", not a MIP solver. It finds infeasible linear conjunctions
   (e.g. sums that can't fit) earlier than bounds propagation, so it cuts
   backtracking, but it does nothing for uniformity. Do this only if a bench
   shows search time dominated by linear conflicts that propagation misses.
   None of the profiled gaps currently show that.

There is one small use for LP-style reasoning in sampling: better volume
estimates for the `DVS_WPICK` bucket weights, which currently use the remaining
box size. That is a refinement, not a reason to take on a dependency.

## 2. BDD

### 2.1 What it would buy

- **Exact uniform sampling.** Annotate each node with the number of solutions
  below it. A sample is then one root-to-leaf walk, taking each branch with
  probability proportional to its count. That is O(total bits) per draw after a
  one-time build. It is the textbook fit for "compile once, randomize many
  times" (Yuan et al., TCAD 2004).
- **Exact `dist` under coupling.** Give each satisfying assignment the weight of
  its `dist` buckets and propagate weights instead of counts. CDCL's
  `_pick_value_dist` re-weights per decision and is only approximate when
  variables are coupled. Bit-blast and `_enumsamp` drop `dist` entirely.
- **Pins without a rebuild.** A pin restricts the BDD (a cofactor), which is
  linear in its size, then the counts are recomputed. This applies to Verilator's
  `dist` pins and `rand_mode`/`pin_var`. The current per-call context rebuild
  has to be fixed anyway (gap item 4), but a BDD would turn it into a cheap
  restrict.
- **A higher ceiling than `_enumsamp`.** `_enumsamp` stores one row per
  solution, at most 65,536. A BDD with 2^40 solutions can be a few hundred nodes
  when the constraints are intervals, bit masks, comparisons, or additions with
  interleaved variable order. That is the "medium" census class
  (`bench_vlt_replay.c:1284`, "box ≤ 2^64: size-wise a BDD candidate").
- **`unique` and `randc` on small domains**, and implication-heavy control
  constraints. `ddr5_timing_full` has 8 variables of at most 6 bits, so 48 BDD
  variables, which is trivially small.

### 2.2 What it would not fix

| bench | why the BDD doesn't help |
|---|---|
| `mul_div_64` | Multiplier BDDs are exponential under any variable order (Bryant 1991). |
| memmap / `addr_map_64` / `soc_addr_map_*` | 24–46 variables of 33–64 bits, with comparisons and sums across many variable pairs. No single interleaved order suits every pair, so expect blow-up (the bench docstrings say BDD-based commercial tools blow up on these too). Sections 1.3 and gap items 1, 5, 6 and 8 are the fix. |
| `mem_partition_knapsack` | A 17-term sum = 2048 is compact only if the partial-sum width stays small. Power-of-two sizes may make it work, or not; measure it. |
| `dist.*` speed | The cost is the per-call rebuild, not the sampler. |

So a BDD addresses **quality on medium components**, not the speed gap.
Gap items 1–4 already target the speed gap.

### 2.3 How it would fit

- **A rung in the sampler ladder, not a third engine.** Put it in `_fresh_base`
  next to or replacing `_enumsamp`, and as a tier in CDCL beside the enumeration
  tier. It works per component (`dvs_components`) and builds from the existing
  AIG (`dvs_aig`), so it needs no new front-end.
- **A node budget, with a decline that sticks.** Use something like 2^16–2^20
  nodes. Once a component declines, remember that for the context. This is the
  lesson from `bus_transaction`, where `_enumsamp` rebuilds and declines on every
  miss.
- **Variable order.** Interleave the bits of variables that share a constraint,
  MSB first. Derive it from the constraint graph and make no attempt at dynamic
  reordering in the first version.
- **Counts** overflow 64 bits. Use log-domain doubles, or 128-bit counts
  (`dvs_i128.h` exists) clamped with a decline above that. Sampling only needs
  ratios, so doubles are fine for the walk.
- **The BDD package.** Options:
  - **CUDD:** mature, but unmaintained since 2015, with a global-ish manager and
    a large footprint.
  - **Sylvan:** Apache-2.0, multicore, but heavier than needed.
  - **BuDDy:** small, old.
  - **In-house:** about 1.5–2k lines of C for a unique table, ITE with a
    computed cache, reference counting or arena GC, counting, restrict and
    sampling. The solver is self-contained C with its own arenas
    (`dvs_arena`, `dvs_block_alloc`), and only a fixed subset of operations is
    needed, so in-house fits best.
- **d-DNNF** (d4 or c2d compile, WAPS-style sampling) is more succinct than an
  OBDD. But it needs an external compiler and a heavyweight compile, and it
  adds little over OBDD at these sizes. Not worth it first.

## 3. Recommendation

1. **Don't add an ILP/MIP solver.** Put the linear-arithmetic effort into
   integer equality elimination (plan step 5, functional dependency) and the
   order-then-gap sampler (gap item 8). Keep a simplex LIA propagator in reserve
   for if a profile ever shows linear conflicts that propagation misses.
2. **Don't start the BDD before plan steps 3b–5.** Those close the measured
   xezim gap; the BDD doesn't.
3. **Then run the go/no-go measurement below.** If enough medium components
   stay compact, add the BDD as an exact sampler rung. Its acceptance criterion
   is joint uniformity and exact coupled `dist` on those components, not speed.

## 4. Go/no-go measurement for the BDD (about a day, offline)

1. Capture the simbench sessions (`SIMBENCH_SESSION_DIR`) and the
   `rand_benches.py` problems via `dvs_problem_write_smt2`.
2. Run `bench_vlt_replay --census` and count the components by class, in
   particular **medium** (box ≤ 2^64, too big for enumeration).
3. For each medium component, build an OBDD offline, using Python `dd` with the
   CUDD backend or a quick C harness on `dvs_aig`. Use the interleaved order
   from 2.3, record the peak and final node count under a 2^20 budget, and the
   build time.
4. Decide:
   - **Go** if most medium components fit in ≤ 2^16 nodes and build in ≤ 1 ms.
     Then measure quality: the JSD of CDCL against the BDD sampler on those
     components, plus `memmap3` and the coupled-`dist` cases.
   - **No-go** if they mostly blow up. Then record which shapes blew up, to
     settle the claims in the bench docstrings.

The DIMACS dump (`DV_DUMP_CNF`) has no map from variable bits to CNF variables,
and a later instance overwrites the file. So start from SMT2 rather than CNF.

Side note: `bench_vlt_replay.c` cites `docs/sampling_techniques_research.md`,
which is not in the repo. Three other referenced design docs are missing too
(`random_diversity_locus_design.md`, `accuracy_closure_plan.md`,
`cube_and_conquer_design.md`).
