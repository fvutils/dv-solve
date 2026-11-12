# Abstract-domain facts in other bit-vector solvers

Research note (2026-11-06) for docs/structural_reasoning_design.md. Question: do
Bitwuzla, Boolector, z3, cvc5, Yices2 and the constraint-programming (CP) BV solvers
use the facts we are adding (known bits, intervals, congruence and parity, powers of
two)? If so, what should dv-solve take from them?

Marking: **[V]** means verified in source code or in a paper's text. **[I]** means
inferred, or read only from an abstract. Local checkouts used:

- Bitwuzla: `/home/mballance/projects/zuspec/zuspec-solver/bitwuzla`, commit 1177124, 2026-03.
- Boolector: `/home/mballance/projects/fvutils/boolector.orig`, 3.2.0-131-gcb8831a1, 2021.

z3, cvc5, Yices2 and the Linux kernel were read on GitHub.

## Summary

| solver | known bits | intervals | congruence / parity | pow2 rewrite | static / dynamic | explanation |
|---|---|---|---|---|---|---|
| **Bitwuzla** local search | yes: ternary `BitVectorDomain`, root-level, from AIG constants [V] | yes: unsigned and signed bounds per node (`prop-ineq-bounds`) [V] | ctz in mul/shl invertibility and consistency conditions [V] | `BV_MUL_POW2`→shl, `BV_UDIV_POW2`→concat/extract, `urem`→zero_extend of extract [V] | known bits static, bounds dynamic | none: local search |
| **Bitwuzla** abstraction (CAV'24) | no | no | `MUL_IC`: ctz(s) ≤ ctz(t) as a lemma [V] | `MUL_POW2` lemma `s=2^i ⇒ t = x<<i` [V] | dynamic, CEGAR | lemmas are BV formulas, then bit-blasted |
| **Boolector** 3.2 local search | no ternary domain in this checkout [V] | no | ctz picks the essential input, ctz-aware consistent values [V] | `apply_power2_udiv` [V] | dynamic | none |
| **z3** rewriter / tactics | SLS only (`sls_bv_fixed`) [V] | `bv_bounds`: one wrap-around interval per term, syntactic, no propagation through operators [V] | SLS: tz(a·b) ≥ tz(a)+tz(b) [V] | shl/lshr/udiv/urem by constant → concat/extract; mul→concat **off** by default [V] | static (preprocessing) | none |
| **z3 PolySAT** (removed from master) | yes, encoded as forbidden intervals on low slices [V] | yes: forbidden intervals in layers by bit-width (`viable.cpp`) [V] | parity lemmas, `parity(p·q)=min(w,parity p+parity q)` [V] | `p=2^k ⇒ p·q=2^k·q` (linearization) [V] | dynamic, CDCL(T) | interval-chain lemma made of bound-like literals [V] |
| **cvc5** | none found [I] | none found [I] | none | `MultPow2`, `UdivPow2`, `UremPow2`; `bv_intro_pow2`: `x&(x-1)=0 → x=1<<k` [V] | static, plus CEGAR mul abstraction [V] | bit-blast |
| **Yices2 MCSat-BV** | bit level, in the eq/extract/concat explainer [V] | forbidden intervals for the linear fragment [V] | multi-width intervals on low extracts, i.e. mod 2^k [V]/[I] | `2^n·u → u[..]∘0^n`; `(u·v)[h:] = u[h:]·v[h:]` [V] | dynamic (MCSat) | interval coverage; core bit-blasting as fallback [V] |
| **mcBV** (SAT'16) | bit-pattern {0,1,u} [V] | `[lo,hi]` per variable [V] | no | no | dynamic (MCSat) | greedy generalization of explanations [V] |
| **CP: COLIBRI, Chihani/Bardin** | bit-list [V] | intervals with one hole [V] | congruence ↔ low bits [V] | n/a | dynamic (propagation) | CP (no clauses); the 2017 word-level CDCL uses masked clauses [V] |
| **eBPF verifier** | tnum [V] | u64/s64/u32/s32 ranges [V] | base/step stride (recent) [V] | n/a | abstract interpretation | n/a |
| **IBM Genesys / X-Gen** (CRV) | masks (DNF of masks) [V] | yes [V] | via masks | n/a | CSP with maintained arc consistency | n/a |
| **pyvsc** | no | static domain ranges [V] | no | no | "swizzler" assumptions on Boolector [V] | n/a |

**Short answer.** Yes, all of them use these facts. Two designs are nearest to ours.

- **PolySAT** is a CDCL search over per-variable interval sets. It has wrap-around
  intervals, fixed bits expressed as intervals on low slices, parity lemmas for
  multiplication, and explanations made of bound-like literals.
- **The CP reduced product of interval × bit-list × congruence** (Bardin 2010, Chihani
  2017, the eBPF tnum plus ranges) gives us exact reduction rules we can copy.

Our split between static facts and dynamic bounds matches Bitwuzla's: its known bits
are root-level constants read from the AIG and need no explanation, while its bounds
change during search.

## Bitwuzla

Sources: [Bitwuzla, CAV 2023](https://cs.stanford.edu/~niemetz/publications/2023/NiemetzP-CAV23.pdf);
[Ternary propagation-based local search, FMCAD 2020](https://bitwuzla.github.io/data/fmcad2020/NiemetzPreiner-FMCAD20.pdf);
[Scalable bit-blasting with abstractions, CAV 2024](https://bitwuzla.github.io/data/NiemetzPZ-CAV24.pdf).

**Known bits come from the AIG, statically [V].** `src/solver/bv/bv_prop_solver.cpp`
(option `prop-const-bits`, default on) bit-blasts each assertion to an AIG and reads
which AIG bits are constant (`d_bitblaster.bits(node)`, `is_true()`/`is_false()`). It
fixes those bits in each node's `BitVectorDomain`.

- This is plain structural constant propagation. The bit-blaster does not solve
  anything, so `x * 4` gets low bits 00 and `x & 0xF0` gets fixed zeros.
- These facts are never explained, because local search has no learned clauses. That
  is our Principle 1.

**Ternary domain [V].** `src/lib/bv/domain/bitvector_domain.{h,cpp}` holds a lo/hi
pair (lo = bits fixed to 1, hi = bits not fixed to 0) with transfer functions for
not, shl, shr, ashr, concat and extract. Our `dvs_bvdom` is a port of it.

**Ranged generator, worth porting [V].**
`BitVectorDomainGenerator(domain, rng, BitVectorRange)` (`bitvector_domain.cpp:398`)
enumerates or randomly draws values that both match the fixed bits and lie in
`[min, max]`.

- It packs the free bits into a counter of `cnt` bits, the number of unfixed bits.
- It computes the smallest packed value whose expansion is ≥ min, and the largest
  whose expansion is ≤ max.
- It draws uniformly between the two and scatters the bits back.
- Packing is monotone, so the draw is **exactly uniform over the matching values in
  the range, with no rejection**.

That is Principle 2's "known bits → `{fixed, free fields}`" change of variable, done
in the sampler without a new variable. `dvs_bvdom.h` has no generator today.

**Local-search invertibility and consistency conditions use ctz
(`src/lib/ls/bv/bitvector_node.cpp`) [V].**

- `BitVectorMul::is_invertible` (l.1236):
  - `IC_wo: ((-s | s) & t) = t`, which is ctz(s) ≤ ctz(t).
  - For odd s the inverse is `s⁻¹·t`.
  - For even s it is `y = (t >> ctz(s)) · (s >> ctz(s))⁻¹`, with the top ctz(s) bits
    free.
- `BitVectorMul::is_consistent` (l.1424): t odd ⇒ x must be odd; t even ⇒ some x
  matching the fixed bits must have ctz(x) ≤ ctz(t).
- `BitVectorShl::is_invertible` (l.1633) for the shift amount: ctz(s) ≤ ctz(t), and the
  amount is `ctz(t) − ctz(s)`.

**Bounds in local search (`prop-ineq-bounds`, default on) [V].**
`BitVectorNode::update_bounds`/`tighten_bounds` (l.203, l.280) keep unsigned and signed
ranges per node, inferred from inequalities. `compute_normalized_bounds` then limits
inverse values, e.g. `bounds.contains(inv)` in the mul code. Known bits and bounds are
used side by side; I found no reduction between them beyond the generator above [I].

**Rewrites [V]** (`src/rewrite/rewrites_bv.cpp`):

- `BV_MUL_POW2` (l.1753): `a·2^k → a<<k` and `a·(−2^k) → (−a)<<k`.
- `BV_UDIV_POW2` (l.2457): `a / 2^n → 0^n ∘ a[w−1:n]`; a second rule rewrites it to
  `bvshr`.
- `urem` by 2^k (l.2739): `zero_extend(a[k−1:0])`.
- `NORM_BV_MUL_POW2_REV` (`rewrites_bv_norm.cpp:248`) goes the other way. In arithmetic
  normalization, `x[..:0] ∘ 0^k` becomes `2^k · x`, so that sums of products can be
  normalized. The right direction depends on what the consumer reasons about.

**`elim_udiv` pass, this is our part 4 link [V].** `src/preprocess/pass/elim_udiv.cpp`
(option `pp-elim-bvudiv`, default **off**) replaces `a udiv b` and `a urem b` by fresh
q and r and adds:

```
b≠0 ⇒ q·b + r = a          b≠0 ⇒ r < b
b=0 ⇒ q = ~0               b=0 ⇒ r = a
¬umulo(q,b)                ¬uaddo(q·b, r)
```

The two no-overflow side constraints are what make `q·b + r = a` hold over the
integers. **We need them too** for interval soundness when part 4 adds the link.

**Abstraction module (CAV'24; `src/solver/abstract/`; `abstraction` on by default for
widths ≥ 33) [V].** Wide mul, udiv and urem start abstract and are refined by lemmas
(`abstraction_lemmas.h`, `LemmaKind`):

- `MUL_POW2: (= s 2^i) ⇒ (= t (bvshl x i))`, instantiated when the model value of s
  is a power of two (`abstraction_lemmas.cpp:186`).
- `MUL_IC: ((−s | s) & t) = t`, the parity rule.
- `MUL_ODD`.
- `UDIV_POW2`, `UREM_POW2`.
- About 50 more lemmas, synthesized by abduction.

Only when these lemmas do not converge is the full multiplier bit-blasted. This is the
CEGAR analogue of our stall oracle.

## Boolector (3.2, local checkout)

- **Local search with ctz [V].**
  - `btorproputils.c:454-495` selects the "essential" input of a mul: an input is
    essential if it is 0 when t ≠ 0, even when t is odd, or has
    ctz(input) > ctz(t).
  - The consistent value for mul (l.1209-1246) chooses a result with
    ctz(res) ≤ ctz(t), preferring 2^n with probability 0.1.
  - The shl consistent value bounds the shift by ctz (l.1096-1116).
- **No ternary domain in this checkout [V].** No `const_bits` or domain option was
  found. FMCAD'20 ternary local search was carried forward into Bitwuzla [I].
- **Rewrites [V].** `apply_power2_udiv` (`btorrewrite.c:4113`):
  `a / 2^n → 0^n ∘ a[w−1:n]`. Rewrite level defaults to 3. No mul-by-2^k → shift rule
  was found by grep [I: it may be handled through the AIG].
- **Preprocessing.** `preprocess/btorextract.c` uses `btor_bv_power_of_two` to detect
  array memset and loop patterns. Not relevant to us.

## z3

Master, plus the `poly` and `polysat` branches. Sources:
[z3](https://github.com/Z3Prover/z3), [poly branch](https://github.com/Z3Prover/z3/tree/poly),
[PolySAT paper, arXiv 2406.04696 / VSTTE'24](https://arxiv.org/abs/2406.04696),
[Rath thesis, TU Wien 2025](https://repositum.tuwien.at/bitstream/20.500.12708/209104/1/Rath%20Jakob%20-%202025%20-%20Efficient%20reasoning%20with%20quantifiers%20and%20theories.pdf).

**Rewriter (`src/ast/rewriter/bv_rewriter.cpp`) [V].**

- `mk_bv_shl` and `mk_bv_lshr` with a constant amount become concat/extract.
- Nested shl becomes `ite(y ≤ y+z, x<<(y+z), 0)`.
- `mk_bv_udiv_core` and `mk_bv_urem_core` with 2^s become lshr or concat/extract.
- `mk_bv_mul` by 2^s becomes concat **only with `mul2concat`, default false**. By
  default `mk_mul_hoist` turns `shl(z,u)·x` into `shl(x·z,u)`.
- `mk_mul_eq`: `c·x = a` with c odd becomes `x = c⁻¹·a`. We could use this as a
  rewrite.
- `le2extract` is disabled (`#if 0`).

**Bounds (`bv_bounds_base.h`, `bv_bounds_tactic`, `bv_bounds_simplifier`) [V].** One
wrap-around interval per term, read from literal shapes only (`x ≤ C`, `x = C`,
`x[w−1:lo] = 0`). There is no propagation through operators (comment: "TBD").
`zero_patch` rewrites x with `hi < 2^b` as `0 ∘ x[b−1:0]`. Simpler than our presolve
plan.

**SLS (`src/ast/sls/sls_bv_fixed.cpp`, `sls_bv_valuation.cpp`) [V].** Fixed bits and a
range for each term:

- `OP_BMUL`: the result's low `tz(a)+tz(b)` bits are fixed to 0. The high-zero rule is
  disabled as "buggy".
- `tighten_range()` uses `inf_feasible(lo)`, the least value ≥ lo agreeing with the
  fixed bits, and `sup_feasible(hi)`.
- When `hi < 2^i`, the bits from i up are fixed. This is the bits↔bounds reduction
  both ways.

**PolySAT [V]. The closest analogue to our CDCL.** It is experimental, needed
`sat.smt=true smt.bv.solver=1`, and is **removed from master**
(`euf_solver.cpp` throws "polysat solver is not integrated"). It survives on the
`poly` branch (2024-07).

- **Domain.** Each variable has a set of forbidden intervals, wrap-around mod 2^w,
  kept in layers by bit-width (`viable.cpp`: `fi_record`, `find_viable`,
  `find_overlap`).
  - `forbidden_intervals.cpp` (`get_interval_ule`, `match_linear1..4`) turns
    `a1·v + e1 ≤ a2·v + e2` into an interval on v, with side conditions
    `e_i = eval(e_i)`.
- **Fixed bits as intervals.** `fixed_bits.cpp::check`: `x[h:l] = n` violated by the
  candidate adds the forbidden interval `x[h:0] ∉ [2^l(n+1), 2^l·n[` on the (h+1)-bit
  layer.
  - The paper (§V-A) gives the equivalent inequalities: `x[h:0]=n ⇔ 2^k·x = 2^k·n`
    with k = w−h−1.
  - **So fixed bits need no new literal kind: they are interval facts on a low
    slice.**
- **Conflict lemma (§IV-C).** When the intervals cover the domain:
  `⋀ C_i ∧ ⋀ h_i ∈ I_{i+1} ⇒ ⊥`, a chain in which each interval's upper end lies inside
  the next interval.
- **Parity.**
  - `parity(p) ≥ k ⇔ 2^(w−k)·p = 0` (`solver.h::parity_at_least`).
  - `try_parity` in old `math/polysat/saturation.cpp` has eight rules on `a·x + b = 0`,
    e.g. `parity(b) ≥ parity(a) + parity(x)`.
  - Paper Lemma 6: `parity(p·q) = min(w, parity p + parity q)`; `p·q = 0 ⇒ parity p +
    parity q ≥ w`; `p·q = 1 ⇒ odd p`.
- **Even coefficients** (`check_equal_lin`). `2^k·a'·v = b` has solutions spaced
  2^(w−k) apart, and the gaps between them are forbidden. This is congruence reasoning
  expressed as intervals.
- **Overflow.** `msb p + msb q ≤ w` means no overflow, `≥ w+2` means overflow, and
  `= w+1` is decided with a (w+1)-bit product (`try_umul_blast`).
- **Lazy op constraints** (`op_constraint.cpp`).
  - Up-front axioms: `r ≤ p` for lshr; `q ≥ w ⇒ r = 0`; `p & q ≤ p, q`;
    `p | q ≥ p, q`.
  - Once the shift amount is fixed, `saturate_lshr`:
    `q = k ⇒ 2^k·r ≤ p ≤ 2^k·r + 2^k − 1 ∧ r < 2^(w−k)`.
  - `saturate_shl`: `q = k ⇒ r = 2^k·p`.
- **Fallback.** Bit-blasting is the last resort.
- **Note.** The paper reports 135 problems solved that z3 did not, yet it was taken out
  of master. That suggests the approach is promising but costly to make robust [I].

**SAT-based bv_solver (`src/sat/smt/bv_solver.cpp`) [V].** A pure bit-blaster.

- `bv.delay` (default off) bit-blasts mul, udiv and urem lazily.
- Before a mul is blasted, it checks `check_mul_invertibility` (`(x | −x) & (x·y) = x·y`,
  the trailing-zeros invariant) and `check_mul_one`.

## cvc5

Source: [cvc5 bv_options.toml](https://github.com/cvc5/cvc5/blob/main/src/options/bv_options.toml)
and `src/theory/bv/`.

- **Rewrites [V].** `MultPow2` (±2^k → shift/concat), `UdivPow2`, `UremPow2`
  (`theory_bv_rewrite_rules_simplification.h`).
- **`bv_intro_pow2.cpp` (`--bv-intro-pow2`) [V].** Rewrites `x & (x−1) = 0` to
  `x = 1 << sk`, where sk is a fresh skolem. It is sound for x = 0 because a shift
  ≥ w gives 0. **This is exactly the change of variable `onehot_mul_64` needed**
  (step 3 part 4).
- **`--bv-abstraction` [V].** CEGAR on mul, udiv and urem of width ≥ 33, as in
  Bitwuzla.
- **`--solve-bv-as-int` [V].** Int-blasting (Zohar et al., VMCAI'22).
- **No interval or known-bits domain** was found in the BV theory solver [I].

## Yices2 MCSat-BV and mcBV

Sources: [Graham-Lengrand, Jovanović, Dutertre, IJCAR 2020, arXiv 2004.07940](https://arxiv.org/abs/2004.07940);
[yices2 src/mcsat/bv/explain](https://github.com/SRI-CSL/yices2/tree/master/src/mcsat/bv/explain);
[Zeljić, Wintersteiger, Rümmer, mcBV, SAT 2016](http://www.philipp.ruemmer.org/publications/mcbv2016.pdf).

**Yices2 [V].**

- Each variable's feasible set is a BDD (CUDD), not an interval.
- Explanations come in three tiers:
  - `eq_ext_con.c`: the equality, extract and concat fragment, explained at bit level.
  - `arith.c`, `arith_intervals.c`, `arith_norm.c`: the linear fragment, explained
    with forbidden intervals.
  - `full_bv_sat.c`: bit-blasting of only the conflict core.
- Intervals are grouped by the width of low-bit extracts `y[w:]`. Coverage starts at
  the widest layer, and a hole narrower than 2^w2 is projected down to the narrower
  layers. This is how mod-2^k structure is covered [I: our reading].
- Normalization rules: `2^n·u → u[..]∘0^n` and `(u1·u2)[h:] → u1[h:]·u2[h:]` (the low
  bits of a product depend only on the low bits of its inputs).
- The core-bit-blasting tier is the same idea as our stall oracle: blast only the
  conflict's cone, and get back a clause in the word-level vocabulary.

**mcBV [V].**

- Each variable carries a bit-pattern {0,1,u} and bounds `[lo,hi]`.
- Reductions:
  - pattern → bounds: lo = pattern with u→0, hi = pattern with u→1.
  - bounds → pattern: the common prefix of lo and hi becomes fixed bits.
- **Explanation generalization** (Alg. 2): walk a lattice of weaker literals, relaxing
  `x = v` to `a ≤ x ≤ b` or turning fixed bits back to u. Each weakening is checked by
  monotone propagation, and is kept if the conflict persists. This is the remedy for
  "each conflict removes one value".

## Constraint programming and abstract interpretation

Sources: Bardin, Herrmann, Perroud, TACAS 2010 (COLIBRI);
[Chihani, Marre, Bobot, Bardin, CPAIOR 2017](https://cea.hal.science/cea-01795779/document);
[Chihani et al., word-level CDCL, arXiv 1706.09229](https://arxiv.org/abs/1706.09229);
[Michel & Van Hentenryck, CP 2012](https://link.springer.com/chapter/10.1007/978-3-642-33558-7_39) (abstract only);
[Linux tnum.c](https://github.com/torvalds/linux/blob/master/kernel/bpf/tnum.c) and
`verifier.c`; Vishwanathan et al. on `tnum_mul` ([arXiv 2105.05398](https://arxiv.org/abs/2105.05398), abstract);
[Alt-Ergo BV, SMT 2025](https://ceur-ws.org/Vol-4008/SMT_paper20.pdf).
Miné 2012 was not read [I].

Reduction rules for interval × bits × congruence, ready to implement. Notation: 1x =
mask of bits known 1, 0x = mask of bits known 0, `fls` = position of the highest set
bit.

1. **Bits → bounds [V].** `lo' = max(lo, 1x)`, `hi' = min(hi, ~0x)`.
2. **Bounds → bits, common prefix [V]** (Chihani; kernel `tnum_range`).
   `d = (1 << fls(lo ^ hi)) − 1`; the bits outside d are fixed to `lo & ~d`.
3. **Round a bound to the next value matching the bits [V].** Kernel
   `tnum_step(t, z)` returns the smallest member of t greater than z, in O(1) word
   operations:

   ```
   d      = z - t.value
   filled = d | ((1 << fls(d & ~t.mask)) - 1) | ~t.mask
   result = t.value | ((filled + 1) & t.mask)
   ```

   Chihani Alg. 1 does the same in O(w). z3 SLS has `inf_feasible`/`sup_feasible`.
   **This generalizes our part 1 stride rounding from trailing zeros to any fixed
   bits.**
4. **Congruence ↔ low bits [V].** k known low bits equal to r ⇔ `x ≡ r (mod 2^k)`. The
   kernel's `__reg_bound_offset` does `var_off &= ~0 << ctz(...)`.
5. **Round both bounds to a stride [V].** Kernel `cnum64_intersect_linear`:
   `lo += (base − lo mod step) mod step`, `hi −= (hi − base) mod step`; an empty
   result is a conflict.
6. **Singleton [V].** If lo is in the tnum and `tnum_step(lo) > hi`, then x = lo
   (`__update_reg64_bounds`).
7. **Gap drilling [V]** (Chihani §4.1). Split the domain at the most significant
   unknown bit into `[1x .. a] ∪ [b .. ~0x]`, which yields one hole.
8. **Extract from an interval [V]** (Chihani §4.2). For `x[i:j]` with x in [a,b]:
   - if `(b>>j) − (a>>j) > 2^(i−j)`, the result is the full range;
   - else if `(a ^ b)` has bit i clear, the result is `[a[i:j], b[i:j]]`;
   - otherwise it is a two-piece range.

   For the top slice (i = w−1) the rule is monotone: `[a>>j, b>>j]`. **This is our
   part 3.**
9. **Mul and add [V].** Alt-Ergo keeps only the known low bits of a product (a
   congruence). Its add rule: `m = 1x+1y`, `M = m + ux + uy`, `ur = ux | uy | (m ^ M)`.
   The kernel's `tnum_mul` is proven sound and optimal.
10. **Chihani 2017 word-level CDCL [V].** Explanations are (level, mask, clause), with
    masks selecting which bits each explanation covers. So a CDCL with bit-pattern
    literals exists, but it needs a new literal kind, which we have decided against.

## Constrained-random and sampling solvers

- **IBM Genesys and X-Gen** ([Naveh et al., AAAI 2006](https://cdn.aaai.org/AAAI/2006/AAAI06-287.pdf)) [V].
  - A CSP with maintained arc consistency and stochastic search.
  - Large domains are stored as DNF sets of masks, i.e. known-bits cubes, so a domain
    is a union of known-bits patterns.
  - "A BDD representation was also tried, but so far did not prove useful."
- **pyvsc** [V].
  - `solvegroup_swizzler_range.py` picks a random value or sub-range from a statically
    computed domain and passes it as a Boolector `Assume`.
  - The part-select swizzler fixes random bit slices [I].
  - So randomness comes from assumptions on top of a complete solver, not from the
    domains.
- **Not checked:** CRAVE, SMTSampler, QuickSampler and vendor papers [I].

## What to adopt for dv-solve

Ranked by expected value over effort, and mapped to the plan in
docs/structural_reasoning_design.md.

| # | technique | source | plan step | value | effort |
|---|---|---|---|---|---|
| 1 | Root-level known bits read from bit-blast AIG constants | Bitwuzla `prop-const-bits` | step 3 part 2 (the root-level pass) | high | low |
| 2 | `tnum_step` rounding of CDCL bounds to the known bits; stride rounding | kernel `tnum_step`, `cnum64_intersect_linear`; z3 `inf_feasible` | step 3 part 1, generalized | high | low |
| 3 | Parity rule for `*`: tz(a·b) = min(w, tz a + tz b); backward ctz(s) ≤ ctz(t) | PolySAT Lemma 6; Bitwuzla `MUL_IC`; z3 SLS | steps 1 and 3 (fact pass), propagator | high | low-med |
| 4 | `x&(x−1)=0` → `x = 1<<k`, fresh k | cvc5 `bv_intro_pow2` | steps 1 and 3 part 4 | high (`onehot_mul_64`) | low |
| 5 | `a = q·b + r` link **with ¬umulo and ¬uaddo side constraints** | Bitwuzla `elim_udiv` | step 3 part 4 | high (`sdiv_pow2var_64`) | low-med |
| 6 | Ranged known-bits generator: uniform over matching values in [lo,hi] | Bitwuzla `BitVectorDomainGenerator` | sampling; retires `_pick_mod_aligned` | high (sampling) | low |
| 7 | Dynamic parity and known bits as **bound literals on a low-slice aux variable** | PolySAT §V-A | after step 3; lifts Principle 1's limit | med-high | med |
| 8 | Explanation generalization: widen `x=v` to the widest refuted `[a,b]` | mcBV Alg. 2 | complements step 4 | med | med |
| 9 | Exact mul via msb overflow classes; odd-c inversion `c·x=a → x=c⁻¹a` | PolySAT; z3 `mk_mul_eq` | step 2 rewrites | med | low |
| 10 | Conflict-core bit-blasting (oracle) | Yices `full_bv_sat.c`; Bitwuzla CEGAR | step 4 | confirms design | — |
| 11 | Wrap-around forbidden intervals, layered by width | PolySAT, Yices | none (architectural) | unknown | high |

**1. Read known bits from the AIG** (Bitwuzla). We already build an AIG in
`dvs_bbsolver`. Instead of writing transfer functions in `dvs_bvdom` for every
operator, blast each term's cone and read the constant bits. That covers mul by a
constant, masks, concat and shifts at once, and is the "small root-level pass" that
part 2 needs.

Constant propagation alone does not see top-level assertions such as `x % 8 == 0`.
For those, either:

- assert the root-level constraints and read the SAT level-0 units, which is still
  static and unexplained (Principle 1 holds, because they are implied by the base
  constraints) [I]; or
- recognise the alignment forms syntactically (as `_mrw_align` did before its
  removal on 2026-11-12, B101).

**2. Bit-aware bound rounding** (kernel, z3 SLS, Chihani). Part 1 rounds to a stride
2^s. `tnum_step` rounds to the next value matching *any* fixed-bit pattern in O(1),
which also covers odd/even (part 2's "a is odd") and masks. Add `dvs_bvdom_step_up`
and `dvs_bvdom_step_down`, plus rules 1, 2 and 6 above. With static bits the
explanation is just the bound being rounded (`x ≥ lo ⇒ x ≥ step(lo)`), which also
stays off the `_explains_without_own_bound` whitelist.

**3. Parity for multiply and shift** (PolySAT, Bitwuzla, z3 SLS).

- *Statically:* in the fact pass, `tz(a·b) ≥ tz(a)+tz(b)`, `tz(a<<k) ≥ tz(a)+k_min`,
  and `tz(2^k) = k`.
- *Dynamically, without new literal kinds:* the forward rule needs only the shift
  amount's bound (`k ≥ k0 ⇒` round p to the 2^(k0+tz a) stride), and that is part 1.
- *Backward:* Bitwuzla's invertibility condition `ctz(s) ≤ ctz(t)` is a ready-made
  conflict test for `t = x·s` once s or t is fixed.

**4. `x & (x−1) == 0` → `x = 1 << k`** (cvc5). This is the rewrite `onehot_mul_64`
was solved with by hand. cvc5 shows it is sound even for x = 0, since a shift ≥ w gives
0, which removes the "0 or power of two" case split, provided k's range is [0, w]. Our
`bb_note_pow2` already recognizes the shape; do the problem rewrite in
`dvs_compile.c`.

**5. Division link with overflow guards** (Bitwuzla `elim_udiv`). When part 4 adds
`a = (q << k) + r`, also add `r < b`, ¬overflow(q·b) and ¬overflow(q·b + r). Without
the overflow guards, wrap-around makes the link non-monotone, and CDCL's interval
propagation through it is unsound or weak [I]. Note Bitwuzla keeps this pass off by
default. It is a known trade-off, so A/B it.

**6. Uniform known-bits sampling** (Bitwuzla generator). Port
`BitVectorDomainGenerator`'s ranged random draw into `dvs_bvdom` and use it when CDCL
picks a value for a variable with static known bits. This gives the exact sampling
Principle 2 promises, without adding a variable for the known-bits row of the
change-of-variable table, and can retire `_pick_mod_aligned`. Our known-bits change of
variable becomes a sampler feature rather than a new variable.

**7. Dynamic facts as bounds on slices** (PolySAT). Principle 1 put
"x became a power of two once y was decided" out of scope because it needs new literal
kinds. PolySAT shows it does not: `tz(p) ≥ k` is `p[k−1:0] ≤ 0`, and a fixed slice is a
bound pair on a slice variable. If we materialize a low-slice aux variable `p_lo`
(part 3 already creates `a[63:1]`-style slices), dynamic parity becomes ordinary bound
literals on `p_lo`. Do it only for the variables where the fact pass found shift or
multiply structure.

**8. Explanation generalization** (mcBV). Our stall is "one value per conflict". Before
learning, try to widen each `x ≥ v`/`x ≤ v` pair in the conflict clause to the widest
interval that still fails propagation (binary search on the bound, re-running the
propagators). This is cheaper than the oracle per call, and has the same effect when
the reason is interval-expressible. It does not help the relational `k = 0` trap, which
is what the oracle (step 4) is for.

**9. Small rewrites** (step 2).

- z3 `mk_mul_eq`: `c·x = a` with c odd ⇒ `x = c⁻¹·a`.
- For `c = 2^j·c'`: the congruence `x ≡ c'⁻¹·(a>>j) (mod 2^(w−j))`, plus
  `tz(a) ≥ j` (PolySAT `check_equal_lin`).
- PolySAT's msb rule decides when `p·q` cannot wrap, so the interval product is exact.

**Validated choices.**

- *Static facts unexplained* (Principle 1): Bitwuzla does exactly this.
- *Change of variable over a new domain* (Principle 2): cvc5's `bv_intro_pow2` and
  Bitwuzla's `elim_udiv`.
- *Bit-blast as a stall oracle* (step 4): Yices' core bit-blasting and the Bitwuzla and
  cvc5 CEGAR abstractions.

**Not recommended now.** A full PolySAT-style wrap-around forbidden-interval domain
(11). It is the principled end state, but z3 removed it from master despite good
results, and our int64 interval plus holes already covers most of it.
