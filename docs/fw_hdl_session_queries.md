# fw-hdl session queries: get-value, get-model, reason-unknown (plan pieces 6.2, 6.3)

Measured 2026-10-09 on the development machine. Plan:
`docs/fw_hdl_implementation_plan.md` §4 Phase 6.

## Question

After a `sat`, fw-hdl reads the counterexample: many terms with `get-value`,
or the whole model with `get-model`. After an `unknown`, it asks why with
`(get-info :reason-unknown)`. Piece 6.2 asked whether reading many terms is
fast and complete; piece 6.3 asked that every `unknown` give a reason.

## 6.2: get-value and get-model

### Many terms

`tests/perf/get_value_terms.py` builds crc32x16 at latency 8, compares one
step early (`sat`), and asks for every `|crc32x16_l8_n <signal>|` at every step:
162 terms (18 signals × 9 steps).

| | time |
|---|---|
| no get-value | 53.9 ms |
| one get-value of 162 terms | 52.5 ms |
| 162 get-values of one term | 52.7 ms |

The cost is within run-to-run noise. The evaluator (`smt2_eval.c`, piece 2.4)
already shares one memo across the terms of a command, and the model is read
once per variable. Nothing needed changing for speed.

Correctness: z3, pinned to dv-solve's `get-model` (the nine state variables),
gives all 162 terms the same values as dv-solve.

### get-model

`get-model` had two bugs (`docs/solver_bug_backlog.md`):

- **B90 (wrong model output).** A variable wider than 64 bits printed only its
  low 64 bits: a 100-bit all-ones `x` came out as 2^64 − 1. Every fw-hdl state
  variable is wider than 64 bits (Yosys `-stbv`). `get-value` was right.
  Nothing tested `get-model`. Wide values now print as `#b` literals.
- **B91 (valid input rejected).** `|c|` and `c` were different symbols, though
  SMT-LIB 2.6 makes them the same. The parser now strips the bars from a
  quoted simple symbol, and printing puts them back, so a `get-value` key
  echoes as written. Internal auxiliary variables were named `__auxN`, so a
  user's `__aux3` could be "already declared", and `get-model` hid user names
  starting `__aux`. Internal names now start with a backslash, which no
  SMT-LIB symbol can contain.

`get-model` lists declared symbols, as SMT-LIB requires and as bitwuzla, cvc5
and boolector do. It does not list `define-fun`s; z3 reprints their
definitions, which does not tell fw-hdl anything a `get-value` would not.

## 6.3: reason-unknown

Every place dv-solve answers `unknown` now records why, and
`(get-info :reason-unknown)` reports it. In order of precedence:

| cause | reason |
|---|---|
| an input error earlier in the session | `"input error: line L column C: ..."` |
| the model check rejected the model | `"model check failed: ..."` |
| a construct dv-solve does not implement | `"unsupported: operation 'bvfoo'"`, `"unsupported: indexed operator 'foo'"`, `"unsupported: symbolic index into a nested array"`, ... |
| out of memory building the problem or a solver | `memout` |
| bit-blast SAT budget (`DV_BB_MAX_CONFLICTS`) | `"incomplete: the SAT search stopped without a verdict (conflict limit)"` |
| bit-blast met a construct it does not encode | `"unsupported: a construct the bit-blast engine does not encode"` |
| CDCL stopped and bit-blasting was not possible | `"incomplete: CDCL search stopped: <bail reason>"` |
| CDCL's model failed validation and bit-blasting was not possible | `"incomplete: the CDCL model failed validation, and ..."` |
| other engine limits (abstract arrays after a CDCL compile, a CDCL context overflow, cube-and-conquer) | `"incomplete: ..."` naming the limit |

Before this piece, the unsupported-construct reason was always "assert used
an untranslatable construct". The specific reason recorded deep in the
translator was overwritten by that generic one. The reason belongs to the check
that answered `unknown`; a later check that answers a verdict reports the
bare `incomplete`, as before.

`tests/formal/test_reason_unknown.py` forces each reachable path. Out of
memory and the internal limits cannot be reached on demand, and are covered
by review only.

## Bugs found on the way

Checking `get-model` in the session fuzzer, and checking CDCL's validation
failures rather than only the final answer, found two more (both in code
from before 6.1, and both hidden by the validation net, which escalates to
bit-blast):

- **B92.** `check-sat` forgot a root conflict found by a flush in a live
  scope: `(push 1) (assert (distinct a a)) (check-sat)` was `unsat`, then
  `(assert (= a #x01)) (check-sat)` was `sat` from CDCL. B88 had fixed this
  for `check-sat-assuming` only.
- **B93.** An incremental assert at the root that compiles to a bound (`a =
  5`, `a ≤ 200`) lost it at the next reset, because the reset restored the
  compile snapshot. Every incremental CDCL session with such an assert after
  its first `check-sat` failed validation and escalated to bit-blast: a
  20-check session of bound asserts escalated 18 times, now none (14 ms →
  3 ms). The bounds now go into the snapshot. Losing a constraint only
  loosens the problem, so neither bug could give a wrong `unsat`.

Turning the validation net off in the fuzzer is not the right test: the net
is how CDCL covers constraints its compile leaves out (`(= a (bvnot #xe))` is
one), so those seeds fail by design. The fuzzer instead logs the number of
constraints the CDCL context left out (a count now kept across the compile
and every incremental flush; before, it counted only the last compile) and
fails when validation rejects a model while none were left out.

## Verification

- Session fuzzer (`tests/formal/test_bb_session_fuzz.py`), now with
  `get-model` and the CDCL validation check: 20000 seeds clean.
- Mutants (`perf-out/p62/mut.py`, log `perf-out/p62/mut.log`): 12 of 12
  caught, covering B90–B93 and the reason paths.
- Regression tests: `tests/formal/test_incremental_protocol.py` (11 new, all
  failing at 5.3) and `tests/formal/test_reason_unknown.py`.
- fwhdl-core A/B against 6.1 (`perf-out/p62/fwhdl_ab.log`): 377 files, no
  verdict changes. 5.45 s against 5.24 s, all of it in four SAT-bound
  mixed-proof files. On popcount_w64 both builds execute the same 25.70 G
  instructions and take the same path; cycles are 2.5% higher. That points
  to memory layout (the front end's struct grew), not more work.
- sat-core A/B: 237 files, no verdict changes, 0.71 s against 0.69 s.
- `pytest -q`: 9117 passed, 35 failed. The 35 are the environment and
  test-order failures from before this piece (bench, e2e, partitioner, tier3,
  doc pages): run on their own, the same 16 pass and 19 fail on this build
  and on the 6.1 build.
