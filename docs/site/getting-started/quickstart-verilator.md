# Quick start: Verilator

[Verilator](https://verilator.org/) solves the constraints in a
SystemVerilog `randomize()` call by running an external SMT solver, named by
the `VERILATOR_SOLVER` environment variable (z3 by default). `dv-solve-smt2`
can take its place.

You need Verilator 5.x and dv-solve. `pip install dv-solve` puts
`dv-solve-smt2` on `PATH` (see {doc}`install`).

## A class to randomize

```{literalinclude} ../../examples/verilator/packet.sv
:language: systemverilog
```

## Build and run

Compile with Verilator as usual. Nothing in the build refers to the solver:

```bash
verilator --binary packet.sv
```

Point `VERILATOR_SOLVER` at dv-solve when you run the simulation:

```bash
export VERILATOR_SOLVER="dv-solve-smt2 --interactive --mode=verilator"
./obj_dir/Vpacket +verilator+rand+sampler+solver
```

```text
addr=0000088c len=3 kind=14
addr=00000580 len=13 kind=11
addr=00000dd4 len=14 kind=2
addr=00000e98 len=16 kind=10
addr=000000d8 len=12 kind=2
```

`+verilator+rand+sampler+solver` lets dv-solve pick each value from the seed
Verilator passes it. Verilator 5.052 and earlier don't have it and stop with
`Unknown runtime argument`; leave it out there, and Verilator samples the
values itself (see {ref}`verilator-sampler`).

If the environment dv-solve is installed in is not active, name the
executable by its full path, which `python -c "import dv_solve;
print(dv_solve.get_smt2_exe())"` prints.

Both options matter: `--interactive` answers each command as Verilator sends
it, and `--mode=verilator` tells dv-solve that its job is randomization (see
{doc}`../guides/verilator`).

## Reproducing a run

Values follow Verilator's seed. The same `+verilator+seed+N` reproduces the
same values:

```bash
./obj_dir/Vpacket +verilator+rand+sampler+solver +verilator+seed+1
```

## Next

{doc}`../guides/verilator` covers choosing the sampler, building dv-solve in
as the model's default solver, checking that dv-solve is the one answering, and capturing a testcase
to report a problem.
