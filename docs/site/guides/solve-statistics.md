# Solve statistics

dv-solve can count its own work while your tests run: how many solves there
were, where the solver time went, which engine answered, and what the
constraints were made of. The counts are useful to you, because they show
which `randomize()` calls cost the most. They are also the most useful thing
you can send us when you can't send a testcase.

Collecting statistics never sends anything anywhere. The library writes a file
on your machine. If you choose to share, you make a separate *bundle* from
those files, read it, check it, and send it yourself.

## Collecting

Set `DV_STATS` to a file name before the run. `%p` is replaced by the process
id, so parallel simulations each write their own file:

```sh
DV_STATS=stats.%p.json ./run_my_tests
```

The file is written when the process exits. `DV_STATS=1` writes
`dv_solve_stats.<pid>.json` in the current directory. With statistics on, a
solve costs about 50 ns more; with them off, it costs nothing measurable.

## Reading your report

```sh
python -m dv_solve.stats report stats.*.json
```

The report merges the files and lists the solve totals, which engines
answered, and the *shape classes* that took the most solver time. A shape
class is a problem's structure with the names, the variable ids and the
constant values left out. Two calls of the same `randomize()` with different
inline constants are one class. For each class, the report gives its share of
solver time, the time per solve, how many distinct problems it held, and the
constraint patterns (*motifs*) it is made of.

Under a class, the report also says how varied its answers were: how many
distinct models it saw among those sampled, and how many variables never
changed. Few distinct models over many solves means either that the
constraints leave few solutions, or that the sampling has collapsed. A
variable that never changed although its range is free is either pinned by a
constraint the solver could not see at compile time (a `soft x == 0`, a `dist`
with a zero weight) or a sampling bug. The statistics hold only what the
values spanned, never the values themselves.

The report also says, per class, how constrained the problem was before the
solver's first decision (how many variables were fixed, and how far the
domains shrank), how many constraints did the narrowing, and the time spent
outside the solves: stating the problem, compiling it, and reading the
answers back. A class whose solves mostly repeat the previous
problem is one where caching the compiled problem pays.

A front end that drives the library directly can add its own time, such as
translating its constraints, so that the statistics show the whole cost of a
`randomize()` call:

```python
from dv_solve import stats
with stats.timed("build"):
    ...   # translate the constraints and call the builder
```

From C, call `dvs_stats_add_time`. Nothing is recorded while statistics are
off.

## Sharing a bundle

A bundle holds only statistics. It is made from your stats files in three
steps:

1. Every class id is replaced by a salted hash, so it can't be matched against
   a guessed design.
2. Every number is rounded down to a power of two.
3. Classes solved fewer than three times are folded into one record, because
   a one-off problem is where a design detail would most likely show.

```sh
python -m dv_solve.stats bundle -o bundle.json --salt-file salt.bin stats.*.json
python -m dv_solve.stats explain bundle.json
python -m dv_solve.stats audit bundle.json --words-from rtl
```

- `bundle` writes the file, and refuses to if it would fail the audit.
  `--salt-file` keeps one salt, so the bundles you make over time can be
  compared with each other. Keep the salt file private. `--no-salt` leaves
  class ids comparable with ours, if you allow that.
- `explain` prints every value in the bundle with what it means.
- `audit` checks the following:
  - every key is one of those listed below;
  - every text value is a value of the schema or a hash;
  - every number is 0 or a power of two;
  - none of your identifiers appears in the bundle, in clear or as a hash.

  `--words-from` takes the identifiers from the SystemVerilog sources in a
  directory; `--words` takes a comma-separated list.

Never in a stats file or a bundle: variable, field or class names, constant
values, file paths, command lines, environment values, host or user names,
and the problem itself.

## What a bundle holds

The keys, each with what it means:

| Key | Meaning |
|---|---|
| `schema` | the format of the file |
| `dv_solve` | the dv-solve version that wrote it |
| `commit` | the dv-solve source commit it was built from (stats files only) |
| `cpus` | CPUs on the machine (stats files only) |
| `level` | how much a bundle holds: `L0` is statistics only |
| `salted` | class ids are salted, so they cannot be matched against another bundle's |
| `k` | classes solved fewer times than this are folded into one |
| `snapshot` | the counters |
| `compiles` | problems compiled |
| `solves` | solves |
| `wall_ns` | solver time, in nanoseconds |
| `results` | solves by result: `sat`, `unsat`, `unknown`, `error` |
| `paths` | solves by the interface they came through: `api`, `handle`, `dpi`, `smt2`, `verilator`, `bvsat` |
| `engine_ran` | solves in which each engine ran: `cdcl`, `bitblast`, `cube` |
| `answered_by` | solves each engine answered (`none`: no engine was needed) |
| `fallbacks` | solves in which more than one engine ran |
| `work` | work done by each engine (CDCL: propagation units; bitblast and cube: SAT solver ticks) |
| `wasted_work` | work by an engine that did not give the answer |
| `bail` | CDCL searches that gave up, by reason |
| `via` | what produced each answer (`cdcl-search`, `cdcl-box`, `bb-sat`, ...) |
| `reasons` | why solves were routed as they were (`multiplier`, `wide-var`, ...) |
| `hist` | log2 histograms of `wall_ns` and of work per engine: `[bucket, count]`; bucket *b* holds values from 2<sup>b−1</sup> up to 2<sup>b</sup> |
| `aux` | solver work done outside check-sat (`get-unsat-core`) |
| `calls` | commands |
| `motif_version` | the version of the motif list |
| `motifs` | constraints by pattern (`range`, `set`, `guarded`, `mod-eq`, ...); at the top level, with the solver time of the classes that hold them |
| `count` | how many |
| `classes_dropped` | solves counted without a class (the class table was full) |
| `classes` | one record per shape class |
| `class` | the class id: a one-way hash of the structure; salted in a bundle; `other` for the folded classes |
| `instances` | distinct problems seen in the class (an estimate) |
| `hll` | registers of that estimate, and of the `solutions` estimate (stats files only, never in a bundle) |
| `features` | what the class's problem is made of |
| `vars` | variables |
| `constraints` | constraints |
| `softs` | soft constraints |
| `alldiffs` | `unique` constraints |
| `dists` | `dist` constraints |
| `ordered` | variables in a `solve ... before` order |
| `components` | groups of variables tied together by constraints |
| `max_component` | variables in the largest group |
| `max_depth` | the deepest expression, in operators |
| `guard_depth` | the deepest nest of `if` or implication |
| `nodes` | distinct operators |
| `widths` | variables by width in bits: `1`, `8`, `16`, `32`, `64`, `wide` |
| `ops` | operators by kind: `bit`, `arith`, `mul`, `cmp`, `eq`, `bool`, `other` |
| `values` | what the class's models spanned, from a sample of them: every model of an engine's first 64, then one in 16 |
| `models` | models sampled |
| `mismatched` | models not counted, because their variables did not line up with the class's first model |
| `solutions` | distinct models among those sampled (an estimate) |
| `width` | a variable's width in bits, up to 64; `vars` under `values` lists up to 64 variables, the widest first |
| `free` | 1 if the variable's range has more than one value: the declared range, narrowed by what compile could deduce (a hard `x == 5` pins `x`) |
| `used` | bits its values used (the highest set bit) |
| `varies` | bits that took both values |
| `max_bias` | the largest bias of a bit that varied, in thousandths off one half |
| `stuck` | 1 if the variable is free but never changed over 16 or more models |
| `time_ns` | time outside the solves, in nanoseconds: `build`, `compile` and `read`; time inside a solve (an SMT2 `check-sat`, or a handle `randomize` that compiles) is part of `wall_ns` instead |
| `build` | stating the problem: the library's builder finalize, SMT2 commands such as `declare-const` and `assert`, and time a front end reports itself; reading SMT2 text is not counted, because in a pipe it includes waiting for the simulator |
| `compile` | compiling the problem for the CDCL engine |
| `read` | reading a model back: `dvs_solver_get_values`, SMT2 `get-value`, and time a front end reports itself |
| `previous` | each solve against the previous solve on the same thread |
| `same` | the same problem as the previous solve |
| `constants` | the same class as the previous solve, with other constants |
| `shape` | another class than the previous solve, or the thread's first solve |
| `root` | how constrained the problem was before the CDCL engine's first decision, summed over compiles (`count`) |
| `fixed` | `rand` variables fixed to one value |
| `tied` | `rand` variables compile made equal to another variable |
| `bits` | log2 of the product of the `rand` variables' declared domain sizes |
| `free_bits` | the same, before the first decision: after compile, propagation and shaving |
| `quiet` | constraints compiled into no propagator: into root bounds, or into equal variables |
| `dropped` | constraints compile could not handle |
| `binding` | which constraints did the work, summed over the CDCL solves of the values sample |
| `narrowed` | constraints that narrowed a variable's range in the solve |
| `conflicted` | constraints that took part in a conflict in the solve |
| `busy` | solves in which any constraint narrowed a range |
| `top_share` | the share of the narrowing done by the busiest constraint, in thousandths, summed over busy solves |
| `other_share` | the share done by `unique`, `dist` and soft constraints, and by constraints added after compile, in thousandths, summed over busy solves |
