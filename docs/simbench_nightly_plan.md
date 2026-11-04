# Nightly simulator benchmarks (simbench in CI): plan

Status: in progress since 2026-11-04. See the status table at the end.

Goal: every night, measure randomize() cost and quality across simulator +
dv-solve combinations, with every number pinned to the exact simulator version
and dv-solve version that produced it, and publish the trends on the Results
section of dvkit.org.

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
| `vlt-dvs-smt` | Verilator ≥ 5.054 | `dv-solve-smt2` over the SMT-LIB pipe, `+verilator+rand+sampler+solver` | `vlt-dvs-solver` (exists) | ready |
| `vlt-dvs-lib` | Verilator (future release) | libdv_solve in-process (`dvs_handle_t` / `dvs_randomize`) | new `vlt-dvs-lib` | reserved; blocked on Verilator |
| `xezim` | xezim | xezim's built-in solver | `xezim` (built in S5) | ready: 93/100 benches pass (§5) |
| `dvs-standalone` | none (Verilator only captures) | captured session replayed in-process by `bench_vlt_replay` | `dvs-standalone` (exists) | ready |
| `vlt-z3-default` (reference) | Verilator ≥ 5.054 | z3, Verilator's sampler | `vlt-z3-default` (exists) | ready; weekly, not nightly (§3) |

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
`sim-latest` follows xezim-bin's `latest`, not upstream tags, because only
xezim-bin gives us a pinned binary.

Moving a pin is a deliberate commit. It changes the lock hash every record
carries, so the trend shows the break.

**dv-solve versions:**

- nightly: `main` HEAD, built Release, stamped with `dv_version` and
  `git describe`, as perf records already are;
- releases: every tag ≥ `v0.5.0` (0.5.0 is the first where plain
  `dv-solve-smt2` spreads seeded answers with the solver sampler). They are
  built by `tests/perf/builds.py` and measured by a catch-up job, one tag per
  night, like `perf.yml`'s `release` job;
- `xezim` does not use dv-solve. Its records carry `dvs: null`, and its series
  depends on the xezim pin alone.

**Pin moves and the release ladder.** When a simulator pin moves, the
catch-up job re-measures every release tag under the new pin, one per night.
This gives a full (dv-solve release × simulator version) table, not just one
diagonal. "Pending" means no valid record for (tag, variant, simulator
version).

**Tracking the newest simulator (recommended, weekly).** A `sim-latest` kind
resolves the newest Verilator and xezim releases at run time and measures them
against the latest dv-solve release. The resolved URL, version and sha256 go
into the record. The lock file is not touched (CI holds no write credential).
New simulator releases then appear within a week without anyone editing the
lock. The nightly series stays on the pinned versions, so their trends stay
comparable.

## 3. What a night runs, and when

Per variant: Verilator configs take about 15 minutes for 100 benches at N=200
(README §6). `dvs-standalone` adds the capture run and the replay.

| tier | when | variants | approx. time |
|---|---|---|---|
| nightly | daily | pre-flight; `vlt-dvs-smt`, `dvs-standalone`, `xezim` (all 100 benches, N=200, seed 1) | 45–60 min (xezim unknown) |
| reference | Sundays | the nightly variants + `vlt-z3-default` | +15–30 min |
| sim-latest | Saturdays | nightly variants at the newest simulator releases | 45–60 min |
| release catch-up | after the nightly, one tag per night | `vlt-dvs-smt`, `dvs-standalone` at the tag | 30–40 min |

The z3 reference runs weekly because it depends only on the Verilator and z3
pins. Running it nightly would only measure noise.

**Schedule.** Soundness runs 02:30 to about 04:45, then `perf.yml` at 05:00.
`simbench.yml` is scheduled at 05:30 in the same `concurrency: group: perf`, so
it queues behind perf instead of measuring alongside it. Before setting the
times, check the length of a current perf nightly in the Forgejo UI.
`docs.yml`'s 06:30 re-render cron moves to after simbench (about 08:30), or a
second cron is added for it.

**Skip rule.** As in `perf.yml`: skip the nightly when HEAD already has a valid
simbench record under 7 days old *with the same lock hash*. A pin change then
forces a run.

**Runner.** This uses the `bench` runner label (results design §6.3) if it
exists by then. Otherwise `ubuntu-latest`, and noisy runs are marked as such.
Benches run serially, as the README requires: one pytest process, no xdist.

## 4. The CI driver

The README tells a human not to use wrapper scripts. CI still needs one entry
point, but it must not reinterpret the procedure. It runs the README's exact
commands and checks:

`python3 -m dv_solve_simbench.ci --kind nightly|reference|sim-latest|release [--release vX.Y.Z] --variants ... --out sim-out`

1. **Tools.** `tests.perf.tools fetch` for `verilator-sc`, `xezim` and `z3`
   (or, for `sim-latest`, resolve and fetch the newest releases with sha256
   recorded). Check that Verilator's `--help` lists
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

**Trend line** (`tests/perf/history/<year>.jsonl`), one per (run, variant):
`{"h":1,"utc":…,"kind":"sim-nightly","variant":"vlt-dvs-smt","sim":"verilator@5.054","dvs":"0.5.1.dev3+g…","m":"<manifest>","us":[…],"dist":[…],"ent":[…],"stuck":[…],"chi2":[…],"geo_us":…,"ok":97,"unsupported":0,"failed":3,"valid":true,"noisy":false}`.
`us`, `dist` (min distinct), `ent` (min entropy), `stuck` (number of stuck
fields; null when unknown) and `chi2` (worst chi²/dof against the reference)
are arrays over the benches in manifest order, like the existing `vs_*`
arrays. With 100 benches a line is about 5 KB. Release records also go to
`history/releases/sim-<tag>-<sim>.json.gz`.

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
- methodology: hand-written, linking the README and the variant definitions.

## 7. Workflow: `.forgejo/workflows/simbench.yml`

Forgejo-only, like `perf.yml`: it measures and uploads, publishes nothing,
holds no credentials beyond the job token, and needs no release-authority gate
or GitHub twin. `.forgejo/workflows/` already exists, so nothing under
`.github/` runs it.

```
on: schedule 05:30 daily; workflow_dispatch(kind, tag, variants)
concurrency: group: perf, cancel-in-progress: false
jobs:
  sim:      kind from day of week (Sun reference, Sat sim-latest, else nightly)
            seen? → tools → build → driver → identifier grep → upload x2
            → history status → fail on invalid
  release:  needs: sim, if: always(); history release-pending --family sim
            → same steps with --release <tag>
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
| S2 | trialled locally; needs a push to run on the runner | `.forgejo/workflows/simbench.yml`, dispatch-only (manual/nightly/reference, variants, tier), `concurrency: perf`, two artifacts, identifier check over the record and raw logs. 2026-11-04: its steps emulated in `catthehacker/ubuntu:act-22.04` on a clean copy of the tree (smoke tier, kind nightly). Tools fetched and verified, dv-solve built with the image's gcc 11, pre-flight passed (soc_mem_map 510 µs vs 502 reference, same spread), 3 variants 6/6 ok plus vlt-dvs-lib skipped, valid record 2.8 KB gz, raw 324 KB (build images excluded), identifiers clean. A checkout with no `.git` named the commit `unknown`, so the driver now falls back to `GITHUB_SHA`. Next: commit and push (by Matt), then dispatch twice on the runner: kind=manual tier=smoke, then kind=nightly with all 100. |
| S3 | Schedule on; skip rule; `consolidate.py` / `history.py` handle `perf-sim-*` | a week of nightlies consolidated; second run of consolidate is a no-op |
| S4 | `render.py` simulators page; `docs.yml` cron moved | page live on dvkit.org with a week of trend |
| S5 | xezim spike → back end → variant on | xezim smoke tier passes or is classified; full matrix in a nightly |
| S6 | Release catch-up (`v0.5.0`+), `reference` and `sim-latest` kinds | release matrix populated for every tag ≥ 0.5.0 under 5.054 |
| S7 | `vlt-dvs-lib` | when a Verilator release can load libdv_solve: config, lock pin, variant on |

S0–S4 deliver the two ready variants (`vlt-dvs-smt`, `dvs-standalone`). S5 can
run in parallel with S3–S4.

## 9. Open questions

1. **Runner:** does the `bench` label (capacity 1) exist yet? If not, the
   first weeks of data are marked noisy more often.
2. **Nightly scope:** all 100 benches at N=200, or the standard tier
   nightly and all 100 on Sundays? About 15 minutes per variant decides it.
3. **sim-latest:** adopt the weekly newest-release run (§2), or move pins by
   hand only?
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
| S1 | built; dv-solve variants blocked by pre-flight | `bench/simbench/src/dv_solve_simbench/ci.py` + `tests/test_ci.py`. Runs the README's checks (build-info, calib, smoke pre-flight), then one pytest per variant, and writes `simbench-<kind>-<utc>-<sha>.json.gz` + `raw/`. Checked locally: an invalid record is written on a failed check (exit 1). **The pre-flight fails on HEAD with Verilator 5.054:** dvs-standalone `memmap.soc_mem_map` 503 µs vs 39 reference, `scenario.bus_transaction` 63 vs 7 (others at or under reference; calib normal). Under investigation. A valid record with the dv-solve variants waits on that. |
| S2 | drafted | `.forgejo/workflows/simbench.yml`, dispatch-only (manual/nightly/reference, variants, tier), `concurrency: perf`, two artifacts, identifier check over the record and raw logs. Not yet run on the runner. |
| S3 | todo | |
| S4 | todo | |
| S5 | back end done | `simulator: xezim` in runner/config/configs.yaml (`--compile` as the build; `sb_time.so` via `--dpi-lib`, absolute path); `provenance.simulator_build()` (`-V` + bundle manifest + BUILD-INFO, xezim-core sha), recorded as `tools.simulator_build`; `check_banner()` fails a run whose start-up git hash is not `-V`'s. All 100 benches, N=200, in 46 s: 93 ok; 6 `failed` ("no solution found" on satisfiable constraints: arith.countones, arith.redops, arith.shift_width, array.sum_partition (5 of 210 calls), memmap.mem_partition_knapsack, memmap.nonoverlap_regions_8); 1 `wrong` (array.array_reduction: `a.xor()/or()/and()` constraints not met, 210 check errors). Every bench elaborates, so none is `unsupported`. Wrong answers from xezim or z3 are recorded and don't invalidate the run; only dv-solve's do. |
| S6 | todo | |
| S7 | blocked | waiting on Verilator |
