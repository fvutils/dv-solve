# Using dv-solve with Verilator

Verilator 5.x hands every `randomize()` call to an external SMT-LIB2 solver
process. This guide covers how to make that solver dv-solve, what to pass at
run time, how to confirm dv-solve is the one answering, and how to capture a
testcase when something goes wrong. For a first run, see
{doc}`../getting-started/quickstart-verilator`.

In short, with Verilator 5.054 or later and dv-solve 0.5.0 or later:

```bash
export VERILATOR_SOLVER=dv-solve-smt2
./obj_dir/Vtop +verilator+rand+sampler+solver
```

Older Verilator 5.x versions work too, with Verilator's own sampler; see
{ref}`verilator-default-sampler`.

## What you need

- Verilator 5.054 or later, built as usual, for the solver sampler. Nothing
  in Verilator has to be rebuilt.
- dv-solve 0.5.0 or later. `pip install 'dv-solve>=0.5.0'` puts
  `dv-solve-smt2` on `PATH` (see {doc}`../getting-started/install`). If the
  environment it was installed in is not active when the simulation runs, use
  its full path, which
  `python -c "import dv_solve; print(dv_solve.get_smt2_exe())"` prints.

With the solver sampler, `dv-solve-smt2` needs no options: it reads
Verilator's commands from the pipe and answers each one as it arrives.
`--mode=verilator` is for Verilator's default sampler (see
{ref}`verilator-default-sampler`).

(verilator-sampler)=
## Let dv-solve pick the values

Verilator has two ways of turning the solver's answers into the values
`randomize()` returns. It calls the component that does this the *sampler*.

`solver` (recommended)
: Verilator sends the constraints once per `randomize()` call, together with
  a seed taken from the object's random state, `(set-option :random-seed N)`,
  and uses the solution it gets back. dv-solve picks that solution at random
  from the seed, spread across all the solutions. One query per call, so calls
  are fast, and values follow `+verilator+seed` and the object's random
  stability from the first call on.

`default`
: Verilator asks the solver several times per call and samples among the
  answers itself: it adds random parity constraints over the variables' bits,
  pins bits to random values, and in 5.052 collects a batch of solutions to
  hand out over the following calls. This is how Verilator gets varied values
  out of solvers that always return the same solution. With dv-solve it mostly
  adds cost (see {ref}`verilator-default-sampler`).

The `solver` sampler is in Verilator 5.054 and later (pull request #8899).
Earlier versions have only the `default` sampler, and stop at
`+verilator+rand+sampler+solver` with `Unknown runtime argument`.

Choose it in any of these ways; the first one found is used:

1. `+verilator+rand+sampler+solver` on the simulation's command line;
2. a C++ harness calling
   `contextp->randSampler(VerilatedRandSampler::SAMPLER_SOLVER)`;
3. `VERILATOR_RAND_SAMPLER=solver` in the environment when the simulation
   starts;
4. `VERILATOR_RAND_SAMPLER=solver` in the environment when `verilator` runs,
   which builds it into the model as its default;
5. Verilator itself configured with `--with-rand-sampler=solver`.

The sampler and the solver are chosen independently: the `solver` sampler
with z3 or Bitwuzla returns much the same values on every call, since they do
not use the seed to vary their solutions.

## Choosing the solver

A compiled model finds its solver in this order:

1. a solver set by the C++ harness with `contextp->solverProgram(...)`;
2. the `VERILATOR_SOLVER` environment variable when the simulation starts;
3. the default built into the model, which is `VERILATOR_SOLVER` as it was
   set when `verilator` ran;
4. the default Verilator itself was built with, normally `z3 --in`.

Pick whichever fits your flow. The first one found is used.

### At run time

Set `VERILATOR_SOLVER` before running the simulation. Nothing in the build
changes, so the same model can run with dv-solve or with z3:

```bash
export VERILATOR_SOLVER=dv-solve-smt2
./obj_dir/Vtop +verilator+rand+sampler+solver
```

Verilator splits the value at each space and does not understand quotes, so
the path to `dv-solve-smt2` must not contain spaces.

(verilator-build)=
### In the build

To make dv-solve the model's default, so that nobody has to set anything when
running it, set `VERILATOR_SOLVER` when running `verilator`. The generated
makefile builds that command into the model as `VM_SOLVER_DEFAULT`, and
`VERILATOR_RAND_SAMPLER` likewise as `VM_RAND_SAMPLER_DEFAULT`.

```bash
export VERILATOR_RAND_SAMPLER=solver
export VERILATOR_SOLVER=dv-solve-smt2
verilator --binary top.sv
unset VERILATOR_SOLVER VERILATOR_RAND_SAMPLER
./obj_dir/Vtop          # dv-solve, solver sampler; the environment still overrides both
```

The model looks `dv-solve-smt2` up on `PATH` when it runs. A built-in command
with options, such as `--mode=verilator` for the default sampler, needs the
full path instead, `"$(command -v dv-solve-smt2) --mode=verilator"`: with a
bare name followed by options, the model fails to start it (see
{ref}`verilator-check`).

You can confirm what was built in: `grep _DEFAULT= obj_dir/*.mk`.
With a build system other than the generated makefile, define the macros
yourself when compiling `verilated.cpp`, for example
`-DVM_SOLVER_DEFAULT='"dv-solve-smt2"' -DVM_RAND_SAMPLER_DEFAULT='"solver"'`.

### From a C++ harness

A harness that builds its own `VerilatedContext` can set the solver before
the first `randomize()`:

```c++
contextp->solverProgram("dv-solve-smt2");
```

This overrides `VERILATOR_SOLVER`, so a user can no longer switch solvers
from the environment.

## Run-time options

None are required beyond choosing the sampler. Verilator's own plusargs
apply:

`+verilator+rand+sampler+solver`
: Lets dv-solve pick the values; see {ref}`verilator-sampler`.

`+verilator+seed+N`
: Sets the seed. The same seed repeats a run exactly, values included. With
  the `default` sampler, the first `randomize()` call of a run currently gives
  the same values whatever the seed; later calls vary with it. With the
  `solver` sampler every call follows the seed.

`+verilator+solver+file+FILE`
: Logs everything Verilator sends the solver, and the replies, to `FILE`
  (Verilator 5.048 and later).
  See {ref}`verilator-check` and {ref}`verilator-capture`.

dv-solve options go in the solver command. With the `solver` sampler none are
needed. With the `default` sampler, use `--mode=verilator`, and optionally
`--verilator-hash=ignore` (see {ref}`verilator-default-sampler`), for example
`VERILATOR_SOLVER="dv-solve-smt2 --mode=verilator --verilator-hash=ignore"`.
Environment variables such as `DV_LOG` are read by the solver process, which
inherits the simulation's environment, so set them before running the
simulation (see {doc}`../reference/environment`).

(verilator-check)=
## Checking that dv-solve is being used

Verilator says nothing about which solver it runs while things work, so check
one of these.

**The solver log.** With Verilator 5.048 or later, run with
`+verilator+solver+file+solver.log`. The log names the program Verilator
started on its `# Open:` line:

```bash
./obj_dir/Vtop +verilator+rand+sampler+solver +verilator+solver+file+solver.log
grep -A1 '# Open' solver.log
```

```text
# Open: dv-solve-smt2
  ; sampler: solver
```

The `; sampler:` line, sent by Verilator versions that have the solver
sampler, shows which sampler the run used. With `solver`, each `randomize()`
call in the log starts with a `(set-option :random-seed N)` line.

**dv-solve's own log.** Set `DV_LOG` to a file name. dv-solve appends one
line to it for every `check-sat`, naming the engine it chose: `route:`, or
`vlt-route:` with `--mode=verilator`. If the file stays empty or is never
created, dv-solve did not run:

```bash
DV_LOG=$PWD/dv-solve.log ./obj_dir/Vtop +verilator+rand+sampler+solver
grep -c 'route:' dv-solve.log
```

Use an absolute path: a relative one is taken from the directory the
simulation runs in.

**The failure message.** If Verilator cannot start the solver, it says so and
names the command it tried, and every `randomize()` returns 0:

```text
%Warning: Unable to communicate with SAT solver, please check its installation or specify a different one in VERILATOR_SOLVER environment variable.
 ... Tried: $ dv-solve-smt2\ --mode=verilator
```

A backslash before each space, as here, means a command with options was
built in without a full path (see {ref}`In the build <verilator-build>`
above). A `Tried:` line naming
`z3` or `bitwuzla` means `VERILATOR_SOLVER` was not set where the simulation
runs.

(verilator-default-sampler)=
## With Verilator's default sampler

Verilator 5.052 and earlier, and later versions run without
`+verilator+rand+sampler+solver`, sample the values themselves: after the
first solution they ask again with random parity constraints over the
variables' bits, with bits pinned to random values, or, in 5.052, with
earlier solutions blocked to collect a batch. For these, run dv-solve in
Verilator mode:

```bash
export VERILATOR_SOLVER="dv-solve-smt2 --mode=verilator"
```

`--mode=verilator` tells dv-solve that the queries are randomization
requests, not proofs: it uses the engine that spreads solutions most evenly,
and keeps recently used problems ready, since a testbench usually calls
`randomize()` on the same class many times. dv-solve answers all of
Verilator's queries correctly, but it already picks a random solution each
time, so the extra queries mostly add cost: they take most of the time of a
call. Where you can, use the `solver` sampler instead.

`--verilator-hash=ignore` skips the parity constraints: dv-solve answers each of those queries
with the solution it already found, which satisfies every constraint of the
class. Only Verilator's request for variety is not honoured.

```bash
export VERILATOR_SOLVER="dv-solve-smt2 --mode=verilator --verilator-hash=ignore"
```

Calls get much cheaper. Whether the values spread as well depends on the
constraints: the parity constraints also even out what dv-solve's own choice
leaves uneven. The {doc}`../results/randomization` page measures both settings
on every benchmark, so you can see where each one stands.

## When the constraints can't be met

If the constraints contradict each other, `randomize()` returns 0, and
Verilator warns about the constraints involved. For this class:

```{literalinclude} ../../examples/verilator/bad.sv
:language: systemverilog
:lines: 1-6
```

the simulation prints:

```text
%Warning-UNSATCONSTR: bad.sv:3: Unsatisfied constraint: 'constraint c1 { x > 10; }'
%Warning-UNSATCONSTR: bad.sv:5: Unsatisfied constraint: 'constraint c3 { x < 5; }'
randomize() failed
```

The warnings name a minimal conflicting set: `c1` and `c3` can't both hold,
and removing either one would make the rest satisfiable. `c2` is not listed
because it plays no part in the conflict.

## When dv-solve can't decide

If a constraint is beyond what dv-solve supports, dv-solve answers `unknown`
(see {doc}`../concepts/soundness`, which lists the limits). For example, a
random variable wider than 65535 bits:

```{literalinclude} ../../examples/verilator/unknown.sv
:language: systemverilog
:lines: 1-4
```

Verilator 5.054 reports that as

```text
%Warning: .../verilated_random.cpp:509: Solver returned unknown (timed out or incomplete), so randomize() may return 0; warned once
```

and `randomize()` returns 0. The line number in the message depends on the
Verilator version.

To see why dv-solve answered `unknown`, set `DV_LOG` to a file name before
running the simulation. dv-solve writes the reason for each `unknown` there,
next to the `route:` line for each `check-sat`, which names the engine it
was routed to and why (with `--mode=verilator`, the `vlt-route:` line; for
example `vlt-route: bitblast (enumeration)` for the batch of solutions
Verilator 5.052 collects).

(verilator-capture)=
## Capturing a testcase

When `randomize()` fails, answers `unknown`, gives values that break a
constraint, is slow, or gives values that don't vary, the most useful thing to
send with a report is the exact conversation between Verilator and dv-solve.
It replays without Verilator, the testbench or the design: dv-solve answers
the same input the same way every time, so a captured session reproduces the
problem on any machine with the same dv-solve version.

### Recording the session

Run the simulation with this wrapper as the solver:

```{literalinclude} ../../examples/verilator/dv-solve-capture
:language: sh
```

```bash
export DVS_CAPTURE=$PWD/capture/run          # where to put the files
mkdir -p capture
VERILATOR_SOLVER=/path/to/dv-solve-capture ./obj_dir/Vtop +verilator+rand+sampler+solver +verilator+seed+1
ls capture
```

```text
run.3396371.out  run.3396371.smt2
```

`run.PID.smt2` is everything Verilator sent, byte for byte; `run.PID.out` is
everything dv-solve answered. Options after the wrapper's name are passed on
to `dv-solve-smt2`, so a session with the default sampler is captured with
`VERILATOR_SOLVER="/path/to/dv-solve-capture --mode=verilator"`.
Verilator starts a new solver process if one fails, so a run can leave more
than one pair; each pair replays on its own.

To keep the files small, capture the shortest run that shows the problem:
stop the test soon after the bad `randomize()` call, and note the seed.

Verilator's `+verilator+solver+file+` log also records the session, with
simulation times, which helps to find *when* something went wrong. It is not
a replayable input, though: Verilator writes each buffer it sends as a line of
its own, so long commands are split mid-token. Use it to read, and the wrapper
to capture.

### Replaying it

```bash
dv-solve-smt2 capture/run.3396371.smt2 > replay.out
cmp replay.out capture/run.3396371.out && echo same
```

The replay gives the same answers and the same values as the simulation did,
as long as the options (add `--mode=verilator` if the session used it) and
the dv-solve version are the same.
`dv-solve-smt2 --version` prints the version. Compare the output, not the
exit status: a session usually ends with Verilator's `(reset)`, after which
there is no answer to report, so `dv-solve-smt2` exits with 2 (see
{doc}`../reference/cli`).

### Checking the answers against another solver

A session recorded with the `solver` sampler compares as it is.
`--mode=verilator` deliberately does not hold the default sampler's variety
requests to their letter (see {ref}`verilator-default-sampler`), so in that
mode some answers differ from another solver's on purpose; replay such a
session without the option. Then compare the `sat`/`unsat` lines with z3's:

```bash
dv-solve-smt2 capture/run.3396371.smt2 > dvs.out
z3 capture/run.3396371.smt2 > z3.out
diff <(grep -xE 'sat|unsat|unknown' dvs.out) <(grep -xE 'sat|unsat|unknown' z3.out)
```

The two lists should be the same, except that dv-solve may answer `unknown`
where z3 answers. A `sat` against an `unsat` is a bug in one of the two
solvers; please report it. The values themselves will differ, since each
solver picks its own solution.

### Cutting it down to one query

A session holds every `randomize()` call of the run. The first query, the
class's constraints followed by a single `check-sat`, is often enough on its
own, and much easier to read:

```bash
awk '/^\(set-option :produce-models/ {p=1} p {print} p && /^\(check-sat\)$/ {exit}' \
    capture/run.3396371.smt2 > first.smt2
dv-solve-smt2 first.smt2
```

When the problem is in a later call, cut out the commands from the `(reset)`
before it up to its `check-sat` in the same way.

With the `solver` sampler a session is far smaller, one query per call, and
each query carries its `(set-option :random-seed N)`. A query cut out this
way, with a `(get-value (...))` of the variables added after its `check-sat`,
gives the same values the simulation got.

### What to send

- the capture files (`.smt2` and `.out`), or a query cut down from them;
- the output of `dv-solve-smt2 --version` and `verilator --version`;
- the solver command (`VERILATOR_SOLVER`), the sampler, and the simulation's
  plusargs, seed included;
- what went wrong: the message Verilator printed, or the values you expected
  and the ones you got;
- the `DV_LOG` file of the run, if you set one.

Report issues at <https://github.com/fvutils/dv-solve/issues>.

A capture contains the names of your classes' random variables and the
constraints over them, as Verilator translated them. Look through it before
sharing it outside your organisation. Renaming the variables consistently
throughout (`declare-fun`, `assert` and `get-value` lines alike) keeps it
usable.
