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
./obj_dir/Vpacket
```

```text
addr=00000a1c len=5 kind=14
addr=000000e0 len=10 kind=9
addr=00000a44 len=3 kind=4
addr=00000cb4 len=1 kind=5
addr=00000b14 len=16 kind=5
```

If the environment dv-solve is installed in is not active, name the
executable by its full path, which `python -c "import dv_solve;
print(dv_solve.get_smt2_exe())"` prints.

Both options matter: `--interactive` answers each command as Verilator sends
it, and `--mode=verilator` tells dv-solve that its job is randomization (see
{doc}`../guides/verilator`).

## Reproducing a run

Values follow Verilator's seed. The same `+verilator+seed+N` reproduces the
same values, and a different seed gives different ones:

```bash
./obj_dir/Vpacket +verilator+seed+1
```
