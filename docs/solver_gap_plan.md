# Closing the xezim gap: work plan

Analysis: docs/simbench_xezim_gap.md (§4 ranked fixes, §6 both front-ends).
Order agreed 2026-11-05. Nothing is committed by the agent; the user commits.

## Design rule: automatic engine choice inside the context, no routing calls

Every API user gets automatic engine selection, and no caller makes a routing
call. Today the choice is made in three places and missing in two:

- the SMT front-end: a declared-logic check, plus a one-shot `dvs_shape_route` call;
- `dvs_handle`: AUTO, by calling `dvs_shape_route` itself, then falling back to
  bitblast if CDCL errors;
- `dvs_solver_*` (`dvs_ctx_t`), which Python `SolveCtx`, DPI and
  `bench_rand_capi` use: CDCL only. A problem CDCL can't compile simply fails
  (step 2: `mul_div_64` fails on both API arms).

Proposal (step 3): **`dvs_ctx_t` itself becomes engine-agnostic.**

- `dvs_solver_compile` makes the choice from the problem's content.
  `dvs_shape_route` becomes private to it.
- The context holds a CDCL search or a `dvs_bbsolver` internally, and moves to
  bitblast if CDCL gives up.
- `solve`, `get_value`, `reset`, `pin_var` and `exclude_value` dispatch to the
  engine the context holds. Pins and exclusion on bitblast come from step 4's
  assumptions; until then a context with pins stays on CDCL, as the handle
  does today.
- An optional override (`dvs_solver_set_engine`, as on the handle) is for
  tests and benches only.
- No new calls for users:
  - `SolveCtx`, DPI and `dvs_handle` get AUTO by doing what they do today.
  - `dvs_handle`'s own routing goes away; it becomes a thin wrapper.
  - The SMT front-end compiles through the same context instead of choosing.
    Its session and incremental machinery is the part that needs care, so it
    moves last.

## Steps

| step | what | status |
|---|---|---|
| 1 | Test: a handle problem with a multiply and a `dist` keeps the dist's weights (AUTO routes to bitblast, which drops them). Then fix: keep dist problems off bitblast in the router, or weight the draw in bitblast. | done 2026-11-05 |
| 2 | API benches in `tests/perf/rand_benches.py`: memmap non-overlap, 64-bit mul/div, weighted `dist`, with a joint-uniformity check on small instances | done 2026-11-05 (dist deferred: the IR has no dist node) |
| 3a | Engine-agnostic `dvs_ctx_t`: `dvs_solver_compile` chooses, `dvs_handle` and `SolveCtx` just use it | done 2026-11-05 (user approved "one path") |
| 3b | SMT front-end onto the context's choice; content routing for QF_ABV without array terms (B130); decide whether shape routing belongs in AUTO (measure on SMT-encoded and API problems) | todo |
| 4 | Core caching: `_enumsamp` decline sticks; `_pinfast` miss remembered; pins as bbsolver assumptions | todo |
| 5 | Signed and unsigned operators; full-range unsigned 64-bit in CDCL (drop "wide-arith"): docs/signed_unsigned_ops_design.md. Then functional dependency in the core | in progress: design steps 1-2 done; 3 revised (typed ops lower during elaboration) and done for the SV builder 2026-11-05; SMT front-end switch tried and reverted until CDCL takes typed ops; open: pin/checkpoint on a bit-blast context (3a); next: CDCL 64-bit (design step 4) |
| 5b | CDCL search above 64 bits (tier 2 has bounds, no search), on typed operators | todo, wanted soon |
| 6 | Word-level diversify, order-then-gap sampler, interval+congruence+known-bits domains | todo |

## Status notes

- 2026-11-05, step 1. `tests/c/test_handle.c` "dist+multiplier": `(x*3)^y == 0x55`
  plus `w dist {0 := 90, [1:9] :/ 10}`. Before the fix AUTO took bitblast and `w == 0` on
  43 of 400 calls (uniform); after, CDCL and 366 of 400. Fix: `dvs_shape_t.n_dist`, and
  `dvs_shape_route` keeps any problem with a dist on CDCL (`src/c/dvs_shape.c`). Left
  open: when CDCL then errors, the handle still falls to bitblast and the weights are
  lost (documented in `dv_solve.h`); weighting the bitblast draw would close that and
  belongs with step 4's pin assumptions (pick the bucket, assume the var into it).
  ctest: 33/33.
- 2026-11-05, step 2. `tests/perf/rand_benches.py`: `memmap3` (3 regions, 1 MiB-aligned,
  sizes 1/2/4/8 MiB, no overlap, 32 MiB window; binned by (size0, region 0's address rank),
  12 bins; the ground truth was checked by brute force over all 843,870 solutions) and
  `mul_div_64` (as simbench; 256 bins over (a>>28, b>>12)). Both added to the `rand-core`
  suite, each with an exact uniform draw. A weighted-`dist` bench is deferred: the soundness IR
  has no dist node, and the SMT arm would have to emulate Verilator's pinning.
  N=500 (JSD against exact; the uniform arm is the noise floor):

  | bench | arm | ms/call | JSD | note |
  |---|---|---|---|---|
  | memmap3 | uniform | | 0.007 | floor |
  | memmap3 | dv-api (SolveCtx, CDCL) | 1.6 | 0.085 | skewed |
  | memmap3 | dv-capi (dvs_solver, CDCL) | 0.93 | 0.119 | skewed |
  | memmap3 | dv-swizzle (SMT front-end) | 4.5 | 0.006 | at the floor |
  | memmap3 | z3-swizzle | 12.5 | 0.061 | |
  | mul_div_64 | uniform | | 0.121 | floor (256 bins) |
  | mul_div_64 | dv-api, dv-capi | | | **fail**: CDCL can't compile; no fallback |
  | mul_div_64 | dv-swizzle | 90 | 0.81 | very skewed: 10% bin coverage |
  | mul_div_64 | z3-swizzle | 384 | 0.39 | |

  So the API front-end is skewed on memmap where the SMT one is not (both use CDCL plus
  sampling, but different paths), and `mul_div_64` is skewed on the SMT path even though
  simbench's distinct-value metric scores it 1.0 (gap doc §2). The scratch driver is not kept;
  run with `tests.perf.run_rand.run_suite({"n": 500, "benches": [...]}, arms)`.
- 2026-11-05, step 3a. `src/c/dvs_engine.{c,h}`; hooks in `dvs_solver_compile`, `_solve`,
  `_get_value`, `_validate_model` (reads values through `get_value`), and a CDCL-only guard
  on pin, exclude, add_constraint, add_array_vars, checkpoint, value selector, optimize, cost
  guidance, LNS and phase hints. Public: `dvs_solver_set_engine` / `dvs_solver_engine`
  (`dv_solve.h`, `docs/site/reference/c-api.md`); Python `SolveCtx(engine="auto"|"cdcl"|"bitblast")`
  and `SolveCtx.engine`. `dvs_handle` lost its own routing and bit-blast code.
  - **AUTO rule, as built:** CDCL first. Bit-blast when CDCL can't compile the problem whole,
    or when a CDCL search gives up (not one with its own `time_limit_ms`). Problems with
    STATE/RANDC vars, solve order, softs or a dist (and vars > 64 bits) stay CDCL. A CDCL-only
    call moves the context back to CDCL for good, or fails if CDCL couldn't compile it.
    `dvs_solver_create` (bounded memory) defaults to CDCL.
  - **Shape routing (`dvs_shape_route`) is not in AUTO.** Tried first: it sent ordinary API
    randomization problems (extends count as bit ops; any multiply) to bit-blast and broke
    `tests/unit/test_mul_square_divisor.py` (CDCL's product/divisor sampling: uniform over
    divisor pairs, 20 ms budget). It stays the SMT front-end's one-shot rule until 3b
    measures it.
  - **SMT front-end, contradiction analysis and `bench_vlt_replay`** ask for CDCL explicitly
    (they make their own choice or explain CDCL's search) until 3b.
  - **Found and fixed:** bit-blast read `hi = INT64_MAX` on a 64-bit unsigned var as the full
    range; CDCL and the API read it as 2^63-1 (the SMT front-end spells the full range as all
    ones). `assert_var_bounds` (`dvs_bbsolver.c`) now agrees; `docs/feature_coverage_probe.py`
    W7 caught it.
  - **Tests:** new `tests/c/test_engine.c`; `test_handle.c` updated (CDCL first);
    `test_const_wide.py` and `test_thrash_memory.py` ask for CDCL (they test CDCL). ctest 34/34.
    Python `tests/unit`: all pass but 2 doc tests that need Verilator ≥ 5.054 on PATH (pass with
    it), and 3 collection errors (scipy, zuspec) that predate this.
  - **Measured** (N=500): `mul_div_64` now solves on both API arms (was a compile failure),
    23 ms/call against 94 ms on the SMT front-end, and as skewed as it (JSD 0.77; floor 0.12).
    `memmap3` is unchanged (CDCL; JSD 0.085–0.12 against 0.006 on the SMT path).
- 2026-11-05, unrelated, found while checking CI for 385c27a: GitHub's `linux-x86_64` Test
  failed `test_stack_recursion` on `smt2_value_print.localalias` (that gcc's local alias of a
  recursive function). The test now folds `.localalias` into the function name, and lists
  `smt2_value_print` as bounded (array values nest only as deep as their sort, 64). Forgejo's
  run passed (a different gcc).
- 2026-11-05, direction for step 5 (user): distinguish signed and unsigned operators, so
  unsigned operations get the full 64-bit range. Today the operators (`dvs_binop_t`) carry no
  signedness. SV elaboration infers it from operand types; an EXPLICIT (SMT-LIB) problem has
  it fixed by its encoding. CDCL holds every domain as an int64 `[lo, hi]`, so an unsigned
  value in [2^63, 2^64) reads as negative. That is why the SMT front-end sends any width-64
  arithmetic to bit-blast ("wide-arith", `smt2_frontend.c:503`), and why CDCL can't compile
  `mul_div_64`. Outline:
  - Signedness on the operator, once elaboration has run: unsigned and signed variants of
    `< <= > >= / % >>`, and of overflow-sensitive `+ - *` where it matters for propagation.
    SV elaboration and the SMT front-end (`bvult`/`bvslt`, `bvudiv`/`bvsdiv`) emit them
    directly, rather than leaving the engine to reconstruct signedness from types.
  - CDCL domains for an unsigned var or aux as a uint64 interval: the propagators and
    `var_lo64`/`var_hi64` readers pick signed or unsigned bounds from the operator, with
    wrap-aware (modular) interval arithmetic for `+ - *` at width 64.
  - Then drop "wide-arith" routing, and the `hi = INT64_MAX`-means-full-range ambiguity
    (step 3a fixed it in bit-blast; an unsigned bound is a uint64 pattern everywhere).
  - Tests: `test_dvs_wide_arith.c`, the feature-coverage W rows, and `mul_div_64` and
    `memmap3` on CDCL. Design note to write first: the operator-set change touches the
    builder ABI, `doors.py` lowering, the validator and every propagator.
- 2026-11-05, step 5 (design steps 1-2). Typed operators `DVS_BIN_ULT .. DVS_BIN_LSHR`
  (19-32) in `dv_solve.h`, `dv_solve/problem.py`, helpers in `dvs_problem.h`. Semantics: the
  node is self-determined; each operand is extended to the wider one's width by the
  operator's signedness; x/0 as SMT-LIB. They are wired into:
  - the shape counter, the dump table and the SMT-LIB emitter (oracle);
  - the validator (`_typed`);
  - the bit-blaster (`bb_typed_opnds`, `bb_sdivrem`, shared with the SV signed `/ %`, and
    SMOD new);
  - SV elaboration (`_typed_node`: operands elaborated at their own type, cast by the
    operator).

  CDCL declines them (compile incomplete), so AUTO solves them on bit-blast. Oracle-attached
  contexts now go through the engine choice too, and the oracle checks bit-blast answers.
  Tests: `tests/unit/test_typed_ops.py` (82: every op at widths 1/8/63/64 against a Python
  SMT-LIB reference, edges, x/0, MIN/-1, narrow operands, LSHR amounts, the validator
  catching a violation, z3 via the oracle). A probe of the typed ops nested under `||`,
  `ite`, `!` and arithmetic: 40 problems, 200 solves, z3 agreed with all. ctest 34/34;
  Python unit as before, plus the new file.
- 2026-11-05, step 5 (design step 3, revised). Typed operators now lower during SV
  elaboration to SV operators on cast operands (design note, "Revision"), so CDCL compiles
  them. Rewriting SV compares into typed ones would have silently switched off the ~300
  pattern matchers keyed on SV operators.
  - **CDCL fix:** an `ite` used as a value got an unsigned aux var even with signed arms.
    It now gets the signed range (`_value_to_var`, `dvs_compile.c`). Signed `ite` auxes of
    63-64 bits are declined: SMOD there came back wrongly UNSAT, which belongs to design
    step 4.
  - **`tests/unit/test_typed_ops.py`** runs every case on bit-blast, CDCL and auto (207
    tests). CDCL takes every typed op at 8 and 32 bits.
  - **Nested probe:** 39 of 40 problems on CDCL now, against 0 before. z3 agreed with all 200
    answers, at 6.6 ms in total against 72 ms on bit-blast.
  - **Suites:** ctest 34/34; Python unit 1407 passed, plus the 3 old collection errors.
  - **Soundness campaign** (n=1500, seeds 1 and 2): the smt2, incr, steps and builder doors are
    clean. The protocol door fails 5 problems (seed 1: i=141, 363; seed 2: i=667, 917, 1114),
    the identical set on a build of HEAD, so they predate this work. They shrink to
    `(xor (not true) ...)` under a pin: an open bug in the Verilator-protocol path, to
    triage separately.
- 2026-11-05, step 5 (design step 3, SMT front-end): tried and reverted. Emitting
  `bvsdiv`/`bvsrem`/`bvsmod` as the typed SDIV/SREM/SMOD gave bit-blast a native encoding
  and made the validator independent of the lowering (B69), but CDCL cannot compile typed
  operators yet, so pure CDCL (`DV_NO_BITBLAST`, `test_cdcl_compile_gaps.py`, 14 cases) lost
  division by a variable. The front end keeps its lowerings for every signed operator
  until CDCL takes typed operators (design step 4); switching then is a two-line change
  (the A/B below is the record).
  - **A/B, typed vs the old lowering** (bit-blast, 3x100 push/pop solves per row): equal or
    faster everywhere except a variable divisor in `bvsrem` at 32/64 bits, 1.1-1.8x, on the
    same path with fewer gates; the old lowering's aux vars give kissat cut points.
  - **Kept, bit-blast:** `bb_eval` (the samplers' IR evaluator) evaluates typed operators;
    it declined them, which sent 64-bit `bvsmod` by a constant to SAT, 3.8x slower.
    `bb_sdivrem` notes the magnitudes' remainder for the word layer, as SV `%` does.
    Tested by `tests/c/test_bb_typed.c` (EXPLICIT problems with every typed op at 8 and 64
    bits, 30 models each, checked against SMT-LIB; `[bb-eval] declined=0`).
  - **Fixed, bit-blast (found by `test_api_oracle.py`):** `bb_predicate` sized a constraint
    to 1 bit, so an unsized constant or a multi-bit expression read as its low bit: the
    constraint `76` was false. A condition is now self-determined, then `!= 0`. It predates
    this work (same answer with HEAD's `dvs_bbsolver.c`); the API reached it once AUTO
    falls back to bit-blast (step 3a).
  - **Tests:** `test_divsamp.py::test_signed_divrem_definition` (signed division through
    the sampler, models checked) and `test_bb_eval_divmod.py` extended to the signed ops.
  - **Open, from step 3a:** `test_concat_value.py::test_signed_part` (22 cases). With AUTO a
    problem CDCL cannot compile now gets a bit-blast context instead of
    `CompileIncompleteError`, and `pin`/`checkpoint` on it fail (CDCL-only), which the test
    reads as UNSAT.
  - **Environment, not code:** z3 4.8.12 here lacks `bvuaddo` and `(as const ..)`
    (`value_overflow`, `test_eval_fuzz` 200, one `test_parser_fuzz` case); scipy is missing
    (12 in `test_divsamp.py`).
- 2026-11-05, step 5 (design step 4a): CDCL takes typed operators in an EXPLICIT problem.
  `dvs_sv_lower_typed` (`dvs_sv.c`) elaborates an EXPLICIT problem when it holds a typed
  operator (`n_typed`, new in `dvs_shape_t`, counted over hard and soft roots), so each one
  lowers exactly as on the SV door. The validator already reads an EXPLICIT problem by the SV
  rules, so the rest keeps its meaning. Compile calls it at both elaboration sites (whole
  problem, incremental add); bit-blast does not, and keeps its native encoding.
  - **Test:** `tests/c/test_cdcl_typed.c` (ctest): every typed op on a CDCL-pinned context,
    test_bb_typed's problems, at 8 and 62 bits: 38/38 compile complete, 30 models each,
    checked against SMT-LIB and `validate_model`. At 64 bits all decline (4b), none wrong.
  - ctest 36/36.
  - **Front end:** `bvsdiv`/`bvsrem`/`bvsmod` now emit the typed SDIV/SREM/SMOD, each bound
    to an unsigned aux var (`_typed_signed_div`): the typed node is signed under the SV rules,
    and `bvult` of two of them must still compare bit patterns. `_signed_divrem_expr` is
    gone. The `sdiv-srem`/`smod` routing stays until design step 5.
  - **Checks with the switch:** `test_cdcl_compile_gaps.py` 60/60 (pure CDCL, division by a
    variable included); a hand probe of `bvsdiv`/`bvsrem`/`bvsmod` by a negative variable on
    `DV_NO_BITBLAST` gives SMT-LIB-correct models. Soundness campaign n=1500, seeds 1 and 2:
    the same 5 protocol-door failures as before (B: `(xor (not true) ..)` under a pin), nothing
    new. Python unit: 1405 passed; `test_doc_pages.py::test_verilator_guide_unsat/_unknown`
    fail on the Verilator runtime's message text (`/tools/verilator/5.040-git-20251011` prints
    `Internal: Solver error: unknown`, no UNSATCONSTR line): environment, not solver.
- 2026-11-05, design step 4b plan (unsigned 63-64-bit values on CDCL), from a survey of
  compile and the propagators. Domain order is already right: `var_b_*` compare uint64 for an
  unsigned var of width >= 64, `ctx_tighten_*64` use them, search spans are uint64. The gaps:
  - **Suspected soundness bugs (to confirm with a test first):**
    `_compile_var_const_cmp` NEQ (`dvs_compile.c` ~205) skips `x != c` by a signed range test,
    so c >= 2^63 on a u64 var is dropped; `_adjacent_const_var` (~824) uses INT64 edges and
    wraps c = 2^64-1; the top-level zero-extend caps a 64-bit operand at INT64_MAX (~3277);
    `bounds_lshr_64` uses maxsh 63 at aw 64 (`dvs_prop_templates.c` ~3709); `bor_64` reads a
    bound >= 2^63 as a conflict (~3343); the search's hole walk is signed (`dvs_search.c` ~639,
    ~3036).
  - **Propagators:** `bounds_div_64`, `bounds_mod_64`, `bounds_lshr_64` compute in signed
    int64; they need a uint64 branch (UDIV [alo/bhi, ahi/blo], UREM r <= min(ahi, bhi-1), LSHR
    0 for amount >= 64) with backward steps.
  - **Compile:** `_rng64`/`_binop_exact` decline any u64 operand whose compile-time domain is
    [0, 2^64-1], i.e. every free one; relax for DIV/MOD/RSHIFT with r, a, b unsigned (result
    <= a). Var-var compares need both operands in one order (else a reinterpretation aux).
  - **Slices:** (1) compare paths and the suspected bugs, with `test_prop_exhaustive.c` rows
    at 2^63 and 2^64-1; (2) uint64 div/mod/lshr and the `_binop_exact` relaxation, tests in
    `test_dvs_wide_arith.c`; (3) un-route bvudiv/bvurem/bvlshr from "wide-arith" and run
    `mul_div_64` on CDCL. `memmap3` is 32-bit: its gap is skew, not range. The dispatch-table
    merge and the bound-read audit follow (design step 5).
  - **Formal suite with the switch:** 8771 passed; the 224 failures are the known set
    (`test_signed_part` 22, z3 4.8.12's missing `bvuaddo`: `value_overflow`, `test_eval_fuzz`
    200, one `test_parser_fuzz`).
- 2026-11-05, design step 4b, slices 1-2: unsigned 64-bit `/`, `%`, `>>` on CDCL.
  - **Probed first** (pure CDCL, builder door): u64 compares at 0, 2^63, 2^64-1, var-var
    across 2^63, a 65-bit zero-extended compare, `y + 1` wrapping: all right. Of the survey's
    suspects only one was real:
  - **Fixed, soundness:** `_compile_var_const_cmp` NEQ tested "already excluded" with signed
    `<`/`>`; a free u64 var is lo 0, hi -1, so `x != c` was dropped for every c >= 0 whenever
    the NEQ compiled before a bound did (`x == a >> 63, x != 0` returned x = 0). Now
    `var_b_lt`/`var_b_gt`. Predates this work.
  - **Propagators** (`dvs_prop_templates.c`): `_u64_op` (all unsigned, some operand 64-bit)
    sends `bounds_div_64` / `bounds_mod_64` to uint64 versions (`_div_u64`: [alo/bhi,
    ahi/blo] once b >= 1; `_mod_u64`: r <= min(a, b-1), exact on singletons, a aligned
    backward when r and b are fixed). `bounds_lshr_64` gets a uint64 branch for a 64-bit
    operand: amounts >= 64 give 0 (it clamped to 63: a >= 2^63 >> 64 gave 1), plus the
    backward step.
  - **Compile:** `_binop_exact` takes unsigned DIV / MOD / RSHIFT whenever r is as wide as a
    (the result never exceeds a), before the `_rng64` refusal.
  - **Tests:** `tests/unit/test_cdcl_u64.py` (11: pinned operands over edge values and 60
    random pairs per op, a dividend found in the upper half, UNSAT past the dividend, a
    variable divisor). `test_cdcl_typed`: at 64 bits 13 of 19 now compile, all right; SDIV /
    SREM / SMOD still decline (signed 64-bit ite aux). `test_engine`: `mul_div_64` now
    compiles on CDCL under AUTO (50 solves valid, a reaches the top half, a pin works); the
    bit-blast fall-back checks moved to a 64-bit typed SDIV. ctest 36/36.
  - **`test_cdcl_targets.py::uart_base`** (strict xfail "phase 5: 64-bit unsigned divide")
    now passes on pure CDCL: mark removed.
  - **Suites, slices 1-2:** Python unit 1416 passed (the 2 Verilator-runtime doc failures as
    before). The formal suite was stopped at 80% by the host running low on memory (other
    projects' yosys jobs, ~55 GB); up to there its only change was `uart_base`'s XPASS. Not
    re-run yet; nor the n=1500 soundness campaign.
- 2026-11-05, design step 4b, slice 3 (partial): the SMT front end no longer routes 64-bit
  `bvudiv`/`bvurem` (`_tr_udivrem`) or `bvlshr` (`_tr_binop`) to bit-blast as "wide-arith";
  wider than 64 still does. `bvmul`/`bvadd`/`bvand`... at 64 bits still route, so the
  `mul_div_64` SMT capture stays on bit-blast for its `bvmul`. Also removed the stale comment
  of the old signed div/rem lowering. ctest 36/36; `test_cdcl_u64`, `test_typed_ops`,
  `test_cdcl_targets`, `test_cdcl_compile_gaps`, conformance corpus: 345 passed (the known
  z3 `value_overflow`).
  - **Wide soundness campaign** (`--wide 0.5`, n=600, seeds 1 and 2): the smt2, incr, steps
    and builder doors are clean. The protocol door fails 21 problems, all under a pin:
    a problem CDCL declines (64-bit `bvor`/`bvxor`/`bvand`/`concat`/`bvmul`, none of this
    work's operators) gets an AUTO bit-blast context, which refuses every pin, and a solve
    after the refusal then breaks the pin (`x|y >= z` at 64 bits: every pin, even 5,
    refused; engine bitblast). The step-3a open item (`test_signed_part`), surfaced by wide
    problems; needs pins on a bit-blast context, or the door to treat a refused pin there as
    unknown. Not addressed here.

- 2026-11-05, design step 4b, slice 3 (rest): every unsigned 64-bit operator stays on CDCL.
  - **Backward multiply at 64 bits** (`_bv_mul_back_singleton`): solving `a * k == c` for a
    skipped w = 64 (it worked modulo M = 2^w with an extended GCD, and 2^64 overflows), so a
    64-bit `x * k == c` was searched value by value: 20 of 20 random cases timed out. Now k =
    ko * 2^t: no solution unless c's low t bits are 0, else a = (c >> t) * inv(ko) mod
    2^(w-t) (Newton inverse of an odd number, all in uint64), any w. The 2^t solutions
    narrow an unsigned operand to the first and last inside its domain in O(1) (was: an
    enumeration capped at 4096); a signed operand keeps the capped enumeration.
  - **Backward shift-left**: `a << s == c` with s fixed is `a * 2^s`, so it uses the same
    rule. 64-bit cases timed out 6 of 20 (62-bit: 8 of 20); now none.
  - **Bitwise and/or/xor at 64 bits** (`_bitwise_u64`): the 64-bit propagators reasoned in
    int64 (`alo >= 0`), so compile declined an unsigned 64-bit operand. With every var
    unsigned and one 64-bit they now read bounds as uint64 (and: r <= min(a, b), a, b >= r,
    a fixed r and b pins a's bits; or: the int64 rules in uint64; xor: each of r, a, b
    below the common power of two of the other two, the single-bit flip including 2^63).
    Their explanations fall back to the conservative one (every bound of r, a, b) for these
    vars. Compile accepts unsigned and/or/xor when both operands fit r. `_masked_ceil` /
    `_masked_floor` return a found flag (a pattern >= 2^63 is no longer the -1 sentinel).
  - **`_tighten_u64`** clamps to a narrower var's mask (a bound >= 2^63 would read as
    negative there).
  - **SMT front end**: only widths above 64 flag "wide-arith" now, plus `bvashr` at 64.
    The SMT-LIB `mul_div_64` capture solves on CDCL (correct model, `--engine=cdcl`).
  - **Bug found by the wide campaign (predates this work):** an ITE materialised with no
    width asked for (a shift amount) got a 32-bit result whatever its arms' width.
    `y >=u z << (c ? z : 0)` at 64 bits came back UNSAT on the builder door (ite_value
    compared z's hi, stored -1, in the 32-bit result's order), and a 32-bit result could
    not carry a shift amount >= 2^32 anyway. The result now takes its wider arm's width
    (`_value_to_var`, EXPR_ITE). Regression: `tests/formal/soundness/regressions/r_b8f0109bed`.
    It surfaced now because the unshrunk problem also had a 64-bit bitwise op, which
    compile used to decline.
  - **Tests:** `test_cdcl_u64.py` 31 (modular pinned/backward, no-inverse UNSAT, overflow
    check, bitwise pinned/backward/exact/UNSAT). `test_cdcl_targets.py::cdcl64_xor_highbit`
    (strict xfail "phase 5: 64-bit unsigned xor") now passes: mark removed.
    `test_reason_unknown`'s CDCL-budget case was one 64-bit product `x * y == c`, which CDCL
    now solves (y = c * inv(x) for any odd x: a correct `sat`); it uses two products now.
  - **Suites (final build of slice 3):** ctest 36/36. Python unit 1436 passed (the 2
    Verilator-runtime doc failures; `test_dist_quality`, `test_e2e`, `test_partitioner`
    skipped: scipy / zuspec not installed). Formal 8774 passed, 224 failed: exactly the
    known set (`test_signed_part` 22, `value_overflow` 1, `test_eval_fuzz` 200,
    `test_parser_fuzz` 1). Soundness campaign n=1500, seeds 1 and 2: the 5 known protocol
    pin failures only. Wide campaign (`--wide 0.5`, n=600): seeds 1 / 2 / 3 fail 7 / 3 / 14,
    all protocol door under a pin (seeds 1 + 2 were 21 before slice 3; the builder,
    smt2, incr and steps doors are clean). Presumably still the step-3a pin item (a problem
    CDCL declines gets AUTO's bit-blast context, which refuses every pin); which operators
    the remaining ones decline on is not yet checked.
  - **Next:** signed 64-bit (`bvashr`, `bvsdiv`/`bvsrem`/`bvsmod`, the signed ite aux at
    63-64 bits); pins on AUTO's bit-blast fall-back (the protocol-door failures above and
    `test_signed_part`); then step 5's clean-up (dispatch tables, bound-read audit, the
    sdiv-srem/smod/sext64 routing).
