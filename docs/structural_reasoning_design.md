# Structural reasoning for CDCL: inferred facts, change of variable, bit-blast pruning

Status (2026-11-06): step 0 done (workload scan, fixtures in
`tests/formal/smt2/pow2/`). Steps 1-2 done for the bit-blast engine (facts and
shift encodings, below). Step 3 parts 1-4 done for CDCL ("Implemented"
entries below), plus the `axi_burst_bytes_64` fix (remainder/low-slice link,
`1 << s` rounding); all verified (full suite, fuzz, pure-CDCL A/B). Every pow2
fixture now solves under pure CDCL in 0.00 s on every seed. Not committed.
Next: plan steps 4 (stall oracle) and 5 (presolve), or the research's cheap
wins (docs/research/solver_abstract_domains.md). Follows the `bvashr` and
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

## Results so far

### Step 0: workloads (2026-11-05)

Scan of about 77k SystemVerilog files from 1,326 repos (the fw-hdl-xls RTL
corpus), plus opentitan, riscv-dv, an AXI VIP and pyvsc's tests; unique lines
inside constraint blocks:

| shape | lines | repos |
|---|---|---|
| `2**var` | 59 | 13 |
| `1 << var` | 44 | 20 |
| `%` by a constant power of two | 45 | 21 |
| `%` by a variable | 31 | 14 |
| 4 KB boundary | 38 | 27 |
| low address bits `== 0` | 34 | 17 |
| `inside {1,2,4,8...}` | ~25 | 17 |
| `$onehot` | 17 | 8 |

- In bus and DMA VIPs the power of two is almost always a random transfer
  `size`: `addr + (len+1) * (2**size)`, `(addr & ((1 << size) - 1)) == 0`,
  `size inside {1,2,4,8}`.
- Verilator (`V3Randomize.cpp` `handlePow`, Feb 2026) lowers `2**n` to
  `(bvshl 1 n)`, or `(ite (bvsge n 0) (bvshl 1 n) 0)` for a signed `n`, and
  `x**K` to K multiplies. Releases up to 5.043 reject `**` in constraints.
- 13 fixtures in `tests/formal/smt2/pow2/`, z3-checked. The AXI shapes are
  instant on both engines. Under pure CDCL three time out at 64 bits: a
  multiply by a variable power of two (`mul_pow2var_64`, `onehot_mul_64`) and
  a signed divide by `1 << k` (`sdiv_pow2var_64`), all with the result held
  in a narrow window. `shl_var_64` is the first already written as a shift,
  and it times out too: **rewrite B does not help CDCL here**; what is missing
  is the low bits of the product (known bits, congruence: step 3).
- Sampling: the default (bit-blast) and pure CDCL disagree on the
  distribution over the power-of-two variable. Bit-blast follows the number of
  solutions behind each value; CDCL is closer to uniform over values. Not yet
  analysed.

### Steps 1-2 in the bit-blast engine (2026-11-05)

No new variables: a divisor or factor known to be 0 or a power of two encodes
as a shift by its one-hot index (an OR-tree encoder), muxed for the zero case.

- **Facts** (`bb_note_pow2`, `bb_pow2_ref`, `dvs_bbsolver.c`): a variable
  confined by a top-level `inside` set or `||` of equalities to powers of two,
  or by `(v & (v - 1)) == 0`, read through the front end's named intermediates
  (substitutions); `1 << s`; zero-extends and ITEs of those; constants.
- **Soundness under scopes:** an encoding outlives its scope in the memo
  cache, so a session records facts only from base-scope assertions (no pop
  retracts them), and a pop clears the entries of the variables it frees.
- **Encodings:** unsigned `/` and `*` (either side), typed `bvudiv`; signed
  `bvsdiv`/`bvsrem`/`bvsmod` by a power of two of the same width (|r| is
  one-hot too: |MIN| = MIN); signed division by a constant ±2^k as
  `(a + (a < 0 ? 2^k - 1 : 0)) >>> k`, negated for a negative divisor (the
  remainder keeps its old path, which already folds to a mask).

Bit-blast clauses, committed baseline vs now:

| fixture | before | after |
|---|---|---|
| `udiv_pow2set_64` | 181,834 | 9,670 |
| `sdiv_pow2var_64` | 137,336 | 10,876 |
| `a / b`, b in {1,2,4,8}, 64-bit | 131,154 | 4,492 |
| `axi_burst_bytes_64` | 53,029 | 9,228 |
| `axi_align_4k_64_unsat` | 52,267 | 8,466 |
| `mul_pow2var_64` | 49,148 | 5,719 |
| `onehot_mul_64` | 48,753 | 5,135 |
| `a / (1 << s)`, 64-bit | 45,783 | 2,459 |
| `pow_int_exp_signed` | 33,367 | 1,666 |
| `a sdiv 4`, 64-bit | 2,087 | 757 |

Checks: a differential fuzz against z3, `tests/formal/pow2_fuzz.py` (6,000
random problems on the final build over all the fact forms and operators at
widths 1-64, each sat model re-checked by z3; 0 mismatches), push/pop cases by
hand (a fact asserted in a popped scope, a popped variable's id reused), and `tests/formal/test_pow2_shapes.py` (answer plus a clause budget per
fixture). `DV_URW_POW2=0` turns the facts off.

### Why CDCL stalls on the 64-bit fixtures (2026-11-05)

Diagnosed with traces and env-guarded prototypes (since reverted). The cause is
the same each time: an interval cannot hold "the low bits are zero" or "the
value is odd", so the search enumerates a 64-bit variable value by value.

- `shl_var_64`: k is fixed (24), then p is decided. `a << k` forward over an
  almost full `a` gives a full range, so p is drawn from a 2^32 window where
  1 in 2^24 values has the needed low zeros; each conflict removes one value.
  The backward rule fires only once p is fixed.
- `mul_pow2var_64`: the same through `_modiv_mul(A, 2^24)`.
- `onehot_mul_64`: `(b & (b - 1)) == 0` never tells CDCL that b is a power of
  two; p is drawn odd, then b is bisected value by value.
- `sdiv_pow2var_64`: `a srem 2^k == -3` is a congruence on a; the remainder
  propagator learns one bound per conflict (2,000 decisions on a).
- The 32-bit siblings have the same weakness with a 4,096-value window.
- No `dvs_bvdom` known-bits code is wired into CDCL propagation, and the 64-bit
  slice propagator (`_fire_bit_slice_64`) moves information between `a` and
  `a[63:1]` only when one side is fixed.

Recommended fix (each part measured as a prototype):

1. **Shift propagator stride:** a result of a shift by `s >= s0` has its low
   `s0` bits (plus a's trailing zeros) zero; round r's bounds to that stride,
   the explanation including r's own same-side bound (~30 lines; ~15 more for
   `bvmul` by a fixed power of two).
2. **Static exact-trailing-zeros fact** (a is odd) from root-level known bits
   (`dvs_bvdom`): gives `k = ctz(p)` once p is fixed and `p = 2^k mod 2^(k+1)`
   once k is fixed. Required: without it part 1 solved 5 of 8 seeds (the odd bit
   and the stride keep nudging each other's bounds until the trail cap).
   ~60-100 lines with a small root-level pass.
3. **Monotone slice:** a slice holding the top bit maps `[alo, ahi]` to
   `[alo >> lo, ahi >> lo]` and back (~15 lines), so a's pattern reaches the
   variable actually decided (`a[63:1]`).
4. **Compile-time changes of variable** (Principle 2, `dvs_compile.c`): rewrite
   B (`x * (1 << k)` to `x << k`); for the power-of-two fact, a new `kb` in
   [0, w-1] with `b = 1 << kb`; for a division and remainder on the same
   operands, the redundant link `a = q * b + r` (`(q << k) + r` for
   `b = 1 << k`), so q, k and r are decided and a follows.

Pure CDCL, 10 s budget, 6-12 seeds each:

| input | before | parts 1-3 (+ part 4 by hand) |
|---|---|---|
| `shl_var_64` | timeout | 0.00 s, 12/12 |
| `mul_pow2var_64`, rewrite B by hand | timeout | 0.00 s, 6/6 |
| `onehot_mul_64`, `b = 1 << kb` by hand | timeout | 0.00 s, 6/6 |
| `sdiv_pow2var_64`, link added by hand | timeout | 0.00 s, 6/6 (part 4 alone) |

The hand-rewritten inputs are in `tests/formal/smt2/pow2/cdcl_rewritten/`.
Parts 1 and 3 changed nothing on the other pow2 fixtures. The prototype's part 2
assumed a is odd, so a real version needs the root-level pass.

**Implemented (2026-11-06), parts 1-3:**

- Part 1, `_round_to_stride` (`dvs_prop_templates.c`): shift result rounded
  to multiples of 2^(s0 + z), z = a's static low zeros; a product with fixed
  operands rounded to their combined trailing zeros. `DV_NO_STRIDE` turns it
  off.
- Part 2, static known bits: `_static_known_bits` (`dvs_compile.c`, initial
  compile only) records `ctx->sk_mask/sk_val` from top-level `x[h:l] == c`,
  `x == c` and `x == {pieces}`, through aliasing. The shift propagator uses
  a's exact trailing zeros: r an odd multiple of 2^(z+s) for a fixed s, and s =
  ctz(r) - z once r is fixed. `DV_NO_STATIC_BITS` turns it off.
- Part 3, monotone slice (`_fire_bit_slice_64`): a slice to a's top bit
  passes bounds both ways by shifting.

Pure CDCL, 6 seeds, 10 s, off -> on: `shl_var_64` 6 timeouts -> 6 x 0.00 s;
`mul_pow2var_32` 5 timeouts -> 6 x 0.00 s; `mul_pow2var_64` 6 timeouts -> 4
solved, 2 unknown; the three hand-rewritten inputs all 0.00 s. Unchanged:
`onehot_mul_32/64` and `sdiv_pow2var_64` (they need part 4), and
`axi_burst_bytes_64` (unknown either way, not yet looked at). Every sat model
re-checked by z3. A pure-CDCL fuzz of shifts, multiplies, static low/high bits
and top-bit slices against z3 (2,400 cases): 0 mismatches, and fewer
unknowns than with the rules off (e.g. 17 vs 28, 25 vs 34 per 300).

Own bounds in explanations (checked, not a bug): `_bit_slice_backward` and
`_bv_mul_back_singleton` narrow a variable from its own current bound, and
their explainers leave that bound out. Conflict analysis adds it: every
explainer not on the `_explains_without_own_bound` whitelist gets the
narrowed variable's previous bound (`dvs_lcg.c`, the `e->old_value` literal).
Part 1's stride rule can rely on the same mechanism, as long as its explainer
stays off that whitelist.

Other solvers' use of these facts, and what to adopt:
docs/research/solver_abstract_domains.md.

**Implemented (2026-11-06), part 4** (`dvs_compile.c`, before the constraint
walk of the initial compile; `DV_NO_POW2_REWRITE=1` turns all of it off):

- Power-of-two links (`ctx->p2_link`, pairs `v == 1 << k`): an aux variable
  materialised from `1 << k`, and a top-level `v == 1 << k`. A product with
  such a factor is emitted as `x << k` (`_mul_as_shift`), so parts 1-2 apply
  to it. Unsigned, factor as wide as the product.
- The power-of-two fact: a top-level `(x & (x - 1)) == 0` (read through the
  front end's named intermediates) gets a new index k in [0, w] (1 << w is 0)
  and the propagator `x == 1 << k`. k is a decision variable and x is treated
  as defined (`box_skip`), so the search draws the exponent, not x. k must
  also be in `unassigned_mask` (variables <= 64), or the MRV pick never sees it.
- Division links (`_divrem_links`, `DV_NO_DIVREM_LINK=1` alone): for top-level
  `q == a div b` and `r == a rem b` (bvudiv/bvurem or bvsdiv/bvsrem as lowered,
  with their zero-divisor guard), the divisor matched by variable or by the
  same `1 << k`, the redundant link `a == q * b + r` mod 2^w (`(q << k) + r`
  for a power of two). **Correction to the note above:** it needs no side
  conditions. It holds for every a and b, b == 0 included (x rem 0 == x, and
  q * 0 == 0), because it is only implied by the division, not used to define
  it. Bitwuzla's `elim_udiv` side constraints are needed when the link
  replaces the division.

Pure CDCL, 6 seeds, 10 s (off = `DV_NO_POW2_REWRITE=1`):

| input | off | on |
|---|---|---|
| `onehot_mul_32` | 6 timeouts | 6 x 0.00 s |
| `onehot_mul_64` | 6 timeouts | 6 x 0.00 s |
| `mul_pow2var_64` | 4 sat, 2 unknown | 6 x 0.00 s |
| `sdiv_pow2var_64` | 6 timeouts | 6 x 0.00 s |

Every fixture in `tests/formal/smt2/pow2/` except `axi_burst_bytes_64` now
solves under pure CDCL in 0.00 s on every seed. `axi_burst_bytes_64` gave up
with `unknown` after 0.2 s, rules on or off: a pre-existing propagation
runaway, diagnosed and fixed below.

**`axi_burst_bytes_64`: congruence vs. low slice (diagnosed 2026-11-06, fixed below).**
`addr & 0xfff` compiles to the low slice `addr[11:0]`, decided (216); with
`size = 6`, `addr % 64 == 0` (`_mod_u64`) rounds addr's bound up to a multiple
of 64 (low bits 256), the slice's backward rule (`_bit_slice_backward`) pushes it
to the next page + 216, and so on: one 4 KB page per round over 2^28 pages,
until the trail cap (8.4M entries, 0.3 s). 216 mod 64 != 0, so the domain is
empty, but neither propagator sees it: a congruence on the whole variable never
reaches its low slice. Fix (~50-70 lines): when r and b = 2^k are fixed, the
mod propagator rounds each low slice `a[j-1:0]` (j >= k) of a to `== r mod b`,
via a per-ctx table of low-slice links filled by `prop_add_bit_slice_*`; the
explainer cites r and b. An env-guarded prototype (a static slice table and
a 20-line rounding helper called from `_mod_u64`; removed) turned seeds 1-6 from `unknown` into `sat` in 2-3 ms, models
checked by z3, other fixtures unchanged. A general form is the research's
`tnum_step` rounding over shared known bits (docs/research/solver_abstract_domains.md).

**Implemented (2026-11-06), the low-slice fix** (`dvs_prop_templates.c`):

- Remainder/low-slice link (`prop_add_lowslice_mod_links`, after the
  constraint walk; `DV_NO_SLICE_MOD=1`, or `DV_NO_POW2_REWRITE=1`, turns it
  off). Each unsigned `r = a % b` is paired with every low slice
  `s = a[j-1:0]` of the same a: a bit slice with lo 0, or the low operand of a
  concat (`a[j-1:0] == c` compiles to `a = {h, c}`, not to a slice); at most
  64 links. Once b is fixed at 2^k: k <= j gives `r = s % b` (the remainder
  rule applied to s); k > j gives `s = r[j-1:0]`. The explainer is the
  remainder's over (r, s, b); b's singleton bounds carry the condition.
  This was the runaway's fix: 216 % 64 != 0 is now a conflict at once.
- `1 << s` rounding (`_fire_bvbin_64`, LSHIFT with a the constant 1;
  `DV_NO_POW2_REWRITE=1` turns it off). A nonzero r is 2^s with s < w, so
  r's bounds round to powers of two and s's follow (r >= 37: s >= 6, r >=
  64). Found by the new fuzz: with b = 1 << k free, the search decided b and
  tried 42, 73, ... one non-power at a time, so the link never saw b fixed.

`axi_burst_bytes_64`: 6 seeds, `unknown` (0.22-0.26 s) before, `sat` in
0.00 s after, models checked by z3. Fuzz generator `slice`
(`tests/formal/pow2_fuzz.py --gen slice`: a remainder beside fixed or bounded
low slices, as `extract` or `a & (2^j-1)`, divisor `1 << k`, a constant 2^k or
a variable): of the 10 seed-1 cases cdcl left `unknown`, 8 now solve in 0.00 s
(the link alone solves none of them, the rounding alone 2; both are needed
for the other 6); the 2
left have a general divisor. `test_pow2_cdcl` no longer has an expected
failure (52 passed; 3 fail with `DV_NO_SLICE_MOD=1`).

Verification: CDCL fuzz, gens var/pair/slice, 3,600 cases plus 1,200
earlier slice cases: 0 wrong answers, 0 bad models (one case flagged where
z3 timed out at 20 s; dv-solve's `sat` model checks in z3). Bit-blast fuzz 900
cases: clean. Full suite: ctest 36/36; unit and formal only the known failures
(2 Verilator doc pages; formal 224); soundness seeds 1/2 and wide seeds 1/2/3:
no mismatches, slow>2s 19/26/35. Pure-CDCL tiers 1-3 (118 fixtures, 10 s), rules on vs. all off: identical answers (79 unsat, 35 sat, 4 timeouts), 0 disagreements with z3, total 46.0 s vs. 46.8 s, no fixture moved by more than 2x.

Verification of parts 1-4 together: CDCL fuzz (`tests/formal/pow2_fuzz.py
--engine cdcl`, generators `var` and the new `pair`, 1,800 cases): 0
mismatches, 0 bad models; every sat model of the pow2 fixtures (6 seeds)
re-checked by z3. Full suite: ctest 36/36; unit and formal only the known
failures (2 Verilator doc pages; formal 224: concat_value 22, eval_fuzz 200,
conformance 1, parser_fuzz 1); soundness seeds 1/2 and wide seeds 1/2/3: no
mismatches, slow>2s 19/25/34. Pure-CDCL tiers 1-3 (118 fixtures, 10 s),
rules on vs. all off (`DV_NO_POW2_REWRITE DV_NO_STRIDE DV_NO_STATIC_BITS`):
identical answers (79 unsat, 35 sat, 4 timeouts), 0 disagreements with z3,
total time 45.7 s either way, no fixture moved by more than 2x.

The new propagators are created before the walk, so they carry no constraint
id (`prop_constraint_id`, used only for diagnostics). Problems with a
power-of-two fact lose the box sampler (k is not a declared variable, so a
box draw leaves it open) and fall back to the search.

## Plan

0. **Find the shapes in real workloads.** In the repo's corpus (642 SMT-LIB fixtures, 21
   SV captures), power-of-two operands are rare: 4 `bvmul` and 2 `bvudiv` by a constant
   2^k, 1 `bvsdiv`, 1 `1 << s` operand. `uart_base` multiplies by
   `2**(nco_width+4)` and divides (`CALC_NCO`). Scan larger testbench sources (AXI, PCIe,
   NVMe bus and DMA models; pyvsc and Verilator examples) for `1 << size`, `2 ** n`,
   power-of-two `inside` sets and `$onehot`. Check how Verilator emits `2 ** s`: the
   IR has no power operator, so it arrives as a shift or a constant. Collect a fixture
   set where CDCL is measurably slow or biased.
1. **Fact pass, power of two only.** *Bit-blast: done (above). CDCL: open.* Compile-time inference over the DAG; tests from
   each producer above, and negative tests (`1 << s` with s able to reach the width is
   0, not a power of two; signed widths).
2. **Rewrites A and B** behind an environment switch. *Bit-blast: done, as
   encodings rather than a problem rewrite. CDCL: step 0 shows rewrite B alone
   does not fix its timeouts, so step 3 comes first.* Bit-blast clause counts, pure-CDCL
   A/B (`DV_NO_BITBLAST=1`, tiers 1-3 and `test_cdcl_targets.py`), and sampling quality
   (`tests/formal/test_sampler_properties.py`, `sample_quality.py`) for the `dist`
   transfer.
3. **Known bits and congruence** into CDCL (design above, "Why CDCL stalls";
   order: parts 1-3, then part 4): `dvs_bvdom` for the fact pass, the
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
