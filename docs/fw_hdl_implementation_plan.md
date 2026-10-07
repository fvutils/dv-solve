# fw-hdl requests: implementation plan

Status: plan for review, 2026-10-07. Nothing implemented yet. D1–D6 recommendations in §5 approved by the owner 2026-10-07.
Source: `docs/fw_hdl_requests.md` (R1–R5). Testcases: `tests/formal/test_fw_hdl_requests.py`
(today on HEAD `022f744`: 12 pass, 30 strict-xfail).
Related: `docs/shared_learning_core_project.md`, `docs/usecase_benchmark_design.md`,
`docs/solver_bug_backlog.md` (B32 is R5), `docs/hard_sat_new_directions.md`,
`tests/perf/` (suite manifests, history, publishing).

The goal has two parts:

1. Meet the five requests, so fw-hdl can use dv-solve as a prover without a wrapper that
   second-guesses it.
2. Make dv-solve **the most capable solver for the fw-hdl workload**, which is proofs
   over unrolled bit-vector datapaths (CRCs, hashes, LFSRs, checksums, ECC), inverse
   queries, and codegen equivalence (IR against pipelined Verilog). "Most capable" has a
   measurable definition (§3), and every phase reports against it.

---

## 1. What we found on HEAD (2026-10-07)

All five requests reproduce on HEAD `022f744`. The investigation found causes the request
doc doesn't mention. Some of them are worse than what the doc reports.

### 1.1 Front end (R1, R2, R4, R5)

| # | Finding | Where |
|---|---|---|
| F1 | **Every diagnostic goes to `/tmp/dv-solve.log` when stdout is not a tty.** It was added so yosys-smtbmc's merged stream stays clean. As a side effect, piped and scripted use (fw-hdl, CI, pytest) never sees an error. This explains why R2 and R4 report "no diagnostic". | `smt2_main.c:227`, `:283` |
| F2 | **Batch mode stops reading at a stray `)`, without a message.** `sexpr_parse` returns NULL on `TOK_RPAREN` and the main loop treats NULL as EOF. Everything after the stray paren is dropped, including `check-sat`. On HEAD **both** `unbalanced_parens*.smt2` print nothing and exit 0. At `10e8bdc` one of them printed `sat`, so the behaviour got worse. | `smt2_parser.c:137`, `smt2_main.c:246` |
| F3 | An unknown command (`define-sort`) prints `(error "unsupported: …")` to the error stream, which is the log file (F1). The alias is never registered, so the following `declare-fun` taints the context and the answer is `unknown`. `define_sort_malformed.smt2` answers `sat` because its malformed alias is never used. | `_dispatch`, `smt2_frontend.c:6975` |
| F4 | `get-value` on a list term prints `(? v)`. When `_eval_sexpr` fails it prints a `#b0` placeholder, which was originally added to keep smtbmc's parser alive. The evaluator only handles about 20 operators, binary forms only, with no parameterized `define-fun` and no `let`. It is capped at 128 bits and has no notion of Bool sort. | `_cmd_get_value` `:5942`, `_eval_sexpr` `:5678` |
| F5 | Exit code 3 is already taken: the z3 oracle uses it for "aborted on a disagreement" (`smt2_oracle.c:385`, `test_oracle_check.py:142`). The new input-error code has to be something else. Usage errors exit 2 today, which collides with `unknown`. | `smt2_main.c` |
| F6 | The Bool sort is not tracked through get-value. This is open backlog entry B32. | backlog |

### 1.2 Performance (R3)

Measured with `python tests/formal/test_fw_hdl_requests.py --bench` on this machine:

| | auto | cdcl | bitblast | z3 5.1 | boolector 3.2.4 | bitwuzla 0.8.2 |
|---|---|---|---|---|---|---|
| `crc32_affine2` | 10.5 s | 10.5 s | 0.54 s | 0.05 s | 0.05 s | 0.53 s |
| `crc32_affine3` | 13.8 s | 13.8 s | 3.82 s | 0.88 s | 0.07 s | >120 s |

What the numbers say:

- **Routing costs 3.6–20×.** QF_BV stays on CDCL, which proves these instances itself,
  slowly.
- **Bit-blast loses on SAT search, not on encoding.** affine3 has 24 input bits, 1039
  AND gates, 700 vars and 2209 clauses, and encodes in 0.29 ms. SAT then takes 3.84 s.
  Changing backend barely moves it: CaDiCaL 3.5 s, minisat 4.5 s, kissat 5.7 s.
  Re-enabling kissat sweep (`DV_KISSAT_SWEEP=1`) makes no difference.
- **Boolector's win is not rewriting.** Its AIG is the same size (996 ANDs, 2848 CNF
  clauses), and it takes 0.06–0.08 s at every rewrite level, `-rwl 0` included. The
  `SAT call returned 20 in 0.000 seconds` line shows the SAT solver decides it at once.
  This build has only Lingeling compiled in, and Lingeling does Gaussian elimination
  over XOR constraints. The miter `crc(a)^crc(b)^crc(0) ≠ crc(a^b)` is affine over
  GF(2), and CDCL without XOR reasoning is exponential on parity (Urquhart/Tseitin
  formulas). **Working hypothesis: we need XOR/Gaussian reasoning.** Phase 4 starts
  by confirming it (piece 4.0).
- z3 has improved since the request was written (affine3 24 s → 0.88 s on z3 5.1).
  bitwuzla still times out on affine3. The bar is boolector at about 0.07 s.

---

## 2. Principles

1. **Soundness and honest answers come first** (standing rule). A front-end change that
   hides an error, or prints a value we did not compute, is a soundness bug. Phase 1 is
   done before any performance work.
2. **Routing never changes a verdict.** `test_crc32_verdicts` pins verdicts on every
   engine, and each routing change also runs the full `sat-core` cross-check with zero
   disagreements.
3. **Measure first, then improve.** Phase 0 builds the workload suite and the scorecard,
   so every later piece reports a before/after number against the same manifest.
4. **No overfitting to crc32.** Every performance lever is judged on a held-out split
   (§3.4) and on the existing suites (`sat-core`, `rand-core`, `app-riscv-dv-core`). A
   lever that wins on fw-hdl and loses elsewhere ships routed or gated, not as a default.
5. **Work with the shared-learning-core project.** XOR/affine reasoning (Phase 4) and
   SAT sweeping (Phase 5) are reasoning modules that the one learning core will host.
   Interfaces are agreed with that project before Phase 4b.

---

## 3. Benchmarking: how we show we are the most capable

### 3.1 Definition of "most capable" (the scorecard)

Each suite run produces one scorecard row per category, with seven dimensions. We claim
"most capable" for a category only when every **gate** holds and we are first on
**rank**.

| Dimension | Metric | Type |
|---|---|---|
| Soundness | zero verdict disagreements with the reference set (z3, bitwuzla, boolector, and cvc5/yices when installed), and every `sat` model checked by an independent evaluator | gate |
| Conformance | pass rate on the SMT-LIB conformance corpus (§3.3) = 100 %; output parses with the same model reader that reads z3's | gate |
| Coverage | % of instances answered (not `unknown` or error) within budget | rank |
| Speed | PAR-2 at three budgets: **1 s** (interactive quickcheck), **60 s**, **600 s**; solved count at each | rank |
| VBS gap | our PAR-2 ÷ the virtual best solver's PAR-2 over all competitors. 1.0 means we match the best tool on every instance | rank (headline number) |
| Scaling | fitted growth exponent along each family knob (bits, rounds, inputs, latency) | rank |
| Overhead | startup + parse + teardown on the trivial family (fw-hdl runs thousands of small quickchecks), and peak RSS | rank |

Headline claim per category: **"solves every instance any competitor solves, at PAR-2 ≤
the best single competitor, with VBS gap ≤ 1.2"**. That is stricter than "fastest on
average", and it is what users experience: no instance where another tool would have
saved them.

### 3.2 The `fwhdl-core` suite

The suite lives in `tests/perf/suites/fwhdl-core.json`. Generators go in
`tests/formal/gen_fwhdl.py`, and fixtures in `tests/formal/smt2/fw_hdl/gen/`. They are
checked in and sha-pinned by the manifest, as `sat-core` is. Each family has knobs, so we
measure growth rather than one point. Each instance carries `(set-info :status …)` from
consensus, and a sidecar records its knobs.

| Category | Families (knobs) | Expected | Why |
|---|---|---|---|
| `affine-proof` | CRC-8/16/32/64 with a table-less loop and a table form (poly, message bytes 1–8, number of property operands 2–4); LFSR/Galois step k rounds; Gray encode/decode; parity trees | unsat | the R3 shape: linear over GF(2) |
| `mixed-proof` | Fletcher/Adler checksums (adds), FNV-1a and murmur finalizers (mul+xor+shift), xorshift PRNG, Hamming/SECDED encode-decode round trip, popcount/clz identities, saturating add | unsat | the nonlinear neighbours of the affine shape, which XOR reasoning alone won't solve |
| `refute` | the same families with a mutated property or constant | sat + cex | finding a counterexample quickly; the model is checked by evaluation |
| `inverse` | find the input for a given output: CRC preimage, hash preimage at reduced width, checksum collisions | sat | inverse queries, which the request doc names as a use |
| `codegen-equiv` | XLS-style IR function against a pipelined Verilog version, via Yosys `write_smt2 -stbv` (`define-sort` state), unrolled over latency 1–8; correct (unsat) and off-by-one-latency (sat) variants | both | the R4 workload; miters with structural similarity |
| `quickcheck-trivial` | thousands of tiny proofs (≤ 16 input bits) | both | startup cost and per-query overhead dominate |
| `incremental` | one design with N properties via `push`/`pop` and `check-sat-assuming`, and get-value on terms after each `sat` | both | the session shape fw-hdl will move to |

Sources other than synthetic generators:

- fw-hdl's own designs (`fw-hdl/examples/xls/*`) captured through its printer, with
  provenance recorded as in `capture_fixtures.sh`.
- The XLS repo's `#[quickcheck]` functions, lowered by fw-hdl, if licensing allows.
- External hardware-flavoured SMT-LIB QF_BV families that we already track (the
  `smtcomp2025_parallel` corpus, brummayer-biere). These guard against overfitting
  (§3.4).

### 3.3 The conformance corpus (`tests/formal/conformance/`)

Front-end behaviour is a capability too. A prover that answers after malformed input is
worse than one that refuses. Each case is a `.smt2` file plus the expected **response
class** (not the exact text), checked by differential comparison with z3 on the
structure of the response:

- parse errors: stray `)`, unterminated list at EOF, bad tokens, bad `#b`/`#x`
  literals, a malformed `define-sort`/`define-fun`/`declare-fun` → an `(error …)`
  response and the error exit code, with no verdict after the error unless the
  continue-after-error policy (decision D2) says so;
- unsupported commands and logics → `(error "unsupported …")` and the error exit code;
- `get-value` on every QF_BV operator, n-ary forms, `let`, parameterized `define-fun`,
  Bool terms, widths 1/7/64/65/128/129/300 → echo the term and evaluate it right
  (values checked by z3 on the same model);
- `get-model`, `get-info :reason-unknown`, `:print-success`, `echo`, `get-assertions`
  where supported;
- exit codes for each of sat/unsat/unknown/error/usage;
- a **parser fuzzer**: mutate valid fixtures (delete or insert a paren, truncate,
  splice two files without a newline, which is the fw-hdl generator bug) and assert we
  never print a verdict where z3 reports an error.

### 3.4 Methodology that keeps the claim honest

- **Pinned competitors** in `tests/perf/tools.lock.json`: z3 5.1 (XLS's prover, so the
  incumbent), bitwuzla 0.8.2, boolector 3.2.4 (the bar on affine; note that it only has
  Lingeling), cvc5, yices2. Add the missing ones to the lock. **ABC `&cec`/`dprove`**
  (via `yosys-abc`, already in packages) is a reference ceiling for the
  `codegen-equiv` category only. It isn't an SMT solver, but it is what an equivalence
  user would compare against.
- **Dev/held-out split.** Generator seeds and knob points are split 70/30. Tuning is
  only allowed on dev. Held-out numbers are what we publish. External corpora are a
  third, never-tuned split.
- **Repeat and calibrate.** Use `tests/perf/calib.py` machine calibration, 3 repeats
  with the median, and a fixed CPU governor. A speedup claim needs a 95 % bootstrap CI
  that excludes 1.0.
- **No hints.** The portable SMT-LIB file is all any solver sees. dv-solve-only options
  are reported as a separate column (as in `usecase_benchmark_design.md` §2.2).
- **Regression gate.** Every PR in Phases 3–5 runs `fwhdl-core` (dev split, 60 s
  budget) and `sat-core`. Merge requires zero disagreements and no category PAR-2
  regression above 5 % (noise band measured in Phase 0).
- **Publish** through the existing pipeline (`tests/perf/history/*.jsonl` →
  consolidate → render), with one scorecard page per suite.

---

## 4. Phases

Each piece lists its size (S ≤ 1 day, M ≤ 1 week, L > 1 week), its dependencies and its
acceptance test. The tracking table in §6 mirrors this list.

### Phase 0: measurement foundation (before any fix lands)

| Piece | What | Size | Acceptance |
|---|---|---|---|
| 0.1 | Add boolector, cvc5, yices2 and ABC to `tools.lock.json` and `tools.py`, with a version probe for each | S | `run_sat.py --tools all` runs each one on a smoke fixture |
| 0.2 | `gen_fwhdl.py` generators for `affine-proof`, `mixed-proof`, `refute`, `inverse`, `quickcheck-trivial`; dev/held-out split by seed | M | ≥ 150 instances; consensus `:status` from ≥ 2 competitors; no instance where they disagree |
| 0.3 | `codegen-equiv` generator: a small XLS-style function set (crc32, adder tree, mux pipeline), pipelined Verilog → Yosys `write_smt2 -stbv` → miter | M | both variants checked with z3; the `define-sort` state form is kept verbatim (R4 exercise) |
| 0.4 | `fwhdl-core.json` manifest, scorecard renderer (VBS gap, PAR-2 at 3 budgets, scaling fit) | M | one baseline run of HEAD and all competitors recorded in history |
| 0.5 | Conformance corpus + response-class checker + parser fuzzer (§3.3) | M | runs in CI; today's failures are listed as strict-xfail (as the fw-hdl tests are) |
| 0.6 | Noise-band measurement: 10 repeats of the suite on the CI runner | S | the regression threshold is set from data |

### Phase 1: front-end correctness (R2, R4, R5, R1.1–R1.2). Do it first; it is soundness-adjacent

| Piece | What | Size | Acceptance |
|---|---|---|---|
| 1.1 | **Error channel policy (F1).** Command errors are SMT-LIB responses on the regular output channel: `(error "line L column C: …")`. Warnings and stats go to stderr. Warnings/info silent by default (stderr only with `--stats`/`-v`/`DV_LOG`); the automatic `/tmp/dv-solve.log` redirect is removed. Honour `:diagnostic-output-channel`. See decision D1 | M | fw-hdl R2/R4 tests see the error; `test_sby_smoke.py` and the Verilator protocol tests stay green |
| 1.2 | **Parser errors (F2).** A stray `)` and an unterminated list at EOF are reported with line and column. The lexer reports bad tokens. Batch mode no longer treats NULL as EOF: it tells EOF apart from errors | S | `unbalanced_parens*.smt2` and fuzzer cases report an error |
| 1.3 | **Sticky input-error state.** After any input error the session is marked. Policy D2: keep reading and report every error (cap 20), but every later `check-sat` prints `unknown` with `:reason-unknown` naming the error, never a verdict; `(reset)` clears it; exit 4 | S | no `sat`/`unsat` after an error in any fuzzer case |
| 1.4 | **Exit codes (F5).** Table in D3: 0/1/2/3 unchanged, **4 input error**, **5 usage error**, **6 internal error**; `--exit-codes=satcomp` (10/20). Document in `--help` and docs, with the `set -e` warning | S | conformance exit-code cases; `--help` text test |
| 1.5 | **Unsupported command → error.** Audit every "ignored" path in `_dispatch` (`declare-datatype`, unknown commands, unsupported `set-option`s that change semantics) and decide error or no-op for each, with the reason. An unsupported *construct* inside a term keeps the honest `unknown`, plus `get-info :reason-unknown` naming it | S | audit table in this doc's appendix; tests per row |
| 1.6 | **`define-sort` (R4).** Arity-0 aliases of any sort (BitVec, Bool, Array), resolved in every sort position (`declare-const/fun`, `define-fun` params and result, `Array` index and elem, nested aliases, alias of alias). Malformed → error (1.2/1.3). Parametric `define-sort` → `(error "unsupported: parametric define-sort")` until needed | M | `test_define_sort` passes on all engines; Yosys `-stbv` output from 0.3 solves |
| 1.7 | **Bool printing (R5/B32).** Record each declared symbol's sort (Bool vs `(_ BitVec 1)`) in `Smt2Var`/`Smt2FunDef`. get-value and get-model print `true`/`false` for Bool. **Check Verilator**: its response parser and the sorts it declares must not change (verilator-mode test suite, transcript replay) | S | `dma2_sstruct.smt2` prints `(stall true)`; Verilator tests unchanged |
| 1.8 | **get-value echo + no placeholders (R1.1, R1.2).** Echo every term with `_fprint_sexpr`. A term that can't be evaluated makes the **whole** command respond `(error "get-value: cannot evaluate …")`, since SMT-LIB has no per-term error. Remove both `#b0` placeholders | S | q2 passes; unsupported terms give an error, never a value |
| 1.9 | Close out: remove the R2/R4/R5 strict-xfail markers, update `fw_hdl_requests.md` with status, and add backlog entries for F1/F2 (they are soundness-class) | S | `test_fw_hdl_requests.py` R2/R4/R5 green |

Ordering: 1.2 → 1.3 → 1.4 (the core of R2), then 1.1, 1.6, 1.7, 1.8 in parallel.
Estimate: about 1.5 weeks.

### Phase 2: a complete, independent model evaluator (R1.3)

Replace the ad-hoc `_eval_sexpr` with `smt2_eval.c`: a complete evaluator for
QF_BV/QF_ABV/QF_UFBV terms over the current model.

Design choice: write an **independent interpreter**, not "bit-blast the term against the
model". The request doc offers both. The interpreter shares no code with the
compile/bit-blast pipeline, so it doubles as a **second soundness net**: Phase 2.5
re-checks every `sat` model against the original SMT2 assertions, independently of
`dvs_solver_validate_model` (which checks the compiled IR and so trusts the
translation).

| Piece | What | Size | Acceptance |
|---|---|---|---|
| 2.1 | Value type: arbitrary-width limb vectors (no 128-bit cap; `SMT2_MAX_BV_BITS`), plus Bool and array values (from the model's store chains) | M | widths 1…`SMT2_MAX_BV_BITS` round-trip |
| 2.2 | Full operator table: every QF_BV op, n-ary `bvand/or/xor/add/mul/concat`, `=`/`distinct` n-ary, chained compares, `=>`, `xor`, Bool `ite`, all signed compares/div/rem/mod with SMT-LIB division-by-zero semantics, shifts ≥ width, `repeat`, rotations, `bvcomp`, `bvnand/nor/xnor`, `select`/`store` | M | per-op unit tests |
| 2.3 | `let` (with shadowing), parameterized `define-fun` applied by environment binding (not textual substitution), recursion-depth guard (B12 lesson: fail with an error, never crash), and memoization of shared subterms (DAG-sized cost, not tree-sized) | M | `(f (f a))`, deep-let fixtures, a 10k-node DAG evaluated in < 10 ms |
| 2.4 | Hook it into get-value, get-model, the array-select path and Verilator get-value. Remove the old evaluator | S | all R1 tests pass on every engine; Verilator transcript replay is byte-identical |
| 2.5 | **Model re-check net**: after every `sat` (opt-out env), evaluate every original assertion with the new evaluator. A violation → `unknown` + a diagnostic + a backlog-worthy log line. Cost is measured on `quickcheck-trivial`, and if it exceeds ~5 % it moves to opt-in/debug | S | mutation test: break a translator rule and the net catches it |
| 2.6 | **Differential evaluator fuzzer** against z3: random terms × random models, comparing `get-value` output | M | 10k seeds, zero disagreements; in nightly |

Estimate: about 2.5 weeks. 2.4 closes R1. `test_crc32_counterexample_value` passes.

### Phase 3: routing and cheap decisive checks (R3 short term)

| Piece | What | Size | Acceptance |
|---|---|---|---|
| 3.1 | **Proof/miter shape detector** for non-Verilator QF_BV, computed on the builder DAG: share of XOR/AND/shift/neg/extract/concat nodes, no arithmetic beyond `bvneg`, a single top-level disequality, `define-fun`-chain depth. Route to bit-blast. Report the reason under `DV_LOG` (like `vlt-route`) | S | auto = bitblast time on crc32 affine2/affine3; `sat-core` is not worse |
| 3.2 | **CDCL budget then escalate** for everything else in QF_BV: the Verilator probe idea (bounded CDCL, then bit-blast) applied to one-shot QF_BV, with the budget tuned on the dev split. This replaces "CDCL until done" | S | no category regresses; the PAR-2 table goes into the PR |
| 3.3 | **Exhaustive bit-parallel simulation** gated by the D6 cost model (`2^n·g/64 ≤ 5e7`, n ≤ 32): compile the AIG to a 64-lane bit-parallel evaluator and sweep the whole input space. Exact for pure QF_BV with no UF or array leaves; returns `unsat` or a concrete `sat` model. affine3 has 24 input bits, about 2^24/64 × 1000 gates ≈ 0.26 G ops ≈ 0.1 s. XLS quickchecks are often byte-sized, so this is a real win for `quickcheck-trivial`. affine3 is above the D6 bound and goes to 4.1 | M | proven-equivalent to bit-blast on 1000 fuzz instances; crc32 affine2 < 10 ms |
| 3.4 | **Random-simulation pre-pass for `sat`**: a few thousand random vectors through the same evaluator before SAT, for fast counterexamples on `refute`. Cheap and sound, since the model is checked by construction | S | `refute` median time down; no unsat regression (bounded cost) |
| 3.5 | Remove the R3 performance gap from `fw_hdl_requests.md`; turn `--bench` into a `fwhdl-core` category gate | S | — |

Estimate: about 1.5 weeks. After Phase 3 we match or beat z3 on affine2/3, but boolector
still wins affine3 by about 3–50×, depending on the 3.3 threshold.

### Phase 4: XOR / affine reasoning (R3 long term: beat boolector)

| Piece | What | Size | Acceptance |
|---|---|---|---|
| 4.0 | **Confirm the hypothesis.** Dump our CNF for affine2/3 (DIMACS). Run Lingeling with and without Gauss, CryptoMiniSat with and without Gauss, kissat, CaDiCaL. Also run boolector at larger knob points (CRC-32 over 2–8 bytes, 3–4 operands) to see where *it* breaks | S | a short report in `docs/` that settles the lever |
| 4.1 | **Affine analysis on the AIG**: for each node, compute its affine form over GF(2) (bitset over input bits + constant) while the degree stays ≤ 1; stop at the first nonlinear gate. If the miter output is affine, decide it by Gaussian elimination: constant 0 → unsat, otherwise solve for a model. If only part of the circuit is affine, replace equivalent affine nodes before CNF. Worst-case cost O(nodes × inputs / 64) | M | affine2/3 and the whole `affine-proof` category in ms; unsound cases impossible by construction, and checked by the 2.5 net |
| 4.2 | **XOR-aware encoding**: native n-ary XOR nodes in the bit-blaster (XAIG), with chunked Tseitin (cut length 4–5) instead of AND-of-ANDs. Also helps SAT on `mixed-proof` | M | CNF size and time on `mixed-proof` |
| 4.3 | **Gauss–Jordan elimination during search** for the mixed case (XORs entangled with nonlinear logic). Option A (recommended): a CaDiCaL **external propagator** (IPASIR-UP), so no solver fork; XORs come from 4.2's encoding, not from CNF pattern matching. Option B: CryptoMiniSat as an extra backend routed for XOR-heavy shapes. Choose after 4.0 and a 1-week spike | L | `mixed-proof` VBS gap ≤ 1.2; soundness fuzz with an XOR-heavy op mix |
| 4.4 | Fit into the shared learning core (`shared_learning_core_project.md`): affine reasoning becomes a module with explain/learn hooks, so XOR conflicts are shared with word-level reasoning. Agree the interface with that project before starting 4.3 | M | design note approved by the owner |
| 4.5 | Widen the QF_BV fuzzer op mix to XOR/shift-heavy generators (the memory notes it is narrow) and add an affine-miter generator | S | in nightly |

Estimate: 4–6 weeks. **Target:** `affine-proof` VBS gap ≤ 1.0 (we are the virtual
best), and `mixed-proof` ≤ 1.2.

### Phase 5: equivalence / miter strength (`codegen-equiv`)

| Piece | What | Size | Acceptance |
|---|---|---|---|
| 5.1 | Structural hashing across the two sides of a miter, after word-level normalization of Yosys idioms (`-stbv` state slicing, `ite`-encoded muxes, `bvcomp`) | M | AIG shrinks on `codegen-equiv` |
| 5.2 | **SAT sweeping / FRAIG**: random simulation (reuse 3.3) proposes candidate node equivalences, incremental SAT proves them, and the proven ones are merged bottom-up. This is ABC's core equivalence lever. Gate it on the miter shape, so kissat sweep stays off for the easy band (the existing hammer) | L | `codegen-equiv` within 2× of ABC `&cec`; beats every SMT competitor |
| 5.3 | Latency scaling: unrolled depth 1–16 with state reuse; evaluate incremental BMC-style deepening (`push`/`pop`) against monolithic | M | scaling exponent reported; picks the default mode |

Estimate: 3–4 weeks.

### Phase 6: fw-hdl session features

| Piece | What | Size |
|---|---|---|
| 6.1 | Many properties per session: `check-sat-assuming` per property over one asserted design, reusing the bit-blast encoding across queries (incremental SAT) | M |
| 6.2 | Fast get-value of many terms (one evaluator pass, shared memo); get-model completeness for define-fun outputs | S |
| 6.3 | `get-info :reason-unknown` everywhere we answer `unknown` (tainted, budget, unsupported) | S |
| 6.4 | Lock in sampling diversity: turn the request doc's χ² check (60 seeds, crc filter) into a `rand-core` benchmark so it doesn't regress | S |
| 6.5 | fw-hdl CI loop: fw-hdl pins a dv-solve release and runs its formal-props suite; failures land in `fw_hdl_requests.md` | S |

### Phase 7: continuous scorecard

- Nightly: `fwhdl-core` (all splits, 600 s budget) + conformance + evaluator fuzz +
  parser fuzz, published through the existing results pipeline.
- Per PR: `fwhdl-core` dev split at 60 s + `sat-core` + conformance (the gate in §3.4).
- Each quarter: refresh the competitor pins, re-run held-out, and update the headline
  claim per category, including where we are **not** first.

---

## 5. Decisions (approved 2026-10-07)

Each recommendation is grounded in how the three real consumers behave. I read their
code:

- **yosys-smtbmc** (`smtio.py:410`) starts the solver with `stderr=STDOUT`, so anything
  on stderr arrives as a protocol line. It reads a response only after
  `check-sat`/`get-value`, and **any** response starting with `(error` makes it exit 1
  (`smtio.py:811`). It invokes dv-solve as if it were boolector (`--smt2 -i`).
- **Verilator** (`verilated_random.cpp:298`) recognises `(error …)` replies
  (`isSolverError`, `finishErrorReply`) and reports a solver failure. A forked child's
  stderr goes to the simulation's terminal.
- **fw-hdl** and other scripts read stdout and the exit status.

### D1. Where do errors and diagnostics go? (piece 1.1)

**Recommendation:**

- **Errors** are SMT-LIB responses on stdout, `(error "line L column C: <what>")`,
  always printed whatever `:print-success` says (as the standard requires).
- **Warnings and info** are **silent by default**. They go to stderr only with
  `--stats`, `-v`/`--verbose`, or `DV_LOG` (and to a file when `DV_LOG` names one).
- **Remove the automatic `/tmp/dv-solve.log` redirect.**
- **Unsupported but harmless input stays a silent no-op**: `set-info`, `set-option` with
  an unknown keyword that doesn't change meaning, `get-info` keys we don't know (answer
  `unsupported`, which the standard allows). Each such case gets a row in the 1.5 audit
  table with its reason.

**Why:** the redirect only existed because smtbmc merges stderr into the protocol
stream. If we print nothing unsolicited by default, smtbmc never needs protecting, and
there is no driver to detect: argv sniffing on `--smt2 -i` would be fragile. `(error …)`
on stdout is what the standard says. All three consumers already treat it as a failure:
smtbmc aborts, Verilator reports a solver error, and fw-hdl can see it. For genuine
errors that is the correct outcome. The real risk is a *false* error line, which would
abort an smtbmc run that works today. Hence the audit in 1.5, and two acceptance gates:
`test_sby_smoke.py`, plus a replay of every captured smtbmc and Verilator transcript in
the repo with no `(error` emitted.

**Rejected:**
- Keep the log file and add stdout errors on top: diagnostics would stay invisible
  (F1), and the hidden log in `/tmp` is shared across users and runs.
- Errors on stderr only: smtbmc would mis-read them and Verilator wouldn't see them. It
  is also non-standard.

### D2. What happens after an input error? (piece 1.3)

**Recommendation: keep reading, report every error (up to 20, then one summary line),
and execute every well-formed command, but poison the session for verdicts.** After the
first input error:

- every later `check-sat` / `check-sat-assuming` answers **`unknown`**;
- `(get-info :reason-unknown)` answers `(:reason-unknown "input error at line L column
  C")`;
- the exit status is **4**.

`(reset)` clears the poison. `reset-assertions` and `pop` do not, because the bad
command may have been anywhere.

**Why:**
- Stopping at the first error would leave interactive drivers blocked on a reply that
  never comes. The REPL already keeps going for that reason (`smt2_main.c:250`).
- Continuing *with* a verdict is z3's behaviour, and it is wrong for a prover: we can't
  know what the malformed text meant. In R2's assert case the stray `)` might have been
  meant to close an `assert` that isn't there.
- `unknown` is a check-sat reply every driver already handles. An `(error …)` there
  would be a second, misleading error.
- Reporting every error gives the generator author the whole list in one run.

**Rejected:** a z3-compatible `--continue-on-error` option. Nothing needs it, and it
reopens the exact hole R2 describes. It can be added later if a real user asks.

### D3. Exit codes (piece 1.4)

**Recommendation:**

| Code | Meaning | Change |
|---|---|---|
| 0 | last `check-sat` answered `sat` | unchanged |
| 1 | `unsat` | unchanged |
| 2 | `unknown`, or no `check-sat` was run | unchanged |
| 3 | oracle disagreement abort (`DV_ORACLE_ON_FAIL=abort`) | unchanged (F5) |
| 4 | input or protocol error (parse error, malformed or unsupported command), even if a `check-sat` ran | **new** |
| 5 | usage error (bad flag, unreadable file) | **new**; today these exit 2 |
| 6 | internal error caught (allocation failure, depth guard tripped on a command, assertion) | **new**; today these are `unknown` (2) or a crash |

Precedence: 3 and 5 exit immediately. Otherwise 6 > 4 > the last verdict. Signals
(SIGSEGV, OOM kill) stay ≥ 128, as the shell makes them.

Also add `--exit-codes=satcomp`, which maps sat/unsat to **10/20** as boolector and
bitwuzla do (boolector exited 20 in the R3 runs above). With that, scripts written for
those tools work unchanged, and `set -e` no longer stops at a proof under either
convention. It costs about 10 lines.

**Why:** don't renumber 0/1. `test_oracle_check.py`, the perf harness and fw-hdl depend
on them, and renumbering would silently invert a prover's meaning for existing wrappers.
The new codes fill only cases that today look like a verdict or like `unknown`, which
was the request. I found no consumer that relies on exit 2 for usage errors. `--help`
gets the table and the `set -e` warning.

### D4. The model evaluator (Phase 2)

**Recommendation: an independent interpreter over the SMT-LIB term**, in a new
`smt2_eval.c`:

- arbitrary-width limb values, Bool and array values;
- `let` and `define-fun` handled through an environment;
- DAG memoization;
- a depth guard that fails with an error and never crashes.

Its leaves read the model through the existing readback functions
(`_fe_get_var_value[_wide]`, the array store chain). That is model *reading*, not
translation, so sharing it doesn't weaken independence.

**Why:**
1. Coverage was R1's ask. The interpreter covers the whole operator table by
   construction, and the z3 differential fuzzer (2.6) checks it.
2. It is a **second soundness net (2.5)**. `dvs_solver_validate_model` checks the
   *compiled* IR, so a translator bug that drops or mis-lowers a constraint passes it.
   Re-evaluating the *original* assertions catches exactly the class of bug behind
   B13, B43–B49 and B70.
3. "Bit-blast the term against the model" would share every translation bug with the
   solver, which is the one thing a checker must not do. It also can't produce values
   on the CDCL path without bit-blasting there too.

**Performance guard:** plain symbols keep today's direct lookup, so Verilator's
per-randomize get-value path doesn't change. The net (2.5) is on by default only if it
costs < 5 % on `quickcheck-trivial` and on the Verilator replay; otherwise it is on in
CI and for `--check-models`.

### D5. XOR reasoning engine (Phase 4)

**Recommendation: staged, with data deciding at each step.**

1. **4.1 affine analysis first, unconditionally.** It is cheap, in-tree and complete
   for purely affine miters, which is the exact R3 shape. It may close the boolector gap
   on its own.
2. **4.0 measures the mixed case with CryptoMiniSat through `pycryptosat`** (on PyPI,
   5.16, with a native `add_xor_clause` API). It takes about a day, needs no build
   integration, and shows what Gauss–Jordan is worth on `mixed-proof` beyond affine
   analysis.
3. **If it is worth it, the production engine is a Gauss–Jordan propagator on the
   bundled CaDiCaL 3.0.0's external-propagator interface** (`ExternalPropagator`, in
   `resources/cadical/src/cadical.hpp`). XOR constraints come straight from 4.2's XOR
   nodes, so there is no CNF pattern matching.

**Why not a CryptoMiniSat backend in production:**
- It would need a second routing decision.
- We would lose kissat's strengths (the easy-band and lean-encoding wins in memory)
  whenever the shape detector is wrong.
- It adds a dependency to every wheel platform.
- It sits outside the shared learning core, where an XOR module must explain its
  conflicts to the word-level side.

An external propagator gives us all of that, and CaDiCaL is already in-tree and
license-clean.

**Kill criterion:** if `pycryptosat` shows < 2× on `mixed-proof` held-out after 4.1
lands, drop 4.3 and spend the time on Phase 5.

### D6. When to try every input (piece 3.3)

**Recommendation: a cost model, not a fixed bit count.** After constant propagation
and cone-of-influence reduction, let `n` be the free input bits and `g` the AIG gates.
Run the exhaustive sweep when `2^n · g / 64 ≤ 5·10^7` gate-word evaluations (about
50 ms single-threaded), with a hard cap of n ≤ 32. It runs **after** 4.1's affine check,
which is cheaper still. It applies only to pure QF_BV with no UF or array leaves.

What the cost model gives:
- `crc32_affine2`: 16 bits × 1039 gates ≈ 10^6 → runs in about 1 ms.
- `crc32_affine3`: 24 bits ≈ 2.7·10^8 → skipped, and left to affine analysis or SAT. A
  fixed threshold of 24 would have spent about 0.3 s, which is 4× boolector.
- The byte-wide quickchecks fw-hdl writes most often are always in range.

**Why:** a fixed bit count ignores circuit size. A cost bound makes the sweep a
guaranteed-small bet that never costs more than the budget it saves. The constant
(5·10^7) is tuned on the dev split in 0.4/3.3. Splitting the sweep across threads
(`DV_PARALLEL`) raises the cap later without changing the rule.

## 6. Tracking

Status: ☐ todo · ◐ in progress · ☑ done · ✕ dropped.

| Piece | Request | Status | Owner | PR / commit | Notes |
|---|---|---|---|---|---|
| 0.1 tools lock (boolector, cvc5, yices2, ABC) | bench | ☐ | | | |
| 0.2 generators: affine/mixed/refute/inverse/trivial | bench | ☐ | | | |
| 0.3 codegen-equiv generator (Yosys -stbv) | bench, R4 | ☐ | | | |
| 0.4 fwhdl-core manifest + scorecard | bench | ☐ | | | |
| 0.5 conformance corpus + parser fuzzer | R1, R2, R4, R5 | ☐ | | | |
| 0.6 noise band | bench | ☐ | | | |
| 1.1 error channel policy | R2, R4 | ☑ | | branch feat/fw-hdl-frontend | D1; B76 |
| 1.2 parser errors | R2 | ☑ | | branch feat/fw-hdl-frontend | F2 = B75 |
| 1.3 sticky input-error state | R2 | ☑ | | | D2 |
| 1.4 exit codes + `--help` | R2 | ☑ | | | D3 |
| 1.5 unsupported-command audit | R2, R4 | ◐ | | | done for command and term errors (Appendix B); `:print-success` still ignored |
| 1.6 define-sort | R4 | ☑ | | | |
| 1.7 Bool printing | R5 / B32 | ☑ | | | check Verilator |
| 1.8 get-value echo, no placeholders | R1.1, R1.2 | ☑ | | | |
| 1.9 close-out of R2/R4/R5 | — | ☑ | | | |
| 2.1 value type | R1.3 | ☐ | | | |
| 2.2 full operator table | R1.3 | ☐ | | | |
| 2.3 let / define-fun / memo | R1.3 | ☐ | | | |
| 2.4 hook-up, remove old evaluator | R1 | ☐ | | | |
| 2.5 model re-check net | soundness | ☐ | | | |
| 2.6 evaluator fuzzer vs z3 | soundness | ☐ | | | |
| 3.1 miter shape → bitblast | R3 | ☐ | | | |
| 3.2 CDCL budget + escalate | R3 | ☐ | | | |
| 3.3 exhaustive bit-parallel simulation | R3 | ☐ | | | D6 |
| 3.4 random-simulation pre-pass | R3 | ☐ | | | |
| 3.5 R3 close-out, bench gate | R3 | ☐ | | | |
| 4.0 confirm XOR hypothesis | R3 | ☐ | | | |
| 4.1 affine analysis + Gauss | R3 | ☐ | | | |
| 4.2 XOR-aware encoding | R3 | ☐ | | | |
| 4.3 Gauss–Jordan during search | R3 | ☐ | | | D5 |
| 4.4 shared-core interface | R3 | ☐ | | | |
| 4.5 widen fuzzer op mix | soundness | ☐ | | | |
| 5.1 miter strash + normalization | codegen | ☐ | | | |
| 5.2 SAT sweeping / FRAIG | codegen | ☐ | | | |
| 5.3 latency scaling | codegen | ☐ | | | |
| 6.1–6.5 session features, fw-hdl CI | — | ☐ | | | |
| 7 continuous scorecard | bench | ☐ | | | |

## Appendix A: reproduction commands

```
cmake --build build -j
pytest -q tests/formal/test_fw_hdl_requests.py         # 12 pass, 30 xfail on 022f744
python tests/formal/test_fw_hdl_requests.py --bench    # §1.2 table
DV_BB_STATS=1 build/dv-solve-smt2 --engine=bitblast --stats \
    tests/formal/smt2/fw_hdl/crc32_affine3.smt2        # bb=0.29ms sat=3838ms
boolector -v tests/formal/smt2/fw_hdl/crc32_affine3.smt2   # 996 ANDs, SAT 0.000 s
```

## Appendix B: error classification (piece 1.5, 2026-10-07)

How each kind of bad or unsupported input is answered. "Error" means an
`(error "line L column C: ...")` response on stdout and exit status 4; "poison"
means every later check-sat answers `unknown` until `(reset)`.

| Input | Answer | Why |
|---|---|---|
| Parse error: stray `)`, `(` never closed, bad `#b`/`#x` literal, `12ab`, numeral overflow, unterminated string or `|symbol|` | error + poison | we cannot know what the text meant (B75) |
| A command that is not a list, `()`, a head that is not a symbol | error + poison | malformed |
| Unknown command (`frobnicate`, `declare-datatype`, `define-fun-rec`, ...) | error + poison | ignoring it could drop an assertion |
| Malformed known command (wrong arity, name not a symbol, bad parameter list) | error + poison | malformed |
| Unsupported form of a known command (`declare-sort` arity > 0, parametric `define-sort`, datatypes with several constructors, UF over BitVec parameters, capacity limits) | error + poison, message starts `unsupported:` | the command cannot be carried out |
| `set-logic` with a logic we don't support | error + poison | answers would not be meaningful |
| `pop` below the bottom, push depth exceeded | error + poison | the assertion stack is not what the script thinks |
| Ill-formed term in `assert` (unknown symbol, arity or sort mismatch, malformed `let`) | error + poison, unless the context is already tainted | an unknown symbol after an unsupported declaration is that declaration's consequence, not a new error |
| Unsupported construct in a term (an operator or sort we don't implement, depth guard) | `unknown`, exit 2, `:reason-unknown "unsupported: ..."`, diagnostic under `-v` | honest unknown, the input is fine |
| Unsupported sort in `declare-const` (`Int`, BitVec wider than the maximum) | `unknown`, as above | same |
| `get-value`/`get-model` with no model, empty term list, a term we cannot evaluate; `echo` without a string | error, **no** poison | the assertions are intact; a later check-sat still answers |
| `set-info`, `set-option` with an unknown keyword | silent no-op | no effect on meaning. TODO: `:print-success true` is ignored, so a driver that waits for `success` would hang |
| `get-info` with an unknown key | `(:unsupported)` | as before |
