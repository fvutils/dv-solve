# Publishing Performance + Distribution Results from CI

Status: PLAN (2026-09-30). Nothing below is built yet.

Goal: every push to `main` publishes, as part of the dv-solve docs on
`dvkit.org/fvutils/dv-solve/`, (a) correctness agreement, (b) performance and
(c) result-distribution quality for dv-solve against three reference arms:

1. **native SMT2**: z3 / boolector / bitwuzla solving the same fixture as-is
2. **Verilator-style swizzled SMT2**: z3 driven through Verilator's own
   randomization protocol (the XOR-hash diversity loop), i.e. what a Verilator
   user gets today with the default `VERILATOR_SOLVER`
3. **uniform floor**: a true RNG over the z3-enumerated solution space (only
   for distribution metrics; it is the bias floor, not a competitor)

---

## 1. Review: what exists today

### CI / docs

| Piece | State | Consequence for this plan |
|---|---|---|
| `.forgejo/workflows/ci.yml` | Conformance check only (internal-identifier grep, no-publisher check). No build, no tests. | The Forgejo runner has never built dv-solve. That is the first thing to prove. |
| `.github/workflows/wheels.yml` | cibuildwheel matrix; no tests beyond wheel smoke. | Not usable for publishing: a docs artifact uploaded from GitHub is consumed by nothing. |
| Docs site | **None.** `docs/` is a flat directory of design notes; no Sphinx/MkDocs config, no `index`. | We have to stand up a site, not just add a page. `docs-fetch` refuses an artifact with no `index.html` at its root. |
| Publishing contract | `.forgejo/workflows/docs.yml` uploading an artifact named exactly `docs-fvutils-dv-solve`; host pipeline auto-promotes on `main`. | No review gate in front of `main` — the workflow's own sanity checks *are* the gate (§6). |

### Benchmark / quality tooling (all local-only, all ad hoc)

| Script | What it measures | Output | Overlap |
|---|---|---|---|
| `tests/formal/run_compare.py` | sat/unsat agreement + time: dv vs z3/boolector/bitwuzla on tier1-3 | CSV | uses `harness/` solver wrappers |
| `tests/formal/sweep_perf.py` | min-of-N wall time dv(auto)/dv(bitblast) vs bitwuzla; bb encode/sat split | CSV | re-implements subprocess running, hard-coded paths |
| `tests/formal/triage_medium.py`, corpus scripts | hard-band triage | CSV/MD | same |
| `tests/formal/sample_quality.py` | coverage / JSD / chi² / thin-branch vs z3-enumerated ground truth, uniform arm | stdout table | owns the distribution metrics we want to publish |
| `tests/unit/dist_harness.py` | `dist` weighting chi² via ctypes API | pytest asserts | API-level, not SMT2 |
| `tests/bench/` | zuspec dataclass scenarios over python/native/bitwuzla/sim back-ends | per-run JSON | different front door (dataclass, not SMT2) |
| `tests/formal/verilator/run_suite.sh`, `phase0/` | real Verilator runs, z3 vs dv-solve | text | needs a custom Verilator build path in `$HOME` |

Three independent subprocess runners, four output formats, no shared schema,
no provenance (commit, solver versions, host), and no history. That is the
core problem to fix before anything is published — otherwise the published
numbers are whatever the last script printed.

### Fixtures available

`tests/formal/smt2/`: tier1 34, tier2 60, tier3 24, tier4 20, gaps 24,
verilator 40, wide 13 (≈215, the "easy band" from the 2026-07-21 sweep), plus
the SMT-COMP 2025 parallel corpus (93, hard; not CI-friendly).

### Correction: Verilator's swizzling is not what our design docs describe

`docs/verilator_solver_integration_design.md` §8 describes a bit-pinning
`check-sat-assuming` / `get-unsat-assumptions` relaxation loop. That was the
Verilator fork used for Phase 0. The **Verilator 5.046** we ship via
`packages/verilator-bin` (`verilated_random.cpp`) does something different:

1. declare vars, assert constraints, `(check-sat)`; if unsat → re-issue with
   named asserts and `(get-unsat-core)` for the error message.
2. Up to `_VL_SOLVER_HASH_LEN_TOTAL = 4` times: `(assert (= #bR (bvxor <≈half
   of all rand bits, chosen randomly>)))` with `_VL_SOLVER_HASH_LEN = 1`
   (a random 1-bit XOR parity constraint), `(check-sat)`; stop at the first
   unsat and keep the last sat model.
3. `(get-value ...)`, `(reset)`.

That is a UniGen-lite random-parity-hash sampler with at most 4 hash bits —
i.e. at most 16 cells, regardless of solution-space size. Its expected
weakness is exactly what our distribution metrics catch (coverage collapses on
spaces ≫16 unless the solver's own model choice is diverse). **The swizzle arm
must be pinned to a named Verilator version**, because this protocol has
already changed once under us.

---

## 2. What gets published

Three pages under a "Results" section of the docs site, each regenerated per
`main` commit, each carrying a provenance block (commit, date, dv-solve
version, z3/boolector/bitwuzla/Verilator versions, CPU model + core count —
**never the hostname**, see §6).

### 2a. Correctness board (the one that can fail the build)

Per fixture: verdict of each arm (sat/unsat/unknown/timeout/error), and a
**disagreement** column. Summary tiles: agree / dv-unknown / dv-timeout /
**disagree (must be 0)**. dv `sat` models are validated by z3 (assert the
model back, expect sat) so a wrong model is a disagreement even when the verdict
matches.

Unknowns link to the honest-unknown catalog (`dv-solve-unknown-gaps`), so an
`unknown` is visibly a known gap, not silence.

### 2b. Performance board

- Per-suite **cactus plot** (instances solved vs time) — dv(auto), dv(bitblast),
  z3, bitwuzla, boolector (boolector QF_BV fixtures only; it cannot parse the
  yosys-smtbmc `declare-sort` dialect).
- Per-fixture table: min-of-N wall ms per arm, ratio dv/bitwuzla, dv
  encode-ms / sat-ms split (`DV_BB_STATS`).
- Aggregates: solved@budget, PAR-2, geometric-mean ratio vs bitwuzla, and the
  fixed startup floor (empty-problem run) reported separately so the ~ms floor
  does not masquerade as solver speed.
- **Trend chart**: geometric-mean ratio vs bitwuzla per commit (§5).

Primary quantity is a **ratio against a reference solver timed in the same
job**, not absolute ms. The Forgejo runner is shared hardware; absolute times
move with host load, ratios mostly don't. Absolute ms is shown, never trended.

### 2c. Distribution board (randomization quality)

Benchmarks: the `sample_quality.py` set (countones, ot_aon_wkup, range_1000,
disjoint_ranges, sum_eq_const, a_lt_b, dep_range, bit_mask), growing toward the
PR#8042 `distr_*` set per `randomness_benchmark_plan.md`.

Arms, all drawing N samples (default 2000) from the same SMT2 problem:

| Arm | How samples are drawn |
|---|---|
| `dv-verilator` | `dv-solve-smt2 --interactive --mode=verilator`, fed the swizzle protocol (dv recognises it and samples via CDCL) — what a Verilator user gets with dv-solve |
| `dv-seeded` | fresh `dv-solve-smt2` per sample with `:random-seed` = i (native sampling) |
| `z3-native` | z3 with `(set-option :random-seed i)` / `smt.random_seed`, no swizzle — the "naive native SMT2" baseline |
| `z3-swizzle` | z3 driven through the Verilator 5.046 XOR-hash protocol — **today's Verilator default** |
| `bitwuzla-swizzle` | same protocol over bitwuzla (optional; shows the protocol, not z3, is the limit) |
| `uniform` | RNG over the z3-enumerated space — bias floor |

Per benchmark × arm: coverage, JSD (normalized, plus the ×100 form for
PR#8042 comparability), χ² p-value, thin-branch ratio, per-randomize latency,
solver round-trips per randomize. Plus a small-multiples histogram per
benchmark (sorted solution frequency vs uniform expectation) — this is the
picture that makes a skew legible at a glance.

Ground truth enumeration is z3 only (independent oracle), cached by content
hash of the problem so CI enumerates once.

---

## 3. The swizzle driver (the one new piece of logic)

`tests/perf/vlt_protocol.py` — a Python re-implementation of
`VlRandomizer::next()` for **one pinned Verilator version**, driving any
interactive SMT2 solver over a pipe:

- input: an SMT2 problem (decls + asserts) and the list of rand vars/widths
- per sample: emit exactly the command sequence above (reset, re-declare,
  re-assert, check-sat, ≤4 random 1-bit XOR-hash asserts, get-value), with
  `VlRNG` replaced by a seeded Python RNG
- output: model + round-trip count + wall time

Why emulate instead of running real Verilator in the loop:
- CI then needs no Verilator compile per benchmark (minutes each).
- The arm is solver-agnostic: the same driver runs z3, bitwuzla and dv-solve,
  so a difference in results is the solver's, not the harness's.

The risk is fidelity drift. Guard it with a **fidelity test**: for 2–3
benchmarks, compile the `.sv` form with the bundled Verilator 5.046, run with
`VERILATOR_SOLVER` pointed at a logging `tee` wrapper around z3, and assert the
captured command stream matches the driver's stream structurally (same command
kinds, same count, same hash-constraint shape). Run it in the docs job when the
bundled Verilator version changes, and locally otherwise. When Verilator is
bumped, the driver is versioned (`protocol="verilator-5.046"`) and the page
says which protocol produced the numbers.

A real-Verilator end-to-end arm (compiled `phase0/t_bench.sv`, 2000
randomize() calls, z3 vs dv-solve) is worth keeping as a single headline number
("per-randomize latency in a real simulation"), but in the weekly job, not per
commit (§4).

---

## 4. Harness consolidation

New package `tests/perf/` (importable, no pytest dependency for the runners):

```
tests/perf/
  arms.py          Arm = (name, kind: oneshot|interactive|protocol, argv, env, version())
                   wraps tests/formal/harness/* solvers; adds dv(auto),
                   dv(bitblast), dv(verilator), z3/bitwuzla swizzle arms
  vlt_protocol.py  §3
  suites.py        named fixture sets + budgets (see below)
  run_perf.py      correctness + timing  -> results/perf.json
  run_dist.py      distribution metrics  -> results/dist.json
                   (metrics code MOVED from sample_quality.py, which then
                    imports it -- one implementation, not two)
  schema.py        dataclasses + schema_version; JSON writer/reader
  render.py        results/*.json + history -> docs/results/*.md + SVG charts
```

`run_compare.py` and `sweep_perf.py` become thin CLIs over `run_perf.py` (or
are deleted); keeping three runners guarantees the published numbers and the
numbers engineers look at locally drift apart.

### Result schema (one record per fixture × arm × rep-aggregate)

```json
{ "schema": 1,
  "run": { "commit": "…", "branch": "main", "utc": "…",
           "dv_version": "…", "solvers": {"z3": "4.16.0", "bitwuzla": "0.8.2", "verilator": "5.046"},
           "host": {"cpu": "…", "cores": 32, "runner_class": "forgejo-bench"} },
  "perf": [ { "suite": "tier2", "fixture": "…", "sha1": "…", "arm": "dv-auto",
              "verdict": "sat", "model_ok": true, "wall_ms_min": 1.8, "wall_ms_med": 2.0,
              "reps": 5, "encode_ms": 0.3, "sat_ms": 0.9 } ],
  "dist": [ { "bench": "countones", "arm": "z3-swizzle", "n": 2000, "space": 70,
              "coverage": 0.31, "jsd": 0.12, "chi2_p": 0.0, "thin": 0.0,
              "ms_per_sample": 3.4, "roundtrips_per_sample": 5.0,
              "hist": [ … ] } ] }
```

Fixture `sha1` is recorded so a trend line breaks visibly when a fixture is
regenerated, instead of silently comparing different problems.

### Suites and budgets

| Suite | Contents | Where it runs | Target wall |
|---|---|---|---|
| `docs` | tier1–4, gaps, verilator, wide (≈215) × {dv-auto, dv-bitblast, z3, bitwuzla, boolector*}; 10 s timeout, min of 3; dist set × 6 arms × N=2000 | every `main` push | ≤ 20 min on the runner |
| `weekly` | `docs` + SMT-COMP parallel subset at 60 s + real-Verilator phase0 arm + PR#8042 `distr_*` at N=10000 | `schedule:` cron, publishes the same pages with a "weekly" section | ≤ 2 h |
| `local` | anything, same CLI | developer machine | — |

Parallelism: run fixtures across cores **but one timed process per core** and
cap at `nproc/2` so timed runs don't contend on memory bandwidth; reference
and dv arms for a fixture are interleaved (A B A B …) so a load spike hits both
sides of the ratio.

---

## 5. History and trends

Needed for trend charts; must not need write credentials in CI.

**Recommended: the published site is the store.** The docs job fetches
`https://dvkit.org/fvutils/dv-solve/results/history.jsonl` (one summary line
per commit: suite aggregates only, not per-fixture rows — keeps it small),
appends the current run, renders, and republishes it with the site.

Fail-closed rules (mirroring the pipeline's own shrink guard):
- fetch returns 404 → allowed only if an explicit `BENCH_HISTORY_BOOTSTRAP=1`
  repo variable is set (first run); otherwise fail.
- any other fetch failure, or a history shorter than the fetched one → fail the
  job; never publish a truncated history.
- a line whose `schema` is unknown is carried through untouched.

Full per-run JSON is also uploaded as a plain CI artifact (`bench-raw`) for
forensics, subject to Forgejo's artifact retention.

Alternative if losing the history on a bad deploy is unacceptable: a
`bench-data` orphan branch the job pushes to with a scoped token. More robust,
but it puts a write credential on the runner and adds a branch that git-sync
mirrors — only worth it if the site-as-store proves fragile.

---

## 6. The docs workflow

`.forgejo/workflows/docs.yml` (Forgejo-only by design: only Forgejo artifacts
are consumed; no GitHub twin needed because it publishes nothing on GitHub —
but add it to the `ci.yml` no-publisher check's allow reasoning so the next
reader knows why it exists only on one side).

```
on: push (main), schedule (weekly), workflow_dispatch
concurrency: docs-${{ github.ref }}, cancel-in-progress: true

jobs:
  bench-and-docs  (runs-on: a dedicated label if one exists, see open Q3)
    1. checkout
    2. build dv-solve wheel, pip install it     # py3.10/pip22: no -e installs
    3. provision solvers, pinned:
         z3        pip z3-solver==4.16.x
         bitwuzla, boolector, verilator: edapack release tarballs, versions pinned
         in one file (tests/perf/solvers.lock); sha256-checked; cached
    4. python3 -m tests.perf.run_perf --suite docs  -> perf.json
       python3 -m tests.perf.run_dist --suite docs  -> dist.json
    5. fetch history.jsonl (§5), append
    6. python3 -m tests.perf.render  -> docs/_generated/results/*.md + *.svg
    7. sphinx-build docs _site
    8. sanity gates (below)
    9. upload artifact  docs-fvutils-dv-solve   path: _site/**
   10. upload artifact  bench-raw               (perf.json, dist.json)
```

**Sanity gates** — these stand in for the review gate auto-promote removed:

- `_site/index.html` and `_site/results/{correctness,performance,distribution}.html` exist
- record counts ≥ expected (a suite that silently ran 0 fixtures fails)
- every arm reported a version (a missing solver fails; it doesn't vanish from the chart)
- **disagreements == 0 and model_ok everywhere** — a soundness finding fails the
  job and blocks publication (soundness-first rule); the raw artifact still
  uploads so it can be diagnosed
- history length ≥ previous
- the internal-identifier grep from `ci.yml`, run over `_site/` too: the
  provenance block must never carry a hostname or runner address

Perf regressions **do not** fail the job; they are flagged on the page
(ratio worse than trailing-10 median by > 15 %, highlighted). Noise on shared
hardware makes a hard perf gate flaky, and a flaky gate gets disabled.

### Docs site

Sphinx + MyST (Markdown in, so the existing `docs/*.md` can be pulled in
selectively later), matplotlib SVG charts rendered at build time — static, no
JS dependency, works under the dvkit.org path prefix. Minimal initial site:
`index` (what dv-solve is, links), `results/` (the three generated pages),
and a hand-picked handful of existing design docs. Most of `docs/` is internal
working notes and should **not** be published wholesale — decide the
allow-list explicitly.

---

## 7. Phasing

| Phase | Deliverable | Done when |
|---|---|---|
| **P0 — runner proof** | `docs.yml` that builds the wheel on the Forgejo runner, installs solvers, publishes a one-page Sphinx site | page visible at dvkit.org/fvutils/dv-solve/ |
| **P1 — schema + consolidation** | `tests/perf/` with `arms`, `schema`, `run_perf`; `run_compare`/`sweep_perf` ported onto it | local `run_perf --suite docs` reproduces the 2026-07-21 sweep numbers within noise |
| **P2 — correctness + perf pages** | `render.py` for 2a/2b; disagreement gate live | pages published per `main` push |
| **P3 — swizzle driver + distribution page** | `vlt_protocol.py` + fidelity test vs real Verilator 5.046; `run_dist` with all six arms; 2c page | z3-swizzle numbers reproduce a real-Verilator+z3 run on one benchmark within sampling error |
| **P4 — history** | history.jsonl store, trend charts, regression highlighting | 10 commits of trend visible |
| **P5 — weekly** | cron job: hard corpus subset, real-Verilator arm, PR#8042 `distr_*` | weekly section published |

P0 is deliberately first and deliberately trivial: every later phase depends
on the runner being able to build and fetch solvers, and it currently has never
done either.

---

## 8. Risks

- **Runner timing noise.** Mitigated by ratios, interleaving, min-of-N, and no
  hard perf gate. If a dedicated runner label is available, use it with
  capacity 1 for this job.
- **Protocol fidelity.** The swizzle arm is only meaningful if it matches real
  Verilator — hence the fidelity test and the explicit protocol version on the
  page.
- **Solver provisioning from GitHub releases** puts github.com on the docs
  critical path. Acceptable for docs (not a release); if it bites, mirror the
  pinned tarballs into the Forgejo package registry.
- **Publishing unfavourable numbers.** The z3-native / z3-swizzle arms will
  sometimes beat dv-solve (e.g. array-heavy or signed-div fixtures that are
  honest `unknown`). Publish them anyway — the credibility of the page depends
  on it — but annotate known gaps.
- **Budget creep.** Keep the per-commit suite under ~20 min; anything heavier
  goes to weekly.

## 9. Open questions

1. Is "Verilator-style swizzling" meant to be Verilator's **current** XOR-hash
   protocol (5.046, assumed above) or the older bit-pin/`check-sat-assuming`
   loop from the covergroup-rt fork? The driver can support both as named
   protocols; the question is which one is the headline.
2. Sphinx+MyST (assumed) vs whatever other fvutils/dvkit repos already use —
   matching the estate is worth more than the choice itself.
3. Does the Forgejo runner have (or can it get) a dedicated label/host for
   timing jobs? Shared scheduling works but makes the absolute numbers noisy.
4. Should the correctness board also gate PRs (cheap, tier1+verilator only, no
   publishing) as a separate `ci.yml` job? Recommended, but out of this plan's
   docs scope.
