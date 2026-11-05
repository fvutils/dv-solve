# Structural reasoning for CDCL: inferred facts, change of variable, bit-blast pruning

Status (2026-11-05): design only, nothing implemented. Follows the `bvashr` and
phase-saving work in docs/solver_gap_plan.md (entries of 2026-11-05).

## Why

CDCL reasons about each variable as one int64 interval, and learns clauses made of bound
literals (`x >= v`, `x <= v`; `Literal` in `src/c/dvs_lcg.h`). Many constraints in
hardware verification carry structure that an interval cannot express:

- `b inside {1, 2, 4, 8}`, `1 << size`, `2 ** n`: b is always a power of two.
- `x % 8 == 0`, `x & ~7`, `addr = base * 64`: x is a multiple of a constant.
- masks, concat, extract: some bits of x are fixed.
- `c = a >> k, c != a`: "k = 0 implies c = a", a relation between variables.

When the structure is invisible, three things go wrong:

1. **Wrong operator cost.** A variable divisor that is always a power of two still gets a
   full divider. At 64 bits, bit-blast: `a / b` with `b in {1,2,4,8}` is 131,154 clauses;
   the equivalent `a >> k`, `k in [0,3]`, is 3,955 (33x). `a * b`: 48,302 vs 3,955 (12x).
   CDCL uses its general non-linear `*` / `/` propagator where a shift propagator would do.
2. **Weak pruning.** The interval hull of a structured set is mostly infeasible values.
   CDCL decides one, conflicts, and learns a clause that excludes about one value. The
   `bvashr` problem above took 10,000 such conflicts (2.1 s) until a restart rescued it.
3. **Biased sampling.** Each conflict rejects the decisions made before it, so the
   distribution follows the search rather than the constraints. The search already
   has one-off fixes for this, each added after a sampling bug:
   `_pick_mod_aligned` (alignment), `_pick_mul_divisor` (operands of a fixed product),
   hole lists for `inside` sets, divisor tables (`src/c/dvs_search.c`).

This design makes structure a first-class layer: infer facts about expressions once,
from the problem's structure, and use them for rewriting, initial domains, value
picking and engine routing. For the facts that structure cannot reveal, CDCL asks the
bit-blast engine at the moment it stalls.

## Principle 1: static facts need no explanation

A fact that follows from the problem's structure holds in every solution and at every
point of the search: `1 << s` is a power of two whatever s is. Inferred once at compile
time by a pass over the expression DAG, such facts are axioms. CDCL never has to explain
them in a learned clause.

Facts that hold only under the current decisions ("x became a power of two once y was
decided") would need new literal kinds in learned clauses. That is out of scope here,
and is revisited only if static facts prove their worth.

## Principle 2: turn a fact into an interval by a change of variable

Rather than teaching CDCL a new domain, rewrite the variable so the fact *becomes* an
interval on a new variable. The existing propagators, clause learning and value picking
then apply unchanged.

| fact about x | change of variable | interval |
|---|---|---|
| power of two, exponent in [lo, hi] | `x = 1 << k` | `k in [lo, hi]` |
| `x ≡ r (mod m)` | `x = m*q + r` | q's range |
| bits fixed by known-bits | `x = {fixed, free fields}` | each free field's range |
| affine `x = a*y + b` | substitute | y's range |

Two consequences:

- **Explanations stay bound literals.** A conflict involving `x` is explained through
  `k`'s bounds.
- **Sampling is exact.** The new variable maps one-to-one onto the legal values of x, so
  drawing it uniformly draws x uniformly over exactly those values, with no
  draw-and-reject. This is what the ad-hoc pick helpers approximate today.

The new variable is the one the search decides. The old one becomes determined (`VAR_AUX`),
or stays a decision variable with the link as a propagator; which works better is an
experiment (see "Open questions").

## Facts to track

| fact | produced by | used for |
|---|---|---|
| power of two (`x = 1 << k`) | `1 << s`; constant 2^k; `inside` set of powers of two; `$onehot`; product, quotient or shift of powers of two | `*`, `/`, `%` by x become `<<`, `>>`, `&`; one-hot sampling |
| known bits (fixed 0/1) | `&`, `\|` with constants, shifts by a constant, concat, extract, constants | tighter bounds; drawing only values matching the fixed bits |
| congruence `x ≡ r (mod m)` | `x % m == r`; multiply by constant m; alignment masks | bounds rounded to the stride ([5,13] with x ≡ 0 mod 4 gives [8,12]); aligned draws |
| small value set | `inside`, `dist`, `ite` of constants | exact choice of value instead of hull plus holes |
| sign / non-zero | compares, divisor guards | drop the zero-divisor branch of `/`; signed vs unsigned propagator |
| popcount bounds | `$countones`, one-hot | direct sampling of k-hot values |
| affine link | `x == a*y + b` | eliminate a variable |

The facts combine as a reduced product: each fact can tighten the interval and the
others (known bits {low 2 bits 0} and interval [5,13] give congruence 0 mod 4 and
[8,12]).

Existing code to build on:

- `dvs_bvdom` (`src/c/dvs_bvdom.h`): three-valued known-bits domain, ported from
  bitwuzla, with transfer functions (e.g. `dvs_bvdom_bvashr_const`). Used today only by
  bit-blast preprocessing and `dvs_bvbounds.c`, never by CDCL.
- `dvs_bvbounds` (`src/c/dvs_bvbounds.h`): range / bounds, also from bitwuzla.
- `bb_pow2_or_zero` (`src/c/dvs_bbsolver.c`): recognises a constant or `1 << s` divisor
  and lowers `urem` to `&`. Bit-blast only, `urem` only.
- `_mrw_pow2` / `_mrw_align` (`src/c/dvs_sv.c`): SV elaboration rewrites
  `x % (1 << s) == 0` and `(x & ((1 << s) - 1)) == 0` into per-size field constraints.
- `DvsModLink`, `DvsMulLink`, hole lists, divisor tables: CDCL's per-shape side
  channels. They would become consumers of the shared facts, or be retired by the
  change of variable.
- `DV_AFFINE` (`src/c/dvs_bbsolver.c`): affine detection, bit-blast side.

## Powers of two: the first instances

### A. Constant powers of two

Measured at 64 bits, bit-blast clauses:

| expression | clauses |
|---|---|
| `a * 4`, `a << 2`, `a / 4`, `a >> 2` | 182 each |
| `a % 4`, `a & 3` | 2 each |
| `a sdiv 4` | 2,082 |
| `a * 12` / `a / 12`, for scale | 1,448 / 18,956 |

Bit-blast already folds unsigned `*`, `/`, `%` by a constant power of two to the shift
or mask (constant propagation in the AIG). What remains:

- **Signed division by 2^k** is not reduced (2,082 clauses). SMT-LIB `bvsdiv` truncates
  toward zero and an arithmetic shift floors, so the rewrite is
  `q = (a + (a < 0 ? 2^k - 1 : 0)) >>> k`, `srem = a - (q << k)`, `smod` from `srem` as
  today.
- **CDCL** keeps the general `*` / `/` propagator for a constant power of two. That is
  exact for intervals, but a multiply by 2^k makes the result's low k bits zero, which
  the interval does not hold, so a decision on the result is drawn from the hull and
  mostly rejected. Rewriting to `<<` lets known bits (or the congruence change of
  variable, `r = 2^k * q`) carry it.

### B. Variables that are always a power of two

The larger win, and the one the corpus cannot show syntactically. Producers:

- `b == 1 << s`, or `b` is used only as `1 << s`;
- `b inside {1, 2, 4, 8}`, or any set or `dist` whose values are all powers of two,
  found from the compiled domain (interval plus holes) as well as the syntax;
- `$onehot(b)`, `$countones(b) == 1`;
- products, quotients and shifts of powers of two.

Rewrite: add `k` with `b == 1 << k` (k's range from b's domain), then `a * b` becomes
`a << k`, `a / b` becomes `a >> k`, `a % b` becomes `a & ((1 << k) - 1)`, and the
signed forms as in A. `b` and `k` are in one-to-one correspondence, so b's sampled
distribution is unchanged when k is drawn uniformly over b's legal exponents. A `dist`
on b transfers to k value by value.

Measured (64 bits, `c = a op b`, `b in {1,2,4,8}`, c in (1000, 2000), a > 5000):

| form | bit-blast clauses | pure CDCL |
|---|---|---|
| `a / b` | 131,154 | 0.00 s |
| `a >> k` | 3,955 | 0.00 s |
| `a * b` | 48,302 | 0.00 s |
| `a << k` | 3,955 | 0.00 s |

The bit-blast gain is large. On this small instance CDCL solves both forms instantly, so
the CDCL gain is unproven until a harder workload shows it (step 0 below).

### Where the pass runs

On the problem IR, before engine selection, so both engines see the rewritten problem.
It must run after SV elaboration and the typed-operator lowering (`dvs_sv_lower_typed`),
which is where `inside` sets, casts and `1 << s` take their final form. The model the
caller sees is unchanged: rewritten-away variables are reconstructed from their
definitions, and `dvs_solver_validate_model` checks the original problem.

## Bit-blast-assisted pruning: the dynamic counterpart

Static facts cannot see relational traps like `c = a >> k, c != a`, where interval
explanations cannot state "k = 0 implies c = a". The bit-blast engine can decide such a
question directly and answer in bound literals.

### Stall oracle

When CDCL stalls, it asks bit-blast whether its current decisions can extend to a
solution at all:

1. **Detect a stall.** For example, N conflicts in a row whose learned clauses all
   assert at the same level, which is the `k = 0` trap's signature. The threshold is a
   tuning knob.
2. **Ask.** Encode each decision on the trail as a bound assumption (`k <= 0`,
   `a >= X`, ...) and call `dvs_bbsolver_solve_assuming` with a conflict limit.
3. **Learn.** If unsat, the assumptions in the unsat core form a nogood. Add it to CDCL
   as an ordinary learned clause of bound literals (`clause_db_add`), here `k >= 1`.
   This is sound, because only infeasible assignments are excluded. If sat, or the
   oracle times out, the search carries on as before.

This replaces thousands of one-value conflicts with one query. Pruning removes only
infeasible regions, so the solution set and the distribution over it are unchanged. It
should reduce sampling bias, since fewer conflicts means fewer rejected decisions.

**Needed in the bit-blast engine:**

- A call that turns a predicate (`x <= v` on a problem variable) into an assumption
  literal. The cube-and-conquer code does this internally for `or` disjuncts
  (`dvs_bbsolver_or_split_lits`); it needs a general form.
- The unsat core of an assumption solve. The SAT back-end has it (`failed()`,
  `src/c/dvs_sat_backend.h`, real with CaDiCaL), but `dvs_bbsolver` does not expose it.
- An encode limited to the cone of the stalled variables, under a clause budget: one
  64-bit `sdiv` is 133,502 clauses.

Decisions are on problem variables (aux variables are never decided), so assumptions
map onto bit-blast's variables directly.

### Presolve bound tightening

Before sampling, compute each problem variable's true feasible min and max with
bit-blast (about one assumption solve per bit, most significant first), and for
small-domain variables, which values are infeasible. Feed the results to CDCL as
domains and holes. Verilator calls `randomize()` thousands of times on the same
constraints, so the cost is paid once and every sample benefits. This is also a source
of static facts (a domain that turns out to be all powers of two).

## Plan

0. **Find the shapes in real workloads.** In the repo's corpus (642 SMT-LIB fixtures, 21
   SV captures), power-of-two operands are rare: 4 `bvmul` and 2 `bvudiv` by a constant
   2^k, 1 `bvsdiv`, 1 `1 << s` operand. `uart_base` multiplies by
   `2**(nco_width+4)` and divides (`CALC_NCO`). Scan larger testbench sources (AXI, PCIe,
   NVMe bus and DMA models; pyvsc and Verilator examples) for `1 << size`, `2 ** n`,
   power-of-two `inside` sets and `$onehot`. Check how Verilator emits `2 ** s`: the
   IR has no power operator, so it arrives as a shift or a constant. Collect a fixture
   set where CDCL is measurably slow or biased.
1. **Fact pass, power of two only.** Compile-time inference over the DAG; tests from
   each producer above, and negative tests (`1 << s` with s able to reach the width is
   0, not a power of two; signed widths).
2. **Rewrites A and B** behind an environment switch. Bit-blast clause counts, pure-CDCL
   A/B (`DV_NO_BITBLAST=1`, tiers 1-3 and `test_cdcl_targets.py`), and sampling quality
   (`tests/formal/test_sampler_properties.py`, `sample_quality.py`) for the `dist`
   transfer.
3. **Known bits and congruence** into CDCL: `dvs_bvdom` for the fact pass, the
   congruence change of variable, then retire `_pick_mod_aligned` where it is subsumed.
4. **Stall oracle** prototype on `c = a >> k, c != a` and `t_constraint_operators`;
   then pure-CDCL A/B for the stall threshold and oracle budget.
5. **Presolve bound tightening** for the repeated-`randomize()` workload.

Every step is measured with bit-blast off (`DV_NO_BITBLAST=1`): with the fall-back on, a
CDCL regression shows only as a slower answer, which is how the phase-init A/B first
misled us (docs/solver_gap_plan.md).

## Open questions

- After a change of variable, should the new variable be the one decided, with the old
  one determined? Uniformity argues yes. But `dist` weights and user-visible pins
  (`dvs_solver_pin_var` on the original b) must map through the link.
- How far to chase facts through arithmetic: `(1 << s) * (1 << t)` is a power of two
  only while `s + t` stays below the width.
- Whether congruence and known bits belong inside CDCL's per-variable domain after all
  (as a reduced product), if the change of variable multiplies variables too much on
  real workloads.
- Stall detection: the signal (conflicts asserting at one level, conflict rate per
  decision) and the threshold.
- Clause budget for the oracle's encode, and caching the encoding across stalls in one
  solve and across `randomize()` calls.
