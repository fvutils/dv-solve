# Separability probe — where is the parallelism in the hard instances?

Tool: `separability_probe.py` (static, no solving). Target: the 6 QF_BV timeout
instances that a reference solver proved solvable at 1200s — the sweet spot for
the parallelization thesis.

## Results

| benchmark | vars | constraints | components | largest comp | top-level `or` |
|---|--:|--:|--:|--:|--:|
| mcm/68        | 36      | 45      | 1    | 100.0% | 21 |
| mcm/140       | 51      | 67      | 1    | 100.0% | 33 |
| mcm/176       | 105     | 139     | 1    | 100.0% | 69 |
| StringMatching/string1x16.3 | 1723 | 1801 | 4 | 99.8% | 0 |
| EdgeMatching/edge-matching-10x10-c13 | 246638 | 287439 | 558 | 99.8% | 0 |
| Labyrinth/laby_18_18_04 | 423307 | 479044 | 3918 | 99.1% | 0 |

Components are connected components of the variable-interaction graph (each
conjunct of a monolithic `(assert (and …))` counted as its own constraint).

## Finding: no exploitable *component* separability — it's all one blob

Every instance is a **single giant connected component** covering 99.1–100% of
the constrained variables. The "extra" 557 / 3917 components in edge-matching /
labyrinth are **size-1 singletons** — free/unconstrained variables, not
independent sub-problems. The constrained core does not decompose.

So the naive form of the thesis — *"solve independent separable components in
parallel"* — **does not apply to this corpus.** There is nothing to farm out at
the constraint-graph level.

## Where the parallelism actually is: search-space partitioning (cube-and-conquer)

The separable structure is in the **search space**, not the constraint graph —
which is exactly what the parallel-track winner **STP-Parti-Bitwuzla** exploits
(partition the problem into cubes, solve cubes in parallel). Evidence:

- The small-but-hard **mcm** instances (68/140/176) are dense with top-level
  disjunctions (21/33/69 `or`-forms) over a handful of bit-vector variables —
  textbook cube seeds. Case-split on a small cut set of decision vars and each
  cube is an independent, easier sub-solve.
- The large **CSP** encodings (edge/laby/string) hide their disjunction inside
  the monolithic `and`, but they are combinatorial search problems where
  cube-and-conquer over the decision variables is the standard parallel win.

## Implication for dv-solve

This refines the original thesis: **for this corpus the value is a
cube-and-conquer partitioner, not constraint-component decomposition.**
Constraint-component decomposition (per `docs/parallelism_analysis.md`) is still
worth having, but it will pay off on *different* workloads (e.g. multi-property
BMC, independent asserts) — not these single-hard-instance competition problems.

Next: once the 1200s single-thread re-run finishes (`timeout_1200s.csv`), cross
it with this table — instances that still time out at 1200s single-thread but
have rich cube structure are the concrete first targets for a cube-and-conquer
prototype.
