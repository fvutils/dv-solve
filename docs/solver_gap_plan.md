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
- **2026-11-05, after the slice-3 checkpoint commit (f6b97a6): the protocol-door
  failures diagnosed.** Every saved failure (wide seeds 1-3 and sound seeds 1-2, 43
  problems) was a problem CDCL declined, so AUTO gave it a bit-blast context that refuses
  every pin. The constructs were not signed operators but two compile gaps
  (`DV_LOG=1` prints `cdcl-uncompiled:`):
  - **A: `concat` with a 64-bit result**, even of two constants
    (`x = (concat #b0.. #b0..)`). Both the value path (`_value_to_var`) and the
    `var == concat` path declined at 64 because the propagator reasoned in int64. Now
    `_fire_bounds_concat_64` hands an all-unsigned op with a 64-bit side to `_concat_u64`
    (r read as uint64; forward, hi = r >> lo_w, lo from r's ends when hi is fixed,
    entailment). Compile admits 64 when r and both parts are unsigned and the parts are
    exactly 64 - lo_w and lo_w bits; the explanation was already all-bounds.
  - **B: `(xor (not true) c)`**, lowered as `(!= (not 1) c)`. No path took a `!` (or an
    ite of truths) side. Now EQ/NEQ of two truths (a `!`, a comparison or connective, the
    constants 0/1, an unsigned 1-bit var, an ite of truths), one of them a `!` or an ite,
    compile as `_bool_to_var` guards with eq/ne (`_is_truth`, depth cap 32).
  - **Result:** all 43 saved problems pass the protocol and builder doors on CDCL.
    Regressions `r_d7801c3e23` (concat with top bits set), `r_d0620422f6`,
    `r_4bf5463fe9` (`!` compare). `test_cdcl_u64.py` 40 (concat forward/backward at
    lo_w 1/17/32/63, UNSAT, pin above 2^63 under AUTO; `!` compares).
    Also `r_4463009f22` (`(xor (not true) (ite true true true))`), and
    `t_constraint_operators` in `test_cdcl_targets.py` now passes (its xfail removed).
  - **Harness:** the protocol door's per-pin oracle call did not catch
    `OracleUndecided`; a z3 timeout there aborted the wide seed-2 campaign. It skips that
    pin now, as the other doors do.
  - **Suites:** ctest 36/36. Unit 1446 passed, the 2 Verilator doc failures. Formal 8780
    passed, 224 failed: the known set (`test_signed_part` 22, `value_overflow` 1,
    `test_eval_fuzz` 200, `test_parser_fuzz` 1). Soundness n=1500 seeds 1 / 2: 0 / 0
    failures (the 5 known protocol-pin failures are gone). Wide (`--wide 0.5`, n=600)
    seeds 1 / 2 / 3: 0 / 0 / 0.
  - **Next:** unchanged: signed 64-bit; pins on AUTO's bit-blast fall-back (still
    needed for whatever CDCL declines, and `test_signed_part`); step 5's clean-up.
- **2026-11-05: signed 64-bit on CDCL** (uncommitted, on top of f6b97a6).
  - **`bvsdiv`/`bvsrem`/`bvsmod` at 63-64 bits** were declined only by a guard in
    `_value_to_var`'s ITE arm: a signed ITE of 63-64 bits returned `EXPR_NULL` (the
    lowering's zero-divisor guard is such an ITE). The guard predates the uint64 domain
    work (step 4b), and a signed 64-bit domain is just the int64 range. Removed.
    `test_cdcl_typed.c` is strict at every width now (8, 62, 63, 64; it tolerated
    declines at 64). The SMT front end no longer forces them to bit-blast
    (`nb=sdiv-srem`, `nb=smod`); only above 64 bits, as every other operator.
  - **`bvashr` at 64** was declined by `_binop_exact`: it wants every operand value to
    have an int64 spelling, and an unsigned 64-bit amount has none. A floor shift of a
    signed operand stays in the operand's range for any amount, and the lshr
    propagator reads a u64 amount >= 2^63 as a huge shift, so that case is now exact
    up front. The front end routes `bvashr` to bit-blast only above 64.
  - **`bvashr` search:** `c = bvashr(a, k), k < 5, c != a` took 3-4 s at 32/62 bits and
    hit the 10 s deadline at 63 (`bvlshr`, the same shape: 0.75 s). The shift propagator
    never narrowed the amount from the result, so with `a` fixed, a decided `c` was
    refuted only after deciding `k`. `_shr_narrow_amount` (`dvs_prop_templates.c`) cuts
    `b` to the amounts whose image `[lo(a)>>s, hi(a)>>s]` meets r's domain (both ends
    monotone in s, so an interval; at most 64 steps), in the signed and unsigned
    branches. Now 2.3 s at 32/62/63/64. New `run_ashr` in `test_cdcl_typed.c` (8, 32,
    63, 64 bits, variable amount, EXPLICIT lowering as `_tr_bvashr`).
  - **Remaining cost, not fixed (search heuristics):** both shifts still pay ~10,000
    conflicts through the SMT front end. The first restart comes at 10,000 conflicts
    (`smt2_frontend.c`, restart base), and until then the search decides `k = 0` first
    (saved phase starts at the lower bound, `dvs_search.c`), where `c != a` refutes `a`
    one value at a time. With `DV_MAX_CONFLICTS=100` every case is instant. Options: a
    smaller restart base (it was A/B-chosen, so re-measure), flip a long-held phase at
    restart, or a compile rule `c != a && c = shift(a, k) => k >= 1`.
  - **`test_engine.c`:** its AUTO fall-back case was a signed 64-bit division, which
    CDCL now compiles. It is now `{s, x[2:0]} >= 6'b111000` with a signed `s` (the
    `test_signed_part` shape), which CDCL still declines. Neither a sign-extend into an
    unsigned 64-bit var (CDCL compiles it) nor a >64-bit var (the C API refuses it
    under AUTO too, `DVS_COMPILE_UNSUPPORTED_WIDTH`) would do.
  - **Suites:** ctest 36/36. Unit 1447 passed, the 2 Verilator doc failures. Formal
    8780 passed, 224 failed, the known set. Soundness n=1500 seeds 1 / 2: 0 / 0. Wide
    (`--wide 0.5`, n=600) seeds 1 / 2 / 3: 0 / 0 / 0 failures; their unknown and
    slow>2s counts are about what they were before this round (seed 1: 74 protocol
    unknowns, 19 slow).
  - **Next:** pins on AUTO's bit-blast fall-back (`test_signed_part`, the `test_engine`
    fixture); the restart-floor item above; step 5's clean-up (dispatch tables,
    bound-read audit, the `sext64` routing: CDCL now compiles `r == sext(a)` into an
    unsigned 64-bit var through the builder door, so check whether the front end's
    `nb=sext64` can go).
- **2026-11-05: research, speeding up `bvashr` on CDCL** (measurements only, no code
  change). Problem: `c = bvashr(a, k), k < 5, c != a`, sign bit of `a` set, through
  `dv-solve-smt2 --engine=cdcl`; inputs and timings in the session scratchpad, re-creatable
  from the forms below.
  - **Rewrites are all worse** than the native lowering (with `_shr_narrow_amount`):

    | form (32-bit; 63/64 alike) | default | `DV_MAX_CONFLICTS=100` |
    |---|---|---|
    | F0 native `bvashr` | 2.3 s | 0.00 s |
    | F1 bit view `msb(a) ? ~(~a >> k) : a >> k` | 10 s deadline | 10 s deadline |
    | F2 case split `ite(k=0, a, ite(k=1, a>>>1, ...))` | 10 s deadline | 2.3 s |
    | F3 signed division `floor(a / (1 << k))` via `bvsdiv` | 8.8 s | 0.00 s (64: deadline) |
    | `bvlshr`, for reference | 0.77 s | 0.00 s |

    Each rewrite adds aux vars and propagators (ite, not, shl, div) whose interval
    reasoning is weaker than one shift propagator's, so CDCL prunes less, not more.
  - **The cost is phase saving, not the operator.** `DV_USE_PHASE_SAVE=0`: F0 and
    `bvlshr` solve in 0.00 s at 32 and 64 bits. `DV_MAX_CONFLICTS=1000`: 0.02 s; 3000:
    0.16 s. The saved phase starts at each var's lower bound (`dvs_search.c`,
    the `ps_fresh` init), so the seed-0 search (SMT `check-sat`; seeded solves skip phase
    saving) decides `k = 0` and keeps re-deciding it after every backjump. With `k = 0`,
    `c = a`, and `c != a` can only be refuted one value of `a` at a time: an interval
    explanation cannot say "k = 0 implies c = a". Only the restart (every 10,000
    conflicts) re-ranks the vars.
  - **Per-conflict cost:** F0 does the same 10,000 conflicts as `bvlshr` in 3x the
    time: the lowering's two value-changing casts (unsigned to signed and back, each a
    modular add with a zero addend) are on every propagation path. A native
    pattern-level ashr propagator (an unsigned w-bit operand; within each half of the
    range ashr is monotone: `a >> k` below 2^(w-1), `~(~a >> k)` above; hull the two
    halves) would drop both casts. F1 is this idea written as a rewrite; inside one
    propagator it avoids F1's extra aux vars.
  - **Bug seen on the way:** phase saving stores `ctx->phase_save[dv] = (int32_t)val`
    (`dvs_search.c`, after a decision) into an `int64_t` array, so a saved value outside
    int32 (any wide var) is truncated. It is range-checked before use, so it costs
    search quality, not soundness.
  - **Options, cheapest first:** (1) fix the int32 truncation; (2) don't let phase
    saving re-decide a value that conflicted at the same decision level: drop the
    saved phase for a var whose decision was the conflict's last decision, or rephase
    at restart (CaDiCaL-style: original / inverted / best); (3) a smaller restart base,
    re-measured on the tier fixtures (the 10,000 was A/B-chosen); (4) the native
    pattern ashr propagator for the 3x per-conflict cost. Phase-saving changes need the
    tier1-3 cross-check, since B18/B19 (`docs/solver_bug_backlog.md`) record why
    randomising the saved phases was wrong before.
- **2026-11-05: phase saving, items 1-2 of the research above** (uncommitted).
  - **Correction to the research entry:** the stall is not phase saving re-deciding
    `k = 0` after each conflict. `k = 0` is decided once; every learned clause
    (`k >= 1 or a != X`) asserts at `k`'s level, so `k = 0` stays on the trail until the
    restart. What matters is the *first* value, and the saved phase started at each var's
    lower bound. Also, phase is saved only when a split flips to its second half, never
    on a forward decision, so seed-0 search was in effect "lower bound first" everywhere.
  - **Fix 1:** the saved value was stored as `(int32_t)val` into the `int64_t` array; now
    stored whole.
  - **Fix 2:** a new `ctx->phase_set` flag per var (alloc / grow / free with
    `phase_save`; `dvs_solver_set_phase_hints` sets it). Seed-0 solves now start with no
    phase set, so a var's first decision takes `_pick_value`'s seeded, hole-aware pick
    (B18's trap was a random phase *not* vetted by that pick). `DV_PHASE_INIT=lo` restores
    the old start.
  - **A/B, tier 1-3 cross-check (`run_compare.py`, 30 s):** `lo` 118/118 definitive,
    1.24 s total; `none` 118/118, 1.21 s; 0 disagreements with z3; no fixture moved by
    more than 2x or 200 ms. `bvashr` / `bvlshr` problem: 2.2 s / 0.7 s -> 0.00 s at 32
    and 64 bits.
  - **Seed-0 models change** (check-sat no longer leans to lower bounds). Seed 0 kept
    its RNG state across solves ("seed or preserve"), harmless while phases decided the
    first values; now a repeated seed-0 solve drew different first values
    (`test_seed_zero_stays_deterministic` failed). Seed 0 now restarts from a fixed RNG
    state every solve.
  - **That A/B was blind:** `run_compare.py` runs with the bit-blast fall-back on, so a
    CDCL stall shows up only as a slower answer. `t_constraint_operators` in
    `test_cdcl_targets.py` (pure CDCL): `lo` sat in 0.00 s, `none` unknown at the 10 s
    deadline (22,266 conflicts). Lower bounds first suits Verilator-style constraint
    sets.
  - **Pure-CDCL A/B** (`DV_NO_BITBLAST=1`; tiers 1-3, 30 s; and `test_cdcl_targets.py`):

    | config | definitive | total | targets |
    |---|---|---|---|
    | `lo` | 114/118 | 45.4 s | 8 pass |
    | `none` | 114/118 | 54.4 s | `t_constraint_operators` stalls |
    | `lo`, restart base 1000 | 114/118 | 51.6 s | 8 pass |

    Both alternatives slow the four tier-3 `cache_direct_1way_bmc` fixtures 2-6x
    (`lo` 0.83 s each; `none` 0.6-5.2 s; base 1000 1.4-3.3 s).
  - **Decision:** `lo` stays the default. Kept: the int32 fix, `phase_set`, and
    `DV_PHASE_INIT=none` as an opt-in (seed 0 restarts its RNG every solve only under
    `none`, so the default's seed-0 behaviour is unchanged). `bvashr` / `bvlshr` in the
    `c != a` shape stay at 2.1 s / 0.7 s by default (0.00 s under `none`).
  - **Left open for the `k = 0` trap:** a targeted fix rather than a global heuristic,
    e.g. compile `c != a` with `c = shift(a, k)` (through the casts) to also assert
    `k != 0`; or rephase at restart with an earlier first restart only when the
    conflicts all assert at one level. Not started; low priority unless a real
    workload hits the shape (bit-blast answers it at once: ~8k clauses at 64 bits).
  - **Suites (final: `lo` default, int32 fix, opt-in `none`):** ctest 36/36. Unit 1447
    passed, the 2 Verilator doc failures. Formal 8780 passed, 224 failed, the known set
    (the quickstart doc tests, the determinism test and `t_constraint_operators` pass
    again). Soundness n=1500 seeds 1 / 2: 0 / 0. Wide seeds 1 / 2 / 3: 0 / 0 / 0.
  - **Follow-on design:** docs/structural_reasoning_design.md (static facts such as
    power-of-two and known bits, change of variable, bit-blast-assisted pruning).
  - **Suites (with `none` as default, before the RNG fix; superseded below):** ctest 36/36; soundness and
    wide campaigns 0 failures; new failures: the determinism test (fixed), the target
    above, and 3 quickstart doc tests whose printed model changed (`addr` 0 -> 0x60).
- **2026-11-05, structural reasoning steps 0-2 (bit-blast):** checkpoint `06635aa` first.
  Workload scan done (13 fixtures, `tests/formal/smt2/pow2/`). Bit-blast encodes a
  divisor or factor known to be 0 or a power of two as a shift; signed division by a
  constant +-2^k as a biased arithmetic shift. 64-bit fixtures drop 5-30x in clauses.
  Details, fuzz and measurements: docs/structural_reasoning_design.md, "Results so
  far". CDCL side open: its 64-bit timeouts need known bits/congruence (step 3; diagnosis
  and design in the same doc, "Why CDCL stalls").
  Suites: ctest 36/36; unit: the 2 Verilator doc failures only; formal 8793 passed (13
  new), 224 failed, the known set; soundness seeds 1/2: 0/0; wide seeds 1/2/3: 0/0/0,
  slow>2s 19/26/35 (in the range of earlier runs).
- **2026-11-06, structural reasoning step 3 (CDCL), parts 1-4:** not committed.
  Shift stride rounding, static known bits, monotone top-bit slice
  (`dvs_prop_templates.c`, `dvs_compile.c`); power-of-two links (a product with a
  `1 << k` factor propagated as a shift), the `(x & (x-1)) == 0` fact as `x == 1 << k`
  with k decided, and the redundant `a == q*b + r` link for a div/rem pair. Every
  pow2 fixture but `axi_burst_bytes_64` now solves under pure CDCL in 0.00 s on 6
  seeds (before: 6 of 13 timed out). CDCL fuzz (`tests/formal/pow2_fuzz.py --engine
  cdcl`, gens var/pair, 1,800 cases) and the earlier 2,400-case fuzz: 0 mismatches;
  every sat model re-checked by z3. Full suite on parts 1-4: ctest 36/36, unit/formal
  only the known failures, soundness and wide campaigns 0 mismatches (slow>2s 19/25/34);
  pure-CDCL tiers 1-3 rules on vs. off: identical answers, same total time. Next:
  `axi_burst_bytes_64` (a congruence that never reaches a low slice). Details:
  docs/structural_reasoning_design.md, "Implemented" entries.
- **2026-11-06, `axi_burst_bytes_64` fix (CDCL):** not committed. A redundant
  remainder/low-slice link (`r = a % b` beside `s = a[j-1:0]`, from a slice or a
  concat's low half; b fixed at 2^k gives `r = s % b` for k <= j, `s = r[j-1:0]`
  for k > j; `DV_NO_SLICE_MOD=1`) and power-of-two rounding of `r = 1 << s` in
  the shift propagator. `axi_burst_bytes_64`: `unknown` on 6 seeds before, `sat`
  in 0.00 s after; every pow2 fixture now solves under pure CDCL, and
  `test_pow2_cdcl` has no expected failure. New fuzz generator `--gen slice`.
  Fuzz 3,600 CDCL + 900 bit-blast cases: 0 wrong answers. Suites: ctest 36/36,
  unit/formal only the known failures, soundness and wide campaigns 0
  mismatches (slow>2s 19/26/35). Pure-CDCL tiers 1-3 (118 fixtures, 10 s), rules on vs. all off: identical answers (79 unsat, 35 sat, 4 timeouts), 0 disagreements with z3, total 46.0 s vs. 46.8 s, no fixture moved by more than 2x. Details:
  docs/structural_reasoning_design.md, "the low-slice fix".
- **2026-11-06, performance check:** docs/perf/perf_status_2026-11-06.md. The default
  replay path against xezim is unchanged since 11-05 (the 12 losing benches lose by the
  same ratios). With `--engine cdcl`, the memmap benches and `mul_div_64` win or tie,
  with entropy 1.0: they are now blocked on routing (step 3b), not on the solver.
  SMT tiers against z3: tier 1 6.5x faster, tier 2 1.6x faster, tier 3 0.6x (BMC shapes).
