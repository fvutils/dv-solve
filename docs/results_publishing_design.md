# Performance and Distribution Results: Collection, Persistence, Views

Status: ACCEPTED 2026-10-02 (persistence revised the same day: no separate
repo; §11 decisions agreed). R0 BUILT 2026-10-02: `tests/perf/{calib,schema,
collect,history,consolidate}.py`, a dispatch-only `.forgejo/workflows/perf.yml`,
and `tests/unit/test_perf_history.py`. Confirmed: artifacts expire 90 days
after upload; the artifact API refuses anonymous reads; the automatic job
token can list this repo's artifacts (download is confirmed in R2). R0 DONE
2026-10-02: two dispatched runs consolidated into
`tests/perf/history/2026.jsonl`, one of them measured during the soundness
nightly and correctly marked noisy (§10).

R1 + R2 BUILT 2026-10-02 (pending the first nightly on the runner):
`tools.py` + `tools.lock.json` (bitwuzla 0.8.2 from the Verilator 5.046
bundle, boolector 3.2.4 from yosys-bin, sha256-pinned), `builds.py`,
`suites.py` + `suites/sat-core.json`, `run_sat.py`, `normalize.py`, `svg.py`,
`render.py`; `perf.yml` nightly at 05:00; `docs.yml` renders
`docs/site/results/` (index, sat; methodology is hand-written). Changes from
the design as first written:
- **The anchor is built from its tag, not installed.** The `v0.1.0` wheel
  carries only the libraries, not the `dv-solve-smt2` CLI; the CLI target
  builds from the tag's source in about 2 s. `v0.1.0`'s CLI also keeps its
  ~10 MB frontend struct on the stack and overflows the default 8 MB stack,
  so every arm runs with a 64 MB stack limit.
- **Speed-ups compare solving time,** CPU time minus each arm's own start-up
  cost (§5.4).
- **Model replay is not done yet.** Answers are checked against the
  references; the 34 fixtures that ask for a model are a later addition.
- **Charts are hand-written SVG** (`svg.py`), not matplotlib: byte-stable
  output and no new docs dependency.

This document builds on `docs/ci_benchmark_publishing_plan.md` (2026-09-30)
and replaces its §4 (harness), §5 (history), §6 (workflow) and §7
(phasing). That plan's §1 review, §2 page content and §3 swizzle driver still
hold and are summarised rather than repeated. Since that plan was written,
three things have changed:

- **The docs site exists.** `docs/site/` is Sphinx with MyST and is published
  by `docs.yml`.
- **The Forgejo runner builds and tests dv-solve.** `test.yml` and
  `nightly.yml` both run there.
- **The request has grown.** It now asks for release snapshots, plots over
  time, and a normalisation baseline so that numbers stay comparable as the
  hardware and the solvers move.

---

## 1. Goals and non-goals

**Goals**

1. The docs site shows *current* numbers for `main` (at most a day old) for
   three things:
   - **SAT performance:** dv-solve against the reference SMT solvers on SMT2
     fixtures.
   - **Randomization performance:** cost per `randomize()` call.
   - **Randomization quality:** how close the distribution is to uniform.
2. Each of these is shown for three ways of reaching a solver (§2):
   dv-solve itself, SMT2 SAT solving, and the SMT2 swizzle (Verilator's
   randomization protocol).
3. **Release snapshots.** Every `v*` tag produces an immutable, fuller
   measurement. The site plots the releases against each other.
4. **Plots over time.** Measurements from different months, solver versions
   and machines can be compared, because every number is normalised against
   baselines measured in the same job (§5).
5. **The history needed for the views is committed in this repo**, is public,
   and can be re-rendered. The views are a function of that data. Full
   per-run detail is kept as CI artifacts, which expire, except for release
   snapshots, which are committed (§4).
6. Collection runs on the local Forgejo runner.

**Non-goals**

- **A hard performance gate.** Regressions are flagged on the page, not
  failed. A noisy gate gets disabled, and then it protects nothing.
- **Per-push benchmarking.** It contends with `test.yml` on a runner with
  capacity 2 and produces noise (see §6).
- **Versioned docs per release.** dvkit.org serves only `latest` for each
  repo. Release history lives in the data and is drawn as one page (§7), not
  as frozen copies of the site.

---

## 2. What is measured

### 2.1 The three arm families

| Family | Question it answers | Arms |
|---|---|---|
| **dv-solve** | What a user of dv-solve's own API gets: builder/C API, SV DPI, zuspec | `dv-api`: ctypes builder, one `SolveCtx`, repeated `solve` with a new seed each call. `dv-scenario`: the `tests/bench/` zuspec scenarios on the native backend, in solves/s. |
| **SMT2 SAT** | How dv-solve compares as an SMT solver on one-shot `check-sat` | `dv-smt2` (auto routing), `dv-smt2-bb` (forced bitblast), `z3`, `bitwuzla`, `boolector` (pure QF_BV fixtures only) |
| **SMT2 swizzle** | What a Verilator user gets: the Verilator 5.046 protocol of up to 4 random 1-bit XOR-hash asserts (old plan §3) | `z3-swizzle` (stock Verilator today), `dv-swizzle` (`--mode=verilator`), `bitwuzla-swizzle` (shows whether the limit is the protocol or z3) |

Two reference arms are always present:

- **`uniform`:** a true RNG over the enumerated solution space. It is the
  floor for distribution bias.
- **`anchor`:** a frozen dv-solve build (§5.2). It is the floor for
  normalisation.

### 2.2 Benchmarks: one definition, every arm

Each randomization benchmark is written **once**, in the soundness campaign's
IR (`tests/formal/soundness/ir.py`). That IR already emits both SMT2 and
builder calls (`doors.py`), so `dv-api`, `dv-swizzle` and `z3-swizzle` solve
the same problem by construction, not by two hand-kept copies. Benchmarks
that the IR cannot express, such as arrays or `dist`, are added to the IR
when needed; the IR has grown before.

Suites are declared in manifests: `tests/perf/suites/<suite>.json`. The
format is JSON, not TOML, because the runner's Python 3.10 has no `tomllib`.

```json
{ "suite": "sat-core",
  "budget_s": 10,
  "reps": 5,
  "categories": [
    { "name": "tier1",     "weight": 1.0, "glob": "tests/formal/smt2/tier1/*.smt2" },
    { "name": "verilator", "weight": 2.0, "glob": "tests/formal/smt2/verilator/*.smt2" } ] }
```

`budget_s` is the per-fixture timeout. `reps` is the N in min-of-N (3 for
fixtures over 1 s). Weights are explained in §5.4.

When a suite is resolved, every fixture's sha256 is recorded. The **manifest
hash** (the sorted list of path and sha pairs) is stored with each run, so a
trend never silently compares different problems (§5.5).

| Suite | Contents | Used by |
|---|---|---|
| `sat-core` | tier1–4, gaps, verilator, wide: about 230 SMT2 fixtures | nightly, release, ladder |
| `sat-hard` | a 15-fixture subset of the SMT-COMP 2025 parallel corpus, 60 s timeout | release, weekly |
| `rand-core` | the `sample_quality.py` set (countones, ot_aon_wkup, range_1000, disjoint_ranges, sum_eq_const, a_lt_b, dep_range, bit_mask); N=2000 | nightly, release, ladder |
| `rand-pr8042` | the PR#8042 `distr_*` set, as it is ported; N=10000 | release, weekly |
| `scenario` | `tests/bench/` zuspec scenarios, 2 s each | nightly, release |
| `calib` | the calibration kernel (§5.1) | every run |

### 2.3 Metrics per record

- **SAT:**
  - verdict;
  - `model_ok` (z3 replays every `sat` model);
  - child **CPU time** (user + sys from `wait4` rusage), minimum and median
    over the reps;
  - wall time;
  - dv's encode-ms / sat-ms split (`DV_BB_STATS`);
  - peak RSS.
- **Randomization:**
  - coverage;
  - JSD, both normalised and ×100 (comparable with PR#8042);
  - χ² p-value;
  - thin-branch ratio;
  - **excess JSD**, which is JSD(arm) − JSD(uniform) at the same N and is
    the number that gets trended;
  - latency per sample at p50, p95 and p99;
  - solver round trips per sample;
  - the sorted frequency histogram.
- **Scenario:** solves/s and ns per solution.

CPU time is the primary timing quantity. Wall time is kept for forensics.
CPU time of the child process ignores the scheduler waits caused by other
load, which is most of the noise on a shared host.

Correctness is gated on every run. Any verdict disagreement, or any
`model_ok == false`, makes the run **invalid**: the job fails, the record is
still written with `valid: false` for diagnosis, and the renderer never plots
it. This follows the soundness-first rule.

---

## 3. Architecture

```
            ┌──────────────── Forgejo runner (label: bench, capacity 1) ───────────────┐
 schedule ─▶│ perf.yml   nightly 05:00   main HEAD (skip if already measured)          │
 tag v*  ──▶│ perf.yml   on tag          release snapshot (fuller suites)              │
 weekly  ──▶│ perf.yml   Sat 05:00       ladder: last K releases + main, same job      │
 dispatch ─▶│                            manual / backfill of an old tag               │
            │   build dv-solve-smt2 (+ anchor/ladder builds from their tags)           │
            │   provision pinned solvers (tests/perf/solvers.lock)                     │
            │   calib → sat-* → rand-* → scenario   → run.json.gz (+ valid flag)       │
            │   upload artifact perf-<kind>-<utc>-<sha>     (no write credential)      │
            └──────────────────────────────────┬───────────────────────────────────────┘
                                               │ Forgejo artifacts (expire, ~90 days)
                       ┌───────────────────────┴───────────────────────┐
                       ▼                                               ▼
     tests/perf/consolidate.py  (run by hand:               docs.yml (push + 06:30 cron)
     at each release, at least monthly)                       committed history
       artifacts → trend lines + release snapshots          + artifacts newer than it
       → ordinary commit to main:                             → tests/perf/render.py
         tests/perf/history/                                  → results/*.md *.svg *.csv
                       │                                      → sphinx → artifact
                       └──────── read by ────────────────────▶ docs-fvutils-dv-solve
                                                                         ▼
                                                    dvkit.org/fvutils/dv-solve/results/
```

Three rules shape this:

- **The measurement job never builds docs and never writes to git.** It
  uploads one artifact and stops. A perf run can therefore never replace the
  site, never race a push to `main`, and needs no write credential.
- **The docs job never measures.** It renders the committed history plus the
  artifacts newer than it. It runs on every push and on a daily cron shortly
  after the nightly perf run, so "current" lags `main` by at most about a day.
- **Only a person commits data.** Consolidation is an ordinary commit to
  `main`, made at release time and at least monthly, so data enters history
  the same way code does.

---

## 4. Persistence

### 4.1 How much data

Measured with a synthetic record shaped like the real thing (215 SMT2
fixtures × 6 SAT arms, 8 randomization benchmarks × 6 arms, N=2000):

| Content of one nightly run | Raw | gzip |
|---|---|---|
| Full record: every fixture × arm with all timings, plus histograms | 635 KB | 130 KB |
| … of which SAT timings | 404 KB | 77 KB |
| … of which histograms | about 230 KB | about 55 KB |
| Distribution metrics without histograms | 6.5 KB | 0.4 KB |
| **Trend line** (§4.3): per-fixture ratios plus metrics | about 4 KB | — |

Over a year of nightly runs, full records are about **47 MB gzipped**, which
is too much to commit. Trend lines are about **1.5 MB of text**, which is
not.

Two facts make the split work:

- **The trend charts need only ratios.** They use per-fixture speedups and
  suite-level metrics. Per-fixture milliseconds, encode/sat splits and RSS
  matter only for the "current" pages and for releases.
- **Histograms can be regenerated.** Sampling is deterministic given the
  commit, the arm, N and the seed base. A nightly record stores only the
  metrics derived from them; release snapshots keep the histograms.

### 4.2 Three tiers

| Tier | What | Where | Lifetime |
|---|---|---|---|
| **Run record** | The full record of every run (§4.4), gzipped | Forgejo artifact `perf-<kind>-<utc>-<shortsha>` | About 90 days. Our instance sets no retention, so the Forgejo default should apply; R0 confirms it. |
| **Trend history** | One trend line per run and build (§4.3) | Committed: `tests/perf/history/<year>.jsonl` | Permanent |
| **Release snapshot** | The full record of each release run, histograms included | Committed: `tests/perf/history/releases/<tag>.json.gz`, about 130 KB each | Permanent |

Committed alongside them:

```
tests/perf/history/
  2026.jsonl                    trend lines, sorted by utc; plain text
  releases/v0.2.0.json.gz       full record of the release run
  manifests/<manifest-hash>.json  resolved suite: fixture order, path, sha256, category
  machines/<machine-id>.json    machine fingerprint (§5.1)
  anchors/<from>-<to>.json      anchor links (§5.2)
tests/perf/truth/<sha256>.json.gz  z3-enumerated solution spaces, keyed by problem hash
```

**Compression.** The trend history is plain `.jsonl` and is never gzipped.
git already compresses its packs and stores an appended text file as a small
delta, so the plain file is effectively the compressed form. A `.gz` blob
defeats deltas and costs its full size on every change. Release snapshots
are gzipped because they are written once and never change, so there is no
delta to lose.

**Ground truth** (`tests/perf/truth/`) is committed together with the
benchmark that needs it. Whoever adds a randomization benchmark runs the z3
enumeration locally (`python3 -m tests.perf.truth <bench>`) and commits the
result. `collect` refuses to run a benchmark without its truth file, so CI
never enumerates and the result never changes under a benchmark.

### 4.3 The trend line

One line per run and per build measured in it. A ladder run therefore writes
one line for `main` and one for each release:

```json
{"h":1,"utc":"20261003T050012Z","commit":"305f281…","kind":"nightly","build":"head",
 "valid":true,"noisy":false,"machine":"m3f9a","calib":{"z3-ops":2044.1,"z3-php10":2338.6},"anchor":"v0.1.0",
 "lock":"5be1…","artifact":"perf-nightly-20261003T050012Z-305f281",
 "sat":{"sat-core":{"m":"9f2e…",
         "vs_anchor":[312,-41,null,…],"vs_bitwuzla":[…],"vs_z3":[…],
         "solved":{"head":212,"anchor":205,"bitwuzla":214,"z3":209},
         "par2_s":{"head":41.2,"anchor":55.0,"bitwuzla":37.9,"z3":60.3},
         "floor_ms":0.71}},
 "rand":{"rand-core":{"m":"11ab…","countones":{"dv-api":{"cov":0.97,"jsde":0.004,"chi2p":0.41,
         "p50":0.21,"p95":0.30,"rt":1.0,"vs_anchor":180},"z3-swizzle":{…}},…}},
 "scen":{"axi4burst":{"sps":871.3,"vs_anchor":95},…}}
```

- **Per-fixture ratios** are integer milli-nepers (1000 × ln(ratio)), held in
  an array in the order of the manifest named by `m`. `null` means excluded:
  unsolved on one side, under the start-up floor, or `n/a`. This keeps a line
  at about 4 KB and still lets every suite score be recomputed with any
  weights (§5.4).
- **Invalid and noisy runs get a line too.** They carry `valid` and `noisy`,
  plus a `reason` when invalid, so the history records that a night was bad
  rather than silently missing.
- **`artifact` names the source record,** so while the artifact still exists
  a trend point can be traced to its full detail.

### 4.4 The run record (schema v1)

```json
{ "schema": 1,
  "valid": true,
  "kind": "nightly | release | ladder | manual",
  "run":  { "utc": "…", "commit": "305f281…", "ref": "refs/heads/main",
            "dv_version": "0.2.0.dev12+g305f281", "workflow_run": 1234 },
  "machine": { "id": "m3f9a", "cpu": "AMD Ryzen 9 9950X 16-Core Processor",
               "cores": 32, "mem_gb": 60, "kernel": "6.8",
               "loadavg_start": 0.4, "loadavg_end": 0.6, "noisy": false },
  "tools": { "z3": "4.16.0", "bitwuzla": "0.8.2", "boolector": "3.2.4",
             "verilator": "5.046", "protocol": "verilator-5.046",
             "lock_sha": "…" },
  "anchor": { "tag": "v0.1.0", "commit": "…" },
  "calib":  { "kernel_cpu_ms": { "cnf-a": 812.0, "cnf-b": 440.1, "z3-fixed": 95.2 },
              "index": 1.000 },
  "manifests": { "sat-core": { "hash": "9f2e…", "fixtures": [ … ] } },
  "sat":  [ { "suite": "sat-core", "fixture": "tier2/…", "sha": "…", "cat": "tier2",
              "arm": "dv-smt2", "build": "head", "verdict": "sat", "model_ok": true,
              "cpu_ms_min": 1.8, "cpu_ms_med": 2.0, "wall_ms_min": 2.1, "reps": 5,
              "encode_ms": 0.3, "sat_ms": 0.9, "rss_kb": 14200 } ],
  "rand": [ { "bench": "countones", "sha": "…", "arm": "z3-swizzle", "build": "head",
              "n": 2000, "seed_base": 1, "space": 70, "coverage": 0.31,
              "jsd": 0.12, "jsd_x100": 12.0, "jsd_excess": 0.09, "chi2_p": 0.0,
              "thin": null, "lat_ms": {"p50": 3.1, "p95": 4.0, "p99": 6.2},
              "roundtrips": 5.0, "hist": null } ],
  "scenario": [ { "scenario": "axi4burst", "build": "head", "solves_per_s": 871.3 } ] }
```

- `build` is `head`, `anchor`, or a release tag on a ladder run. One record
  format serves all four kinds of run.
- `hist` is a sparse list of (value, count) pairs on `release` runs and
  `null` otherwise.
- The record carries its resolved manifests in full, so consolidation can
  write a manifest file the first time it sees one.

**Nothing in the record identifies the host.**
- `machine.id` is the first 5 hex digits of
  sha256(cpu, cores, mem_gb, kernel major and minor). It names the machine
  class, not the host.
- The internal-identifier grep runs over the record before upload, again in
  consolidation over every file it writes, and again over the rendered site.

### 4.5 Consolidation

`tests/perf/consolidate.py` turns artifacts into committed history. It
writes files and **does not commit**; the result is reviewed and committed
like any other change.

1. Find the newest `utc` already in `tests/perf/history/*.jsonl`.
2. List the `perf-*` artifacts newer than that through the Forgejo API,
   using a read-only token from the environment, and download them.
3. Validate each record against the schema.
4. Append its trend lines to the year's `.jsonl`, keeping the file sorted by
   `utc` and dropping duplicates by (utc, commit, kind, build). Running it
   twice changes nothing.
5. For a `release` record, write `releases/<tag>.json.gz`. If a release has
   more than one record (for example a backfill), keep the earliest valid one
   as the snapshot.
6. Write any manifest, machine or anchor-link file it has not seen before.
7. Grep everything it wrote for internal identifiers.
8. Print a summary: runs added, invalid and noisy runs, date range, releases
   added.

**When it runs:**
- **At each release.** The tag's perf run finishes after the tag is pushed,
  so its snapshot lands in the next commit after the tag.
- **At least monthly.** The deadline is artifact expiry, about 90 days.

**Missing it loses data,** so it is watched:
- **At 60 days:** once the oldest unconsolidated artifact is more than 60
  days old, the docs job prints a warning annotation, and the `trends` page
  shows a banner giving the number of unconsolidated runs.
- **At 80 days:** the nightly perf job fails with "consolidate now", so it
  shows as a red run in the Forgejo UI rather than a quiet banner.

### 4.6 Credentials

- **The perf job:** none beyond the automatic job token, which it uses to
  upload its artifact.
- **The docs job:** needs to list and download this repo's artifacts. If the
  automatic job token can do that, nothing is added. If not, a read-only
  repository secret is added. R0 settles which.
- **Consolidation:** run by a person with their own read-only token.

**No write credential exists in CI.** Nothing in CI can change git history.

---

## 5. Normalisation: keeping numbers comparable over time

Raw times move with the CPU, the kernel, the boost clock, the solver
versions, other load on the host, and the fixture set. Each of those has its
own guard. Every published trend number is a **ratio of two measurements
taken in the same job on the same machine**. Raw milliseconds are stored but
never trended.

### 5.1 Calibration kernel: a machine index

Each run starts by timing a fixed, pinned workload:

- `z3-ops`: the pinned z3 on `tests/formal/smt2/verilator/t_constraint_operators.smt2`,
  the only fixture that takes z3 more than 0.1 s;
- `z3-php10`: the pinned z3 on a generated pigeonhole problem (10 into 9),
  a pure Boolean search;
- each takes about 2 s of CPU time (on a Ryzen 9 9950X); 5 reps, minimum
  taken, and the answer is checked (`tests/perf/calib.py`).

A kissat kernel from the anchor build was the first idea. The anchor is
installed from its PyPI wheel, though, which does not expose kissat on its
own, so the kernels use the pinned z3.

```
index(run) = geomean_k( cpu_ms_k(run) / cpu_ms_k(epoch reference run) )
```

The index is not used to correct the trends, because ratios already cancel
the machine. It detects that the machine changed:

- **What starts a new epoch:** a different `machine.id`, or an index outside
  [0.9, 1.1] for three consecutive runs.
- **What the trend chart shows:** epoch boundaries as vertical bands.
- **What the absolute-ms views show:** time ÷ index, labelled "on the epoch
  reference machine".

The index also flags a single run that is far out (above 1.15): that run is
marked `noisy` and drawn hollow.

### 5.2 The anchor: a frozen dv-solve build

Every run also measures a **frozen dv-solve build**, the anchor. It is
`v0.1.0` until it is deliberately moved.

- **Where the build comes from:** the anchor is always a release tag, and
  its `dv-solve-smt2` target is built from the tag's source in each run
  (about 2 s; `tests/perf/builds.py`). The published wheels carry only the
  libraries, so installing them would not give the CLI.
- **What is reported:** the core trend quantity is
  ```
  speedup_vs_anchor(f) = cpu(anchor, f) / cpu(head, f)        (same job)
  ```
  It is immune to machine drift, solver upgrades and host load to first
  order, because both builds suffer the same conditions within minutes of
  each other. Anchor and head reps are **interleaved** (A H A H …) so a load
  spike hits both sides.
- **Moving the anchor** (for example when v0.1.0 can no longer run a suite):
  the next ladder run measures both anchors. The ratio between them is
  committed as an **anchor link**
  (`tests/perf/history/anchors/v0.1.0-v0.3.0.json`), and the trend is
  chained across the link.
  It is never re-based silently.
- **Arms an old anchor cannot run** (for example `--mode=verilator` before it
  existed) are recorded as `n/a` and left out of that arm's anchor ratio.
  They are not counted as failures.

### 5.3 Reference solvers: the competitive ratio

```
ratio_vs_ref(f) = cpu(dv, f) / cpu(bitwuzla, f)     (and vs z3)
```

The versions are pinned in `tests/perf/solvers.lock` (sha256-checked
tarballs and pip pins). Upgrading a reference solver is a commit to the lock
file. The renderer draws a marker on the competitive trend at that commit,
because the denominator changed. The anchor trend (§5.2) is unaffected.

### 5.4 Aggregation and weights

Fixtures are not equally informative: there are 34 tier1 micro-fixtures and
40 real Verilator transcripts. Suite scores use **category-weighted geometric
means**, with weights in the manifest:

```
S = exp( Σ_c w_c · mean_{f∈c} ln r_f  /  Σ_c w_c )
```

Rules for what goes into a score:

- Only fixtures that both sides solved, and that have the same sha as the
  comparison run, enter `r_f`.
- Fixtures that **time out** are counted separately, as solved@budget and
  PAR-2, and never enter a ratio.
- **Ratios compare solving time:** each side's CPU time minus its own
  start-up cost (its time on a one-variable problem, measured in the same
  run), floored at 0.5 ms. Start-up is reported separately.
- A fixture where both sides solve within 1 ms of start-up is counted as
  answered but **excluded from ratios**: at that scale a ratio measures noise.
  In the first runs this left 33–122 of the 215 fixtures with a usable ratio,
  depending on the pair compared.

Starting weights:
- `verilator` and `scenario`: 2.0. This is the product's use case.
- `tier1`–`tier4`, `gaps` and `wide`: 1.0.
- `sat-hard`: 1.0, but kept as a separate headline because it is a different
  regime.

The manifest hash covers fixtures and categories, not weights. The renderer recomputes
**every** historical point from raw records with the current weights. Weights
are a property of the view, not of the stored data.

### 5.5 Noise band and the regression flag

The anchor runs every night against unchanged code. Its own run-to-run
variation, σ of ln(cpu(anchor)) over the last 20 runs, is a direct
measurement of noise.

- **When a fixture is flagged:** when its `speedup_vs_anchor` moves by more
  than max(3σ, 5%) from the trailing 10-run median.
- **When a suite is flagged:** when its suite score moves by more than
  max(3σ_suite, 3%).
- **How it shows:** the flag is drawn on the page and listed in the run
  summary. The job does not fail.

### 5.6 Distribution metrics need almost no normalisation

- **They are deterministic.** Given the problem, arm, build, N and seed base,
  the samples are fixed, so there is no machine noise. They are trended
  directly as `jsd_excess`, coverage and thin.
- **Seed sets are fixed per suite version.** The trend therefore shows
  changes in the sampler, not luck of the seeds. A release snapshot also runs
  a second, independent seed base, so that over-fitting to the fixed seeds
  would show.
- **Latency per sample is a timing.** It follows the §5.2 and §5.3 rules: it
  is reported against the anchor and against `z3-swizzle` measured in the
  same job.

---

## 6. Workflows and schedule

All of these are Forgejo-only, like `test.yml` and `nightly.yml`. None
publishes a package, so none needs release authority. `ci.yml`'s
no-publisher check is unaffected; add a header comment explaining why there
is no GitHub twin.

### 6.1 `perf.yml`

```
on:
  schedule:  '0 5 * * *'      # nightly main; Saturday also runs the ladder
  push:      tags: ['v*']     # release snapshot
  workflow_dispatch:          # inputs: kind (manual|ladder|backfill), ref
concurrency: perf             # one perf job at a time; queue, don't cancel
runs-on:   ${{ vars.PERF_RUNNER || 'ubuntu-latest' }}   # 'bench' once it exists (§6.3)
timeout-minutes: 150
```

| Kind | Builds | Suites | Budget |
|---|---|---|---|
| nightly | head + anchor | calib, sat-core, rand-core, scenario | about 40 min |
| release | tag + anchor | all suites; second seed base; N=10000 | about 90 min |
| ladder (Saturday, only if a tag or lock-file change is new; or by hand) | head + anchor + the last K=4 releases | calib, sat-core, rand-core | about 2 h |
| backfill (dispatch) | an old tag + anchor | the release suites, recorded as `kind: release` with `measured_later: true` | about 90 min |

Rules for when a run is skipped or marked:

- **Nightly** skips if `main` HEAD already has a valid nightly record less
  than 7 days old on this machine. It checks the committed history and the
  existing `perf-nightly-*` artifacts. The calibration and anchor still run
  weekly, so the noise band stays fresh.
- **A tag push is idempotent.** If `tests/perf/history/releases/<tag>.json.gz`
  is committed, or a valid `perf-release-*` artifact for the tag exists, the
  job exits.
- **A mirrored tag** (pushed on GitHub and synced in) triggers the same job,
  and is safe for the same reason.
- **Host load:** before and after each suite, the job records the host's
  `/proc/loadavg` (it is not namespaced, so the container sees the whole
  host). If the load exceeds 4, the run is marked `noisy` and is shown, but
  it is excluded from the noise band and from regression flags.

The job's steps:

1. Checkout.
2. Install pip tools; fetch and verify the pinned solver tarballs.
3. Build the head `dv-solve-smt2`; the anchor and ladder builds are built
   from their tags by `collect`.
4. (merged into 2)
5. Run `python3 -m tests.perf.collect --kind … --out run.json`.
6. Validate the record against the schema.
7. Grep it for internal identifiers.
8. Upload it gzipped as the artifact `perf-<kind>-<utc>-<shortsha>`. Always
   upload, even for an invalid run, so the failure can be diagnosed.
9. Fail if `valid == false`.
10. Fail if the oldest unconsolidated artifact is more than 80 days old
    (§4.5).

### 6.2 Fitting around the soundness nightly

```
02:30–04:45  nightly.yml   soundness campaign (12 processes; machine saturated)
05:00–05:45  perf.yml      nightly perf (worker count = 4, pinned with taskset)
06:30        docs.yml      cron re-render (also runs on every push)
```

- **Why not overlap:** a perf run alongside the 12-process campaign would be
  measuring the campaign.
- **Cost:** the perf job adds about 45 minutes a night on top of the 2-hour
  soundness budget, and about 2 h on Saturdays for the ladder.
- **Fallback if the two collide** (for example the campaign overruns):
  `noisy` marking, not a hard lock.

### 6.3 Runner

**Recommended:** register a second runner instance on the local server with
the label `bench`, capacity 1, used only by `perf.yml`. That keeps a
push-triggered `test.yml` from landing on the machine mid-measurement through
the second slot of the existing runner.

- **Optionally:** give its container a fixed `--cpuset-cpus` range, so perf
  workers do not share cores with whatever else the host runs.
- **Who does it:** this is an operator change in the runner config, not in
  this repo.
- **Fallback:** without the label, `runs-on: ubuntu-latest` still works. The
  data is noisier, and `noisy` marking catches the worst of it.

Inside the job, measurement uses `nproc_bench / 4` workers, each pinned to
its own core with `taskset`, with one timed process per core.

### 6.4 `docs.yml` changes

1. Add a `schedule: '30 6 * * *'` trigger.
2. Before `sphinx-build`:
   - download the `perf-*` artifacts newer than the committed history
     (`python3 -m tests.perf.history fetch`; about 130 KB each, so at most
     about 12 MB for 90 days);
   - run `python3 -m tests.perf.render --out docs/site/results`, which reads
     `tests/perf/history/` plus the downloaded records.
3. **Fail closed.** If the artifact listing or a download fails, the docs
   build fails. A successful build without the results pages, or with
   "current" quietly rolled back to the last consolidation, would deploy.
   Zero unconsolidated artifacts is fine and is not an error.
4. New sanity gates:
   - the results pages exist;
   - the newest valid record is under 10 days old (stale data fails loudly);
   - a warning annotation and a page banner once the oldest unconsolidated
     artifact is more than 60 days old (§4.5);
   - every plotted arm reported a tool version;
   - the existing identifier grep runs over the generated CSV/SVG too.
5. Raise the page-count floor to cover the new pages.

---

## 7. Views (the "Results" section of the site)

All charts are static SVG rendered by matplotlib when the docs are built, so
there is no JavaScript and they work under the dvkit.org path prefix. Each
chart has a CSV of exactly the plotted numbers next to it, so readers can
check them or plot them their own way. Every page opens with a **provenance
block**:
- commit and date;
- dv-solve version;
- tool versions from the lock file;
- anchor;
- machine class (CPU model, cores; never a host name);
- run kind.

| Page | Content |
|---|---|
| **`results/index`** — overview | Headline tiles comparing current `main` with the last release: SAT score vs bitwuzla, randomize latency vs `z3-swizzle`, mean excess JSD vs the uniform floor, scenario solves/s, correctness (agree / unknown / **disagree = 0**). Each tile has a sparkline of the last 60 days. Links to the pages below. |
| **`results/sat`** — SAT performance | Cactus plot per suite (instances solved vs CPU time, one line per arm, log x). Distribution of `ratio_vs_ref` per category as a box/strip plot on a log scale. Per-fixture table (category, verdict per arm, CPU ms, ratio, encode/sat split), sortable as a plain HTML table. The startup-floor fixtures listed separately. Honest unknowns linked to the soundness page's list of unknown constructs. |
| **`results/randomization`** — randomization | Per benchmark × arm, a table of coverage, JSD (normalised and ×100), excess JSD, χ² p, thin and latency p50/p95/p99. **Small multiples:** for each benchmark, the sorted solution-frequency histogram of every arm over the uniform expectation and the uniform arm's band. This is the picture that makes swizzle collapse (at most 16 hash cells) visible at a glance. A scatter of quality vs cost: x = ms per sample, y = excess JSD, one point per arm per benchmark. The bottom-left is the goal. |
| **`results/trends`** — over time | x = commit date on `main`. (1) The SAT suite score vs the anchor, with the noise band shaded, release tags as labelled vertical lines, and epoch and lock-file changes as markers. (2) The SAT score vs bitwuzla on the same axes. (3) Randomize latency vs the anchor and vs `z3-swizzle`. (4) Excess JSD per benchmark, one line each. (5) Scenario solves/s vs the anchor. Points flagged as regressions are red; `noisy` runs are hollow. A regression list shows the fixture, the commit window and the size of the change. |
| **`results/releases`** — release over release | x = release tag, from the **latest ladder run**: every release re-measured in one job on one machine. This is the fairest comparison and the default view. Bars show the suite score vs the anchor and vs bitwuzla, per release. A table of the "at-release" snapshot numbers next to the "re-measured" numbers, so drift in the reference solvers or the machine is visible. Distribution metrics per release. A link to each release's committed snapshot (`tests/perf/history/releases/`). |
| **`results/methodology`** — methodology | The arms and how each is driven. The swizzle protocol and its fidelity test. Why CPU time is used, and why ratios. Calibration, anchor, epochs, weights, noise band. How to reproduce one number locally (`python3 -m tests.perf.collect --suite … --fixture …`). Where the data lives (§4.2), the schema, and how to consolidate. |

The `trends` and `releases` pages need at least 2 points. Until then they
render a "collecting since <date>" placeholder rather than an empty chart.
The placeholder must still pass the page-count gate.

---

## 8. Code layout

```
tests/perf/
  arms.py          Arm(name, family, kind: oneshot|session|protocol|api, argv, env, version())
                   wraps tests/formal/harness solvers; dv builds addressed by wheel path
  vlt_protocol.py  Verilator 5.046 swizzle driver over a pipe (old plan §3)
  suites.py        manifest resolution: globs → (path, sha, category, weight), manifest hash
  calib.py         calibration kernel, machine fingerprint, loadavg sampling
  run_sat.py       SAT timing: interleaved reps, rusage CPU, z3 model replay
  run_rand.py      sampling + metrics (moved here from tests/formal/sample_quality.py,
                   which then imports it: one implementation)
  run_scenario.py  adapter over tests/bench (reads its per-run JSON)
  collect.py       entry point: --kind, --suites, --builds → one run record
  schema.py        dataclasses, schema_version, JSON Schema export, validation
  history.py       read tests/perf/history/ + list/download unconsolidated artifacts
                   (Forgejo API); one loader for render, consolidate and the skip rules
  consolidate.py   artifacts → trend lines, release snapshots, manifests (§4.5);
                   writes files, never commits
  truth.py         z3 enumeration of a benchmark's space → tests/perf/truth/
  normalize.py     anchor ratios, ref ratios, weighted geomeans, epochs, noise band, flags
  render.py        history → docs/site/results/*.md + *.svg + *.csv
  solvers.lock     pinned tool versions + sha256
  suites/*.json
  history/         committed data (§4.2)
  truth/           committed ground truth (§4.2)
```

`tests/formal/run_compare.py` and `tests/formal/sweep_perf.py` become thin
wrappers over `run_sat.py` or are deleted. Keeping separate runners
guarantees that the published numbers and the local numbers drift apart.

---

## 9. Phasing

| Phase | Deliverable | Done when |
|---|---|---|
| **R0: data plumbing** | `schema.py`, `calib.py`, `history.py`, `consolidate.py`; a dispatch-only `perf.yml` that uploads a calib-only record; confirm artifact retention and whether the docs job's automatic token can list and download artifacts | A calib record goes from the runner to an artifact, then through `consolidate.py` into a committed trend line, and the docs job can see an unconsolidated one |
| **R1: SAT collection** | `arms.py`, `suites.py`, `run_sat.py`, the anchor wheel cache; nightly schedule | Local `collect --suite sat-core` reproduces the 2026-07-21 sweep ratios within noise; 3 nightly records in a row |
| **R2: first views** | `normalize.py`, `render.py`, `docs.yml` wiring; the `index`, `sat` and `methodology` pages | Results pages are live on dvkit.org |
| **R3: swizzle and distribution** | `vlt_protocol.py` and its fidelity test against real Verilator 5.046; `run_rand.py`; the IR-defined `rand-core`; the `randomization` page | `z3-swizzle` reproduces a real Verilator + z3 run on one benchmark within sampling error |
| **R4: time** | The `trends` page; noise band; regression flags; epochs | 14 nightly points with a stable noise band |
| **R5: releases** | Tag trigger; ladder; backfill of `v0.1.0`; the `releases` page | The next tag produces a snapshot without manual steps; the ladder shows ≥ 2 releases |
| **R6: breadth** | `scenario`, `sat-hard`, `rand-pr8042`; the weekly real-Verilator end-to-end arm (old plan §3) | All suites appear on the pages |

R0 comes first because every later phase depends on records getting from
the runner into history reliably. Nothing exercises that path today: no
current job reads artifacts back through the API.

---

## 10. Risks

- **Timing noise on a shared developer host.** This host is also an
  interactive workstation. CPU time does not escape contention: R0's second
  run, taken while the soundness nightly held the host at load 12.7,
  measured the calibration kernels 28% (`z3-ops`) and 15% (`z3-php10`)
  slower than the quiet run, through shared caches, SMT siblings and a lower
  boost clock. A single global correction factor would therefore be wrong. The mitigations are, in order of strength:
  ratios within one job; interleaving; CPU time instead of wall time; the
  dedicated runner label; the 05:00 schedule; `noisy` marking. Instruction
  counts (`perf stat`) would be immune to frequency and load, but
  `perf_event_paranoid` inside the job container is unlikely to allow it.
  `collect` records them if they are available, and they become the primary
  quantity if they turn out to be.
- **The anchor rots.** An old build can stop compiling against newer
  toolchains, and the anchor is built from source because the wheels carry no
  CLI. Shipping `dv-solve-smt2` in the wheels would remove the risk for future
  anchors; until then a failed anchor build fails the run loudly.
- **Swizzle fidelity.** The `z3-swizzle` arm only means something if it
  matches real Verilator. Hence the fidelity test, and the protocol version
  in every record and on every page.
- **Missed consolidation loses data.** Artifacts expire after about 90
  days. The 60-day warning and the 80-day nightly failure (§4.5) make a miss
  loud long before that. Even then, only the per-fixture detail of a missed
  window is lost permanently. Release snapshots are written when the
  release is consolidated, so they are safe as long as consolidation happens
  at each release.
- **Repo growth.** Trend history is about 1.5 MB a year of text that git
  delta-compresses. Release snapshots are about 130 KB each. Ground truth is
  a few KB per benchmark. That is negligible next to the fixtures already
  committed.
- **Unfavourable numbers.** Arrays, signed division, and the hard band where
  bitwuzla wins all show up. They are published with the known gap linked,
  because the page is credible only if they are.

---

## 11. Decisions (agreed 2026-10-02)

Settled 2026-10-02: no separate repo. Run records are artifacts, and trend
history plus release snapshots are committed here by consolidation (§4).

1. **Runner: yes, a dedicated `bench` runner, but not before R1.**
   - What: a second runner instance with the label `bench`, capacity 1, and a
     cpuset of 8 physical cores (16 threads) that the default runner does not
     use.
   - Why not earlier: R0 measures nothing, so it runs on `ubuntu-latest`.
   - Before it exists, `perf.yml` takes the label from a repository variable,
     so the move is not a workflow edit.
   - If the cpuset turns out awkward to set up in the runner config, the
     label alone is still worth having.
2. **Anchor: `v0.1.0`, built from its tag.** It stays the anchor until a release
   exists that runs every arm in `rand-core` (in particular `dv-swizzle`).
   The anchor then moves once, with an anchor link (§5.2). After that, the
   anchor moves only when a release is a year old, so the trend is chained
   at most once a year.
3. **Ladder: K=4, but not on a fixed day.** It runs on Saturdays only if a
   new tag or a lock-file change has appeared since the last ladder.
   Otherwise the Saturday slot is a normal nightly run. With a release
   cadence well under weekly, that costs about 2 h a few times a month
   rather than every week. It can also be started by hand.
4. **Headline references: bitwuzla for SAT and `z3-swizzle` for
   randomization.**
   - bitwuzla is the strongest SMT2 competitor.
   - `z3-swizzle` is what a Verilator user gets today.
   - z3 (SAT) and `bitwuzla-swizzle` are shown in every table and chart but
     not in the headline tiles.
   - The overview tile wording is "vs bitwuzla 0.8.2" and names the version
     from the lock file, so a reference upgrade is visible in the headline
     itself.
5. **Weights as proposed** (`verilator` and `scenario` at 2×, everything else
   at 1×), plus one rule: no single category may carry more than 40% of a
   suite score. As fixture sets grow, a heavily weighted category could
   otherwise come to dominate the score unnoticed. The weights are shown on
   the methodology page.
6. **Consolidation: done by Claude in a session, reviewed and committed like
   any change.**
   - When: at each release, and at the first session in a month where the
     nightly summary says unconsolidated runs exist.
   - Reminder: the nightly perf job's summary carries an "N runs
     unconsolidated, oldest D days" line from day 30, ahead of the 60-day
     warning and the 80-day failure.
   - Fallback: a person can run it with a read-only token if no session
     happens.
