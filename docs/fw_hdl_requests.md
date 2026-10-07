# Requests from fw-hdl (2026-10-03)

fw-hdl (featherweight-hdl) has started using `dv-solve-smt2` to prove properties
of SystemVerilog functions for all inputs, before synthesis. It does the same
thing XLS does with `#[quickcheck]` and `prove_quickcheck_main`. The flow:

1. fw-hdl lowers the SV to an unrolled bit-vector dataflow graph.
2. It prints that graph as QF_BV: one `define-fun` per SV function, and the
   property as an assertion of its negation.
3. `unsat` means the property is proven. `sat` comes with a counterexample, read
   back with `get-value`.

The same encoding also serves inverse queries (find inputs for a given output) and
constrained-random stimulus.

The first design is crc32 (`fw-hdl/examples/xls/crc32`, prototype in
`fw-hdl/spikes/formal-props/`). It produced the right verdicts: the affine
property proven, a false property refuted with the right counterexample. Along the
way it hit the first three issues below; R4 came with the codegen check (2026-10-04).

Build tested: `10e8bdc` (`build/dv-solve-smt2`).

**Testcases:** the `.smt2` files are in `tests/formal/smt2/fw_hdl/`, and the tests in
`tests/formal/test_fw_hdl_requests.py`:

```
pytest tests/formal/test_fw_hdl_requests.py                 # 31 pass, 19 strict-xfail (R1 evaluation) on feat/fw-hdl-frontend
python tests/formal/test_fw_hdl_requests.py --bench          # R3 timings
```

The R1, R2 and R4 tests are `xfail(strict=True)`: they start passing when the request
is met, and strict mode fails the run until the marker is removed.

---

## R1 (high). `get-value` on terms: echo the term, evaluate it or say you can't

> **Status 2026-10-07:** asks 1 and 2 done (branch `feat/fw-hdl-frontend`): terms are
> echoed z3-style, and a term that can't be evaluated gets `(error "...")`, never a
> value. Ask 3 (evaluate `define-fun` applications and the missing operators) is
> Phase 2 of `docs/fw_hdl_implementation_plan.md`.

`(get-value (t))` with `t` a term rather than a symbol has two problems:

- The response prints `?` where the term should be. SMT-LIB 2.6 §4.2.6 says the
  response echoes each term, and consumers pair values with terms by that echo.
- Any term `_eval_sexpr` doesn't handle comes back as **`#b0`**, with no
  diagnostic and exit status 0. That `#b0` can't be told apart from a real value.

Every engine behaves the same. This is what hit fw-hdl. After the counterexample
`a = #x31`, the natural next query is the design's output at that point,
`(get-value ((|main| a)))`. The right answer is `#x83DCEFB7`; dv-solve says
`((? #b0))`.

Minimal case, `get_value_terms.smt2`. Here `f` is a `define-fun` with a parameter,
`k` one without, and `a = #x5`:

| query | z3 | dv-solve |
|---|---|---|
| `a`, `k` | `#x5`, `#x6` | right |
| `(bvadd a #x1)` | `((bvadd a #x1) #x6)` | `(? #b0110)`: right value, no echo |
| `(f a)`, `(f #x2)`, `(f (f a))` | `#x6`, `#x3`, `#x7` | `(? #b0)` ×3: **wrong** |
| `(bvneg a)`, `(bvlshr a #x1)`, `(bvxor a a a)` | `#xb`, `#x2`, `#x5` | `(? #b0)` ×3: **wrong** |
| `(= a #x5)` | `true` | `(? #b1)`: a Bool printed as a bit-vector |

**Where:** `_cmd_get_value` in `src/c/smt2/smt2_frontend.c`, around line 5372, does
two things wrong:

- the list branch prints `"(? "`, although `_fprint_sexpr` is right there to echo
  the term;
- the fallback after `_eval_sexpr` fails also prints `#b0`.

`_eval_sexpr` itself has no case for applying a parameterized `define-fun`, nor
for `bvneg`, the shifts, n-ary `bvand`/`bvor`/`bvxor`/`bvadd`, the signed
compares, and so on.

**Ask, in order of value:**

1. Never print a placeholder value. If a term can't be evaluated, respond with
   `(error "...")`, as the standard says.
2. Echo the term.
3. Evaluate parameterized `define-fun` applications by substituting the
   arguments, plus the missing operators.

Alternatively, evaluate the term by bit-blasting it against the model. That also
gives full coverage.

## R2 (high). Report malformed input, and never answer after it

> **Status 2026-10-07: done** (branch `feat/fw-hdl-frontend`). Parse errors are
> `(error "line L column C: ...")` on stdout; after one, check-sat answers `unknown`;
> exit status 4. Exit codes are in `--help` (0 sat, 1 unsat, 2 unknown, 3 oracle
> abort, 4 input error, 5 usage, 6 internal; `--exit-codes=satcomp` for 10/20).
> Root causes: B75 and B76 in `docs/solver_bug_backlog.md`.

An unbalanced `)` is never reported. Depending on the surrounding text, dv-solve
does one of two things:

- `unbalanced_parens.smt2`: a stray `)` after `declare-const`. dv-solve ignores it
  and prints `sat`. z3 prints `(error "line 10 column 30: invalid command, '('
  expected")` first.
- `unbalanced_parens_assert.smt2`: a stray `)` after an `assert`. dv-solve prints
  **nothing**, not even the `check-sat` result, and exits 0. Which variant you get
  even depends on the comment header: the same commands under a header that
  contains parentheses print `sat`.

Both cases exit 0, the exit code `smt2_main.c` uses for sat. For a
tool used as a prover this is the dangerous direction. fw-hdl hit it from a
generator bug: two files were concatenated without a newline, which put a stray
fragment in the query. A wrapper that treats "no `sat` on stdout" as "proven" would
have reported a proof.

**Ask:**

- Report every parse error: `(error "...")` on stdout, as z3 does, or a message on
  stderr.
- Make the exit status nonzero and distinct from the sat/unsat codes. 2
  ("unknown") would do; something else is better.
- Document the exit codes in `--help`. Today they aren't (0 sat, 1 unsat,
  2 unknown/error). Note that `set -e` scripts stop at the first `unsat`, which is a
  successful proof.

**Added 2026-10-04** (with R4): `define_sort_malformed.smt2`, a `define-sort`
whose name is not a symbol. dv-solve ignores the line and answers. fw-hdl
generated it by a bug while writing the R4 workaround, and dv-solve proved the
query anyway; this time the ignored line was harmless.

## R3 (medium). Engine routing on unrolled XOR/shift datapaths

> **Status 2026-10-07:** not started; Phases 3–4 of `docs/fw_hdl_implementation_plan.md`.

Proofs over unrolled arithmetic, such as CRCs, hashes, LFSRs and checksums, are the
main fw-hdl workload. On them, the default route (CDCL) is 3–14× slower than the
bit-blast engine. Bit-blast already beats z3 and bitwuzla on the harder case;
boolector shows how far off the floor is.

| | dv-solve auto | cdcl | bitblast | z3 | boolector | bitwuzla |
|---|---|---|---|---|---|---|
| `crc32_affine2` (2 vars × 8 b) | 10.8 s | 10.7 s | **0.8 s** | 1.5 s | 0.05 s | 0.7 s |
| `crc32_affine3` (3 vars × 8 b) | 15.4 s | 15.7 s | **5.8 s** | 24.0 s | 0.08 s | >120 s |

All of them answer `unsat` (correctly). Timings are from
`python tests/formal/test_fw_hdl_requests.py --bench`.

CDCL does not escalate here. It proves the property itself, slowly.
`test_crc32_verdicts` pins the verdict on every engine, so routing changes can't
regress soundness.

**Ask:**

- Route this shape to bit-blast by default. One cue: an unsat-seeking query (a
  negated equality over `define-fun` chains) whose operators are almost all
  `bvxor`/`bvand`/shifts/`bvneg`. The alternative is a short CDCL budget before
  escalating.
- Longer term, look at what boolector does here. The likely candidates are term
  rewriting or structural hashing before bit-blasting, or XOR/Gaussian reasoning
  in SAT. The miters are affine over GF(2).

## R4 (medium, added 2026-10-04). `define-sort`

> **Status 2026-10-07: done** (branch `feat/fw-hdl-frontend`): `define-sort` without
> parameters, in every sort position, on every engine.

fw-hdl now checks XLS's code generator: the optimized IR function against the
pipelined Verilog, unrolled over the pipeline's latency
(`fw-hdl/python/fw/hdl/formal/codegen_equiv.py`). The Verilog side comes from
Yosys's `write_smt2 -stbv`. That output names the module's state with a sort
alias:

```
(define-sort |crc32_pkg__main_s| () (_ BitVec 75))
```

dv-solve does not take `define-sort`. It prints `unknown` and exits 2, with no
diagnostic (`define_sort.smt2`), so the answer looks like a solver giving up,
not like unsupported input.

fw-hdl works around it by writing the alias out before it sends the query.
With that, dv-solve proves crc32's codegen check in 0.01 s (z3 agrees, and both
find the counterexample when the latency is wrong).

**Ask:**

- Support `define-sort` with no parameters, an alias of a bit-vector sort. That
  is all Yosys writes.
- Until then, an `(error "unsupported command define-sort")`, not `unknown`.

## R5 (low, added 2026-10-07). `get-value` prints Bool as `#b1`

> **Status 2026-10-07: done** (branch `feat/fw-hdl-frontend`; backlog B32).

`(get-value (b))` with `b` declared `Bool` prints `#b1`/`#b0`. SMT-LIB 2.6 says `true` /
`false`, and z3 and bitwuzla print those. Harnesses that parse models across solvers
trip on it; the use-case suite (`docs/usecase_benchmark_design.md`) parses models from
every solver.

Repro: `tests/formal/smt2/usecase/dma/dma2_sstruct.smt2`. Its `stall` is a Bool, and
dv-solve prints `(stall #b1)`. Seen on `build/dv-solve-smt2` built 2026-10-02; recheck
on HEAD.

**Ask:** print declared-Bool symbols and Bool terms as `true`/`false`.

## Not a request: sampling diversity over SMT-LIB2

We checked: 60 seeds of `(set-option :seed N)` on "m in 'A'..'Z' with
crc(m)[1:0] == 0" produced all 6 solutions, 6–15 times each (10 expected,
χ² ≈ 7, df 5). That is good enough for stimulus. `DV_FAIR_PICK=1` made no
difference with a single variable.
