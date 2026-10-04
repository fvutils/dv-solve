# Soundness and `unknown`

dv-solve gives one of three answers to a satisfiability question:

`sat`
: The constraints can be met. A model (the values found) is available.

`unsat`
: The constraints cannot be met.

`unknown`
: dv-solve could not decide. This is an honest answer, not a guess.

**`sat` and `unsat` are meant to be definitive.** dv-solve is designed never
to report `unsat` for a problem that has a solution, and never to report `sat`
with a model that breaks a constraint. When it can't be sure, it answers
`unknown`. A wrong `sat` or `unsat` is a bug; please report it with the input
that produced it.

Before a `sat` answer is reported, the model is checked against every
top-level constraint. If the check fails, the answer becomes `unknown`
instead. dv-solve is also tested continuously against other solvers on
generated problems.

## When dv-solve says `unknown`

- **The problem uses something dv-solve doesn't support.** dv-solve works on
  bit-vectors, arrays of bit-vectors, Booleans and uninterpreted functions.
  Problems that need other theories, such as integer or real arithmetic,
  strings or quantifiers, get `unknown`. So do a few bit-vector cases,
  listed below.
- **The problem is too hard for the time available.** A solve that runs out of
  its budget reports `unknown` (`SOLVE_TIMEOUT` in the Python and C APIs).

Current bit-vector limits that produce `unknown`:

| Construct | Limit |
|---|---|
| Bit-vector width, including constants | up to 192 bits |
| Array indices | arrays are expanded element by element, so an array with a wide index sort (for example 32 bits) that is read at a symbolic index is not supported |

## Finding out why

`dv-solve-smt2 --stats` writes the reason for each `unknown` to the diagnostic
stream. See {doc}`../reference/cli`.
