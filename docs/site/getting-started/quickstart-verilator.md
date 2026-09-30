# Quick start: Verilator

[Verilator](https://verilator.org/) solves the constraints in a
SystemVerilog `randomize()` call by running an external SMT solver, named by
the `VERILATOR_SOLVER` environment variable (z3 by default). `dv-solve-smt2`
can take its place.

You need Verilator 5.x and a `dv-solve-smt2` built from source (see
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
export VERILATOR_SOLVER="/path/to/dv-solve-smt2 --interactive --mode=verilator"
./obj_dir/Vpacket
```

```text
addr=000003dc len=14 kind=14
addr=000002a8 len=15 kind=11
addr=000002dc len=3 kind=5
addr=00000a44 len=15 kind=5
addr=00000fa8 len=11 kind=3
```

Both options matter: `--interactive` answers each command as Verilator sends
it, and `--mode=verilator` tells dv-solve that its job is randomization (see
{doc}`../guides/verilator`).

## Reproducing a run

Values follow Verilator's seed. The same `+verilator+seed+N` reproduces the
same values, and a different seed gives different ones:

```bash
./obj_dir/Vpacket +verilator+seed+1
```
