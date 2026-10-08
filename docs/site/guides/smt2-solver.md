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
| `set-logic` | the logics above; once per session, or again after `reset` |
| `set-option` | `:produce-models`, `:produce-unsat-cores`, `:produce-assertions`, `:print-success`, `:seed`; other options are accepted and ignored |
| `get-option` | the options above; `unsupported` for any other |
| `set-info` | accepted and ignored |
| `declare-const`, `declare-fun` | Bool, `(_ BitVec n)` for n up to 192, arrays of bit-vectors, and functions over them; a name can be declared once per scope |
| `declare-sort` | arity 0 (opaque sorts) |
| `declare-datatypes` | single-constructor records whose fields are Bool or bit-vectors |
| `define-fun`, `define-sort` | yes; like declarations, they are removed by the `pop` of the level they were made in |
| `assert` | yes |
| `get-assertions` | the assertions in scope, as written; needs `:produce-assertions` in `--mode=verilator` |
| `check-sat` | yes |
| `check-sat-assuming` | yes |
| `get-value`, `get-model` | after `sat` |
| `get-unsat-assumptions` | after `check-sat-assuming` returns `unsat` |
| `get-unsat-core` | after `unsat`; see below |
| `get-info` | `:name`, `:version`, `:authors` |
| `push`, `pop` | yes, including nested scopes |
| `reset` | yes |
| `reset-assertions` | yes |
| `echo` | yes |
| `exit` | yes |

Not supported: `get-proof`, `get-assignment`,
`declare-datatype` (singular), and datatypes with more than one constructor.
An unsupported command is answered with an `(error ...)`; see
[Errors](#errors). An unsupported query (a `get-` command) changes nothing, so
later `check-sat` commands still answer.

With `(set-option :print-success true)`, every command that has no other
response answers `success`.

`get-unsat-core` lists the `:named` assertions in an unsatisfiable set. With
`:produce-unsat-cores` set to `true` the list is minimal: removing any one of
the names leaves a satisfiable set. dv-solve finds it by re-solving without
each name in turn, which takes up to 10 seconds; if it runs out of time, the
list is still unsatisfiable but may not be minimal. Without the option, and
after `check-sat-assuming`, the list is every named assertion in scope.

`reset-assertions` removes every assertion and pops every `push` level. It
keeps the declarations, definitions and options made outside any level.

## Expressions

Supported bit-vector operators:

| Kind | Operators |
|---|---|
| arithmetic | `bvadd`, `bvsub`, `bvmul`, `bvneg`, `bvudiv`, `bvurem`, `bvsdiv`, `bvsrem`, `bvsmod` |
| bitwise | `bvand`, `bvor`, `bvxor`, `bvnot`, `bvnand`, `bvnor`, `bvxnor`, `bvredor`, `bvredand` |
| shifts | `bvshl`, `bvlshr`, `bvashr`, `rotate_left`, `rotate_right` |
| comparison | `bvult`, `bvule`, `bvugt`, `bvuge`, `bvslt`, `bvsle`, `bvsgt`, `bvsge`, `bvcomp` |
| overflow | `bvnego`, `bvuaddo`, `bvsaddo`, `bvumulo`, `bvsmulo`, `bvusubo`, `bvssubo`, `bvsdivo` (`bvumulo` and `bvsmulo` up to 96 bits) |
| structure | `concat`, `extract`, `zero_extend`, `sign_extend`, `repeat` |

Also supported: `=`, `distinct`, `ite`, `let`, the Boolean connectives,
arrays (`select`, `store`, `(as const ...)`) and calls to `define-fun`
definitions.

Not yet supported (a problem that uses them gets `unknown`): `bvnand`, `bvnor`, `bvxnor`, `bvcomp`, `rotate_left` and `rotate_right`.

Bit-vector widths, constants included, up to 192 bits are supported.
See {doc}`../concepts/soundness` for what happens beyond these limits.

## Batch and interactive input

A file given on the command line is read whole (batch mode). Standard input is
read one command at a time, and each command is answered as soon as it is
complete (interactive mode), which is what a tool driving the solver over a
pipe needs. `--batch` and `--interactive` override the default.

## Output

`check-sat` prints `sat`, `unsat` or `unknown`. Values are printed in binary:

```{literalinclude} ../../examples/smt2/random.expected
:language: text
:lines: 2-
```

Bool values print as `true` and `false`. `get-value` accepts any term, not
only names: calls to `define-fun` definitions, `let`, every bit-vector
operator (including `bvnand`, `bvnor`, `bvxnor`, `bvcomp`, the rotations and
the overflow predicates such as `bvuaddo`), and arrays. Each term is echoed as
written, with bit-vector literals in `#x` form when their width is a multiple
of 4.

## Model check

Before `check-sat` answers `sat`, dv-solve evaluates every assertion in scope,
as written, under the model it found. The evaluator is separate from the code
that translates assertions for the solving engines, so a translation error
cannot hide from it. If an assertion is false under the model, the answer is
`unknown` and `(get-info :reason-unknown)` says which assertion. The check is
on by default, and off in `--mode=verilator`; `DV_CHECK_MODEL` turns it on or
off (see {doc}`../reference/environment`).

## Errors

An error in the input is answered on standard output, as SMT-LIB specifies:

```text
(error "line 10 column 31: unexpected ')'")
```

This covers malformed s-expressions, malformed commands, and commands or forms
dv-solve does not support. After an error, dv-solve keeps reading (so a tool
driving it over a pipe stays in step), but every later `check-sat` answers
`unknown`, because the meaning of the bad input is not known.
`(get-info :reason-unknown)` names the error, `(reset)` starts afresh, and the
exit status is `4`.

A `get-value` or `get-model` that cannot be answered (no model, or a term
dv-solve cannot evaluate) is also an `(error ...)`, but leaves the assertions
alone. dv-solve never prints a placeholder value.

A construct that is well formed but not supported inside a term, such as an
unsupported operator, makes `check-sat` answer `unknown` without an error;
`(get-info :reason-unknown)` says which construct.

## Diagnostics

Warnings and statistics are not printed unless asked for: `--stats` or `-v`
print them to standard error, and `DV_LOG` sends them to a file. Nothing is
written to standard error otherwise, because some tools (yosys-smtbmc) read it
as part of the response stream.
