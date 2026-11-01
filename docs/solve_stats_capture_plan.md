# Plan: solve statistics and problem capture

Started 2026-10-26. The aim is to find out what real randomization problems look
like and where real users' solver time goes, and to turn that into
**representative testcases** for performance work. Today the perf corpora
(svcorpus, rand-core, the xezim set, Verilator transcripts) are problems we
chose. This plan replaces that guess with measured data.

Every part is local-first. Nothing is sent anywhere by the library. A user
produces a bundle, reviews it and decides whether to send it.

Revised 2026-11-01. **Goal: get as much useful input as possible from users
who may not send any recognizable testcase.** That is the usual case:
- most verification teams are prohibited from sending design content;
- a constraint problem's structure alone can be recognizable (a protocol, a
  register map, a known block).

So the plan is built for the user who can send **only** statistics. Captures
are a bonus from the rare user who may send more.

Three principles follow:

1. **Move the analysis to the data.** Anything that needs the problem itself
   runs on the user's machine, inside dv-solve:
   - measurement (S0);
   - shadow-solve experiments (S7);
   - reduction to a hard core and matching a synthetic stand-in (S8).

   Only the conclusions leave, as numbers.
2. **The user can check everything that leaves.** Every bundle is plain, closed
   schema JSON that the user can read, `explain` and `audit` before sending (S2,
   "Inspectable by design"). Nothing is sent that the user can't understand.
3. **Every finding must be actionable from statistics alone.** A statistics
   bundle must be able to tell us:
   - where the time goes;
   - why a solve was slow;
   - whether we chose the right engine;
   - whether the random values were good;
   - a **synthetic test case of our own** that behaves like the user's problem
     (S8).

   The last item is what makes statistics-only input reach the perf corpus.

### Sharing levels

| Level | What leaves the site | Recognizable? | Expected from |
|---|---|---|---|
| L0. Statistics | Tiers A, B, B+ and the motif histogram: counts, log buckets, salted hashes, experiment results | No: no names, no constants, no structure beyond bucketed counts | Every participating user (the default) |
| L1. Synthetic matches | For each class worth covering: our generator's version, parameters and seed, which regenerate a problem **made by our code** that behaves like the user's (S8) | No: the problem is generated, and the user can regenerate and read it | Every participating user (on by default with L0) |
| L2. Skeleton | The operator graph, or only its reduced hard core, with constants replaced by relations (S3, C1) | Possibly, through structure | Friendly users, per class, after review |
| L3. Capture | The problem buffer, with constants handled by policy (S3, C2) | Yes | Rare; our own runs and very friendly users |

L0 and L1 are the main channel, and the design is judged on them. L2 and L3
add fidelity where they're allowed.

## Status

| # | Step | Status | Result |
|---|---|---|---|
| S0 | Stats core: counters, reason enum, hooks on every path | in progress | S0a–S0c: hooks on every path, exact counts, answer path, routing reasons, per-class records keyed by the S1 hash; ~50 ns on, none off. S0d: value quality (signals 4 and 7) and the distinct-model count, sampled; within noise on svcorpus. S0e: signals 3, 5 and 6 (root constrainedness, binding constraints, time outside the solves and repeats); within noise. S0a–d committed (817c946) |
| S1 | Canonical shape hash and feature vector | done (rand-core sweep left; committed 5b63cdc) | Class hash: invariant and mutation-tested over svcorpus, no collisions between cases. Feature vector, 22 motifs, 0.2% `other` on svcorpus |
| S2 | Local stats file and end-of-run report | in progress | S2a done (committed 38849ba): exit-time stats file, `python -m dv_solve.stats` report/merge/explain/bundle/audit; svcorpus stats pass the audit against all corpus names |
| S3 | Versioned capture format, anonymizer, reproduction check | open | |
| S4 | Corpus ingestion: replay a capture as a perf and distribution case | open | |
| S5 | Representativeness: the corpus gap report and shape-faithful synthesis | open | |
| S6 | Dogfood on our own runs before any external user | open | |
| S7 | Experiments on the user's machine: shadow solves under alternative settings | open | |
| S8 | Local hard-core reduction and synthetic matching | open | |

Update this table and each step's "Log" line as each step finishes.

## How it becomes testcases

The chain the data has to support:

```
                 on the user's machine                 │  leaves the site
                                                       │
solve ──► per-shape record (S0, S1) ──► stats file ────┼──► L0 statistics
   │                                       (S2)        │
   ├──► shadow solves (S7) ──► experiment results ─────┼──► L0 (B+)
   │                                                   │
   └──► costly class ──► hard core ──► synthetic match ┼──► L1 generator params
             (S8)          (S8)            (S8)        │
                             └──► skeleton / capture ──┼──► L2 / L3 (if allowed)
                                                       │
           on our side:  L1 regenerated, L2 regenerated, L3 loaded
                         ──► corpus case: problem + reference + weight (S4)
                         ──► gap report: field time by class vs coverage (S5)
                         ──► "write or synthesize these next" ──► perf history
```

The key unit is the **shape class**. It is a canonical structural hash that
ignores names and constant values. Two calls to `pkt::randomize()` with
different inline `with {}` constants are one class. Each user's stats file
reports, per class, how many solves it had and **how much solver time** it took.
Summed over users, that is the field's time distribution. The corpus is
representative when it covers the classes that hold most of that time.

## What is collected

| Tier | Content | Anonymity | Default |
|---|---|---|---|
| A. Run counters | Solves, compile count, time histogram, engine used, fallback and bail reasons, unknown and timeout counts, cache hit rates, `validate_model` failures | No problem content | Collected in memory; written only when `DV_STATS` is set |
| B. Shape records | Per shape class: feature vector, solve count, time and work histograms, engine mix, reasons, the signals listed below, distinct instance count, distinct solution ratio | One-way hash plus counts; structure only in aggregate | In the stats file |
| B+. Experiment results (S7) | Per shape class: work ratio and verdict agreement of shadow solves under alternative settings | Numbers only | Only on request (`DV_STATS_EXPERIMENT`) |
| C1. Skeletons | Operator graph with widths; constants replaced by their relations (S3) | Structure can hint at a protocol | Only on request, per class, reviewed before sending |
| C2. Captures | The problem buffer for selected classes, with constants handled by policy | Structure and constants can identify a design | Only on request (`DV_STATS_CAPTURE`), per class, reviewed before sending |

Tiers A, B and B+ make up sharing level L0. C1 is L2 and C2 is L3. L1, the
synthetic matches, is described in S8.

### Signals in a shape record

Each signal is listed with the decision it drives. A signal with no decision is
left out.

| # | Signal | Drives |
|---|---|---|
| 1 | **Work counts:** decisions, propagations, conflicts, learnt clause length (histogram), restarts, SAT solver ticks, variables bit-blasted. Unlike time, these are comparable across hosts. They are also the unit that routing budgets use (`docs/deterministic_routing_plan.md`) | Routing and escalation thresholds |
| 2 | **Path taken:** which fast path answered (cache, R1 resample, affine/closed form, enumeration, box probe on or off), engine route and reason, every bail and fallback, which rewrites fired (mask rewrite, field decomposition, ite-self). **Wasted work:** units spent by an engine that bailed before another engine answered | Routing loss with no visible failure; which fast paths to build next |
| 3 | **Constrainedness:** log2 of the domain product before and after the root propagation, the share of variables fixed at the root, an estimated log2 solution count | Features for engine choice |
| 4 | **Used range vs declared width:** per variable (by canonical position), the bit width the observed values actually span | Automatic width narrowing |
| 5 | **Binding constraints:** per constraint (by canonical position), how often it propagated and how often it appeared in a conflict; constraints that presolve removed or that are always satisfied | Which propagators to add; wasted compile work |
| 6 | **Behaviour across calls:** repeat count per class; what changed since the last call of the same class (constants only, inline `with`, `rand_mode`/`constraint_mode`); time split between parse, compile, solve and reading the values back | Plan caching; cost of the Verilator pipe protocol |
| 7 | **Value quality:** per-bit one-counts per variable; a stuck flag (domain > 1 but the value never changed over N solves); effective `dist` weights; soft constraint drop rate | Uniformity bugs, which become new distribution cases |
| 8 | **Tail latency:** time and work as mergeable log-bucket histograms, from which p50 and p99 are read | Heavy tail → restarts or portfolio; high median → encoding |
| 9 | **Motif histogram:** counts of small constraint patterns from a fixed list (S1), weighted by solver time, for example `x inside [a..b]` with `x % c == 0`, a masked compare, `if`-guarded ranges, a sum over an array, chained `<` | Which constructs to give dedicated propagators or rewrites; what the synthetic generator must be able to produce |

"Canonical position" is the index given by the S1 walk. It holds no name, and it
is stable across runs of the same class.

Bucket every count and size on a log2 scale in the stats file. Exact values can
fingerprint a design; the buckets are enough to tune with.

**Never collected:** variable, field or class names, file paths, argv, env
values, host or user names, raw SMT2 text. Source locations appear **only** in
the local report (S2), never in the stats file or a bundle.

The library never opens a network connection. Many semiconductor sites run
simulation with no outside network, and a tool that phones home gets banned.

## S0: stats core

**Goal:** count every solve on every path (C API, Python builder, DPI, SMT2,
Verilator mode, pyvsc) for a cost under 50 ns per solve when stats are on, and
one pointer test when off.

- Add `src/c/dvs_stats.{h,c}`. Its hooks go beside the oracle hooks
  (`src/c/dvs_oracle_hooks.h`; call sites in `dvs_search.c` around 2296, 2558,
  2650 and 2828, `dvs_ctx.c:435`, `dvs_compile.c:4231`).
  - Per-context counters are updated during the solve. At the end of the solve
    they merge into a process-global shape table under a `dvs_mutex_t` (the
    pattern in `dvs_search.c:1574`, `:2337`). This is needed because cube and
    portfolio run worker threads inside one solve.
- Time every solve with the monotonic clock. Today the C API times solves
  only when an oracle is attached (`dvs_search.c:2295-2301`). (Changed
  2026-11-01. A thread CPU clock read costs about 77 ns on Linux and a
  monotonic one about 16 ns, so two CPU-clock reads alone would break the
  50 ns budget. The work counts are the measure that compares across hosts.)
- **Reason enum.** Replace the free strings with one `dvs_reason_t`:
  - `fe->nb_why` ("wide-var", "sext64", "sdiv-srem", …),
  - `fe->qfbv_why`,
  - `fe->vlt_route` ("hash-skip", "bitblast/array", "cdcl-declined", …),
  - the bail reasons (`dvs_solver_bail_reason_str`, `dvs_search.c:1107`),
  - the count of uncompiled constraints from `dvs_solver_compile`.

  Keep the strings for `DV_LOG`, generated from the enum.
- SMT2: wrap `_cmd_check_sat` for every mode, not just Verilator
  (`smt2_frontend.c:6097`). The record comes from the internal problem buffer,
  never from the transcript.
- **Accuracy signal:** keep a per-shape HyperLogLog of solution hashes. The
  distinct-solution ratio flags shapes whose sampling has collapsed (for
  example `cond_weight` at 0.53). This is cheap, holds no content and doubles
  as a uniformity alarm.
- **Signals 1, 2, 6 and 8** (see "Signals in a shape record") are counters on
  the solve path.
  - Gather the existing counters into one per-context `dvs_stats_t` (for
    example `n_propagations`/`n_conflicts` in `dvs_lcg.h:122` and the SAT
    backend's tick count).
  - Wasted work is the work count of each attempt that ended in a bail,
    recorded with the reason.
  - For signal 6, compare the S1 class hash and the instance hash with those of
    the previous solve on the same handle or object.
- **Signals 3, 4, 5 and 7** need a little more work:
  - Signal 3 is taken once per compile, at the root propagation.
  - Signals 4 and 7 come from the model at the end of each solve: OR and AND of
    the values, and per-bit one-counts. Cap them at 64 tracked variables per
    class, the widest first.
  - Signal 5 comes from per-constraint propagate and conflict counts, which
    are kept only when `DV_STATS` is set.

**Tests:** counter totals equal the known call counts on fixed scripts, through
each path. Hammering with threads gives exact totals. A timing check shows the
off cost is in the noise and the on cost is under 50 ns per solve with the
C-API arm (enhancement plan item 0). The cost of signals 4, 5 and 7 is reported
on its own. If it is over 5% on the svcorpus loop, they are sampled (one solve
in N) rather than dropped.

**Log:**

- 2026-11-01, **S0a** (uncommitted): run counters (tier A) and hooks on every
  path.
  - `src/c/dvs_stats.{h,c}`. The public API in `dv_solve.h` is
    `dvs_stats_enable`, `dvs_stats_enabled`, `dvs_stats_reset` and
    `dvs_stats_snapshot_json`. `DV_STATS` is read on first use.
  - **Exactly once.** Each path's outermost entry opens a per-thread record:
    `dvs_solver_solve` (api), `dvs_randomize` (handle), `dvs_dpi_solve_h`
    (dpi), `_check_sat_checked` (smt2 or verilator), and
    `dvs_bbsolver_check`/`sample`/`check_maxsat` (bvsat). A nested entry
    finds the record open and only adds to it. So `solve_n(N)` is N solves,
    a `randomize` is 1, and an SMT2 CDCL-then-bitblast escalation is 1.
  - **Engine scopes.** CDCL, bitblast (every public `dvs_bbsolver_*` solve
    entry) and cube each open a scope, and only the outermost scope on a
    thread counts. The engine whose scope opened last is credited with the
    answer, and the others' work counts as wasted. Work is measured as:
    - CDCL: `dvs_ctx_work` units, via a new `ctx->conflicts_ever`, because
      `conflict_count` is cleared on reset;
    - bitblast and cube: SAT backend ticks, read around `dvs_sat_solve`.

    The units differ, so the work histograms are kept per engine.
  - **SMT2 commands other than check-sat** (get-unsat-core replays) are
    counted as `smt2-aux` work, not as solves, and only when an engine ran.
  - Counters: solves by path × result, compiles, `engine_ran`,
    `answered_by` (including `none`), `fallbacks`, `work` and `wasted_work`
    per engine, CDCL bail reasons, and log2 histograms of wall ns and work.
  - Test: `tests/c/test_stats.c` (ctest `test_stats`). It checks:
    - exact counts on the api, solve_n, handle (CDCL and bitblast routes),
      bvsat, SMT2, Verilator mode and get-unsat-core paths;
    - 4 threads × 150 solves give an exact total;
    - nothing is counted when off;
    - `DV_STATS` is read from the environment.

    ctest 34/34 passes with and without `DV_STATS=1`.
  - **Cost** (x<y over 8 bits, reset+solve loop, best of 12): about 40 ns
    per solve with stats on (221 → 261 ns). With stats off it matches a HEAD
    build (220–227 ns for both).
  - **Not yet:**
    - SAT work done on cube and portfolio worker threads is not counted
      (thread-local record);
    - DPI is hooked but not tested in `test_stats`;
    - `dvs_solver_optimize` and the LNS and contradiction loops count each
      inner solve as an api solve.
- 2026-11-01, **S0b** (uncommitted): answer path and routing reasons
  (signal 2).
  - **`via`**, a closed list of 26 entries (`dvs_stats_via_t`), records what
    produced the answer:
    - CDCL: root, search, enum, box;
    - bitblast: SAT, each of the six samplers, affine, the four simulation
      outcomes, the resample `sim-once` and `sat-fresh`, rediversify;
    - cube;
    - SMT2: hash-skip, model-reuse, tainted, cache-unsat, compile-unsat.

    An engine scope notes its default when it opens, and a deciding fast path
    overrides it. Every solve has exactly one `via`.
  - **`reasons`**, a closed list of 24 entries (`dvs_stats_reason_t`):
    - why the problem needs bitblast (`nb_why`);
    - the shape rule (`multiplier`, `bit-level`, `seeded sample`);
    - logic, array, `DV_ENGINE`;
    - the Verilator routes (probe, sticky, enumeration, opt-out, cdcl-slow,
      cdcl-declined);
    - escalations (compile-failed, pool-overflow, cdcl-unknown, cdcl-error).

    The handle notes its shape-rule reason on every call.
  - **Deviation from the plan.** The front end keeps its reason strings.
    `dvs_stats_reason_of` maps them onto the list once per check-sat, and only
    when stats are on, instead of every assignment site switching to an enum.
    `tests/unit/test_stats_names.py` fails if a reason string in
    `smt2_frontend.c`, `dvs_shape.c` or `dvs_handle.c` is missing from the
    list, or if a name table and its enum differ in length.
  - `test_stats` adds:
    - `via` and `answered_by` each sum to the solve count on every path;
    - an unsat API solve is credited to `cdcl-root`;
    - the multiplier handle carries reason `multiplier` on all 4 calls;
    - Verilator mode probes CDCL once, then keeps the route (`sticky`).

    ctest 34/34 passes with stats off and on.
  - **Cost** is now 33–46 ns per solve with stats on (budget 50). Off is
    unchanged.
- 2026-11-01, **S0c** (uncommitted): per-class records (tier B).
  - Every solve record carries its shape class:
    - CDCL computes it once per compile (`ctx->stats_class`, next to
      `problem_fp`);
    - the bit-level solver computes it lazily and caches it with the
      instance fingerprint (`S->stats_class`, `S->stats_inst`);
    - SMT2 answers no engine gave take it from `fe->problem`.

    A process-wide fingerprint→class cache (4096 slots) means a re-solved
    instance is never re-hashed.
  - **Counters are kept per class only.** The run-level counters are their
    sum, made at snapshot time, so a solve updates one set of counters under
    one lock. A class record holds:
    - path × result;
    - engines ran and answered;
    - fallbacks;
    - work and wasted work per engine;
    - bail, `via` and reasons;
    - total wall time;
    - log2 histograms of wall time and of work per engine;
    - a 64-register HyperLogLog of distinct instances.

    The table is capped at 2048 classes; beyond that, solves go to class 0
    and are counted in `classes_dropped`.
  - The snapshot (schema `dv-solve-stats-snapshot/1`) has the run counters
    at the top, then `classes` sorted by wall time. Run and class use the
    same member names.
  - `test_stats` adds:
    - `x<y` gets the same class on the api, handle and threaded paths;
    - the multiplier gets the same class on the handle and bvsat paths;
    - 18 constants of one class make one record with an estimate of 18
      instances;
    - every snapshot parses as JSON.
  - **Cost:** about 50 ns per solve with stats on, for the 220 ns
    x<y solve (micro: begin+end alone 39 ns, of which the two clock reads
    are 32). That is at the budget. If it has to come down, the lever is the
    clock (a calibrated cycle counter), not the counters.
  - **Aside, not fixed here:** `getenv`/`strncmp` are about 10% of that
    220 ns solve whether stats are on or off. The CDCL solve reads an
    environment variable on every call, which is a cheap win for the perf
    plan.
- **S0d, value quality (signals 4 and 7) and the distinct-solution count:**
  - The class walk also lists the *tracked variables*: up to 64 rand and
    randc variables, widest first, then by refined colour, then by id
    (`dvs_class_track_t`). Two instances of a class list the same widths in
    the same order. The class cache keeps the list per fingerprint, so a
    cache hit costs a copy.
  - The CDCL and bit-level engines note the tracked variables' values when
    they answer sat (`dvs_stats_note_values`); the last model noted in a
    solve stands. CDCL reads them with `dvs_solver_get_value`, bitblast with
    `dvs_bbsolver_value`: both are pure reads, what the caller reads.
  - Per class, in memory: OR and AND of each variable's values, per-bit
    one-counts (the first 512 bits of the tracked list), and a 64-register
    HyperLogLog of model hashes. **The snapshot never holds a value**
    (decision 11): per variable it gives `width`, `free`, `used` (highest
    set bit of the OR), `varies` (bits that took both values), `max_bias`
    (per mille, over bits that varied) and `stuck`; per class `models`,
    `solutions` (capped at `models`) and the registers, so files merge.
  - `free` starts as the declared range and is narrowed after compile:
    compile applies variable-against-constant constraints as bounds, so a
    variable whose domain is then one value (a hard `x == 5`) is not free.
    (Compile does not propagate to a fixpoint; that happens at the first
    solve's root, which signal 3 measures.) Variables pinned by a soft constraint or a
    zero-weight `dist` still show as stuck: on svcorpus all five such are
    (`soft d_error == 0`, the jtag knobs, aes `sideload_en`).
  - **Cost and sampling.** Unsampled, the values cost ~30 ns on the 220 ns
    x<y solve and 5–13% on the ~1 us svcorpus cases, over the 5% rule. So
    an engine takes every model for its first 64 solves after a compile,
    then one in 16, by count (`dvs_stats_take_values`). With that, stats on
    with values measured the same as a build without values (svcorpus sum
    of medians +0.9% vs +0.1%, per-case differences within run noise).
  - Merge (`stats.py`): models and registers exactly; `used`, `varies` and
    `max_bias` as the larger of the two (so `varies` is a lower bound);
    `stuck` recomputed. Layouts that differ keep the larger record. The
    bundle drops the registers and coarsens the rest.
  - The report flags classes with fewer distinct models than half those
    sampled, and counts stuck variables.
  - Tests: `test_stats` `values` (x<y with z==5 over 40 seeds: z pinned, not
    stuck; one seed 20 times: one model, x and y stuck; handle and bit-level
    paths; unsat adds none), `test_tracked_variables`, `test_merge_values`;
    `dvs_class_sizeof` guards the ctypes mirrors (the struct grew; the old
    mirror segfaulted).
  - Full pytest with `DV_STATS` on: 30 failures, all in the earlier
    stats-on set (pre-existing or order-dependent; the coverage and e2e ones
    fail with stats off too). The run's 26,819 stats files audit clean and
    merge to 31.3M solves in 12,383 classes and 30.8M sampled models;
    23,303 models were `mismatched` (classes whose tracked layout differs
    between instances, not yet looked into).
- **S0e, signals 3, 5 and 6:**
  - **Signal 3, constrainedness** (`root`). Compile records the rand and
    randc variables and the log2 of their declared domain product; the first
    solve after a compile measures, before its first decision (after level-0
    propagation and shaving): variables fixed, variables tied by an alias,
    and the log2 domain product left (`bits` → `free_bits`, intervals only,
    holes ignored). Variables over 64 bits count whole on both sides. CDCL
    only; a class solved only by bitblast has no `root`. The estimated
    solution count of the plan is left at these bounds: `free_bits` is an
    upper bound, the `values` HLL an observed lower one.
  - **Signal 5, binding constraints** (`binding`, and `constraints`/`quiet`/
    `dropped` under `root`). Compile maps each propagator id to the position
    of the problem constraint it came from (0: unique, dist, soft, later
    adds). On a sampled CDCL solve (the values schedule, its own counter)
    the propagate loop counts bounds narrowed per constraint, and the
    conflicting propagator plus every LCG reason count a conflict. The solve
    is then summarised and the counters cleared: constraints that narrowed,
    that took part in a conflict, the busiest constraint's share and the
    no-one-constraint share. No per-position array leaves the library, so
    the canonical constraint order the plan assumed is not needed (the class
    walk has none per constraint). `quiet` counts constraints compiled into
    no propagator (root bounds, aliases), `dropped` the uncompiled ones.
  - **Signal 6** (`time_ns`, `previous`). Compile is timed per class.
    SMT2 commands other than check-sat already open an aux record; its wall
    time now goes to the next solve on the thread as `parse` (declare,
    assert, push, ...) or, for get-value/get-model, to the last solved class
    as `read`. Reading the command text is not counted: in a pipe it
    includes waiting for the simulator. `previous` compares each solve with
    the previous one on its thread: the `same` problem, the same class with
    other `constants`, or another `shape`. Per thread, not per handle: an
    interleaved randomize of two objects shows as `shape`.
  - **Cost** (svcorpus, sum of medians of 3 runs against the committed
    build 38849ba): stats off −1.8%, stats on +0.3%; per case within about
    ±4% (the ~1 us cases), i.e. run noise. Stats off adds one trail-count
    load and one flag test per propagator fire.
  - Tests: `test_stats` `signals` (x<y with z==5: 24 → 16 bits, z fixed,
    z == 5 quiet, 65 of 70 solves sampled, x<y all the narrowing, 69
    repeats; two instances solved in turn: `constants`; SMT2 parse and
    read time); the guide's key table is the schema.
  - Full pytest with `DV_STATS` on: no new failures (the 5 beyond the
    earlier set come from pointing `DVS_SOLVER_PATH` at a build directory,
    which has no headers, and pass without it). 26,816 stats files audit
    clean and merge to 31.6M solves in 12,385 classes; 1,491 classes have a
    `root` and 1,621 a `binding` record (the rest never reached a CDCL root:
    bitblast, or answered before it). `parse` and `read` are 0 there: the
    SMT2 tests run the dv-solve-smt2 binary of the default build, without
    these changes.
- **S0f, the core owns the time split (2026-11-01):** signal 6's
  `parse`/`read` were SMT2-only, so a front end on the API (pyvsc) got no
  time split. Now one core function attributes time outside the solves
  (`dvs_stats_time`): `build` (renamed from `parse`) to the next solve on
  the thread, `read` to the class last solved (or the context's). Sources:
  the library's own `dvs_builder_finalize*` (build) and
  `dvs_solver_get_values` (read), timed only outside a solve (inside one
  the time is the solve's) and not when nested; SMT2 commands, as before;
  and the public `dvs_stats_add_time` (Python `dv_solve.stats.add_time`,
  `stats.timed("build")`) for a front end's own time.
  - Everything else was already core: records open in `dvs_solver_solve`,
    the handle, DPI and `dvs_bbsolver_*` entries; class, values, root and
    binding come from compile, search and the bit-level engine.
  - Checked with pyvsc (`../pyvsc-dv-solve`, `VSC_SOLVER=dv-solve`): 220
    randomize calls give one class on the API path with values, root,
    binding, compile/build/read time and 99% repeats. pyvsc sent only the
    `dist` variable to the solver; it solved the range and inline `with`
    fields itself, so the stats see only what pyvsc hands over, and its own
    translation time only if pyvsc reports it (`stats.timed`; a pyvsc
    change, not made).
  - The report no longer flags "few distinct models" when the root allows
    about that many solutions (`2^free_bits`).
  - Tests: `test_stats` (reported and library build/read time on the API
    path), `test_front_end_time` (Python `stats.timed`).
- **Still to do in S0:**
  - pyvsc: report its translation time with `stats.timed("build")` (in
    pyvsc);
  - values for answers no engine gave (SMT2 model reuse), and from the cube
    engine;
  - SAT work on cube and portfolio worker threads.

## S1: canonical shape hash

**Goal:** two problems that differ only in names, variable ids or constant
values get the same class hash. Problems that differ in structure get different
hashes.

- Walk the compiled problem like `dvs_shape.c` does. Hash the operator, width
  and signedness. Renumber variables in first-visit order. Sort the operands of
  commutative operators and the top-level constraint list by their subtree hash,
  so ordering doesn't matter.
- **Constants** are hashed as a class, not a value: 0, 1, all-ones, power of
  two, small (< 256) or other, plus their width. Constants decide hardness only
  sometimes. A finer hash (`H_inst`, the existing `dvs_problem_fingerprint`) is
  kept as a HyperLogLog per class to count distinct instances.
- Soft constraints, dist, solve-before and all-different are part of the
  structure.
- **Feature vector** (kept in the record so classes can be grouped without the
  problem): the `dvs_shape` counts, number of vars, a width histogram, construct
  flags (mul, div, mod, shift, countones, array, unique, dist, soft), the
  number of coupled components and the largest component, and the depth of
  `if`/implication.
- **Motifs** (signal 9). Match each constraint against a fixed list of small
  patterns, one to three operators deep, over canonical operators with constant
  classes.
  - The list is versioned and built into the release, so the stats file holds
    only motif ids and counts. The user's `explain` shows each id as its
    pattern.
  - A motif is common to many designs and is too small to be recognizable. A
    histogram of motifs, weighted by time, shows which constructs cost the
    most across the field.
  - Constraints that match no motif are counted as "other", by top operator.
    The share of "other" tells us when the list needs new entries.
- Compute once at compile, not per solve. Cache on the context next to
  `problem_fp` (`dvs_ctx.h:169`).
- This also gives a non-name key that could later replace the Verilator
  `shape_fp`, which hashes names (`smt2_frontend.c:3725`). That is a follow-up,
  not part of this plan.

**Tests:**
- Renaming, renumbering, reordering and changing constants within a class leave
  the hash unchanged.
- Each structural mutation changes the hash. Use a mutation sweep over svcorpus
  and rand-core.
- Report the collision rate on all corpora.

**Log:**

- 2026-11-01, **S1a** (uncommitted): class hash. `src/c/dvs_class.{h,c}`,
  `dvs_class_compute(p, &out)` gives the class `hash` and the instance hash
  (`inst` = `dvs_problem_fingerprint`). Internal for now; the tests call it
  through ctypes.
  - Node hashes come from the operator, the node's own widths and flags, and
    its operands' hashes:
    - commutative operands, set members, ranges and summands are sorted;
    - `a > b` is hashed as `b < a`;
    - top-level AND spines are flattened;
    - a constant contributes its class (zero, one, all-ones, power of two,
      small, small negative, other) and its width;
    - declared variable bounds are classed the same way, plus type-min and
      type-max.
  - **Variable colours, two rounds.** The first is the type: width, sign,
    kind, order stage and bound classes. The second adds the round-1 hashes
    of every constraint, soft, all-different and dist the variable appears
    in. This is one step of colour refinement (1-WL), so `x<y, y<z` and
    `x<y, z<w` differ.
  - **Known limit.** As with any 1-WL refinement, a few regular structures
    that are not isomorphic get one class (two triangles of `!=` against a
    hexagon). That only merges stats records and is harmless.
  - Iterative throughout (no recursion). 100k nested additions hash in 22 ms,
    about 110 ns per node.
  - Tests:
    - `tests/unit/test_class_hash.py`: invariance under renumbering,
      reordering, `>` vs `<`, a `&&` join versus two constraints,
      commutative operands, and a constant changed within its class;
      11 structural mutations each change the hash and are all distinct;
      the instance hash tells constants apart; deep nesting.
    - `tests/perf/svcorpus/test_class_hash.py`, over all 21 svcorpus cases:
      renumbered ids, shuffled constraint order and remapped small constants
      keep the class, 3 trials each. All 300+ single-operator mutations
      change it. No two cases share a class.
  - **Collision rate:** 0 between different problems across svcorpus and its
    mutants. Mutants of one case do share a class where the case repeats a
    block: `flash_ctrl_otf_base`'s 8 regions, `keymgr_base`'s three
    `x == 0` constraints over same-type fields. Those mutants are the same
    problem up to renaming, which is correct.
  - **Not yet:** the rand-core sweep. It needs rand-core's terms turned into
    a problem; it will run through the SMT2 front end with S0c.
- 2026-11-01, **S1b** (uncommitted): feature vector and motifs.
  `dvs_class_compute` also fills `dvs_class_features_t`:
  - variables, conjuncts, softs, all-differents, dists and ordered
    variables;
  - a width histogram (1, ≤8, ≤16, ≤32, ≤64, wider);
  - coupled components and the largest one (union-find over the variables
    each constraint touches);
  - the deepest expression and the deepest nest of guards;
  - the `dvs_shape` operator counts;
  - motif counts.
- **Motifs:** a versioned list (`DVS_MOTIF_VERSION` 1), 22 ids, names from
  `dvs_motif_name`:
  - var-cmp-const, var-eq-const, var-cmp-var, var-eq-var;
  - range, set, ranges;
  - mod-eq, mask-eq, shift-eq;
  - linear, nonlinear, bitwise;
  - countones, sum, array;
  - guarded, disjunction, bool-var, ite-value, constant;
  - other.

  Operands are read through casts and extends, and a bit-slice (or a slice
  of a slice) of a variable counts as a variable. A guarded constraint counts
  as `guarded` and also as its body's motif; `!expr` counts as `expr`.
- **svcorpus result:** after two fixes found on it (nested fields, and the
  new `constant` motif), 1 of about 490 constraints is `other` (0.2%):
  `inside` compared with `==` against an `||`. The top motifs are:
  guarded 145, var-eq-const 92, set 81, range 55, var-cmp-const 27,
  ranges 26.
- **In the snapshot:**
  - each class carries `features` and `motifs`;
  - the run carries `motif_version` and, per motif, the constraint `count`
    and the `wall_ns` of the classes holding it (the time-weighted
    histogram).

  Features are recorded once per class, when its hash is first computed.
- Tests:
  - `tests/unit/test_class_hash.py`: a 10-constraint problem gets exactly
    the expected motifs, widths, components (2, largest 4) and guard depth;
    motifs and features do not change under renaming or a constant changed
    in class; the motif names match the list.
  - `test_stats`: motifs are summed over classes, and each class carries
    features.
- **Still to do in S1:** the rand-core sweep.

## S2: local stats file and end-of-run report

- `DV_STATS=<path>` (or `DV_STATS=1` for `./dv_solve_stats.json`). The file is
  written at process exit and at `dvs_solver_free` of the last context.
  - Verilator mode is one process per simulation, so this is simple there.
  - pyvsc and DPI share the simulator process, so the table is process-global.
  - Python registers its own `atexit`, as `VSC_PROFILE` does, to flush before
    the interpreter frees contexts.
- **Schema:** versioned JSON with a run header (dv-solve version, commit, path
  used (`api`/`smt2`/`verilator`/`pyvsc`/`dpi`), CPU count, and no host name)
  and one record per shape class (feature vector, histograms, counts, reasons).
  Records are only added up, so files from many runs and users merge by summing
  per class (`dv-solve-stats merge`).
- **End-of-run summary** to stderr or a log, aimed at the user's own benefit,
  for example:

  ```
  dv-solve: 1.2M solves, 3 shapes take 81% of solver time;
  slowest my_pkt::randomize at seq.py:212, 40 µs/call
  ```

  Source locations are local only:
  - **pyvsc:** `do_randomize` already has `SourceInfo(file, line)`
    (`model/randomizer.py:1455`). Pass it on `solve_info` or a thread-local
    to the backend.
  - **Verilator:** the SMT2 variable names give a class hint.

  These go into a **separate** local file (`DV_STATS_LOCAL`) that maps a class
  hash to source locations. That file is never part of a bundle.

### Inspectable by design

A user who cannot share problems must be able to check the stats file
themselves and confirm it holds nothing proprietary. Ways to ensure that:

- **Plain JSON with a closed schema.** Every key comes from a fixed list that is
  documented in the user guide, one line per key. Values are only numbers,
  enum strings from that list, or hex hashes. No free text, no base64, no
  opaque binary blobs. Sketches (HyperLogLog registers, histograms) are written
  as plain arrays of small integers.
- **`dv-solve-stats explain <file>`** prints the file as a readable report.
  Each value is labelled with what it means.
- **`dv-solve-stats audit <file> [--words <list>]`** checks:
  - every key is in the schema;
  - every string is an enum value or a hash of the right length;
  - no number is outside its bucket set.

  With `--words`, it also checks that none of the user's own identifiers
  appear, in clear or as a hash (for example class names, field names and
  project names from a list they give, or `--words-from <dir>` scanning their
  SV sources). The user can run the audit on any bundle before sending it.
- **Hashes are salted per bundle** by default. Salting means two bundles from
  different users can't be matched to each other, and a guessed structure
  can't be confirmed by hashing it. We still merge within one bundle, and
  across one user's bundles if they choose to keep one salt. Merging classes
  across users, and matching them against corpus cases in the S5 gap report,
  uses the feature vector instead. A user who wants exact matching can
  turn the salt off (`DV_STATS_SALT=none`).
- **k-threshold.** A class seen fewer than `k` times in the run (default 3) is
  merged into an "other" bucket with its features coarsened. One-off problems
  are where a design detail is most likely to show.

**Tests:** the schema validates. Merging is associative. A pyvsc run reports the
right `file:line` per class. A grep-based test shows no name or path appears in
the stats file. The audit passes on every stats file the test suite produces,
and fails on a file with an injected name, an unknown key or a free-text value.
Running `--words` with the svcorpus class and field names finds no match in
their stats.

**Log:**

- 2026-11-01, **S2a** (uncommitted): the stats file and the tools.
  - **C side.** `dvs_stats_write(path)` is public and documented in the
    C API page. It writes `dv-solve-stats/1`: a header (dv-solve version,
    commit, CPU count; no host, user or path) and the snapshot, to a temp
    file that is then renamed into place.
    - `DV_STATS=<path>` (with `%p` for the pid), or `DV_STATS=1` for
      `dv_solve_stats.<pid>.json`, registers an `atexit` writer on first
      use. One file per process, so parallel simulations do not clobber each
      other; `merge` combines them.
    - The stats live in process globals, not in contexts, so the C `atexit`
      covers Python too. No Python `atexit` is needed.
    - Each class also carries its 64 HyperLogLog registers (`hll`, small
      integers), so instance counts merge exactly.
  - **Python side.** `python -m dv_solve.stats` (`src/dv_solve/stats.py`):
    - `report`: totals, results, engines, `via`, reasons, the top classes
      by solver time (share, cumulative, µs per solve, instances, top
      motifs), and motifs by the time of the classes holding them;
    - `merge`: exact; associative and order-free;
    - `explain`: every value labelled from `SCHEMA`;
    - `bundle`: L0;
    - `audit`.
  - **Change from the plan: anonymize in the bundle, not in the stats
    file.** The stats file is exact and local, so runs merge exactly. Salting,
    log2 coarsening and the k-threshold are applied by `bundle`, which makes
    the only file meant to leave. Specifically:
    - class ids become `sha256(salt ‖ id)[:32]`. The salt is random per
      bundle by default; `--salt-file` keeps one salt so a user's bundles
      match each other, and `--no-salt` leaves ids matchable against our
      corpus;
    - every number is rounded down to a power of two;
    - classes solved fewer than `k` times (default 3) fold into one `other`
      record, with no features or motifs;
    - `hll` registers and the commit are dropped.

    `bundle` refuses to write a bundle that fails its own audit.
  - **Audit.** It checks that:
    - every key is in the closed schema (`SCHEMA` plus the name lists);
    - every string is a schema value, a class id of 16 or 32 hex digits, the
      version, or the commit;
    - in a bundle, every count is 0 or a power of two, histograms are well
      formed, and there are no registers;
    - none of the user's words (`--words`, `--words-file`, or
      `--words-from DIR` with SV identifiers scanned from source) appears in
      clear or as an md5, sha1 or sha256 hex prefix. Schema vocabulary is
      exempt (`range`, `set`, `sum` are also motif names).
  - Tests:
    - `tests/unit/test_stats_cli.py`: the Python vocabulary matches the C
      tables; the file is written at exit from a subprocess; merge is exact
      (one class across two runs keeps 1 instance) and associative; bundle
      salts, coarsens (12 → 8) and folds the 2-solve class; the same salt
      gives the same ids and another salt does not; the audit fails on an
      unknown key, a name as a class id, free text, an uncoarsened count, and
      a word in clear or hashed; the CLI round-trips.
    - `tests/perf/svcorpus/test_stats_audit.py`: every svcorpus case solved
      with `DV_STATS`. The file and both bundles (salted and not) pass the
      audit against all of the corpus's 100+ case and field names.
- **Still to do in S2:**
  - the end-of-run summary line (opt-in);
  - `DV_STATS_LOCAL`, the class → source-location map (pyvsc `SourceInfo`;
    Verilator name hints);
  - the user-guide page that lists the schema one key per line (`SCHEMA`
    is the source);
  - a `dv-solve-stats` console script.

## S3: capture format, anonymizer, reproduction check

There are two capture levels:
- **C1, the skeleton**, which sits between the statistics and a full capture;
- **C2, the full capture**.

### C1: skeleton

A skeleton is the canonical operator graph from S1: operators, widths,
signedness, variable sharing, and soft, dist, order and all-different markers.
Each constant is replaced by its **relations**. Hardness usually depends on how
constants relate (overlapping ranges, alignment, modular structure), not on
their values. The relations keep what the algorithms see and drop what
identifies a design. Relations recorded:
- the class from S1: 0, 1, all-ones, power of two, small, other;
- an alignment mask, meaning low bits that are zero;
- the order among constants compared against the same variable (`c1 < c2`);
- equality among constants;
- the constant's use: equality only, order, arithmetic, or mask.

**Regeneration.** `dv-solve-stats regen` picks constants that satisfy the
relations. The regenerated problem is kept only if its verdict, engine route
and work count (within 2×) match the shape record of the class. This is the
same reproduction check as in C2, but run by us, so the user sends no
constants at all.

A skeleton holds no names and no constant values. It does show the structure,
and that can hint at a protocol or a block. Many users won't be allowed to send
one; L1 (S8) is the route for them. Where a skeleton is allowed, prefer the
**core skeleton**: the skeleton of the reduced hard core from S8. It is smaller,
less recognizable, and keeps what makes the class slow. So it is opt-in per class
(`DV_STATS_SKELETON=top:N`) and listed in the bundle manifest. The user checks
it with `dv-solve-stats explain`, which prints a skeleton as indented
pseudo-constraints, for example `v3[8] inside [c1..c2]`, `c1 < c2`,
`c1 aligned 4`.

### C2: full capture

- **Payload:** the raw problem buffer (`dvs_problem_s`, `src/c/dvs_problem.h:351`).
  It is already name-free and lossless (softs, dist, order, all-different).
  - Add a header with magic, format version, endianness and the dv-solve
    version, because today it must be read by the same version.
  - Write a loader that rejects a mismatch. Replay later by version-tagged
    upgrade, or by the build that captured it.
  - Write `dvs_problem_write_smt2` output next to it, for z3/bitwuzla
    cross-checks of the hard part only.
- **What is captured:** `DV_STATS_CAPTURE=top:N` captures up to K instances
  (default 4, picked by reservoir sampling) of the N classes with the most
  solver time. It also always captures every unknown, timeout or
  `validate_model` failure (soundness first). Captures go under the stats
  directory, laid out like the oracle run directory (`dvs_oracle_core.c`), but
  with no argv, no env values and no raw text.
- **Constant policy** (`DV_STATS_CONSTANTS`):
  - `keep`: the default for our own runs.
  - `renumber`: rewrite constants that are used only in `==`/`!=`/`inside` sets
    (enum codes, opcodes, addresses compared for equality) to fresh values of
    the same width that keep the equal/distinct pattern. This preserves meaning
    exactly. Arithmetic and order constants stay.
  - `perturb`: for range and arithmetic constants, scale or shift by a random
    offset, then **re-check** (below). A capture whose check fails falls back
    to `renumber`, and is marked so.
- **Reproduction check**, before a capture is kept:
  - re-solve the transformed problem;
  - it must give the same verdict (sat/unsat/unknown);
  - its median time must be within 2× of the original, with the same engine and
    route.

  A capture that is no longer slow is not a useful perf case.
- **Bundle:** `dv-solve-stats bundle <dir> [--level L0|L1|L2|L3]` packs the
  user's chosen level and every level below it into one tarball. The default
  is `L1`:
  - the stats file;
  - experiment results;
  - synthetic matches;
  - no skeletons or captures.
  - The bundle includes a text manifest listing every class, its share of time,
    what each class contributes (stats, skeleton or capture), and each constant
    left in the clear.
  - `bundle` runs `audit` and refuses to pack a file that fails it.
  - The user reads the manifest and sends the bundle, or not.

**Tests:**
- Round-trip: capture, load, solve gives the same models for the same seed.
- `renumber` preserves the sat/unsat result and the solution count on the small
  exhaustive set.
- `perturb` with a failed re-check falls back.
- A grep test shows no names in a bundle made from svcorpus run through Verilator.

**Log:**

## S4: corpus ingestion

- New case type in `tests/perf/`: a **capture case**, made of the buffer, its
  shape class, a weight (the field share of time) and a reference. Add a loader
  in `run_rand.py` for compile once, then N × `reset(); solve(seed)`, the same
  loop as svcorpus.
- **Reference for distribution checks:**
  - Exact when the solution count is small (below 2^20): enumerate, then use
    the pooled χ² from `run_direct.py` and excess JSD.
  - Otherwise use per-variable marginals from a projection that can be counted,
    or no reference. In that case the case is perf-only, with a z3 check of
    every model.
- New suite `tests/perf/suites/field.json`. Results go to `tests/perf/history`
  through `consolidate.py`, like the other suites. Hard-constraint SMT2 copies
  can also join `sat-core`, which takes globs.
- Deduplicate by class hash. Keep at most K instances per class, chosen to
  spread over the feature vector and the time histogram.
- Each case records where it came from (a bundle id, never a company) and its
  constant policy. Captures from outside users go in a private store, not the
  repo, unless the sender agrees. They are like `docs/notes`.

**Log:**

## S5: representativeness

This step makes the data actionable.

- **Gap report** (`dv-solve-stats gaps`):
  - Merge all stats files and rank shape classes by field solver time.
  - Mark each class covered (it has a corpus case), near (a corpus case within
    a small distance on the feature vector) or uncovered.
  - Output: the time share the corpus covers, and an ordered **"make these
    next"** list, each item with its feature vector, typical size and
    reason/engine mix.
- **A test case for every class, whatever was shared.** Each class takes the
  most faithful source available. Each case is marked with its source, and the
  gap report counts a class as covered at that level:
  1. **Capture (C2):** the problem itself.
  2. **Skeleton (C1):** regenerated constants on the real graph (S3).
  3. **Synthetic match (L1, S8):** a problem from our generator, already
     checked on the user's machine to behave like theirs. This is the usual
     source for a field class.
  4. **Shape-faithful synthesis on our side**, from the feature vector alone.
     It is the fallback for a class with no L1 match, and it is weaker because
     we can't compare it with the user's problem:
     - generate problems with the same var count, width histogram, construct
       mix, component structure, implication depth, constrainedness (signal 3)
       and used ranges (signal 4);
     - keep the generated ones whose work count and engine route match the
       class record: work within 2×, same route, same fast path (signal 2);
     - they become stand-in cases, marked as synthetic.
- **Not every decision needs a test case.** Many rows of the table below are
  answered from the statistics alone. With statistics only, signals 1–8 and the
  S7 results still say where routing loses, which fast path to build, and which
  classes sample badly.
- **What the report drives:**

| Signal | Decision |
|---|---|
| Field time by class vs. corpus coverage | Which testcases to write or synthesize next |
| Time by construct flag and by fallback reason | The order of the CDCL construct gap work (`docs/cdcl_construct_gap_analysis.md`) and the evaluator fallback |
| Repeat count and time by class (high repeats, tiny classes) | Which shapes get a direct-sampling fast path |
| Distinct instances per class (inline-constant churn) | Whether a cached base plus added constraints pays (plan caching in the core) |
| Component count and largest component | Whether component splitting pays, and on how much time |
| Engine route vs. measured time per class | Retune `dvs_shape`/`_qfbv_route` thresholds; data for learned strategy selection |
| Distinct-solution ratio per class | Uniformity bugs, as new distribution cases |
| Unknown, timeout, `validate_model` failures | Soundness and completeness bugs: fixed first |
| Wasted work by bail reason and class (signal 2) | Routing thresholds; which engine to try first |
| Root fixed share, domain shrink, log2 solution count (signal 3) | Features for engine choice; when enumeration or closed form pays |
| Used range vs declared width (signal 4) | Width narrowing at compile |
| Constraints that never bind, or that do most of the propagating (signal 5) | Presolve pruning; which propagators to add |
| Parse, compile, solve and read-back split (signal 6) | Plan caching; the cost of the Verilator pipe protocol |
| Bit bias, stuck variables, effective dist weights (signal 7) | Uniformity bugs, as new distribution cases |
| p99 vs p50 work per class (signal 8) | Restarts or portfolio (heavy tail) vs. encoding work (high median) |
| Shadow solve work ratio by feature bucket (S7) | Evidence for a routing or heuristic change before it becomes the default |
| Shadow solve verdict disagreement (S7) | Soundness bug: fixed first |

**Log:**

## S6: dogfood

Before any external user, run the whole chain on our own workloads:
- the pytest and perf suites;
- the svcorpus OpenTitan classes through Verilator;
- the pyvsc test suite and examples with `DV_STATS`;
- any Verilator OpenTitan simulation.

Check that:
- the gap report ranks what we already know is slow;
- the captures reproduce;
- synthetic matching finds matches for our own costly classes, and the
  matched cases rank the same as the originals in the perf suites;
- no names leak;
- the overhead is in the noise.

Only then document `DV_STATS` for users. Ask a friendly user for one bundle as
the first field test.

**Log:**

## S7: experiments on the user's machine

**Goal:** let a user who can share only statistics run our A/B tests on their
own problems, so routing and heuristic changes can be checked on real
workloads before they become defaults.

- `DV_STATS_EXPERIMENT=<name>[:rate]` turns it on. The default rate is 1 solve
  in 100 per class, capped per class.
- On a sampled solve, after the real solve has returned its result, solve a
  **copy** of the problem under the alternative settings the experiment names.
  - Experiments are a fixed list built into the release, for example
    `route-cdcl`, `route-bitblast`, `box-probe-off`, `kissat-vs-cadical`, or a
    candidate threshold.
  - No experiment can be defined at run time, so the audit can check that the
    names are from the list.
- Record per class, and per experiment:
  - the work ratio (alternative over default) as a log2-bucket histogram;
  - the route each side took;
  - verdict agreement.
- **Must not affect the user's run:**
  - the shadow solve uses its own context and its own random stream;
  - the real result and the user's seed sequence are exactly the same with the
    experiment on or off;
  - the shadow solve has a work budget (a multiple of the real solve's work,
    never time) and stops at it, recorded as `budget`.
- A **verdict disagreement** is a soundness event:
  - it is always recorded;
  - with a capture level of C1 or C2 enabled, the class is captured at once.

**Tests:**
- The models and seed sequence of a seeded run are identical with an
  experiment on and off (C API, Python, Verilator mode).
- The work ratios in a stats file match running the two settings directly on
  the same svcorpus loop.
- A deliberately broken alternative engine (a mutant) shows up as a
  disagreement.

**Log:**

## S8: local hard-core reduction and synthetic matching

**Goal:** a user who may send only statistics still gives us a test case that
behaves like their problem, without any of their problem leaving the site. The
problem is matched on their machine; only the match leaves.

**Which classes.** The classes that hold the most solver time (`top:N`), plus
every class with an unknown, timeout, `validate_model` failure or S7
disagreement. The run that collects stats keeps the instances it needs in
memory (reservoir sampling, as in S3). Nothing is written to disk unless the
user asks.

**Hard-core reduction.** For each chosen instance, shrink the problem to the
smallest part that is still costly. Use delta debugging on the problem buffer:
- drop constraints;
- fix variables to a value from a model;
- split off components that aren't coupled to the rest.

Keep a step only if the verdict, engine route and fast path are unchanged and
the work count stays within 2× of the original. The core is what we need to
tune on. It is usually a small part of the class, and so much less
recognizable. Its feature vector and motif histogram go into the class record.

**Synthetic matching.** Run our generator on the user's machine to find a
synthetic problem that behaves like the core:
- The generator is built into the release and versioned. Its input is a
  parameter vector: var count, width histogram, motif mix, component structure,
  implication depth, constrainedness, used ranges. The feature vector of the
  core gives the starting parameters.
- Search the parameters and seeds (bounded by work count, never time). Accept
  a candidate whose verdict, engine route and fast path match the core, whose
  work count is within 2×, and whose value-quality signals (signal 7) are in
  the same buckets. A candidate that fails to match is still worth reporting,
  as "nearest, distance d".
- **What leaves the site:** the generator version, the parameter vector, the
  seed, the match distance, and the core's statistics. The parameter vector is
  bucketed like every other field.
- On our side, the same generator with the same inputs rebuilds the problem
  exactly. It becomes a corpus case (S4), marked `synthetic-matched` with its
  match distance.

**Why it is safe to send, and how the user checks it:**
- the synthetic problem is a pure function of our generator and the numbers in
  the bundle;
- `dv-solve-stats explain` regenerates it and prints it as pseudo-constraints,
  so the user sees exactly what we will see;
- the audit checks that the bundle holds only generator inputs, and that the
  regenerated problem shares no constant with the user's problem beyond the
  constant classes (0, 1, all-ones, powers of two).

**When no match is found:**
- the class still contributes its statistics (L0);
- its nearest candidate and the distance tell us which motif or structure the
  generator is missing;
- generator gaps are ranked by field time like any other gap (S5).

**Tests:**
- On svcorpus and rand-core, synthetic matching finds a match for most classes
  that take most of the time, and a matched case's work count on our side is
  within 2× of the original's. Report the match rate and the distances.
- Regeneration is bit-exact across hosts.
- The audit rejects a bundle whose parameters have been edited to carry an
  extra constant.
- Reduction keeps the verdict, the route and the fast path on every corpus
  case.

**Log:**

## Decisions (2026-10-26)

1. **The stats file is off by default.** It is written only when `DV_STATS` is
   set. Counters in memory are always cheap. In pyvsc, the end-of-run summary is
   on only when `VSC_PROFILE` is set.
2. **Captures from outside users live in a private store**, never in the repo.
3. **External bundles default to `DV_STATS_CONSTANTS=renumber`.** Our own runs
   default to `keep`.

## Decisions (2026-11-01)

4. **The plan is built for the statistics-only user.** Assume no recognizable
   test case can leave the site. L0 statistics and L1 synthetic matches are the
   main channel. L2 skeletons and L3 captures are extras where allowed. Every
   kind of finding must be reachable from L0 and L1.
5. **The analysis moves to the data.** Measurement, shadow experiments (S7),
   hard-core reduction and synthetic matching (S8) run on the user's machine.
   Only numbers leave.
6. **Everything that leaves can be checked by the user.** The rules:
   - a closed schema;
   - `explain`, which regenerates and prints synthetic matches;
   - `audit`, with the user's own word list;
   - hashes salted per bundle;
   - a k-threshold on classes.

   `bundle` defaults to L0 and L1 and won't pack a file that fails the audit.
7. **Shadow-solve experiments (S7)** are opt-in and use a fixed built-in list.
   They have a work budget and never change the user's results.
8. **Hash salt.** Hashes are salted per bundle by default, because users may not
   reveal anything that could confirm a guessed design. Classes are matched
   across users by feature vector and motif histogram.
9. **Anonymize at bundle time** (made during S2a). The local stats file is
   exact and unsalted, so runs merge exactly and the user's own report is
   precise. Salting, power-of-two coarsening and the k-threshold are applied
   by `bundle`, which writes the only file meant to leave, and which the
   user audits.
10. **Wall time from the monotonic clock** (made during S0a). A thread CPU
    clock read costs about 77 ns against 16 ns, which would break the
    50 ns-per-solve budget. Work counts are the measure that compares across
    hosts; wall time is for the user's own ranking.
11. **No values in the stats file** (made during S0d). The plan listed
    per-bit one-counts per variable, but a bit that is always set gives away
    a constant, and the guide promises no constant values even in the local
    file. The library keeps the counts in memory and writes only what they
    span (`used`, `varies`, `max_bias`, `stuck`). The cost: these merge as
    bounds, not exactly.
12. **Value signals are sampled** (made during S0d): every model for an
    engine's first 64 solves after a compile, then one in 16, by count.
    Unsampled they cost 5–13% of a 1 us solve.

## Ground rules

- Every step lands as its own commit. Run the whole `env -u LD_LIBRARY_PATH
  pytest -q` with each. S0 and S1 touch compile and solve, so they also run
  the QF_BV fuzzer.
- Overhead is measured with the C-API arm (enhancement plan item 0), median of
  3, built from a ref.
- Build, test and timing output goes to log files.
