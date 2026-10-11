# How random solutions are drawn

This page describes how dv-solve turns "find a solution" into "draw a
solution at random", engine by engine. It is for readers who need to know why
a distribution looks the way it does, or who are changing the code that
shapes it. Like the rest of this section it describes the implementation, not
an interface. {doc}`../concepts/randomization-seeds` covers what users see;
{doc}`../results/randomization` has the measurements.

## The goal, and how it is measured

The target is the **uniform distribution over solutions**: across many
seeds, every assignment that satisfies the constraints should come up about
equally often. (SystemVerilog asks for this, and `dist` is the way to ask for
something else.) A solver doesn't do this by default. Its job is to find
*a* solution, and the one it finds is decided by its search order, which has
nothing to do with how solutions are spread.

The `rand-core` suite (`tests/perf/rand_benches.py`, `tests/perf/run_rand.py`)
draws 2000 solutions from each of eleven small problems whose solution sets
are known exactly (or binned, for the two wide ones), and scores each sampler
by:

- **excess JSD**: Jensen-Shannon distance from the uniform distribution,
  less the distance a true random draw of the same size shows. 0 is as good
  as true random.
- **coverage**: the share of solutions drawn at least once.
- **thin**: how often a rare branch (`thold == 0`, the small band of a
  disjoint range) comes up against its share. 1 is right.
- **chi-square p** against uniform.

Every drawn model is also checked against the constraints; a wrong model
invalidates the run. The gates in the test suite are
`tests/formal/test_seed_diversity.py` (seeded SMT-LIB2 scripts),
`tests/unit/dist_harness.py` (the C API, including `dist`), and
`tests/unit/test_vlt_protocol.py` (that the Verilator driver reproduces a real
Verilator run).

## Where bias comes from

Three effects pull a solver's answers away from uniform. Each mechanism below
exists to counter one of them.

**Decision order.** A search assigns variables one at a time. The first is
drawn from its whole domain; each later one is drawn from what the earlier
choices left. Under `a < b` (6-bit), deciding `a` first, uniformly, and then
`b` uniformly from `[a+1, 63]` gives the pair `(a, b)` probability
`1/63 · 1/(63−a)`, so pairs with a large `a` come up far more often than pairs
with a small one. The uniform answer is `1/2016` for every pair.

**Search bias.** When a choice leads to a conflict, the search tries
something else. Which something else, and how often each branch of the
problem conflicts, shapes the result. A search that always retries the lower
half of a domain walks down to the minimum; one that redraws every decision
after a conflict under-samples the parts of the problem that conflict more.

**Witness clustering.** A SAT solver returns one model, and from the same
formula it tends to return the same one, or one close to it. Changing the
seed changes the search a little, not the answer much.

## Seeds and the random stream

**CDCL.** The random stream is an xorshift64 state held in the context
(`ctx->rng_state`, `_rand64` in `dvs_search.c`). A solve with a non-zero seed
resets the state to `_seed_mix64(seed)`, a SplitMix64 finaliser, so seeds 1,
2, 3 give unrelated streams rather than correlated ones (before the mix,
`a + b == 100` walked through solutions in steps of 65). A solve with seed 0
does not reseed: it carries on from where the context's stream left off,
starting from a fixed constant in a new context. Values in a range are drawn
as `lo + r % span`.

**Bit-blast.** There is no stream. Each consumer derives its randomness from
the seed directly: the flip-check target for bit `k` of variable `v` is a
SplitMix64 hash of `(seed, v, k)` (`_free_var_fill`), the simulation sweep
runs its own SplitMix64 from the seed, and the SAT solver is given the seed
(below).

**Seed 0 means "deterministic".** Throughout, seed 0 is the formal-tool
setting: CDCL uses phase saving (each variable's first value is its lower
bound) and always takes the lower half of a split; bit-blast skips the
flip-check and stops its simulation at the first solution. A seeded solve is
the sampling setting. The front ends choose which one a call gets:

| Front end | Seed per call | `fair_pick` |
|---|---|---|
| C API | `dvs_solve_opts_t.seed` | caller's choice (default off) |
| Python | `SolveCtx.solve(seed=…)` | `fair_pick=True` to turn on |
| `solve_n` | `base_seed + i` | on |
| DPI | the `seed` argument of each solve | on |
| SMT-LIB2 | `(set-option :seed N)` or `:random-seed`, default 0 | on for the Verilator CDCL route, or `DV_FAIR_PICK=1` |
| `--mode=verilator` | a new seed for every `check-sat` (below) | on |

## The CDCL engine

The CDCL engine decides one variable at a time, and both choices, which
variable and which value, are randomized when the solve is seeded.

### Which variable

The engine picks the unassigned variable with the **fewest remaining
values**. Auxiliary variables (one per subexpression) come last, so the
values that are randomized are the user's.

With `fair_pick`, **ties are broken at random**, by reservoir sampling over
the variables that share the smallest domain (`_select_unassigned`). Without
it the lowest-indexed one wins, and the same variable is decided first in
every solve: its values are spread evenly, while every other variable's are
drawn from what it left. Random tie-breaking gives each variable its turn at
being first. That evens out the per-variable distributions. It does not make
the joint distribution uniform: in the `a < b` example, half the solves draw
`a` first and half draw `b` first, and the mixture of the two skewed
distributions is still skewed. That is the `a_lt_b` row on the results page.

Once the engine has learned from a conflict (SMT-LIB2 input, where learning
is on), it switches to the variable with the highest conflict activity. That
choice is deterministic, and `fair_pick` doesn't apply to it.

### Which value

`_pick_value` draws, in order:

1. **A kept draw.** In a seeded solve each variable keeps its first draw.
   When a backjump undoes its decision because of a conflict elsewhere, it
   takes the same value again if that is still allowed. Drawing afresh would
   make every conflict re-roll every decision before it, so branches of the
   problem that conflict more would be drawn less. A kept value whose own
   decision conflicts is dropped, and keeping stops at the first restart.
   `DVS_KEEP_DRAWS=0` turns this off.
2. **A `dist` draw**, for a variable with a distribution: each entry is
   clipped to the current domain and weighted (`:=` by weight × values left,
   `:/` by its weight), and the value is uniform within the chosen entry. If
   every weight is zero or no entry overlaps the domain, the draw is uniform.
3. **Otherwise a uniform draw** over the domain's bounds `[lo, hi]`, moved
   onto a remainder that an `r == a % b` relation still allows
   (`_pick_mod_aligned`).
4. **Avoiding holes.** If the value is one ruled out individually (the gaps
   of an `inside` set, `dvs_solver_exclude_value`), it is redrawn up to 32
   times, and then the k-th allowed value is taken for a random k, which is
   uniform over the allowed values (`_pick_avoiding_holes`).

Phase saving is off in a seeded solve. Besides biasing toward the lower
bound, it re-tried a value in the gap of a union domain at every restart
(B18).

### After a conflict

When a value decision conflicts, the engine excludes that value and splits
the rest of the domain in two. In a seeded solve it explores the halves in
random order, **weighted by their sizes** (`_split_upper_first`). Always
taking the lower half first made the search walk down to the smallest
feasible value whenever a propagator, not the hole list, made the feasible
values sparse: 93% of draws of a 64-aligned address in a 4 KiB window were
the lowest one. `DVS_SPLIT_RANDOM=0` restores lower-first.

With learning on, a wide variable that has been in conflicts can instead be
decided by a bound (`x <= v` or `x >= v`), so that a failure teaches a range
rather than one value. That matters for a product that must divide exactly,
whose solutions are sparse. As a sampler it is biased: a failed bound drops a
side chosen by the width of its interval, not by how many solutions it holds,
while a failed value decision just excludes `v`, and the next draw is uniform
over what is left. So a seeded solve keeps making value decisions on a
variable until 32 of them have failed in that solve, and only then switches
to bounds (B97).

### What CDCL does not do

There is no rejection step, no solution counting and no hashing. A CDCL
sample is "uniform at each decision, given the decisions before it". That is
exact for one variable, close for loosely coupled ones (ranges, masks, sums
with a free term), and measurably off for orderings and some disjunctions.
The cost is a few microseconds per solve.

## The bit-blast engine

The bit-blaster has a pipeline of steps before SAT, then SAT, then a
correction step. Whichever step produces the model decides how uniform it is.

```text
 seeded check-sat
   │
   ├─ XOR reasoning ─────────────── model: fixed witness ──────────┐
   ├─ simulation                                                   │
   │    ├─ exhaustive sweep (small cone) ─ model: exactly uniform ─┤
   │    └─ 512 random vectors ──────────── model: exactly uniform ─┤
   └─ SAT (seeded phases) ──────────────── model: witness ─────────┤
                                                                   ▼
                                     flip-check (one Gibbs sweep) ─▶ free-bit fill ─▶ answer
```

### Simulation draws exact samples

Before SAT, the bit-blaster simulates the assertions' input cone 512 inputs
at a time (`dvs_aigsim.c`). Two of its outcomes are exact uniform samples.

- **Exhaustive sweep.** When trying every input value costs at most 1.5 M
  gate-word evaluations (`DV_SIM_MAX_WORK`; up to 32 inputs), a seeded sweep
  goes through every assignment and keeps one by **reservoir sampling**: in
  each pass of 512 with `c` hits, `seen` grows by `c`, the kept assignment is
  replaced with probability `c/seen`, and the replacement is a uniformly
  chosen hit of that pass. The survivor is uniform over all solutions. With
  seed 0 the sweep stops at the first solution, as a proof wants.
- **Random vectors.** Otherwise it tries 512 uniformly random inputs. The
  first that satisfies is a uniform sample too: among independent uniform
  draws, conditioning on "satisfies" gives the uniform distribution over the
  solutions.

When neither decides and the sweep would take at most about 2 s
(`DV_SIM_RACE_WORK`), the sweep runs on a second thread beside SAT and the
first answer wins. If SAT finishes too, its model is the one used.

The sweep used to stop at the first solution even when seeded, so every seed
gave the lowest solution once Phase 3 routed bit-level problems here (B94;
`docs/fw_hdl_sampling_diversity.md`).

### SAT gives a witness

When the answer comes from SAT, the seed goes to the SAT solver. kissat gets
its `seed` option, has its "lucky" phases turned off, and, in a patch to the
bundled copy, starts every variable from a random saved phase; its random
decisions use the same seeded generator. CaDiCaL gets its `seed` and an
initial phase from the seed. This moves the witness around but doesn't make
it uniform: there is no hashing, no random assumption cube and no rejection.

XOR reasoning (Gaussian elimination) runs first and returns a fixed model:
free columns at 0. Only the flip-check varies it.

### The flip-check

After every seeded `sat`, `_diversify` makes one pass over the bits of the
problem's variables that are inputs of the circuit. Each bit has a target
drawn from the seed. If the bit differs from its target, it is flipped, the
change is propagated through that bit's fanout cone only, and the flip is
kept if every assertion still holds; otherwise it is undone. Flips are
sequential, so each is checked against the model as changed so far, and the
result always satisfies the assertions.

Each flip is a **Gibbs step** for the uniform distribution. Given all the
other bits, a bit can be 0, 1, or both. If both satisfy, it ends at a random
value; if only one does, it keeps it. That leaves a uniform sample uniform,
which is why the flip-check can run after simulation without harm. From a
witness it does less: it can only move to solutions one satisfying bit-flip
away, so it cannot leave a solution none of whose neighbours satisfy
(`countones(x) == 4`, `a + b == 100`, a CRC filter), and one sweep from a
witness is not a sample from the uniform distribution. It does reliably
spread the bits the constraints leave free.

The flip-check is skipped above 200,000 AIG nodes. Bits the circuit never
uses, and variables no assertion mentions, are filled from the seed when the
model is read.

### Arrays

Arrays expanded into bit-vectors are diversified like any other variable.
Under the lazy array engine (`DV_ARRAY=1`), the refinement loop runs
unseeded, and `_array_diversify` then re-solves under a random cube of up to
32 input literals with random polarities, halving it while it is
unsatisfiable or too costly, and flip-checks the result. Array elements
nothing constrains get a seeded value.

## Choosing the engine for a draw

The two engines have different strengths as samplers, and the routing takes
that into account.

**One-shot SMT-LIB2 with a seed.** A `QF_BV` script with a non-zero `:seed`
or `:random-seed` goes to the bit-blaster. With the seeded sweep it was
uniform on all eleven `rand-core` benchmarks, while CDCL skewed the orderings
and disjunctions. Without a seed nothing changes: `QF_BV` routes by shape as
{doc}`../concepts/engines` describes.

**C API, Python and DPI** always use CDCL, with `fair_pick` on for `solve_n`
and DPI.

**`--mode=verilator`.** Verilator randomizes by sending its constraints, then
up to four times adding a random 1-bit parity constraint over about half the
random bits, re-solving after each, and keeping the last solution (Verilator
5.046; see {doc}`../results/randomization`). Each `check-sat` in this mode
gets a new seed from a counter that survives `(reset)`, or `given seed + 1`
when the driver set one. Then:

- **CDCL first.** A problem not seen before gets a 50 ms CDCL probe; if CDCL
  answers, problems with that fingerprint stay on CDCL. Bit-blast's
  flip-check from a cached witness put half of all draws on one solution of an
  OpenTitan-style range constraint, where CDCL's draws were close to uniform.
  `DV_VERILATOR_CDCL=0` turns this off.
- **By measured cost.** Per problem shape (the declared names), once CDCL
  averages more than 3 ms, the next two calls try bit-blast, and bit-blast is
  kept only if it is at least twice as fast.
- **Bit-blast reuses its model.** When a bit-blast problem is identical to the
  last one, it is not re-solved: the cached model is flip-checked with the new
  seed (`dvs_bbsolver_rediversify`). Every draw is then one Gibbs sweep from
  the same witness. `DV_VERILATOR_RESEED=N` re-solves every N draws.
- **Simulation is off**, so bit-blast draws here always start from a SAT
  witness.
- **`--verilator-hash=ignore`** drops the parity constraints and answers the
  rounds after the first with the first model. It is about ten times cheaper
  per `randomize()`; the base solve's draw is then the whole sample.
- **Newer Verilator** (5.052 and later) enumerates solutions with blocking
  clauses and hash cells itself. dv-solve recognises that shape and turns the
  flip-check off, since Verilator picks within the cell.

## What each path gives

| Path | Sampler | Distribution |
|---|---|---|
| Seeded SMT-LIB2, small cone | exhaustive sweep + flip-check | exactly uniform |
| Seeded SMT-LIB2, dense solutions | random vectors + flip-check | exactly uniform |
| Seeded SMT-LIB2, otherwise | SAT witness + flip-check | free bits spread; coupled bits follow the witness |
| C API, Python, DPI (`fair_pick`) | CDCL decisions | uniform per decision; joint skew on orderings |
| Verilator, CDCL route | CDCL decisions, plus parity rounds | close to uniform (excess JSD 0.003 on `rand-core`) |
| Verilator, bit-blast route | cached witness + one flip-check per draw | free bits spread; coupled bits concentrated |
| Unseeded (any) | first solution found | deterministic, usually the minimum |

## Known limits

- **CDCL joint skew.** Orderings (`a < b`) and some disjunctions are sampled
  per decision, not jointly. Random tie-breaking evens the per-variable
  distributions only. After a conflict the variable order follows activity
  and is not randomized.
- **Witness-based draws.** When SAT supplies the model, single-bit moves
  cannot cross parity, one-hot, equality or sum constraints. Portfolio and
  parallel-cube winners (`DV_PORTFOLIO`, the cube engine) skip the
  flip-check entirely, and an exhaustive sweep raced beside SAT is discarded
  when SAT also finishes.
- **`dist` details.** A redraw after hitting a hole is uniform, not
  weighted. A `:/` entry partly outside the domain keeps its full weight.
  Holes inside an entry don't reduce its weight. With several `dist`s on one
  variable, the oldest is used. The bit-blaster treats `dist` as a domain
  restriction only.
- **Modulo bias.** `lo + r % span` favours low values by up to `span/2^64`,
  which is immaterial except for spans near `2^64`.
- **`randc`.** There is no automatic cycling. `VAR_RANDC` is declared but
  unused; a caller can cycle with `dvs_solver_exclude_value`.
- **Verilator seed.** Verilator 5.046 doesn't send its `+verilator+seed` to
  the solver, so dv-solve's per-`check-sat` counter starts at 1 in every
  simulation. Different seeds still differ through Verilator's parity
  constraints; with `--verilator-hash=ignore` they may not (not yet tested).

## Not built yet

`docs/random_diversity_locus_design.md` proposes keeping sampling state per
*locus* (thread, class, object, constraint fingerprint) so draws from one
object stay spread across resets, using low-discrepancy sequences, and
optionally biasing toward coverage goals. None of it is implemented. Other
open ideas: a two-bit repair move for the flip-check (to cross equalities),
turning simulation on in Verilator mode (it now samples, so it would give
Verilator's bit-blast problems exact draws; it needs a cost measurement
first), and seeding the XOR-reasoning model.

## Where it is in the code

| Mechanism | Function | File |
|---|---|---|
| Random stream, seed mixing | `_rand64`, `_rand_range64`, `_seed_mix64` | `src/c/dvs_search.c` |
| Variable choice, `fair_pick` | `_select_unassigned` | `src/c/dvs_search.c` |
| Value choice, kept draws | `_pick_value` | `src/c/dvs_search.c` |
| `dist` | `_pick_value_dist` | `src/c/dvs_search.c` |
| Holes | `_pick_avoiding_holes` | `src/c/dvs_search.c` |
| Split order | `_split_upper_first` | `src/c/dvs_search.c` |
| Simulation sweep, reservoir | `dvs_aigsim_exhaustive`, `dvs_aigsim_random` | `src/c/dvs_aigsim.c` |
| Simulation before SAT, race | `_solve_with_sim` | `src/c/dvs_bbsolver.c` |
| Flip-check | `_diversify`, `dvs_bbsolver_rediversify` | `src/c/dvs_bbsolver.c` |
| Flip targets, free-bit fill | `_free_var_fill` | `src/c/dvs_bbsolver.c` |
| SAT seeding | `ki_set_seed`, `cd_set_seed` | `src/c/dvs_sat_kissat.c`, `src/c/dvs_sat_cadical.c` |
| Seeded routing | `_route_qfbv` | `src/c/smt2/smt2_frontend.c` |
| Verilator seed, routing, cache | `_cmd_check_sat_body`, `_check_sat_bitblast_body` | `src/c/smt2/smt2_frontend.c` |
| Array diversification | `_array_diversify` | `src/c/smt2/smt2_frontend.c` |
