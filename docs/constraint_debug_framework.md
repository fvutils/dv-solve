# Constraint Debug Framework — Research, Design & Recommendations

**Status:** research / design proposal for review
**Date:** 2026-06-11
**Scope:** Minimizing UNSAT cores, communicating failure reasons, and
suggesting resolutions to users — designed as a *separable add-on* so the
core solver stays lightweight enough for embedded targets.

---

## 0. TL;DR

dv-solve already ships a solid first-generation debug capability
(`ZSP_CONTRADICTION_ANALYSIS`: QuickXplain MUS, var-const relaxations,
soft-constraint diagnostics, text/JSON output, build-flag gating — see
`docs/contradiction_analysis.md`). It also has the harder-to-build
asset that most explanation tools lack: a **lazy-clause-generation
(LCG) layer with per-propagator `explain()` callbacks**
(`zsp_explain.c`, `zsp_lcg.c`). That is a working *white-box proof
engine* sitting unused by the debug layer.

The research below converges on a clear path forward. The recommended
framework is a **layered, cost-escalating diagnosis pipeline** with a
strict separation between:

1. a tiny **core hook layer** (in C, no-op when compiled out),
2. a **black-box analysis layer** (solver-agnostic, only needs
   incremental solve-under-assumptions — already what QuickXplain uses),
3. a **white-box / proof layer** (reuses the LCG `explain()` callbacks
   to produce derivation trees), and
4. a **presentation + advice layer** in Python (`dv_solve`) that owns
   the heavy, non-embedded work: enumeration orchestration, the
   domain-specific failure taxonomy, natural-language reports, and
   fix suggestions.

Embedded builds keep only layer 1 (zero cost when off). Everything a
host workstation needs lives in layers 2–4.

---

## 1. Problem framing

When `solver_solve()` does not return a usable result, the user faces
one of four situations, and each needs a *different* explanation:

| Outcome | User's real question | Primary technique |
|---|---|---|
| **UNSAT** | "Which of *my* constraints fight each other, and how do I fix it?" | MUS + MCS + corrective (counterfactual) explanations |
| **SAT but wrong distribution / a soft dropped** | "Why did I get this value / why was my preference ignored?" | soft-relaxation diagnosis, model core, distribution analysis |
| **TIMEOUT / UNKNOWN** | "Is it really infeasible, or just slow? What's making it slow?" | timeout-core, difficulty scoring, incompleteness reasons |
| **SAT but user *expected* UNSAT (or vice-versa)** | "Why is the outcome X and not Y?" | contrastive / counterfactual explanation |

Today's module covers UNSAT (single MUS, var-const relaxations) and
part of the soft case. The big gaps, by the module's own "Limitations"
section and the survey below, are: only one MUS (no enumeration of
*independent* conflicts), relaxations limited to `var op const`, no
proof/derivation output, weak timeout-vs-unsat distinction, no
performance ("why slow") diagnosis, and no domain-aware fix advice.

---

## 2. Literature survey

### 2.1 Minimal Unsatisfiable Subsets (the "what conflicts" question)

A **MUS** (a.k.a. minimal conflict / minimal unsat core) is a subset of
constraints that is unsatisfiable but becomes satisfiable if any single
member is removed. Finding one is an instance of the general **MSMP**
problem (Minimal Set subject to a Monotone Predicate); the same engine
that finds a MUS also finds prime implicants, minimal correction
subsets, etc. ([Rodler, *Understanding the QuickXPlain Algorithm*,
arXiv:2001.01835](https://arxiv.org/pdf/2001.01835)).

Core extraction algorithms and their cost (in solver calls, `n` =
constraints, `k` = MUS size):

- **Deletion-based** — try removing each constraint; keep it only if
  removal makes the rest SAT. `O(n)` calls. Simple, robust baseline.
- **Insertion-based** — grow a core until it flips UNSAT.
- **QuickXplain (Junker 2004)** — divide-and-conquer, `O(k·log(n/k))`
  calls. *This is what dv-solve already uses.* Generalizes to any
  monotone predicate, treating the solver as a black box — which is
  exactly why it's the right backbone for an add-on module
  ([Junker, *QUICKXPLAIN*](https://www.semanticscholar.org/paper/QUICKXPLAIN:-Preferred-Explanations-and-Relaxations-Junker/dee653e324b42311f93adf83611a985028f8b54c)).
- **Dichotomic / progression** — binary-search variants.
- **Model rotation** — cheaply confirms a constraint is "transition"
  (necessary) by mutating a model, avoiding a solver call. A pure
  speedup for MUS, *not* applicable to MCS.

**Enumeration of multiple cores** — a single MUS hides the fact that a
problem can have several *independent* contradictions. Fixing one
leaves the others. Key algorithms:

- **CAMUS** — fastest for enumerating *all* MUSes when tractable; works
  via the MUS↔MCS hitting-set duality (below).
- **MARCO** — explores a SAT-solver-encoded "map" of the subset
  space; yields MUSes *and* MCSes incrementally, and produces *some*
  results even when full enumeration is intractable
  ([Liffiton & Sakallah, *Algorithms for Computing MUSes*](https://www.semanticscholar.org/paper/Algorithms-for-Computing-Minimal-Unsatisfiable-of-Liffiton-Sakallah/815b2ca4f366cba71778d2bbe7556adbd96ec841);
  [Liffiton et al., *Fast, flexible MUS enumeration*](https://www.researchgate.net/publication/276905908_Fast_flexible_MUS_enumeration)).

### 2.2 Relaxations / corrections (the "how to fix" question)

- A **conflict** is a subset of user constraints that has no solution
  with the background.
- A **relaxation / Minimal Correction Subset (MCS)** is a subset whose
  *removal* restores satisfiability; a **maximal relaxation** keeps as
  much as possible.
- **MUSes and MCSes are hitting-set duals**: every MCS hits (intersects)
  every MUS, and vice-versa. So enumerating one side yields the other —
  this is the formal bridge from "what's wrong" (MUS) to "what to
  change" (MCS).

**Preferred explanations / relaxations** — when constraints carry
priorities, QuickXplain can be ordered to return the conflict/relaxation
that drops the *least important* constraints first (Junker's original
motivation: over-constrained configuration). dv-solve already has a soft
**priority** model — this is the natural input to a *preferred* core.

**Corrective / counterfactual explanations** — "drop constraint C" is
weak advice. Users want *actionable* changes: "raise the bound from 5 to
10" or "move course B to Tuesday." The CP literature computes these by
iterating conflict detection + maximal relaxation and then finding the
minimal *value* change per relaxed constraint
([Dev Gupta, Genç & O'Sullivan, *Finding Counterfactual Explanations
through Constraint Relaxations*, arXiv:2204.03429](https://arxiv.org/pdf/2204.03429)).
dv-solve's existing `contra_compute_relaxations` is a first step
(var-const only); the generalization is binary-search on a violation
metric over *any* relaxable constraint.

**Core-guided soft solving** — instead of dv-solve's current *sequential*
relax-lowest-priority-and-retry loop, the MaxSAT-style approach treats
soft constraints as hard, solves, extracts the UNSAT core, and re-softens
only the constraints actually in the core. This both diagnoses *and*
speeds up soft solving, and extends cleanly to LCG/CP solvers like
dv-solve ([Downing, Feydy & Stuckey, *Unsatisfiable Cores and Lower
Bounding for Constraint Programming*, arXiv:1508.06096](https://arxiv.org/pdf/1508.06096)).
This is directly relevant: that paper's whole premise is "LCG solvers
can already explain failures, so adapt MaxSAT core-guided solving to
CP" — and dv-solve *is* an LCG/CP solver.

### 2.3 White-box / proof-based explanation

Beyond "which constraints," users (and downstream tools) often want
*why*: a derivation showing how the contradiction follows. dv-solve's
LCG layer already records, per propagation, *which literals implied a
bound change* via `explain_*` callbacks (`zsp_explain.h`). Conflict
analysis resolves these into a learnt clause. That same machinery can
emit a **proof/derivation DAG** — the module's stated "future sprint."
Caveat from `docs/cdcl_explain_soundness_plan.md`: the explain callbacks
must be *sound* (cite the bounds that held at fire time, not current
bounds) before their output is trustworthy for user-facing proofs.

SMT solvers expose this family of interfaces, a useful menu to mirror
([cvc5, *Interfaces for Understanding Solver Results*](https://cvc5.github.io/blog/2024/04/15/interfaces-for-understanding-cvc5.html)):

- **unsat core** — sufficient subset of assertions (≈ our quick core).
- **unsat-core-lemmas** — the *internal theory lemmas* used, i.e. a
  white-box reason set (≈ our LCG explanations).
- **get-proof** — step-by-step refutation, configurable granularity.
- **model core** — the subset of *variables* whose assignment actually
  matters; every extension is also a model. Great for "why this value."
- **get-difficulty** — maps assertions → a difficulty score (which
  constraints cost the most runtime). The performance-debug analogue.
- **get-timeout-core** — a subset sufficient to *reproduce the timeout*.
  Directly addresses dv-solve's "timeout vs UNSAT" limitation.
- **incompleteness explanation** — why "unknown" was returned.
- **learned-literals** — unit facts learned in preprocessing.

### 2.4 Domain practice: SystemVerilog / constrained-random verification

This is dv-solve's target audience (pyvsc / PSS randomization), so the
debug UX should speak their language. State of practice:

- Commercial debuggers (e.g. Cadence **Verisium Debug** with Xcelium)
  give "complete visibility of how constraints are resolved": a
  constraint-structure/relationship view, conflict identification, and
  **pre-randomization** of selected variables to inspect the value
  *distribution* and validate constraint quality
  ([SemiEngineering, *Debugging SystemVerilog Constraint
  Randomization*](https://semiengineering.com/debugging-systemverilog-constraint-randomization-a-comprehensive-guide/)).

- A canonical **failure taxonomy** for constrained-random (which should
  drive a fix-suggestion engine)
  ([DVCon, *The Top Most Common SystemVerilog Constrained-Random
  Gotchas*](https://dvcon-proceedings.org/wp-content/uploads/the-top-most-common-systemverilog-constrained-random-gotchas.pdf)):
  - **Silent failure** — `randomize()` returned 0 and was ignored.
  - **Hard contradiction** — over-specified / mutually exclusive bounds.
  - **Implication direction** — `a -> b` written backwards; one-way
    implication where the user expected bi-directional.
  - **`solve...before` ordering** — variable solved before a dependent,
    pruning the feasible region.
  - **`randc` cyclic dependency** — constraints make a cyclic var unable
    to cycle / unsolvable.
  - **Array reduction** (`sum()`, `product()`) over a sized array —
    blows up or silently over-constrains.
  - **`dist` skew** — output histogram doesn't match intent.
  - **Signed/unsigned & sign-extension** mismatches.
  - **Inheritance / scope** — base-class or inline constraints not in
    effect as expected.

The lesson: the *core engine* answers "which constraints, what change";
the *presentation layer* should recognize these patterns and translate a
raw MUS into "this looks like an implication written backwards at
line 42."

---

## 3. Proposed framework design

### 3.1 Design principles

1. **Layered, cost-escalating.** Cheapest diagnosis first; spend solver
   calls only as the user asks for more. (quick core → MUS → relaxations
   → corrective deltas → enumeration → proof.)
2. **Black-box where possible, white-box where it pays.** Black-box
   analysis (QuickXplain/MARCO over incremental solve) needs *nothing*
   special from the core and is trivially separable. White-box (proof
   DAG, difficulty) reuses LCG hooks but degrades gracefully when LCG is
   compiled out.
3. **Embedded-first separation.** The core solver exposes a tiny stable
   primitive API; *all* debug logic is additive and compiled out by
   default (the existing `ZSP_CONTRADICTION_ANALYSIS` pattern, extended).
4. **Heavy logic in Python, not C.** Orchestration, taxonomy, NL
   reports, and fix advice live in `dv_solve/*` where binary size is a
   non-issue. C provides primitives; Python provides intelligence.
5. **Domain-aware presentation.** Raw cores are translated through a DV
   failure taxonomy into human guidance.

### 3.2 Module architecture

```
                 ┌─────────────────────────────────────────────┐
   Python  (host only, no embedded cost)                        │
   dv_solve.debug                                                │
     • diagnosis pipeline orchestration                          │
     • MUS/MCS enumeration driver (MARCO-style)                  │
     • DV failure taxonomy + fix-suggestion engine               │
     • report rendering (text / JSON / structured)               │
     • distribution / pre-randomization analysis                 │
                 └───────────────┬─────────────────────────────┘
                                 │ ctypes (stable primitive API)
   C add-on (compiled out by default)                            
   libdv_solve_debug.so   [ZSP_CONTRADICTION_ANALYSIS]           
     Layer 2 (black-box):  QuickXplain MUS, quick core,          
                           relaxation search, soft diagnosis     
     Layer 3 (white-box):  proof/derivation DAG builder          
                           [needs ZSP_LCG; #ifdef-guarded]       
                 └───────────────┬─────────────────────────────┘
                                 │ Layer 1 primitive hooks
   C core (always present, ~zero cost when debug off)            
   libdv_solve.so                                                
     • solver_solve(ctx, opts)        incremental                
     • assumption push/pin (already used by soft)                
     • get_unsat_core()  ← thin: report assumptions in conflict  
     • LCG explain() callbacks (already exist)                   
```

Key point: **Layer 2 needs only "solve under a set of assumptions and
tell me which assumptions were in the conflict."** dv-solve already has
this for soft constraints. So the entire black-box analysis is an
add-on that the embedded core never pays for.

### 3.3 The diagnosis pipeline

A single entry point runs stages and stops at the depth the caller
requested (each stage is individually budgetable, matching the existing
`max_solver_calls` / `time_limit_sec` options):

```
classify(result)
 ├─ UNSAT ──────────────────────────────────────────────────────────┐
 │   S1. quick_core          (1 solve; over-approx core)             │
 │   S2. MUS                 (QuickXplain; minimal conflict)         │
 │   S3. preferred MUS       (priority-ordered → drops least-        │
 │                            important constraints first)           │
 │   S4. relaxations / MCS   (per-constraint corrective delta;       │
 │                            generalize beyond var-const via        │
 │                            binary search on a violation metric)   │
 │   S5. enumerate           (MARCO: independent conflicts +         │
 │                            alternative correction sets)           │
 │   S6. proof DAG           (white-box, from LCG explanations)      │
 │   S7. taxonomy + advice   (Python: pattern-match → fix hints)     │
 │
 ├─ SAT + soft relaxed ──────────────────────────────────────────────
 │   • per-soft: MUS of {soft + conflicting hard} (already present)  │
 │   • corrective delta for the hard constraints that forced it      │
 │   • alternative softs that could have been kept                   │
 │   • model core: which vars' values are actually pinned            │
 │
 ├─ TIMEOUT / UNKNOWN ───────────────────────────────────────────────
 │   • timeout-core: smallest subset that still times out            │
 │   • difficulty score per constraint (which are expensive)         │
 │   • incompleteness reason (resource? unsupported construct?)      │
 │
 └─ SAT + user expected otherwise (contrastive) ─────────────────────
     • counterfactual: minimal change to flip the outcome            │
```

### 3.4 Output / data model

Keep the existing C structs (`ContraResult`, `ContraRelaxSuggestion`,
…) and extend with:

- `cores[]` — multiple MUSes (enumeration), not just one.
- `corrections[]` — MCSes paired (hitting-set dual) with each core, each
  carrying per-constraint corrective deltas.
- `proof` — optional derivation DAG (nodes = bound facts / constraints,
  edges = "implied by").
- `difficulty[]` / `timeout_core[]` for the slow/unknown path.
- `diagnosis` — taxonomy classification + human advice string
  (populated in Python).

Output formats (already text + JSON): keep both; make JSON the stable
machine interface for pyvsc / IDE integration, and add a compact
"one-line summary + most actionable fix" mode for the common case.

Example target report (UNSAT):

```
UNSATISFIABLE — 1 conflict found among 12 constraints.

Conflict (minimal):
  [C3]  payload_len <= 64        (pkt.sv:42)
  [C7]  payload_len >= header*8  (pkt.sv:51)   with header in 9..16

Likely cause: lower bound forced by C7 (≥72) exceeds upper bound C3 (≤64).
  This pattern often means an implication or scaling factor is off.

Suggested fixes (any ONE restores satisfiability):
  • C3: raise 64 → 128            (delta +64)
  • C7: change 'header*8' → 'header*4', or bound header ≤ 8
  • relax C7 to soft (priority N) if it is a preference

Other conflicts: none.   Analysis: 9 solver calls, 4 ms.
```

### 3.5 Failure-taxonomy / fix-suggestion engine (Python)

A rule table maps *structural signatures of a MUS* to DV-domain advice:

| Signature in the MUS | Suggested diagnosis |
|---|---|
| two opposing bounds on one var (`x≥a`, `x≤b`, a>b) | hard contradiction; relax either bound |
| an implication whose antecedent is always true | implication likely written backwards |
| `solve A before B` + B-narrowing constraint | reorder / drop `solve...before` |
| `randc` var inside the core | cyclic-dependency exhaustion |
| `sum()/product()` over sized array in core | reduction over-constraint; bound the size |
| signed var compared to large unsigned const | sign-extension mismatch |
| soft dropped despite high priority | priority inversion vs a hard constraint |

This table is the cheap, high-leverage differentiator vs. a generic
"here's your core" tool, and it lives entirely outside the embedded core.

### 3.6 API sketch (additions, C add-on)

```c
/* Enumerate up to `max` independent conflicts (MARCO-style). */
int contra_enumerate_cores(SolveCtx*, SolveProblem*, const ContraOpts*,
                           ContraResult **out_cores, uint32_t *n_out);

/* Correction sets (relaxations) dual to the cores. */
int contra_correction_sets(SolveCtx*, SolveProblem*, const ContraOpts*,
                           ContraCorrection **out, uint32_t *n_out);

/* White-box derivation (requires LCG; returns -2 if compiled out). */
int contra_build_proof(SolveCtx*, SolveProblem*, ContraProof *out);

/* Slow/unknown path. */
int contra_timeout_core(SolveCtx*, SolveProblem*, const ContraOpts*,
                        uint32_t *ids, uint32_t *n);
int contra_difficulty(SolveCtx*, SolveProblem*,
                      ContraDifficulty *out /* per-constraint score */);
```

All gated by `ZSP_CONTRADICTION_ANALYSIS`; the proof functions
additionally `#ifdef ZSP_LCG`.

---

## 4. Recommendations & phased roadmap

Ordered by leverage-per-effort, building on what exists:

**Phase 1 — close the high-value gaps in the black-box layer (no core
changes).**
1. **Preferred MUS** using the existing soft `priority` field — return
   the conflict that drops least-important constraints first. (Small
   change to the QuickXplain ordering.)
2. **Generalize relaxations beyond var-const** via binary search on a
   per-constraint violation metric (works for `x+y≤C`, reductions, etc.),
   removing the module's stated relaxation limitation.
3. **Core-guided soft solving** — replace the sequential relax-and-retry
   loop with extract-core-then-soften (faster *and* it's the diagnosis).

**Phase 2 — multiplicity & domain UX (mostly Python).**
4. **MUS/MCS enumeration** (MARCO-style driver in `dv_solve.debug`) so
   multiple independent conflicts surface at once.
5. **DV failure taxonomy + fix-suggestion engine** (the §3.5 table) and
   the richer report format in §3.4. Wire constraint→source-location
   mapping through from pyvsc (the `ContraConstraintInfo` hook already
   exists).

**Phase 3 — white-box & performance.**
6. **Proof/derivation DAG** from the LCG `explain()` callbacks — *after*
   the soundness fixes in `docs/cdcl_explain_soundness_plan.md` land
   (proofs must be sound to be shown to users).
7. **Timeout-core + difficulty scoring** to fix the timeout-vs-unsat
   ambiguity and answer "why is it slow."

**Phase 4 — SAT-side & contrastive.**
8. **Model core** ("which variables actually matter") and
   **pre-randomization / distribution analysis** for the "SAT but
   suspicious" case.
9. **Counterfactual / contrastive** explanations ("why X and not Y").

**Cross-cutting (do throughout):**
- Keep every addition behind the build flag; verify `OFF` adds zero
  bytes to `libdv_solve.so` (regression-test binary size).
- Keep Layer-1 core hooks minimal and stable — the debug `.so` and the
  Python layer are the only consumers.
- Treat the JSON output as the stable contract for pyvsc/IDE tooling.

---

## 5. References

- Junker, *QUICKXPLAIN: Preferred Explanations and Relaxations for
  Over-Constrained Problems* (AAAI 2004) —
  https://www.semanticscholar.org/paper/QUICKXPLAIN:-Preferred-Explanations-and-Relaxations-Junker/dee653e324b42311f93adf83611a985028f8b54c
- Rodler, *Understanding the QuickXPlain Algorithm: Simple Explanation
  and Formal Proof*, arXiv:2001.01835 — https://arxiv.org/pdf/2001.01835
- Liffiton & Sakallah, *Algorithms for Computing Minimal Unsatisfiable
  Subsets of Constraints* —
  https://www.semanticscholar.org/paper/Algorithms-for-Computing-Minimal-Unsatisfiable-of-Liffiton-Sakallah/815b2ca4f366cba71778d2bbe7556adbd96ec841
- Liffiton, Previti, Malik & Marques-Silva, *Fast, flexible MUS
  enumeration* (MARCO) —
  https://www.researchgate.net/publication/276905908_Fast_flexible_MUS_enumeration
- Dev Gupta, Genç & O'Sullivan, *Finding Counterfactual Explanations
  through Constraint Relaxations*, arXiv:2204.03429 —
  https://arxiv.org/pdf/2204.03429
- Downing, Feydy & Stuckey, *Unsatisfiable Cores and Lower Bounding for
  Constraint Programming*, arXiv:1508.06096 —
  https://arxiv.org/pdf/1508.06096
- *Debugging Unsatisfiable Constraint Models* (Leconte/Stuckey-style CP
  model debugging) —
  https://link.springer.com/chapter/10.1007/978-3-319-59776-8_7
- cvc5, *Interfaces for Understanding Solver Results* —
  https://cvc5.github.io/blog/2024/04/15/interfaces-for-understanding-cvc5.html
- cvc5 docs, *SMT Solver Outputs* (unsat cores, proofs, get-difficulty,
  get-timeout-core, model cores) —
  https://cvc5.github.io/tutorials/beginners/outputs.html
- SemiEngineering, *Debugging SystemVerilog Constraint Randomization: A
  Comprehensive Guide* —
  https://semiengineering.com/debugging-systemverilog-constraint-randomization-a-comprehensive-guide/
- DVCon, *The Top Most Common SystemVerilog Constrained-Random Gotchas* —
  https://dvcon-proceedings.org/wp-content/uploads/the-top-most-common-systemverilog-constrained-random-gotchas.pdf

### Internal references
- `docs/contradiction_analysis.md` — current debug module user guide
- `docs/soft_constraints.md` — soft-constraint relaxation model
- `docs/cdcl_explain_soundness_plan.md` — LCG explain soundness (prereq
  for the proof-DAG work)
- `src/c/zsp_contradiction.h`, `src/c/zsp_explain.h` — current API
</content>
</invoke>
