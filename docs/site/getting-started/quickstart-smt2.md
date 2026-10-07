# Quick start: SMT-LIB2

`dv-solve-smt2` reads [SMT-LIB2](https://smt-lib.org/) from a file or from
standard input and answers each `check-sat` with `sat`, `unsat` or `unknown`.
`pip install dv-solve` puts it on `PATH`; see {doc}`install`.

## Solve a file

This file describes a word-aligned memory access that must stay inside a
256-byte window:

```{literalinclude} ../../examples/smt2/quickstart.smt2
:language: lisp
```

```bash
dv-solve-smt2 quickstart.smt2
```

```{literalinclude} ../../examples/smt2/quickstart.expected
:language: text
```

The exit code reflects the last `check-sat`: `0` for `sat`, `1` for `unsat`,
`2` for `unknown` or an error. See {doc}`../reference/cli`.

## Get a different solution each time

Without a seed, dv-solve returns the same solution every run, which is often
the smallest one. Set a seed to choose among the solutions:

```{literalinclude} ../../examples/smt2/random.smt2
:language: lisp
:lines: 3-4
```

```{literalinclude} ../../examples/smt2/random.expected
:language: text
```

The same seed always gives the same solution, so a run can be reproduced from
its seed. {doc}`../concepts/randomization-seeds` explains how solutions are
spread across the solution space.

## Use it interactively

With no file argument, `dv-solve-smt2` reads commands from standard input and
answers each one as soon as it arrives. That is how tools drive it over a
pipe:

```text
$ dv-solve-smt2
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (bvugt x #x10))
(check-sat)
sat
(get-value (x))
((x #b00010001))
(exit)
```

`push` and `pop` add and remove scopes of assertions between checks:

```{literalinclude} ../../examples/smt2/incremental.smt2
:language: lisp
```

```{literalinclude} ../../examples/smt2/incremental.expected
:language: text
```

## Next steps

- {doc}`../guides/smt2-solver` covers the supported logics, commands and
  limits.
- {doc}`../concepts/soundness` explains what `unknown` means.
