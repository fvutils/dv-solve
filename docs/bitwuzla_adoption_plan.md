# Bitwuzla through the Formal+SAT lens: what dv-solve should adopt

Date: 2026-05-25
Status: design proposal

## Scope

dv-solve is not trying to be a general SMT solver. It is a *toolbox* of
techniques composed for two distinct domains:

- **Formal** (BMC / property checking): tight, structural BV problems where
  unsat cores, learning, and bit-blasting matter; counterexamples are short
  but search is adversarial.
- **Randomization** (CRT / DV stimulus): mostly-satisfiable, *wide* problems
  where we want a good distribution of solutions, fast first-solution time,
  and the ability to nudge bounds rather than search exhaustively.

Bitwuzla is the strongest BV solver in the open ecosystem and conveniently
contains *both* a CDCL(T)-style bit-blasting pipeline (good for Formal) and a
propagation-based local-search engine (good for Randomization). Reading it as
a menu of components rather than as a monolith, here is what is worth pulling
into dv-solve and in what order.

## What bitwuzla actually is, as components

```
SMT2 parser
  ↓
Rewriter  (rewrites_bv.cpp, rewrites_bv_norm.cpp, rewrites_bool.cpp, ...)
  ↓
Preprocessor passes:
    flatten_and, contradicting_ands, normalize, embedded_constraints,
    variable_substitution, skeleton_preproc, elim_udiv, elim_lambda
  ↓
SolverEngine drives BV theory solver, which is one of:
    (a) BvBitblastSolver   → AigBitblaster → CaDiCaL / Kissat   (Formal)
    (b) BvPropSolver       → lib/ls (LocalSearchBV)              (Random-ish)
  ↓
optional BvInterpolator (Craig interpolants from the SAT proof)
```

Two pieces in `lib/` are *standalone libraries* with no bitwuzla-solver
dependency and are the cleanest reuse targets:

- `lib/bv/domain/bitvector_domain.{h,cpp}` — three-valued domain (fixed-0,
  fixed-1, unknown) per bit, with per-op invertibility / consistency checks.
- `lib/bv/bounds/bitvector_bounds.{h,cpp}` — interval bounds (signed +
  unsigned) per BV term.
- `lib/ls/` — the propagation-based BV local-search engine itself, built on
  top of the domain + bounds libraries.
- `lib/bitblast/` — AIG bit-blaster (templated on the AIG node type).

These are MIT-licensed and reusable without dragging the SolverEngine in.

## Current dv-solve gaps (by component)

| Bitwuzla piece                    | dv-solve analog                       | Gap |
|-----------------------------------|---------------------------------------|-----|
| `BitVectorDomain` (3-valued bits) | `zsp_wiremask`                        | dv-solve tracks known bits but not the per-op invertibility/consistency rules that drive value selection; wiremask is consumed, not exploited for value choice |
| `BitVectorBounds` (sign+unsigned) | trail/range on integer vars in CDCL   | no first-class BV interval domain; bounds are encoded into clauses rather than propagated as a domain |
| `Rewriter` (5 files of patterns)  | partial in `zsp_compile.c`            | no systematic rewrite layer; we re-derive simplifications per propagator |
| Preprocessing passes              | none coherent                         | no normalize / variable_substitution / contradicting_ands / embedded_constraints; these are the *cheapest* wins on hard fixtures |
| AIG bit-blaster                   | none                                  | we have CDCL over high-level domain only; no bit-blast path |
| External SAT (CaDiCaL/Kissat)     | not wired                             | sources are now in `resources/`; no build hookup |
| `LocalSearchBV` (propagation LS)  | none                                  | randomization currently relies on CDCL-with-bounds-adjustment; no LS engine |
| Interpolation                     | `zsp_explain` / `zsp_nogood`          | we produce learned clauses but no Craig interpolants |
| CDCL(T) loop                      | `zsp_search` + LCG                    | core present, but theory layer is monolithic |

## Recommended adoption order

The principle: **adopt cheap leveraged pieces first, keep dv-solve's CDCL(T)
core, and use bitwuzla components as *theory back-ends* rather than replacing
the search.**

### Phase A — Preprocessing & rewriting (highest ROI, lowest risk)

Pre-search simplification is what closes the gap between dv-solve and
bitwuzla/z3 on tier-1/tier-3 fixtures more than any search-side change. The
five tier1 false-unsat bugs (see [[false_unsat_tier1]]) are all
`bvand + range + masked-bound` patterns that fall out under a `normalize` /
`contradicting_ands` style pass.

1. **Port `BitVectorDomain`** as a C library (`zsp_bvdomain.{c,h}`) backing
   `zsp_wiremask`. The crucial extension over the current wiremask is the
   per-op **invertibility / consistency** predicates: given target value `t`
   and fixed sibling bits, can operand `x` be inverted? This is what lets the
   domain *prune values*, not just record them.
2. **Port `BitVectorBounds`** as `zsp_bvbounds.{c,h}` — signed and unsigned
   intervals per BV term, with the standard meet/widening operators. Hook
   into the trail so each decision/propagation tightens or restores bounds.
3. **Stand up a rewriter** (`zsp_rewrite.c`) with the bitwuzla pattern set
   ported by category:
   - bool: `not(not x) → x`, ITE pushdowns, `and/or` absorption
   - bv: `bvand x 0 → 0`, `bvor x ~0 → ~0`, shift/mul-by-power-of-2,
     extract-of-concat, sign-ext idempotence
   - normalization: AC flatten of `and`/`or`/`xor`/`bvadd`/`bvmul`
4. **Preprocessing passes** as a pipeline run before CDCL:
   - `flatten_and` (cheapest, mostly a normalization win)
   - `contradicting_ands` (directly attacks the tier-1 false-unsat shape)
   - `embedded_constraints` (substitute equalities into the body)
   - `variable_substitution` (bitwuzla's largest preprocessing pass, 1391
     lines; port the BV-affine subset first)
   - `normalize` (1782 lines; port last and incrementally — the polynomial
     normalization is what cracks bvand+mask+bound)
   - `skeleton_preproc` (small but effective skeleton-level Boolean SAT pass)

Exit criterion for Phase A: tier-1 false-unsat fixtures resolve without any
search-side change.

### Phase B — Bit-blast + Kissat-derived SAT path (Formal back-end)

For formal problems we should give up on doing *everything* with high-level
CDCL and just bit-blast. The SAT layer is Kissat-derived, not a multi-backend
shim — see D1.

1. **[DONE 2026-05-25] Bridge (B.0): link Kissat as a submodule** in CMake.
   Implemented via `cmake/kissat.cmake` — builds 88 .c files into a static
   `kissat` target, generates `build.h` reproducibly (no git dependency),
   uses default release flags (`NDEBUG QUIET NPROOFS`), links libm publicly.
   `tests/c/test_kissat_smoke.c` exercises `kissat_init/add/solve/value/release`
   on trivial SAT+UNSAT instances; registered with CTest as `test_kissat_smoke`.
   Use it non-incrementally — one fresh solver instance per `check-sat`. BMC
   frames are still produced incrementally at the dv-solve level (we re-blast
   only the new frame), so this is acceptable as a bring-up step.
2. **Port `lib/bitblast/`** as `zsp_aig.{c,h}` + `zsp_aig_cnf.{c,h}` +
   `zsp_bitblast.{c,h}` — templated AIG node type becomes a concrete `int32_t`
   id (positive id / negative id = literal sign; arena-stored data). ~1k lines
   of mostly mechanical code, ported to C.
   - **[DONE 2026-05-25] `zsp_aig.{c,h}`** — AIG manager with arena storage,
     hash-consing of AND gates, full Brummayer/Biere level-1..4 rewriting
     rules. No ref-counting / GC (AIG built monotonically per check-sat).
     `tests/c/test_zsp_aig.c` exercises all rule families + hash-consing.
   - **[DONE 2026-05-25] `zsp_aig_cnf.{c,h}`** — Tseitin encoder. Iterative
     DFS over the AIG, emits 3 clauses per AND or 4 clauses per detected
     ITE (with the `parents() == 1` sharing check from upstream). Top-level
     flattening through positive ANDs. Identity mapping from AIG id to SAT
     var id. `tests/c/test_zsp_aig_cnf.c` covers AND, XOR, ITE, asserted-FALSE.
   - **[DONE 2026-05-25] `zsp_bitblast.{c,h}`** — Full BV bit-blaster.
     Operations: not, and, or, xor, eq, ult, slt, shl, shr, ashr, add, neg,
     sub, mul, udiv, urem, extract, concat, zero_ext, sign_ext, ite. Bit
     ordering matches upstream (MSB = index 0). Result `zsp_bv_t` arrays
     allocated from a bump arena owned by the bb context (freed wholesale).
     `tests/c/test_zsp_bitblast.c` exercises every op end-to-end through
     AIG → CNF → kissat, including model readback for a factoring problem.
3. **`zsp_bbsolver.c`** — a theory-level solver that bit-blasts a goal,
   feeds it to the SAT layer, and surfaces the model. Used in formal mode.
4. **Incremental bit-blasting at the dv-solve layer**: bitwuzla's
   `BvBitblastSolver` only blasts newly-asserted terms between checks;
   preserve that property because BMC adds frames incrementally — important
   even when the underlying SAT solver is rebuilt from scratch each call.
5. **Driver flag**: `--engine=cdcl|bitblast|auto`. Auto picks bitblast for
   BV-only, quantifier-free, no-propagator problems.
6. **Adapt (B.1): fork Kissat into `src/c/sat/`**. Add incremental
   assumptions and push/pop. Route allocations through `zsp_alloc_t`,
   migrate the clause arena to `zsp_pool`-style 32-bit references, integrate
   with the dv-solve trail and `LevelMark` checkpoints. See
   [[sat_memory_management_plan]] for the detailed adoption sequence — the
   pluggable allocator, checkpoint-keyed arena marks, and unified trail are
   the dv-solve-native advantages we layer on top of the Kissat core.

Exit criterion for Phase B.0: tier-2 and tier-3 formal fixtures match or
beat yosys-smtbmc-driven bitwuzla using non-incremental Kissat.
Exit criterion for Phase B.1: incremental BMC across many frames stays
within bounded RSS and matches Phase B.0 on first-frame latency.

### Phase C — Local search back-end (Randomization)

This is the *crossover* the user called out. Propagation-based local search
is exactly the right engine for randomization: it is incomplete, it produces
diverse solutions, and the "essential input" path selection naturally maps
to the random-variable bias dv-solve already tracks.

1. **Port `lib/ls/`** to C — `LocalSearchBV` + `BitVectorNode` + the per-op
   inverse/consistent value functions. Re-uses the C `zsp_bvdomain` /
   `zsp_bvbounds` libraries from Phase A. Per D2, the port is C-only;
   bitwuzla's C++ template polymorphism collapses to explicit dispatch tables
   over `NodeKind`.
2. **Hybrid scheduler**: run LS as a *first attempt* with a short propagation
   budget (e.g. 10k props). If it finds a model, return it. If it stalls,
   fall through to CDCL. This is what bitwuzla itself does as
   `--engine=prop` vs. the default bitblast portfolio.
3. **Randomization-specific knobs**: bitwuzla's `prob_pick_inv_value`,
   `prob_pick_ess_input`, and `use_ineq_bounds` are all knobs that *directly
   correspond* to DV randomization concepts (distribution bias, soft
   constraints, range hints). Expose them through the dv-solve API rather
   than hard-coding bitwuzla defaults.
4. **Solution diversity**: extend LS with a "tabu" / "blocking" mode that,
   after returning a solution, perturbs the value selection RNG and the
   essential-input path probability before the next `solve()` — this is what
   makes LS a *good randomization engine* rather than just a fast SAT
   engine.

Exit criterion for Phase C: randomization workloads (large `randomize` calls
with soft constraints and distribution hints) get sub-millisecond solutions
and visibly diverse distributions.

### Phase D — Crossover techniques

Once Phases A–C exist as independent engines, the interesting work is
combining them. Candidates:

- **Phase-A domain → CDCL guidance**: feed bound/domain tightening from the
  preprocessor into CDCL as initial unit clauses on encoded variables. We
  already have wiremask; with `BitVectorBounds` this becomes uniform.
- **LS-seeded CDCL**: run LS for a small budget at the root; if it finds a
  candidate that satisfies most-but-not-all roots, use the candidate as the
  initial *phase* in CDCL's decision heuristic (Glucose-style phase saving
  with an external seed). This is bitwuzla's `--preseed-ls` idea.
- **CDCL-driven LS restart**: when CDCL learns a conflict clause that names a
  small set of variables, restart LS with those variables free and others
  pinned to the conflict-side assignment. Particularly useful for the tier-3
  fixtures where CDCL is fine on the Boolean skeleton but LS is faster on
  the arithmetic body.
- **Interpolant-driven preprocessing**: port `BvInterpolator` to extract
  Craig interpolants from UNSAT proofs and feed them back as learned
  invariants in BMC-style frames. This is the piece that lets dv-solve
  *beat* a vanilla bitwuzla invocation on long BMC chains.
- **Shared rewriter, two consumers**: the rewriter and preprocessor from
  Phase A apply equally to both Formal and Randomization. Make them
  engine-agnostic so the same `simplify()` call serves both paths.

## What NOT to adopt

- **SolverEngine / Env / Options machinery**. dv-solve already has its own
  context (`zsp_ctx`); duplicating bitwuzla's option system is gratuitous.
- **The full FP and quantifier theories** (`solver/fp`, `solver/quant`). Out
  of scope for DV.
- **Bitwuzla's term/node manager**. We already have a DAG; map bitwuzla
  patterns onto it rather than swapping in a foreign node manager.
- **CryptoMiniSat / Gimsatul backends**. CaDiCaL + Kissat covers the space.

## Ordering rationale

Phase A first because it is the lowest-risk, highest-leverage change and
directly retires the five known tier-1 soundness bugs (see
[[false_unsat_tier1]]). Phase B unlocks Formal performance and is mechanical
once SAT is wired. Phase C is the most novel work for dv-solve and depends
on the domain + bounds libraries from Phase A. Phase D is research.

## Decisions

These were the original open questions; the user's answers are now baked into
the relevant phases above. Recording them here for traceability.

### D1 — SAT backend strategy: adapt Kissat, not just consume it

Kissat is simpler and a better adaptation target than CaDiCaL, *but* it does
not support incremental solving. That changes Phase B from "wire both
backends behind a shim" to a two-step plan:

- **B.0 (bridge)**: link Kissat as a build-time submodule and use it
  non-incrementally — one fresh solver instance per `check-sat`. Acceptable
  for the initial bit-blast path because BMC frames are produced
  incrementally at the *dv-solve* level (re-blasting only the new frame),
  even if the SAT layer below is non-incremental.
- **B.1 (adapt)**: fork Kissat into `src/c/sat/` and graft on the pieces it
  is missing: incremental assumptions, push/pop, and — critically — routing
  through `zsp_alloc_t` and the dv-solve checkpoint/trail patterns (see
  [[sat_memory_management_plan]]). This is the path that lets us *replace*
  the external SAT solver over time with a dv-solve-native CDCL core that
  inherits our memory-management strengths.

CaDiCaL is *not* a parallel backend. We keep its sources around as a
reference for incremental-SAT correctness (assumption interface, restart
heuristics, GC policy) but we do not link it. Drop the
`zsp_sat.h`-with-two-backends shim — there is only one backend, and it is
ours-built-on-Kissat.

### D2 — Phase C local search is C-only

The LS engine ports to C, consistent with the rest of `src/c/`. No C++
option. Cost is roughly 2× the effort of a faithful C++ port, paid for by
ABI uniformity, no exception-handling juggling, and consistency with the
allocator vtable. Phase C item 1 ("Port `lib/ls/`") should be read as a C
port from the start.

### D3 — Phase A "normalize" is tier-1-first, then grows

Do not attempt a faithful 1782-line port of bitwuzla's `normalize.cpp`.
Implement only the rewrite rules and polynomial-normal-form steps needed
to retire the five known tier-1 false-unsat fixtures (the `bvand + range +
masked-bound` family). Grow the pass only as new failing fixtures motivate
new rules — every added rewrite must point at a fixture it unlocks.
