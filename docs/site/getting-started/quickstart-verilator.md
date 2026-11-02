# Quick start: Verilator

[Verilator](https://verilator.org/) solves the constraints in a
SystemVerilog `randomize()` call by running an external SMT solver, named by
the `VERILATOR_SOLVER` environment variable (z3 by default, or Bitwuzla in
some binary builds). `dv-solve-smt2` can take its place.

You need Verilator 5.054 or later and dv-solve 0.5.0 or later.
`pip install 'dv-solve>=0.5.0'` puts `dv-solve-smt2` on `PATH` (see
{doc}`install`).

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
export VERILATOR_SOLVER=dv-solve-smt2
./obj_dir/Vpacket +verilator+rand+sampler+solver
```

```text
addr=00000aa0 len=7 kind=12
addr=00000f10 len=11 kind=15
addr=000003a8 len=12 kind=5
addr=00000ed0 len=16 kind=7
addr=00000afc len=14 kind=10
```

`+verilator+rand+sampler+solver` lets dv-solve pick each value from the seed
Verilator passes it, and `dv-solve-smt2` needs no options with it. Verilator
5.052 and earlier don't have it and stop with `Unknown runtime argument`;
leave it out there, and run dv-solve in Verilator mode instead (see
{ref}`verilator-default-sampler`).

If the environment dv-solve is installed in is not active, name the
executable by its full path, which `python -c "import dv_solve;
print(dv_solve.get_smt2_exe())"` prints.

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
