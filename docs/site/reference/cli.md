# `dv-solve-smt2` command line

```text
dv-solve-smt2 [options] [file.smt2]
```

With no file, commands are read from standard input.

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
: Behave as Verilator's external constraint solver. See the Verilator guide.

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
