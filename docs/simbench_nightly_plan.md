# Nightly simulator benchmarks (simbench in CI): plan

Status: in progress since 2026-11-04. See the status table at the end.

Goal: every night, measure randomize() cost and quality across simulator +
dv-solve combinations, with every number pinned to the exact simulator version
and dv-solve version that produced it, and publish the trends on the Results
section of dvkit.org.

The data has to answer two questions without anyone dispatching anything:

- **How is xezim improving?** Time, spread and, above all, which benches it
  fails or gets wrong, across xezim releases as they appear (§2, "xezim
  floats").
- **Where can dv-solve improve?** Per-bench time and spread at `main` HEAD,
  ranked against the best other simulator on the same bench (§6, "gap view").

Every run's record is kept for good on the `bench-data` branch (§6a), not just
in an artifact that expires.

Revised 2026-11-05: both simulators float (newest release each night) instead of being pinned; session hashes attribute a step to Verilator or dv-solve; per-bench outcomes
join the trend line; the gap view; the `bench-data` branch and the job that
writes it; no skip rule.

This builds on what already exists. It is not a parallel pipeline:

- `bench/simbench/`: 100 plain-SV benches, the config × bench pytest harness,
  JSONL records, and pre-flight checks (`bench/simbench/README.md`).
- `.forgejo/workflows/perf.yml`: the measure-and-upload-one-artifact pattern,
  the release catch-up job, and the internal-identifier check.
- `tests/perf/{tools,schema,collect,history,consolidate,render}.py`: pinned
  tool tarballs (`tools.lock.json`), the run record, artifact → committed
  history, and the Results pages (`docs/results_publishing_design.md`).

## 1. Variants

| variant id | simulator | solver path | simbench config | status |
|---|---|---|---|---|
| `vlt-dvs-smt` | Verilator, newest verilator-bin release ≥ 5.054 (floats, §2) | `dv-solve-smt2` over the SMT-LIB pipe, `+verilator+rand+sampler+solver` | `vlt-dvs-solver` (exists) | ready |
| `vlt-dvs-lib` | Verilator (future release) | libdv_solve in-process (`dvs_handle_t` / `dvs_randomize`) | new `vlt-dvs-lib` | reserved; blocked on Verilator |
| `xezim` | xezim, newest xezim-bin release (floats, §2) | xezim's built-in solver | `xezim` (built in S5) | ready: 93/100 benches pass (§5) |
| `dvs-standalone` | none (Verilator only captures) | captured session replayed in-process by `bench_vlt_replay` | `dvs-standalone` (exists) | ready |
| `vlt-z3-default` (reference) | Verilator, as `vlt-dvs-smt` | z3, Verilator's sampler | `vlt-z3-default` (exists) | ready; weekly, not nightly (§3) |

The simbench config ids stay as they are. The variant id is the published
name, and a table in the CI driver maps variant ids to config ids. Commercial
simulators are left out: there is no licence on the runner, and their terms
restrict publishing numbers.

`vlt-dvs-lib` is reserved now (id, page column, schema slot) so it appears
without a schema change when Verilator can load the library. Until then the
driver skips it, and the run record says `skipped: "no simulator support"`.

## 2. Pinning: what makes a series

Every result is keyed by **(variant, simulator@version, dv-solve@version)**.
A chart line is one (variant, simulator@version). Its x axis is time, and each
point names its dv-solve build.

**Simulator versions are pinned in `tests/perf/sim_tools.lock.json`**, by URL
and sha256, like the solver pins, but in a file of their own (built in S0).
`tools.lock_sha("sim")` is its own hash, so moving a simulator pin leaves the
solver perf records' hash, and their trend, alone. Each bundle unpacks into
its own directory (`into`). `python3 -m tests.perf.tools fetch --lock sim`
fetches them, and `tools.py provenance <name>` reads the bundle's
`manifest.json` and `BUILD-INFO.txt`.

- `verilator-sc`: `edapack/verilator-bin` v5.054 (published 2026-11-02),
  manylinux_2_28 x86_64, the first release with sampler control (#8899). It
  sits next to the 5.046 pin in `tools.lock.json`, which `vlt_protocol.py`
  fidelity still uses.
- `xezim-bin`: `EDAPack/xezim-bin`, release track, asset
  `xezim-manylinux_2_34_x86_64-<ver>.tar.gz`. This needs glibc 2.34; the runner
  image's 22.04 has 2.35. Each asset has a published `.sha256` file. For
  v0.11.3 it is `9c9a3db6…63b4`, and a local download matched it. The
  tarball holds `bin/xezim`, DPI headers in `include/`, `manifest.json` and
  `share/xezim/BUILD-INFO.txt`.

**What identifies a xezim build** (checked 2026-11-04 on v0.11.3 and
snapshot v0.11.3.40208794201):

| source | gives |
|---|---|
| `xezim -V` | `xezim version 0.11.3`, `git 058f2bba (2026-10-23T17:05:55-07:00)`, `tag 0.11.3` |
| `share/xezim/BUILD-INFO.txt` | the xezim-bin version and track; full xezim sha; **xezim-core sha** (not in `-V`); rustc version; features (`interpreter only`) |
| `manifest.json` | xezim-bin `release.version`, `track` (`release` or `dev`), `built_at`, `recipe_sha`, `inputs[].resolved_sha` |
| the run log | xezim prints `=== xezim 0.11.3 ===` / `git 058f2bba` when it starts, so a run can check it ran the pinned binary |

`-V` alone is not enough. The snapshot built `main` at the same commit as the
0.11.3 tag, so its `-V` output was identical to the release's, but the two
are different packages (different `track` and `recipe_sha`). xezim-core also
moves on its own. The record's simulator identity for xezim is therefore:

```json
"xezim": {"version": "0.11.3", "git": "058f2bba5864…", "core_git": "48820a89e113…",
          "pkg": "xezim-bin@0.11.3", "track": "release", "recipe_sha": "f1fbd823…",
          "rustc": "1.99.1", "features": "default; interpreter only",
          "sha256": "9c9a3db6…", "src": "lock"}
```

A series is keyed by `version` + `git` + `core_git`. Page labels use
`xezim@0.11.3 (058f2bb)`. The driver fails the run when the start-up banner's
git hash does not match `-V`.

**Lag:** upstream tagged 0.11.4 on 2026-10-31, and as of 2026-11-04 xezim-bin
has not built it, even though its release track says it checks daily.
The floating xezim follows xezim-bin's releases, not upstream tags, because
only xezim-bin gives us a checksummed binary. It trails upstream by however
long xezim-bin takes to build, but it does so without anyone acting.

Since 2026-11-05 both simulators float (below), so the lock's pins are the
fallback when resolution fails, the pre-flight's Verilator, and what local
runs use. Moving a pin is still a deliberate commit, and it changes the lock
hash every record carries, but it no longer moves the nightly series.

**dv-solve versions:**

- nightly: `main` HEAD, built Release, stamped with `dv_version` and
  `git describe`, as perf records already are;
- releases: every tag ≥ `v0.5.0` (0.5.0 is the first where plain
  `dv-solve-smt2` spreads seeded answers with the solver sampler). They are
  built by `tests/perf/builds.py` and measured by a catch-up job, one tag per
  night, like `perf.yml`'s `release` job;
- `xezim` does not use dv-solve. Its records carry `dvs: null`, and its series
  depends on the xezim build alone, which floats (below).

**Simulator versions and the release ladder.** When the floating Verilator
resolves to a version not seen before, the catch-up job re-measures every
release tag under it, one per night.
This gives a full (dv-solve release × simulator version) table, not just one
diagonal. "Pending" means no valid record for (tag, variant, simulator
version).

**xezim floats (nightly).** The point of the xezim series is to watch xezim
improve, and a pinned xezim cannot show that: its line stays flat until
someone commits a pin bump. The `xezim` variant has no dv-solve in it
(`dvs: null`), so floating it costs no comparability at all. (Verilator also
floats; how a step is attributed there is below.) Each new xezim version simply starts a
new segment of its series (the series key above already includes the
version). So every night:

- the driver resolves xezim-bin's newest **release-track** asset (GitHub
  releases API, `EDAPack/xezim-bin`; never the `dev` track), checks it against
  the asset's published `.sha256`, and records the URL, version, sha256 and
  `src: "latest"`;
- if resolution or the checksum fails, it falls back to the lock's pin and
  records `src: "lock"`, so a GitHub outage costs a segment boundary, not a
  night;
- the lock keeps a xezim pin for that fallback and for local runs. Bumping it
  is housekeeping, not what moves the series.

This costs nothing extra: xezim runs all 100 benches in under a minute.
Correctness is the headline here: today 6 benches fail ("no solution found")
and 1 is wrong, and the series has to show those turning green (§6,
per-bench outcomes).

**Verilator floats too (decided 2026-11-05).** How Verilator forms its SMT
constraints is part of what we measure: a Verilator release that sends
different (bigger, smaller, differently shaped) problems changes
`vlt-dvs-smt` *and* `dvs-standalone`, whose sessions Verilator captures. We
want to see that when it happens, not a week later. So Verilator resolves the
same way as xezim: the newest `edapack/verilator-bin` release, never below
5.054 (the first with sampler control), checked against its sha256, with the
lock's pin as the fallback and `src` recorded.

Floating both sides means a night can change Verilator and dv-solve at once.
The record separates them:

- **Session hashes.** `dvs-standalone` already captures each bench's solver
  session, and `provenance.session_info()` hashes it, but `ci.bench_row`
  drops the hash. It goes into the bench row and the trend line (`ses`, §6).
  A step in the series with changed session hashes is Verilator's (it sent a
  different problem); a step with the same hashes is dv-solve's. Evidence
  that the session doesn't depend on dv-solve's own answers: the 10-28
  sessions (Verilator 5.053, an older dv-solve) were byte-identical to the
  11-04 captures (5.054, HEAD) for the two benches compared.
  Checked 2026-11-05 (S3): smoke+standard captured with dv-solve and with z3
  as the capture solver on Verilator 5.054. 88 of 96 sessions are
  byte-identical. The 8 that differ feed the solver's earlier answers back
  into later queries, so their session is a function of the solver: randc
  (`not (= a <previous value>)`), `solve…before` (the first phase's pick),
  and dynamic arrays/queues (element asserts sized by the chosen size):
  array.dyn_size, array.queue_basic, array.sorted_increasing,
  array.unique_dyn, control.solve_before, control.solve_before_array,
  dist.randc_basic, dist.randc_enum. Attribution therefore reads `ses` only
  on benches whose hash is stable across dv-solve commits at a fixed
  Verilator. The render (S4) derives that set from the data itself (a bench
  whose `ses` changed while `sim` did not is solver-dependent) instead of
  hard-coding the list.
- **The pre-flight stays on the pinned Verilator.** The pre-flight checks the
  *dv-solve build* against README §5's table, which was measured at 5.054. A
  new Verilator that legitimately changes time or spread would otherwise
  invalidate the run that is supposed to show it. So the pre-flight runs on
  the lock's Verilator, and the measured variants run on the floating one.
  The lock's pin moves only when the reference table is re-measured.
- **The reference moves with it.** `vlt-z3-default` uses the same floating
  Verilator, so the Sunday z3 baseline describes the same problems.

## 3. What a night runs, and when

Per variant: Verilator configs take about 15 minutes for 100 benches at N=200
(README §6). `dvs-standalone` adds the capture run and the replay.

| tier | when | variants | approx. time |
|---|---|---|---|
| nightly | daily | pre-flight; `vlt-dvs-smt`, `dvs-standalone`, `xezim` at its newest release (all 100 benches, N=200, seed 1) | about 14 min (measured: runs 443, 444) |
| reference | Sundays | the nightly variants + `vlt-z3-default` | +15 min |
| release catch-up | after the nightly, one tag per night | `vlt-dvs-smt`, `dvs-standalone` at the tag | 30–40 min |

The z3 reference runs weekly because it depends only on the Verilator version
and the z3 pin, not on dv-solve. Running it nightly would mostly measure
noise. It also runs on the first night the floating Verilator resolves to a
version not seen before (the driver adds `vlt-z3-default` to that night's
nightly), so the reference never describes a different Verilator's problems
for up to a week.

**Schedule.** Soundness runs 02:30 to about 04:45, then `perf.yml` at 05:00.
`simbench.yml` is scheduled at 05:30 in the same `concurrency: group: perf`, so
it queues behind perf instead of measuring alongside it. Before setting the
times, check the length of a current perf nightly in the Forgejo UI.
`docs.yml`'s 06:30 re-render cron moves to after simbench (about 08:30), or a
second cron is added for it.

**No skip rule** (changed 2026-11-05). A night costs about 14 minutes, and
with xezim floating, an unchanged HEAD and lock no longer mean an unchanged
run. Repeat measurements of the same build are also the cheapest noise
estimate the series can have: runs 443 and 444 (same commit) agree within
about 3% on every variant's geomean.

**Runner.** This uses the `bench` runner label (results design §6.3) if it
exists by then. Otherwise `ubuntu-latest`, and noisy runs are marked as such.
Benches run serially, as the README requires: one pytest process, no xdist.

## 4. The CI driver

The README tells a human not to use wrapper scripts. CI still needs one entry
point, but it must not reinterpret the procedure. It runs the README's exact
commands and checks:

`python3 -m dv_solve_simbench.ci --kind nightly|reference|manual|release [--release vX.Y.Z] --variants ... --out sim-out`

1. **Tools.** Resolve and fetch the newest `verilator-sc` and `xezim`
   releases (sha256 checked and recorded; the lock's pins as the fallback),
   plus the lock's `verilator-sc` for the pre-flight, and `z3` (§2). Check that Verilator's `--help` lists
   `+verilator+rand+sampler+`.
2. **dv-solve.** Build Release (`dv-solve-smt2`, `bench_vlt_replay`, `dv_solve`).
   For `--release`, use `tests/perf/builds.py`. Check `--build-info` for
   `optimized=1 asan=0`, the expected commit, and the library path under
   `env -u LD_LIBRARY_PATH`.
3. **Pre-flight** (README §5): `bench_vlt_replay --calib` plus the smoke tier.
   A failed criterion marks the record **invalid**. The driver stops, writes the
   record anyway, and the workflow uploads it and goes red, as `perf.yml` does.
4. **Variants**, one pytest process each:
   `pytest -q -rs -k "<config>-" --simbench-out=...`, with
   `SIMBENCH_BUILD_DIR` and `SIMBENCH_SESSION_DIR` under `sim-out/`.
   `SIMBENCH_HOST` is set to the **machine-class id** (`machine.id` from
   `tests/perf/calib.py`), never the host name.
5. **Wrap** the per-(config, bench) JSONL into one gzipped run record (§6) and
   write the artifact name to `$GITHUB_OUTPUT`.
6. Exit 1 when a check failed or a result was wrong (check errors). Exit 0
   when benches are only unsupported (common on xezim, §5).

The driver lives in `bench/simbench/src/dv_solve_simbench/ci.py`, with a unit
test that runs it over `tests/trivial_bench.sv` with a fake tool directory.

## 5. xezim back end

xezim (`aionhw/xezim`, Rust, Apache-2.0) supports classes, constrained
randomization and DPI-C. simbench has no back end for it yet. A manual run
on 2026-11-04 settled invocation and timing. xezim-bin v0.11.3 ran
`construct.range` through the unmodified harness:

```
gcc -shared -fPIC -O2 -o sb_time.so sb_time.c -I<xezim>/include
xezim --dpi-lib sb_time.so -s simbench_top simbench_pkg.sv <bench.sv> simbench_top.sv \
      +sb_n=200 +sb_dump=range.dump
```

It produced `SIMBENCH_RESULT … "n":200,"failures":0,"check_errors":0`, 17.6
µs/call, and a 200-row dump. On that dump, `metric.field_metrics` gave min
distinct 0.45 and min entropy 0.82. So:

- **Invocation:** interpreted, with no separate compile step. Plusargs
  (`+sb_*`) pass straight through. `-V` prints the version (§2). xezim's own
  `+seed=` defaults to 1. The harness's `+sb_seed` is passed as for the other
  simulators. Confirm that it changes the stream.
- **Timing:** `sb_time.c` through `--dpi-lib`, the same DPI timer as
  Verilator, so per-call numbers are comparable. No fallback is needed.
- **Start-up:** the interpreter parses and elaborates on every run, and the
  timed loop excludes that cost. `first_call_ns` is recorded as for the
  others.
- **Coverage of the 100 benches.** Many will not compile or will hit
  unsupported constraint constructs. The back end must classify each outcome
  as `unsupported` (compile error, or a message about an unsupported
  construct), `failed` (ran, but a check failed), or `ok`. Unsupported is
  shown as coverage, not as a red cell. A wrong answer (check error) still
  makes the record invalid, because it means the constraints are not met.
- **Quality.** `+sb_dump` gives distinct/entropy for xezim exactly as for
  Verilator. It is the only cross-simulator quality comparison we have
  without commercial tools.
- Add `simulator: xezim` to `runner.py` and `configs.yaml`, and a skipped-test
  path when `SIMBENCH_XEZIM` / `xezim` is absent.

## 6. Record, history, views

**Run record.** Reuse schema v1 (`tests/perf/schema.py`) with a new optional
top-level `sim` section, and bump to schema 2 only if validation needs it:

```json
"tools": { "sims": { "verilator-sc": {"version": "5.054", "sha256": "…", "src": "lock"},
                     "xezim": {"version": "0.11.4", "sha256": "…", "src": "lock|latest"},
                     "z3": "4.x" },
           "lock_sha": "…" },
"sim": { "n": 200, "seed": 1, "manifest": "<hash of bench list + bench.sv sha256s>",
         "preflight": { "ok": true, "calib": {…} },
         "variants": { "vlt-dvs-smt": { "config": "vlt-dvs-solver", "sim": "verilator@5.054",
                                         "dvs": {"version": "0.5.1.dev3+g…", "commit": "…"},
                                         "results": [ <simbench JSONL records, trimmed> ] },
                       "vlt-dvs-lib": { "skipped": "no simulator support" }, … } }
```

The artifact is named `simbench-<kind>-<utc>-<sha>`, and the record carries
`"family": "sim"`. The name can't be `perf-sim-…`: `schema.ARTIFACT_RE` reads
the kind as `[a-z]+` right after `perf-`, so the name would not parse. Its kinds
(`nightly`, `manual`, `reference`, `latest`, `release`) are also not all
`schema.KINDS`. So S3 teaches `consolidate.py` and `history.py` a second
artifact family, rather than stretching the perf schema. Full JSONL, value
dumps, captured sessions and pytest logs go into a second, non-consolidated
artifact (`simbench-raw-…`) for diagnosis. It expires after 90 days.

**Random spread is a first-class result, not a footnote.** Every
(variant, bench) record keeps the existing quality fields:

- **From the simulator's own answers** (`+sb_dump`, `metric.field_metrics`):
  per-field distinct ratio and entropy normalized by log N, plus `min_distinct`
  and `min_entropy` over the fields. This covers `vlt-dvs-smt`, `xezim` and
  `vlt-z3-default`. The driver always sets `SIMBENCH_DUMP_DIR`, so no run
  skips the dump.
- **For `dvs-standalone`,** the timed answers are the replay's, so its spread
  comes from `bench_vlt_replay --quality` (`randomization.replay`). That adds
  checked and invalid models, and `forced` vs `stuck` fields: a stuck field is
  a sampler that never moves a variable that could move. The capture's spread
  (`source: "capture"`) is not used as this variant's spread.
- **Against a reference:** `metric.chi2` per field, against `vlt-z3-default`
  on the Sundays it runs. On other nights the most recent reference dumps from
  the raw artifact are used, if they are under 7 days old and have the same
  manifest.
- Dumps go into the raw artifact (`simbench-raw-…`). Only the metrics are
  consolidated.

**Trend line** (`simbench/history/<year>.jsonl` on `bench-data`, §6a), one
per (run, variant):
`{"h":1,"utc":…,"kind":"sim-nightly","variant":"vlt-dvs-smt","sim":"verilator@5.054","dvs":"0.5.1.dev3+g…","m":"<manifest>","us":[…],"out":"oofo…","ses":[…],"dist":[…],"ent":[…],"stuck":[…],"chi2":[…],"geo_us":…,"ok":97,"unsupported":0,"failed":3,"wrong":0,"valid":true,"noisy":false}`.
`us`, `out`, `dist` (min distinct), `ent` (min entropy), `stuck` (number of
stuck fields; null when unknown) and `chi2` (worst chi²/dof against the
reference) are arrays over the benches in manifest order, like the existing
`vs_*` arrays. `ses` is the first 12 hex digits of each bench's captured session sha256
(from `dvs-standalone`; null for the other variants), which attributes a step
to Verilator or dv-solve (§2). **`out` is the per-bench outcome**, a string of one letter per bench (`o` ok,
`w` wrong, `f` failed, `u` unsupported, `e` error, `-` not run). The totals
alone would show "93 ok" on two nights even when one bench was fixed and
another broke; `out` shows which, and that is the xezim story. `us` is null
for a bench that wasn't `ok`. With 100 benches a line is about 5 KB.

**Geomeans are over a common set.** `geo_us` is over the variant's own ok
benches, as now. Every comparison across variants (the headline table, the
gap view) recomputes over the benches *every compared variant* got right on
that night. As xezim fixes benches, the common set grows, and the page says
how many benches each ratio covers. (On 2026-11-05 the common set was 93:
standalone 14.5 µs, xezim 48, vlt-dvs-smt 128.)

**Views** (`tests/perf/render.py`, new `docs/site/results/simulators` page):

- headline table: variant × latest µs/call (geometric mean over the benches
  every variant ran), bench coverage, and spread (median and worst
  `min_entropy`, number of benches with a stuck field);
- spread chart per variant: `min_entropy` per bench, coloured by group, next to
  the reference. A drop in spread marks the run, as a slowdown does;
- trend chart per variant, one line per simulator version, with release tags
  marked;
- release matrix: dv-solve release × simulator version, per variant;
- per-bench table, collapsed by group, with the overhead
  `vlt-dvs-smt / dvs-standalone` (the cost of Verilator and the pipe, which
  `vlt-dvs-lib` is meant to remove);
- **gap view: where dv-solve can improve.** Benches ranked by
  `dvs-standalone / best(xezim, vlt-z3-default)` µs/call, and separately by
  spread shortfall (`min_entropy` under the reference's by more than 0.05, or
  any stuck field). Each row links the bench source and the backlog entry, if
  any. On 2026-11-05 the time ranking led with `arith.mul_div_64` (xezim 25×
  faster), `memmap.memmap_tight_32`, `memmap.addr_map_64`, the `soc_*` maps
  and `scenario.bus_transaction`; the spread ranking with
  `memmap.soc_mem_map` (B130). The ranking is also kept as a trend: the number
  of benches where dv-solve loses, night by night;
- **xezim view: bench outcomes over time.** A bench × night grid of `out` for
  `xezim`, with version boundaries marked, so a fix (or a regression) shows
  as a colour change in a column;
- methodology: hand-written, linking the README and the variant definitions.

## 6a. Retention: the `bench-data` branch

Artifacts expire, and perf's `consolidate.py` is run by hand ("it never
commits"). Neither keeps a series going on its own. So simbench commits what
it collects, automatically, to a data branch.

**What is kept.** Per run, the record (`simbench-<kind>-<utc>-<sha>.json.gz`,
about 15 KB) and its trend lines. That is about 6 MB a year with the weekly
reference. The raw artifact (1.1 MB a night: JSONL, value dumps, sessions,
logs, about 400 MB a year) stays an artifact and expires after 90 days. The
record already holds every metric the views use. Invalid and noisy runs are
kept too, flagged, because they are evidence; the views skip them.

**Layout** (orphan branch `bench-data`, nothing else on it):

```
simbench/records/<yyyy>/<artifact>.json.gz   every run's record, as uploaded
simbench/history/<yyyy>.jsonl                trend lines (§6), sorted, deduplicated
simbench/manifests/<hash>.json               bench list + bench.sv sha256s, by manifest hash
simbench/sims.json                           variant -> simulator identity -> first/last utc, runs
                                             (the Kind step reads it: has this Verilator a z3 reference?)
README.md                                    what this branch is; written once
```

**Why a branch, and not `main`:**

- A nightly commit to `main` would trigger `ci.yml`, `test.yml` and `docs.yml`
  every night, on both forges, and bury code history in data commits.
- An orphan branch has no `.forgejo/workflows/` and no `.github/workflows/`,
  so a push to it runs nothing on either forge. That includes Forgejo's
  fallback to `.github/workflows/` (dvkit-infra rule 1). Pushing it releases
  nothing (rule 2).
- git-sync mirrors it to GitHub within 5 minutes. That's fine: the data is
  public once consolidated anyway, and the identifier check (below) runs
  before every commit.

**The writer: a `record` job in `simbench.yml`.** It runs `needs: simbench`
with `if: always()`, so an invalid night is kept too. It is a separate job so
the push credential never shares a job with the bench and build code. Steps:

1. Fetch `bench-data` into a worktree, or create it as an orphan the first
   time.
2. `python3 -m tests.perf.simstore add --root <worktree>/simbench`.
   It lists every unexpired `simbench-<nightly|reference|release>-*` artifact
   (Forgejo API, through `history.Forgejo`), adds any record not yet on the
   branch, and rebuilds the trend lines, manifests and `sims.json` from
   `records/`. It is idempotent: a second run changes nothing, and a night
   whose push failed is picked up by the next night's job, as long as it's
   within the 90 days.
3. Run the identifier grep (same inline pattern) over the diff.
4. Commit as `simbench <simbench@noreply.invalid>`: `simbench: N run(s)`,
   with the artifact names in the body.
5. Push fast-forward only. If the push is rejected, refetch and redo steps
   1–4 once, then fail. **Never force** (rule 5). There is one writer per
   night anyway, since the `perf` concurrency group serializes simbench runs.

Manual runs (`kind=manual`) are not recorded. They are for trying things;
dispatch with `kind=nightly` to make a run count.

**Credential.** Forgejo's job token (`secrets.GITHUB_TOKEN`), which
`actions/checkout` leaves configured for `git push`. The `record` job asks for
`contents: write`; every other job keeps the default. Forgejo's docs say only
`pull_request` runs get a token without write access, so a scheduled or
dispatched run's token should push. Nothing in `/srv/forgejo/bin` pushes with
the job token yet, though, so the first `record` run is the proof. If it doesn't, the fallback is a repository secret holding a
token for a bot account, scoped to this repo, with write access. It is used
only by `record`. No write credential is ever exposed to `pull_request` runs:
`simbench.yml` has no `pull_request` trigger.

**Readers.** `render.py` (S4) and `docs.yml` do
`git fetch origin bench-data` and read `simbench/` from it. Locally:
`git worktree add ../bench-data bench-data`.

**Perf history** stays on `main` (`tests/perf/history/`) and keeps its manual
`consolidate.py` for now. Moving it onto `bench-data` with the same `record`
job is an obvious follow-up (§9).

## 7. Workflow: `.forgejo/workflows/simbench.yml`

Forgejo-only, like `perf.yml`: it measures and uploads, publishes nothing to
the site, and needs no release-authority gate or GitHub twin.
`.forgejo/workflows/` already exists, so nothing under `.github/` runs it. Its
only write is the `record` job's push to `bench-data` (§6a), with the job
token.

```
on: schedule 05:30 daily; workflow_dispatch(kind, tag, variants, tier)
concurrency: group: perf, cancel-in-progress: false
jobs:
  simbench: tools (pins + newest releases) → kind (simstore kind: Sunday, or a
            Verilator with no z3 reference yet → reference) → build → driver
            → identifier grep → upload x2 → fail on invalid
  record:   needs: simbench, if: always() && kind != manual
            permissions: contents: write
            simstore add onto bench-data → grep → commit → push (ff only, retry once)
  release:  needs: record, if: always(); history release-pending --family sim
            → same steps with --release <tag> (S6)
```

The identifier-grep step uses the same pattern as `perf.yml`, inline. It cannot
move into a shared script: `ci.yml` greps every tracked file except
`.forgejo/workflows/` for that pattern, so a script holding it would fail the
check. Runner
constraints that apply (dvkit-infra): Python 3.10, `python3` only, no
third-party actions, build a wheel rather than `pip install -e`. For simbench,
use `PYTHONPATH=src`, which the README allows. Verilator needs C++20 from the
image's g++ 11. Trial-run the steps in `catthehacker/ubuntu:act-22.04` before
landing them.

## 8. Phases

| # | step | exit criterion |
|---|---|---|
| S0 | Lock `verilator-bin` 5.054 (`verilator-sc`) and xezim; `tools.py fetch --suites simbench` | fetch + sha256 OK in the act-22.04 container; `verilator --help` shows the sampler option |
| S1 | done 2026-11-04 | `bench/simbench/src/dv_solve_simbench/ci.py` + `tests/test_ci.py` (40 simbench unit tests pass). Local smoke run of `--kind nightly`: valid 11 KB record (vlt-dvs-smt, dvs-standalone, xezim 6/6 ok; vlt-dvs-lib `skipped`), and a failed check writes an invalid one (exit 1). The first runs failed the pre-flight, and that was the reference's fault: the 10-28 table was measured before ce37488, when a reused bitblast context returned its last answer (all 12 soc_mem_map fields and 3 of 4 bus_transaction fields stuck). README §5 was re-measured at 17fb20a with a spread column, and the pre-flight now also fails on a stuck field, an invalid model, or min entropy more than 0.05 under the reference. Evidence: `docs/perf/simbench_preflight_spread_2026-11-04.md`. The remaining soc_mem_map slowness and clumping is B130 in `docs/solver_bug_backlog.md`. |
| S2 | one valid run on the runner; quiet-host wait added | Pushed 2026-11-04 (1dfa5e8). Two dispatches: run 438 (kind=manual, smoke) was **invalid**: calib 1.31 vs 0.72 µs, because it ran beside push-triggered `test.yml`/`docs.yml` (load 18). The `perf` concurrency group keeps out only `perf.yml`. Run 439 (kind=nightly, all 100) was **valid** (noisy, load 18 → 9): dvs-standalone 100/100 ok, geomean 16.9 µs/call; vlt-dvs-smt 100/100, 172; xezim 93 ok / 6 failed / 1 wrong (as locally), 53.6; record 15.5 KB, raw 1.1 MB. Then `ci.py` gained `wait_quiet()`, before the pre-flight and before each variant: 1-minute load under 2 (once it has been over, held 60 s), up to 30 min, then measure anyway; each wait goes into `sim.quiet_waits`; `--quiet-load`, `--quiet-timeout`. Then runs 443 (kind=manual) and 444 (kind=nightly) at 9760e23, both **valid**, all 100 benches: dvs-standalone 100/100, 15.8 µs geomean; vlt-dvs-smt 100/100, 153 and 148; xezim 93/6/1 again, 48.2 and 47.6. Same-commit agreement about 3%. **Done 2026-11-05.** (The quiet wait, 385c27a, came after these; its first real test is the first scheduled night.) |
| S3 | Automated collection and retention: (a) job-token push (proved by the first `record` run); (b) xezim floats (resolve newest release, sha256, lock fallback); (c) `out` per-bench outcomes in the record/trend line; (d) `tests/perf/simstore.py` → `bench-data`; (e) `record` job; (f) Verilator floats, pre-flight on the pinned one, `ses` session hashes in the bench row and trend line; (g) 05:30 schedule (Sunday reference); (h) seed `bench-data` with runs 439, 443, 444 (artifacts still live) | a week of scheduled nights on `bench-data` with no one dispatching; the consolidate step a no-op when rerun; one Sunday reference recorded |
| S4 | `render.py` simulators page from `bench-data` (headline, gap view, xezim outcome grid, trends); `docs.yml` cron moved | page live on dvkit.org with a week of trend |
| S5 | xezim spike → back end → variant on | xezim smoke tier passes or is classified; full matrix in a nightly |
| S6 | Release catch-up (`v0.5.0`+), re-run per new Verilator version | release matrix populated for every tag ≥ 0.5.0 under each Verilator version seen |
| S7 | `vlt-dvs-lib` | when a Verilator release can load libdv_solve: config, lock pin, variant on |

S0–S4 deliver the two ready variants (`vlt-dvs-smt`, `dvs-standalone`). S5 can
run in parallel with S3–S4.

## 9. Open questions

1. **Runner:** does the `bench` label (capacity 1) exist yet? If not, the
   first weeks of data are marked noisy more often.
2. **Nightly scope:** all 100 benches at N=200, or the standard tier
   nightly and all 100 on Sundays? About 15 minutes per variant decides it.
3. ~~sim-latest~~: settled 2026-11-05: both simulators float nightly; no
   `sim-latest` kind (§2).
7. **Job-token push:** can Forgejo 16.0.1's job token push to `bench-data`
   with `permissions: contents: write`? Trial in S3(a); the fallback is a bot
   token in a repository secret (§6a).
8. **Perf history onto `bench-data`:** move `tests/perf/history/` and give
   `perf.yml` the same `record` job, ending the manual consolidate? Not part
   of this plan; decide after S3 has run for a week.
4. ~~xezim source~~: settled 2026-11-04: `EDAPack/xezim-bin` release
   track, pinned by sha256 (§2).
6. ~~Bench count~~: settled 2026-11-04: the existing 100 benches at N=200
   calls each.
5. **vlt-dvs-lib:** which Verilator mechanism will it use (a build-time
   library hook, or a runtime plugin)? This decides whether the config is a
   `solver:` command or new build args.

## Status

| step | state | note |
|---|---|---|
| S0 | done 2026-11-04 | `tests/perf/sim_tools.lock.json` (verilator-sc 5.054, xezim 0.11.3), `tools.py fetch/check --lock sim`, `tools.py provenance <name>`; perf lock hash unchanged (8b1e806d5455). In act-22.04: both fetch and verify; Verilator 5.054 + g++ 11 + z3 5.1.0 runs construct.range (sampler default); xezim runs it via `--dpi-lib` (needs an absolute path: dlopen does not search the cwd). `tests/unit/test_perf_tools.py`. |
| S1 | done 2026-11-04 | `ci.py` + `tests/test_ci.py`; the pre-flight failure was a stale reference (see the S1 row in §8). |
| S2 | done 2026-11-05 | runs 439, 443, 444 valid on the runner; quiet-host wait in 385c27a. |
| S3 | next | revised 2026-11-05 (§2 xezim floats, §6 `out` and gap view, §6a `bench-data`). Start with the job-token trial push. |
| S4 | todo | |
| S5 | back end done | `simulator: xezim` in runner/config/configs.yaml (`--compile` as the build; `sb_time.so` via `--dpi-lib`, absolute path); `provenance.simulator_build()` (`-V` + bundle manifest + BUILD-INFO, xezim-core sha), recorded as `tools.simulator_build`; `check_banner()` fails a run whose start-up git hash is not `-V`'s. All 100 benches, N=200, in 46 s: 93 ok; 6 `failed` ("no solution found" on satisfiable constraints: arith.countones, arith.redops, arith.shift_width, array.sum_partition (5 of 210 calls), memmap.mem_partition_knapsack, memmap.nonoverlap_regions_8); 1 `wrong` (array.array_reduction: `a.xor()/or()/and()` constraints not met, 210 check errors). Every bench elaborates, so none is `unsupported`. Wrong answers from xezim or z3 are recorded and don't invalidate the run; only dv-solve's do. |
| S6 | todo | no `sim-latest` kind: both simulators float nightly. |
| S7 | blocked | waiting on Verilator |
