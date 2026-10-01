# Soundness coverage plan

Status: in progress, started 2026-10-01; see §8 for progress and §9 for what
the work has found so far. Comes before `ci_benchmark_publishing_plan.md`:
there is little point publishing speed numbers for a solver whose `unsat` can
be wrong.

Goal: stop finding wrong answers by accident. Define the space of inputs and
of solver behaviour that soundness depends on, check every run against an
oracle that cannot share dv-solve's bugs, measure what the runs actually
reached, and close the gaps deliberately — the way a verification team
closes coverage on a design.

---

## 1. Why now

Most of the 50-odd entries in `docs/solver_bug_backlog.md` were found
*incidentally*: while writing documentation (B40–B49), while bringing up
Verilator (B14–B37), or by a fuzzer whose operator set happened to include
the construct (B34 surfaced only once `bvneg` was added to the generator).
Two more were found today, both while testing an unrelated feature
(minimised unsat cores):

| Found | Wrong answer | Why nothing caught it |
|---|---|---|
| `z - x == 15 && (z - x) != z` → `unsat` (B50) | the `!=` explainer omitted the narrowed variable's own previous bound, so learning concluded `z - x != 15` everywhere | needs a shared subterm, a value at the top of the range, and clause learning on; the QF_BV fuzzer never generates shared subterms |
| `z - x == 12 && x + y < y && x < z` → `unsat` (B51) | the search's own conflicts left a stale "culprit propagator", and learning blamed it | needs a domain split to be exhausted; no test exercises that conflict path with learning on |

The pattern across the backlog has five clusters:

1. **Clause-learning explanations** (B26, B43–B45, B50, B51): each step can
   be wrong in a way that only matters if the bad clause cuts the *last*
   solution. A test catches it only by luck.
2. **Signedness and the 2^63 boundary** (B9, B22, B26, B29): bins exist only
   at a few widths and value ranges.
3. **Width and tier of auxiliary variables** (B24, B25, B27, B38).
4. **Incremental protocol** (B14, B33, B36, B37, B40): push/pop, assert after
   check-sat, reset, repeated solves.
5. **Compile-time rewrites** (B46, B47 alias bypass; B3, B4 shared
   sub-expressions): merges and rewrites the generators don't produce.

Today's tests are a large collection of point tests plus three generators
(`fuzz_bv_smt2.py`, the array fuzzer and the builder brute-force sweeps).
They cover a narrow slice of the operator × shape × width × engine space, and
nothing measures which slice. The fix is not more point tests: it is a model
of the space, oracles strong enough to catch a single bad step, and a measure
of coverage.

---

## 2. The coverage model: defining the space

Two halves, as in hardware verification: **stimulus coverage** (what problems
we gave the solver) and **implementation coverage** (what the solver did with
them). Neither alone is enough: a stimulus bin can be hit without the
interesting code path running, and a code path can run without the input that
makes it go wrong.

### 2.1 Stimulus coverpoints

| Coverpoint | Bins |
|---|---|
| Front door | Python builder; C API; SMT-LIB2 batch; SMT-LIB2 incremental; `--mode=verilator`; DPI |
| Variable width | 1, 2, 3, 4, 7, 8, 15, 16, 31, 32, 33, 63, 64, 65–128 |
| Signedness | unsigned, signed |
| Domain shape | full; interior; touches 0; touches max; crosses the sign boundary (2^(w-1)); crosses 2^31 / 2^63; singleton; holed (`inside`) |
| Operator | every `dvs_binop_t`/`dvs_unop_t` value, `ite`, extract, concat, extend, `inside`/ranges, `$countones`, `$clog2`, sum, all-different, `dist`, soft, implication; every SMT-LIB2 operator in the frontend's table, including signed compares/div/rem and `bvneg` |
| Operand shape | var–var; var–const; const–var; same var twice (`x op x`); merged by `x == y`; shared subterm (the same expression twice); nested composed expression |
| Boolean structure | conjunction; `or` of 2 / 5 / 16 / 17+ comparisons; `not`; implication; `xor`; reified compare; nesting depth 1–4 |
| Problem size | 1, 2–5, 6–16, 17+ constraints; 1, 2–4, 5+ variables |
| Answer | sat with one solution; sat with few; sat with many; unsat with core size 1, 2, 3+; expected `unknown` |
| Value position of the solution | at 0; at max; at the sign boundary; interior |
| Protocol | push/pop depth 0–3; assert after check-sat; reset; check-sat-assuming; get-unsat-core; pin / exclude / checkpoint / restore (C, DPI); repeated solve with new seed |
| Engine and options | CDCL with learning on/off; phase saving; fair pick; bitblast; cube; portfolio; DV_ARRAY |

### 2.2 Implementation coverpoints

These are recorded by counters compiled into an instrumented build (§4.2):

| Coverpoint | Bins |
|---|---|
| Propagator narrowing | kind × storage variant (32-bit / 64-bit / wide-watch) × target variable (each watched position) × direction (lb / ub) × outcome (tighten / conflict / retire) |
| Explanation | explainer × branch taken × target × direction; includes the "own previous bound" case; refusal (−1) |
| Learning: seed | empty domain; clause conflict; propagator conflict; search-created conflict |
| Learning: resolution | through propagator; through clause; reaches a decision; singleton pair |
| Learning: outcome | asserting clause; non-asserting; tautology fallback; duplicate fallback; each bail reason; clause length and LBD buckets |
| Compile rewrites | alias merge; constant fold; aux tier choice; flattening; reified-or / reified-and split; long `or` through guards; all-different with merged variables |
| Engine routing | CDCL → bitblast escalation; validation downgrade to `unknown`; per-logic routing; Verilator sticky route |
| Frontend | each translated operator; each taint reason |

### 2.3 Crosses

Not every cross is worth closing. These come straight from the clusters in §1:

- operator × width bin × signedness × domain shape (clusters 2, 3)
- operator × operand shape (cluster 5, and both of today's bugs)
- propagator narrowing × explanation branch × learning on (cluster 1)
- learning seed kind × resolution kind (cluster 1)
- protocol step × engine (cluster 4)
- front door × operator (the same problem through every door)

The full product is far too large; closure targets apply to these crosses,
and the rest is sampled.

---

## 3. Oracles: what makes a run fail

Coverage without checking only measures activity. Four layers, the cheapest
and strongest first:

1. **Exhaustive enumeration.** For problems with at most ~16 free bits, a
   brute-force evaluator in Python computes the exact solution set. It shares
   no code with the solver, so it decides `sat`/`unsat`, checks any model,
   and checks unsat cores for unsatisfiability and minimality. This is the
   primary oracle; the generator's default mode is sized to fit it.
2. **Step checkers inside the solver** (instrumented build only). A wrong
   `unsat` from clause learning shows up in the answer only when the bad
   clause happens to remove the last solution; the bad step itself happens
   far more often. So check each step against the problem's solution set,
   computed once per run by enumeration:
   - every **learned clause** must be satisfied by every solution;
   - every **explanation** must be implied by its constraint: enumerate the
     variables involved, over the domains they had at the time;
   - every **propagator narrowing** must remove no value that appears in a
     solution of that constraint within the domains it had when it fired.

   This turns "a wrong answer that happened to surface" into "any unsound step
   on any run". Both bugs from today would have been caught by the
   learned-clause check on their first conflict.
3. **Reference solvers** for problems too wide to enumerate: z3 and bitwuzla
   on the SMT-LIB2 lowering. These are already in `tests/formal/harness`.
4. **Metamorphic relations** for every size: the answer must not change when
   - constraints or variable ids are reordered;
   - a conjunction is split or merged, or a tautology is added;
   - a subterm is duplicated or shared;
   - `y` is replaced by a fresh variable with `x == y`;
   - the problem goes through a different front door or engine;
   - assertions are wrapped in push/pop or replayed after a reset.

Model validation (already in place) remains the backstop for a wrong `sat`.
These four layers are about wrong `unsat`, which validation cannot see.

---

## 4. Collecting coverage

### 4.1 Stimulus: one generator, every front door

Replace the per-test generators with one constrained-random generator that
emits a small **problem IR**: variables, typed expressions and protocol steps.
It lowers the IR to every front door:
- the Python builder;
- the C API, through the Python wrapper;
- SMT-LIB2 text, in batch or incremental form;
- for a subset, a SystemVerilog class run through Verilator.

The IR is also what the brute-force evaluator and the metamorphic transforms
operate on.

The generator samples every coverpoint in §2.1 for each problem and records
the bins it hit. Sampling is weighted, and the weights are steered by the
current holes (§5.3): coverage-directed generation, as in constrained-random
DV.

A failing problem is shrunk automatically, by delta-debugging on the IR while
the failure reproduces, and written out as a regression test in the existing
style.

### 4.2 Implementation: counters in an instrumented build

Add a `DVS_COVER(point)` macro that compiles to nothing in release builds and
to a counter increment in a `-DDVS_COVERAGE=ON` build. The counters sit at
the sites in §2.2: each propagator narrowing, each explainer branch, each
learning path, each compile rewrite and each routing decision. They are
dumped at process exit.

This stays out of the public surface. It lives in the instrumented library
only, like `libdv_solve_debug`, with no new API in `dv_solve.h` and no new
documented environment variable.

The step checkers of §3.2 live in the same build. They need a hook to read
the problem and its solution set; the test harness passes that hook through
the debug library.

### 4.3 Code coverage

Add a `--coverage` (gcov / llvm-cov) build of the C library. Run the fuzz
campaign and the full test suite under it, and keep line and branch reports.
The files that matter most are:
- `dvs_prop_templates.c`
- `dvs_explain.c`
- `dvs_lcg.c`
- `dvs_compile.c`
- `dvs_search.c`
- `smt2/smt2_frontend.c`

Code coverage is a cross-check on the model, not a goal in itself: an
uncovered branch in an explainer means §2.2 is missing a bin.

### 4.4 Storage and merging

Write stimulus and implementation coverage in UCIS through pyucis, so runs
merge and existing coverage viewers and reports work on them. A JSON fallback
is enough for the first phase. Each campaign run produces one database;
nightly runs are merged.

---

## 5. Assessing gaps

### 5.1 Closure report

Per coverpoint and per cross in §2.3: bins hit and the goal. The report also
lists the unhit bins, ranked by risk, where risk is weighted by how many
backlog bugs came from that region. It is published alongside the nightly run
(see §6, P5).

### 5.2 Validate the model against history

Before trusting the model, replay the backlog. For each fixed bug B1–B51, ask
two questions:
- Does some bin in §2 describe the input that triggered it?
- Would the generator, with the oracles in §3, have produced and caught it
  within a nightly budget?

Do this concretely: revert the fix on a branch and run the campaign. Every
"no" is a gap in the model or the generator, and gets fixed first.

### 5.3 Measure detection, not just activity: mutation testing

Coverage says the code ran; it does not say a bug there would be caught. Make
deliberate, small mutations in the soundness-critical code, such as:
- drop one literal from an explainer's output;
- skip the own-bound literal;
- an off-by-one in a bound;
- swap a signed compare for an unsigned one;
- skip an alias resolve;
- ignore one watched variable.

Then run the campaign. The share of mutants caught within the budget (the
mutation score) is the closure metric for soundness. A surviving mutant points
to exactly the stimulus or checker that is missing.

### 5.4 Coverage-directed generation

Unhit bins feed back into the generator's weights. Bins that stay unhit under
re-weighting are either unreachable, and get documented as such, or need a
new generator capability, which becomes a work item.

---

## 6. Phases

| Phase | Deliverable | Exit criterion |
|---|---|---|
| **P0 — settle the open fixes** | The two wrong `unsat`s from today (§1) fixed and pinned by tests. Minimised `get-unsat-core` landed. | Both repros and the core stress test pass; no regressions in unit, ctest, fuzz and formal; no slowdown in the formal suite |
| **P1 — step checkers** | Instrumented build with learned-clause, explanation and narrowing checkers (§3.2), run over the existing unit and formal suites | All suites pass under the checkers; every violation found is fixed or logged in the backlog |
| **P2 — generator, oracle, shrinker** | Problem IR, lowerings to Python / C / SMT-LIB2 (Verilator subset later), brute-force evaluator, metamorphic transforms, delta-debug shrinker with regression-test output | A fixed-seed campaign of ~10k problems runs in under 10 minutes; any failure shrinks to a minimal pinned test |
| **P3 — coverage collection** | Stimulus bins from the generator, `DVS_COVER` counters, gcov build, merged database | One report shows all three, per coverpoint and per §2.3 cross |
| **P4 — gap assessment** | Backlog replay (§5.2), mutation campaign (§5.3), closure report, coverage-directed weights | Every historical bug reachable by the campaign; mutation score ≥ 95% on the files in §4.3; unhit crosses either closed or documented as unreachable |
| **P5 — continuous** | Fixed-seed smoke (~2 min) on every push in the Forgejo/GitHub CI pair; nightly campaign with new seeds, merged coverage and report | A wrong answer found by the nightly run reaches the backlog with a shrunk repro, without anyone looking for it |

P1 comes before P2 deliberately: the step checkers make every existing test
stronger at once, and P0's own fixes need them to be trusted.

---

## 7. Decisions needed

1. **Coverage database format:** UCIS through pyucis (recommended: it merges
   and fits existing tools) or plain JSON.
2. **Where the closure report lives:** a page in the docs site's future
   Results section, next to the benchmarks, or CI artifacts only.
3. **Nightly budget:** how many CPU-hours per night the campaign may use. It
   sets the size of the space sampled each night.
4. **Whether the generator's IR replaces the existing generators**
   (`fuzz_bv_smt2.py`, the array fuzzer, the builder sweeps) or sits beside
   them. Recommended: replace them once P2 reaches parity, keeping their
   pinned regressions.

---

## 8. Progress

### P0 — settle the open fixes (in progress)

| Item | State |
|---|---|
| B50: explainers omit the narrowed variable's own previous bound | Fixed in the clause-learning rewrite (`dvs_lcg.c`): every explainer not on a short whitelist gets the own-bound literal; literals are attributed to the trail entry that made them true; explainers are asked for the bound the working set needs, not the current one; overflowing the clause buffer bails instead of dropping literals |
| B51: stale culprit propagator after a search-made conflict | Fixed: `conflict_prop_ref` / `conflict_clause_idx` cleared at every search tightening and at propagate entry |
| Livelock in the rewrite (7 hangs in 3000 brute-force problems) | Fixed: the propagation that empties a domain is now always resolved, never taken as the UIP (§9.2) |
| Analysis quadratic in the trail (one sweep seed 3 s → 49 s) | Fixed: the maker of a literal is found by binary search in a per-analysis index of the trail, not a scan |
| B52: `bvudiv`/`bvurem` by zero unconstrained on CDCL | Fixed in the SMT-LIB2 frontend: a zero-divisor guard unless the divisor is a nonzero constant (§9.3) |
| B53: constraint-level `ite` and soft constraints compiled ungated | Fixed in `dvs_compile.c`: every branch and soft body is reified and implied by its guard (§9.5) |
| B54: `explain_sum_eq` cited the wrong bounds for a summand | Fixed (§9.6) |
| B55: reification with guard 0 accepted `x > y` at the edge of the range | Fixed in `_fire_reification_32/64` (§9.7) |
| B56: explainers read bounds made after the step they explain | Fixed: analysis rewinds the domains to just before each explained step (§9.8) |
| B57: `explain_ite_value` explained every narrowing as the result's | Fixed (§9.8) |
| `bounds_mul_32` ignored a zero factor | Fixed; found by the propagator harness (§9.7) |
| Guard-gated propagators: the learnt clause did not cite the guard | Fixed: analysis adds `guard >= 1` for a gated propagator's step |
| Minimised `get-unsat-core` | Done; tests pass, including 400 brute-force random cores and a 6000-case stress run |
| Pinned regressions | `tests/formal/test_lcg_soundness.py` (B50 ×2, B51, the livelock); `test_signed_ops.py::test_divrem_of_variables_matches_z3` (B52); `test_bool_ite_constraint.py` and `tests/unit/test_gated_constraints.py` (B53); `tests/c/test_prop_exhaustive.c` (B54, B55, B57, mul) |
| Suites | unit 904 passed; ctest; formal: pass except four bitwuzla reference timeouts that fail on HEAD too |
| Speed | Formal suite 208 s against 201 s on HEAD, with 19 more tests. No change on the 13 fixtures that learn clauses. The synthetic same-operand slowdowns of §9.4 are gone since B56 (`slow0`: 10 s → 3 ms; every case within a few ms of HEAD or faster) |

### P1 — step checkers (clause, explanation and model checks done; propagator harness done)

`-DDVS_STEP_CHECK=ON` builds a library and CLI that check, as the solver runs:

1. **Clause falsity** (cheap, in process): every literal of a learnt clause
   must be false at the conflict. A literal that already holds means the
   clause excludes nothing, which is how the livelock in §9.2 looked from
   inside.
2. **Clause validity** (forked child): backtrack to the root, turn learning
   off, assert the clause's negation and search with the plain solver. A
   solution proves the clause removes a real solution. The fork is an exact
   copy, so nothing has to be restored, and the check shares no code with
   clause learning or the explainers.
3. **Explanation validity** (forked child, every explanation analysis uses):
   assert the explanation's antecedents, plus the own-bound and guard
   literals analysis adds, and the negation of the explained bound. A
   solution names the faulty explainer, which a bad clause does not.
4. **Child model validation**: the child's search trusts the propagators to
   reject a violating assignment, so before a child "solution" counts as
   evidence it is validated against the original problem. A violation is
   reported as an incomplete propagator (a wrong-model bug of its own); a
   problem with uncompiled constraints makes the check inconclusive.

A violation aborts the run. Suite runs instead set `DV_STEP_CHECK_LOG` (append
violations, tagged with `PYTEST_CURRENT_TEST`) and `DV_STEP_CHECK_CONTINUE`.
`DV_STEP_CHECK_STATS` prints counts at exit and `DV_STEP_CHECK_DUMP` lists the
propagators with a violation. These are internal test switches of an
instrumented build and are not documented for users.

**Propagator harness** (`tests/c/test_prop_exhaustive.c`, in ctest, ~30 s).
This is the per-constraint narrowing checker of §3.2, done exhaustively
rather than at run time. Each of 73 cases adds one propagator to a context
of 1–4 small variables (2–3 bits exhaustively; 6 bits sampled for the
bit-level ones; unsigned and signed) and runs it on every box of
sub-intervals. Checks, against a truth function written in the test:
- a conflict only when the box holds no solution;
- no solution pruned;
- a conflict on every violating fixed point (completeness);
- every explanation, given in the state the propagator saw, implies its bound
  over the full domains.

It checks 630,000 explanations per run. It found the `bounds_mul_32` zero
factor, B55 and B57 directly, and would have found B54.

Validated against history:
- the HEAD build with the checker rejects the first bad clause of B50
  (`v2 <= 14`, violated by `z=15, x=3`) and of the bvand variant;
- a deliberately reintroduced B54 is reported as `INVALID explanation from
  _fire_sum_eq_32`.

### P2 — generator (seed version running)

A typed QF_BV generator with a Python brute-force evaluator covers:
- all 14 binary bit-vector operators and both unary ones;
- `ite` as a value and as a Boolean constraint, including the `x == K` and
  `x == y` conditions that take the guarded compile paths;
- extract, concat and both extends;
- all ten comparisons and the Boolean connectives;
- shared subterms, same-operand terms and `x == y` aliases;
- mixed widths from 1 to 8 bits, up to 12 free bits.

It checks sat/unsat against enumeration, every model against the
constraints, and every 10th answer against z3. Under the step-checker build,
every learnt clause and explanation is checked as well.

What it found:
- The first 400 problems found B52.
- The first run with Boolean `ite` found B53: 47 wrong `unsat` in 10,000.
- Under the explanation checker it led to B55, B56 and B57.

It is still a scratch script; it moves into `tests/formal/soundness/` with
the IR and shrinker.

---

## 9. Findings: what each bug says about coverage

Each bug found during this work is a hole in the model of §2 or in the oracles
of §3. The table records which, so the gap is closed and not just the bug.

### 9.1 B50, B51 (clause learning)

Both need clause learning on, plus a shape the old generators never produce:
- B50 needs a shared subterm next to a value at the top of the range;
- B51 needs a domain split to be exhausted.

They fit cluster 1 and are caught directly by the learned-clause checker.
Coverage lesson: **the fixtures barely exercise learning.** Only 13 of the
267 fixtures learn a single clause; every other fixture is decided by
propagation alone or by bitblast. The implementation coverpoints for learning
(§2.2) have to come from generated problems, and the generator's default
setting must keep the CDCL engine in play.

### 9.2 Livelock in the rewrite

Found by brute-force fuzzing the B50/B51 fix: 7 of 3000 problems hung. The
cause was a propagation that empties a domain being taken as the UIP. When
the opposite bound held at the root, the learnt clause was already true, so
it excluded nothing, and the search re-derived the same conflict for ever.

Coverage lessons:
- **A hang is a failure.** Every oracle run needs a time budget and must
  report a timeout as a failure, not as `unknown`.
- **Validity is not enough.** The livelock clause was valid, so a pure
  soundness check passes it. Hence the falsity invariant in the step checker.
- **New bins:** learning seed × "the crossing bound's partner holds at the
  root"; learning outcome "clause true at the conflict".

### 9.3 B52: `bvudiv` / `bvurem` by zero

Found in the first 400 problems from the new generator, giving wrong models
and wrong `sat`. SMT-LIB defines `a / 0 = ~0` and `a % 0 = a`, but the CDCL
division propagator leaves the result free when the divisor can be zero (the
SystemVerilog `x`). Model validation skips any term with a zero divisor, so
it could not see the bad model either. The signed operators were right only
because they force the bitblast engine.

Coverage lessons:
- **Constant operands hide engine behaviour.** The exhaustive division test
  used constant operands only, which the frontend can fold, so the engines'
  own division was never exercised at zero. Operand shape (variable,
  constant, folded constant) has to be crossed with every operator, as in
  §2.3.
- **Value-position bins apply to operands, not only solutions.** "Divisor is
  zero", "shift amount ≥ width" and "operand at the sign boundary" are bins
  of their own.
- **Oracle gap:** validation skips the zero-divisor case rather than
  evaluating SMT-LIB semantics. With the frontend guard in place the skipped
  branch is never selected (validation evaluates only the taken branch of an
  `ite`), but any path that builds a raw division still validates nothing.
  Recorded for P1.
- **Front-door semantics.** The builder API follows SystemVerilog, where
  division by zero is `x`, while SMT-LIB defines it. A cross of front door ×
  operator must check each door against its own definition.

### 9.4 Slow cases in the clause-learning rewrite

13 of 16,000 generated problems took over 2 s, against milliseconds on HEAD.
They are synthetic same-operand shapes. The extra literals the rewrite adds
for soundness make the clauses more specific; each part of the change costs a
different case. No regression on real fixtures.

Resolved by the B56 fix (§9.8): with explanations taken at the right moment
the learnt clauses are valid *and* general, and `slow0` went from the 10 s
deadline to 3 ms. The extra conflicts were the circular explanations at work.

Coverage lesson: **speed is part of the oracle**. A per-problem time budget
relative to a reference build belongs in the nightly report, because a
soundness fix that turns milliseconds into a 10 s deadline is a regression
users feel.

### 9.5 B53: Boolean `ite` and soft constraints compiled ungated

Found the first time the generator produced a Boolean `ite` as a constraint:
47 wrong `unsat` in 10,000 problems, all decided at compile time.
`(ite (= x y) false true)`, which just says `x != y`, came back `unsat`. HEAD
has it.

Compile handled an `ite` at the constraint root by compiling each branch
normally and gating the propagators it could find. In one path that was only
the last propagator; in another it was one off by one. Everything else the
compile did applied whichever way the condition went:
- root-domain tightening;
- `x == y` merging;
- the outright `unsat` of a `false` branch;
- an else-branch compile failure, which was ignored.

Soft constraints had their own copy of the same fallback, which made a soft
constraint partly hard. The backlog had already noted the symptom for soft
`!=`.

Coverage lessons:
- **The operator list must include Boolean-valued forms of every
  operator**, not just bit-vector ones. `ite` was in the generator only as a
  term.
- **Compile paths are a coverpoint.** The same constraint compiles through
  different code depending on the condition's shape (variable, `x == K`,
  `x == y`, general) and on whether it sits under a guard. Each path needs
  its own bins.
- **Front doors reach different compile paths.** The builder's `if`/`else`
  and soft constraints go straight to these paths; SMT-LIB2 reaches them only
  through a Boolean `ite`. `tests/unit/test_gated_constraints.py` checks them
  through the builder against brute force.

### 9.6 B54: `explain_sum_eq` used the forward rule for summands

Found by the learned-clause checker in an existing unit test
(`test_wide_watch.py`, all-different + sum + wide `or` + a merged pair)
whose answers were right. The bad clause did not cut the last solution:
exactly the case §3.2 predicted. The explainer cited the other summands'
same-direction bounds for a summand, so `v3 <= 1` was explained by `v0 <= 1`
and `v0 <= 1` by `v3 <= 1`.

Coverage lesson: **existing tests already had the stimulus; they lacked the
oracle.** Checking steps, not answers, turned a passing test into a failing
one. The explanation checker, added next, names the explainer directly.

### 9.7 B55 and `bounds_mul_32`: propagators that accept a wrong model

Both were found when the explanation checker's child returned a "solution"
that violated a constraint.
- `_fire_reification_32/64` with the guard at 0 must enforce `x > y`. At
  the edge of the range (`y` at its maximum or `x` at its minimum) the
  overflow guards skipped the pruning, which is correct, but also returned
  no conflict, which is wrong.
- `bounds_mul_32` did nothing for a zero factor, so `r = 1, a = b = 0`
  passed.

Through the front doors both are masked: compile folds the values, model
validation catches a bad SMT-LIB2 model, and the modular propagators take
most multiplications.

Coverage lessons:
- **Completeness on fixed points is its own coverpoint.** Every propagator
  must reject every fully fixed assignment that violates its constraint;
  otherwise the search's models depend on validation to be right.
  `test_prop_exhaustive.c` now checks every point of every case.
- **An oracle that trusts the propagators is not independent.** The step
  checker's child search is now validated against the original problem.
- **Edge-of-range values** (`x` at its minimum and `y` at its maximum at the
  same time) need bins in the domain-shape coverpoint, for every operand
  pair.

### 9.8 B56 and B57: explanations read the wrong moment

Found by the explanation checker on `bxor` explanations that the propagator
harness said were valid. Explainers read the **current** bounds of the
other variables. At a conflict the current state includes bounds derived
*from* the step being explained, so each explanation was locally valid but
the chain was circular (`v2 <= 45` because `v8 <= 12`; `v8 <= 12` because
`v2 == 45`), and the learnt clause removed a solution. HEAD has it. Analysis
now rewinds the domains to just before each explained step.

Rewinding exposed B57: `explain_ite_value` explained every narrowing as if
it were the result's, and for the hull case it left out the result's own
range. Neither showed while an explainer could cite the bound it was
explaining. The propagator harness had the same blind spot and passed it. It
now explains in the pre-fire state, and then failed on `ite_value` until the
fix.

Coverage lessons:
- **When a check runs matters as much as what it checks.** A step checked in
  the state after the step can be validated by its own conclusion. Both
  checkers now use the state the propagator saw.
- **The harness and the run-time checker disagreed, and that was the
  signal.** Two oracles at different levels (per propagator, whole problem)
  catch different classes. Keep both.

