# `dv-solve-smt2` command line

```text
dv-solve-smt2 [options] [file.smt2]
```

With no file, commands are read from standard input.

`pip install dv-solve` installs the executable into the environment's `bin/`
directory on Linux and macOS (not yet on Windows). It is statically linked
against the solver, so it needs nothing from the Python package at run time.
From Python, `dv_solve.get_smt2_exe()` returns its path: the installed
wheel's copy first, then a source checkout's `build/bin`, then `PATH`. A
source build puts it in `build/` (and `build/bin` after `cmake --install`).

## Options

`--engine=E`
: Choose the solving engine: `auto` (the default), `cdcl` or `bitblast`.
  See {doc}`../concepts/engines`.

`--batch`
: Read standard input as a whole before processing it. This is the default
  for a file argument.

`--interactive`
: Process each command as soon as it is complete. This is the default for
  standard input.

`--stats`
: Explain `unknown` results: write the reason to the diagnostic stream.

`--mode=verilator`
: Serve Verilator's `randomize()` calls. See {doc}`../guides/verilator`.

`--verilator-hash=H`
: With `--mode=verilator`: `honor` (the default) solves the random parity
  constraints Verilator adds to each `randomize()` call; `ignore` skips them
  and keeps the solution already found. See {doc}`../guides/verilator`.

`--version`
: Print the version and exit.

`--help`
: Print a usage summary and exit.

`--smt2`, `--no-incremental`
: Accepted for compatibility with tools that pass them to other solvers. They
  have no effect.

## Exit codes

| Code | Meaning |
|---|---|
| `0` | the last `check-sat` returned `sat`, or there was no `check-sat` |
| `1` | the last `check-sat` returned `unsat` |
| `2` | the last `check-sat` returned `unknown`, or a command failed |

## Diagnostics

Diagnostics go to standard error when standard output is a terminal, and are
appended to a log file otherwise. See {doc}`environment`.
