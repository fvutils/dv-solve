# The SMT-LIB2 solver

`dv-solve-smt2` implements the parts of [SMT-LIB 2.6](https://smt-lib.org/)
that verification tools use: quantifier-free bit-vectors and arrays, with
uninterpreted functions. This page lists what it supports. For command-line
options and exit codes, see {doc}`../reference/cli`.

## Logics

| `set-logic` | Contents |
|---|---|
| `QF_BV` | bit-vectors |
| `QF_ABV` | bit-vectors and arrays |
| `QF_UFBV` | bit-vectors and uninterpreted functions |
| `QF_AUFBV` | bit-vectors, arrays and uninterpreted functions |
| `ALL` | accepted; the problem must still fit the theories above |

Any other logic is an error. The logic also selects the solving engine; see
{doc}`../concepts/engines`.

## Commands

| Command | Support |
|---|---|
| `set-logic` | the logics above |
| `set-option` | `:produce-models`, `:seed`; other options are accepted and ignored |
| `set-info` | accepted and ignored |
| `declare-const`, `declare-fun` | Bool, `(_ BitVec n)` for n up to 128, arrays of bit-vectors, and functions over them |
| `declare-sort` | arity 0 (opaque sorts) |
| `declare-datatypes` | single-constructor records whose fields are Bool or bit-vectors |
| `define-fun` | yes |
| `assert` | yes |
| `check-sat` | yes |
| `check-sat-assuming` | yes |
| `get-value`, `get-model` | after `sat` |
| `get-unsat-assumptions` | after `check-sat-assuming` returns `unsat` |
| `get-info` | `:name`, `:version`, `:authors` |
| `push`, `pop` | yes, including nested scopes |
| `reset` | yes |
| `reset-assertions` | accepted; see below |
| `echo` | yes |
| `exit` | yes |

Not supported: `get-unsat-core`, `get-proof`, `get-assignment`,
`declare-datatype` (singular), and datatypes with more than one constructor.
An unsupported command writes an error to the diagnostic stream and produces
no response on standard output; the session carries on.

`reset-assertions` is accepted but has no effect. Use `push`/`pop` to retract
assertions, or `reset` to start again.

## Expressions

Supported bit-vector operators:

| Kind | Operators |
|---|---|
| arithmetic | `bvadd`, `bvsub`, `bvmul`, `bvneg`, `bvudiv`, `bvurem`, `bvsdiv`, `bvsrem`, `bvsmod` |
| bitwise | `bvand`, `bvor`, `bvxor`, `bvnot` |
| shifts | `bvshl`, `bvlshr`, `bvashr` |
| comparison | `bvult`, `bvule`, `bvugt`, `bvuge`, `bvslt`, `bvsle`, `bvsgt`, `bvsge` |
| structure | `concat`, `extract`, `zero_extend`, `sign_extend`, `repeat` |

Also supported: `=`, `distinct`, `ite`, `let`, the Boolean connectives,
arrays (`select`, `store`, `(as const ...)`) and calls to `define-fun`
definitions.

Not yet supported (a problem that uses them gets `unknown`): `bvnand`, `bvnor`, `bvxnor`, `bvcomp`, `rotate_left` and `rotate_right`.

Bit-vector widths up to 128 bits are supported. Constants must fit in 64 bits.
See {doc}`../concepts/soundness` for what happens beyond these limits.

## Batch and interactive input

A file given on the command line is read whole (batch mode). Standard input is
read one command at a time, and each command is answered as soon as it is
complete (interactive mode), which is what a tool driving the solver over a
pipe needs. `--batch` and `--interactive` override the default.

## Output

`check-sat` prints `sat`, `unsat` or `unknown`. Values are printed in binary:

```text
((addr #b00000000000000000000000011001000)
 (len #b00001010))
```

## Diagnostics

Errors and warnings go to standard error when standard output is a terminal.
When standard output is a pipe or a file, they are appended to a log file
instead, so they can never be mistaken for a solver response by a tool reading
the output. The log is `/tmp/dv-solve.log` unless `DV_LOG` names another file.
