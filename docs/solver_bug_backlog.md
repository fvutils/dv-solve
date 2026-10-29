# Solver bug backlog — cleanup queue

Bugs found (mostly unexpectedly) while bringing dv-solve up against Verilator's
real SMT output (Track B differential corpus + gap-closing). Each is either
**FIXED**, **worked-around** (safe but the underlying defect remains), or
**OPEN**. Come back and clean these up. Cross-referenced from
`docs/verilator_coverage_test_plan.md` §8 and the session memories.

Legend: 🟥 open · 🟧 worked-around (latent) · ✅ fixed

---

## Principle & audit harness

**CDCL must be correct-or-`unknown`, never wrong. Bitblast is a performance
optimizer, not a correctness crutch.** Verify with `DV_NO_BITBLAST=1` (forces
CDCL, disables the auto-route AND the unknown→bitblast escalation). Permanent
test: `tests/formal/test_cdcl_soundness.py` (cdcl vs z3, `unknown` allowed,
disagreement fails). Soundness model established this pass:
- CDCL `sat` is re-checked by the model-validation net against the FULL problem
  (incl. constraints the compile dropped) → downgrades to `unknown` on violation,
  then escalates to bitblast. So a wrong `sat` is already prevented.
- CDCL `unsat` with dropped constraints is monotone-correct (dropping only
  loosens). So `crc>0` (uncompiled) is sound without special handling.
- **The only real CDCL soundness risk is a wrong `unsat`** (validation doesn't
  run on unsat). That's what the audit hunts.

A 2026-07-09 audit (bitblast disabled) found **5 masked CDCL soundness bugs**
(all wrong `unsat`), 4 now fixed, 1 tracked (B8).

A 2026-07-10 generative fuzz sweep (`tests/formal/test_fuzz_cross_check.py`,
800 seeds vs z3) found **B9** below — a broad 64-bit-unsigned wrong-`unsat`.

### B9. CDCL: 64-bit ARITHMETIC/ite result in [2^63, 2^64) → wrong `unsat`  — ✅ FIXED 2026-07-10
- Minimal: `(assert (bvult (bvadd x y) x))` for two 64-bit vars (the unsigned-
  overflow check) → CDCL `unsat`, z3/bitblast `sat`. Also `bvsub/bvxor/bvnot`
  and `ite` with a >=2^63 branch. **Width-64-specific**; <=32-bit and folded
  constants are fine; plain 64-bit comparisons/equality are sound.
- Root cause: CDCL models a var's domain in `int64`, so a 64-bit value in
  [2^63, 2^64) reads as *negative* and breaks unsigned propagation once it is
  materialized as an aux var (arith/ite do; concat/extract/extend do NOT — they
  stay bit-exact and were verified sound). Same 64-bit-sign family as B6.
- Fix (`smt2_frontend.c` `_flag_wide_arith`): a width>=64 result from binary
  arith / `bvnot` / `bvneg` / `ite` sets `fe->needs_bitblast` → routed to the
  bit-exact engine (same route-to-bitblast pattern as B5, signed div/rem).
  Targeted: comparisons (1-bit result) and concat/extract/extend stay on CDCL,
  so the ~free routing only hits width-64 arithmetic (rare: 0/94 tier1+tier2,
  4/24 tier3, 1/40 verilator). Regressions: `cdcl64_{add_overflow,ite_highbit,
  xor_highbit}_sat` in test_gap_coverage; guarded broadly by the fuzz sweep.

## 🟥 OPEN

- **B128: affine seeded model ignored rows pivoted on a gate — FIXED 2026-10-29 (DIVERSITY: 3 distinct models of 200).**
  A parity base plus `r = q + 1`: each carry AND gate is the lowest column
  of its row `r_i + q_i + c_i`, so it took the pivot; `_model` evaluates
  gate pivots from the AIG instead of the row, so the row never
  constrained q/r, the seeded draw failed on the AIG and the seed-0 model
  (data 2, q 0) stood for every call, under api and api-cache alike. No
  single-bit flip moves it. Fixed (`dvs_affine.c`, `_repivot`): the seeded
  draw moves a gate pivot to a free input column of its row, and a free
  gate column takes its AIG value, pivots recomputed to a fixed point.
  q/r 3 → 140 distinct. `DV_AFFINE_REPIVOT=0` = old. Test
  `test_carry_repivot` (test_affine_exact.py). Found by hand in step 101.
- **B127: `bb_word_eq` cut list read uninitialised stack and could write past it — FIXED 2026-10-28 (memory safety, performance).**
  The right side's concat cuts were collected with `cuts + n` as the base
  AND `&n` as the running index, so they landed at `2*n` on: slots
  `n..2n-1` stayed uninitialised and were sorted in as split points, and a
  side with 64 cuts let the other write up to index 192 of a 130-entry
  stack array. Answers were never affected: whatever the cuts, the parts
  cover `[0, l.size)` and the conjunction is exactly `l == r`. But garbage
  cuts split the comparison where no word fact applies, so bitblast fell
  into a hard search: `fletcher16_bytes16` (test_word_facts) went from 3 ms
  to a 120 s timeout on macOS arm64, where the stack held small values.
  Fixed: both calls write into `cuts` at the shared `n`
  (`dvs_bbsolver.c`). Found by the first macOS CI run; confirmed with
  valgrind (0 uninitialised-value errors after the fix).
- **B126: DPI `dvs_dpi_compile_b64` trusted the decoded blob's size — FIXED 2026-10-12 (memory safety, malformed input).**
  The decoded bytes were cast to `dvs_problem_t` and compiled without
  checking that they hold the header and the pool it describes, so a
  truncated or corrupt blob was read past its end. Fixed: the header and
  pool extent are checked (`dvs_dpi.c`); offsets inside the pool are still
  trusted, as for any problem. Found by the malloc lifecycle survey
  (docs/malloc_lifecycle_analysis.md).
- **B125: contradiction report heap overflow and NULL dereference — FIXED 2026-10-12 (memory safety, diagnostics).**
  `_format_text`/`_format_json` appended with `pos += snprintf(...)`; on
  truncation `cap - pos` wrapped and the next append wrote past the buffer
  (a long constraint name or source path). And `mus_size` was set before
  its id array was allocated, so an OOM there left a NULL array the
  formatters indexed. Fixed: the buffer grows (`_appendf`); `mus_size` is
  zeroed when the ids could not be allocated. Found by the malloc survey.
- **B124: `dvs_bbsolver_rediversify` started from a stale model — FIXED 2026-10-12 (WRONG MODEL, latent).**
  It dropped `node_val` and rebuilt the base model from the SAT instance's
  assignment. When the last SAT verdict came from simulation, the affine
  check or a cube/portfolio worker, that instance never solved (or holds an
  older solve), and the diversity pass keeps the starting values of
  asserted nodes, so the result need not satisfy the problem. The cached
  Verilator path runs with simulation off, so it was reachable only with a
  worker-installed model (`DV_ENGINE=cube`). Fixed: a `model_in_sat` flag;
  otherwise diversify from `node_val`, the current model. Found by the
  malloc survey.
- **B123: costguided LNS buffer overflows and hang — FIXED 2026-10-12 (memory safety).**
  `dvs_solver_lns_optimize`: with fewer than two macros the random-pair
  fallback wrote two entries into a one-entry buffer, looped forever (one
  macro) or divided by zero (none); `new_pos[512]` on the stack overflowed
  for a neighborhood over 256; designs over 256 macros silently skipped
  every improvement (`trial[512]`). Fixed: under two macros the greedy
  placement is returned; buffers are sized from the problem. Found by the
  malloc survey.
- **B122: cube engine could answer UNSAT for an unexplored region — FIXED 2026-10-12 (WRONG UNSAT on OOM / solver init failure).**
  `build_cartesian_frontier` and `build_frontier` stopped at the first
  failed allocation and returned the seeds pushed so far, which the caller
  treats as a complete cover; and if every cube worker failed to start (no
  SAT instance, no child buffer) the queue was never drained but the
  provisional UNSAT stood. Fixed: the frontier builders are all-or-nothing
  (`q_clear`), and a non-empty queue after the join marks the run
  unresolved. Found by the malloc survey.
- **B121: operator rewrites dangled across `(reset)` — FIXED 2026-10-12 (`unknown` / spurious parse errors, Verilator mode).**
  The cache of parsed rewrite templates (rotate_left/right, bvnand, bvnor,
  bvxnor, bvcomp, ...) is kept across a soft reset but pointed into
  `persistent_arena`, which the reset reuses; later declarations overwrote
  cached templates and the operator translated as garbage. At 65acd8d, 4 of
  24 seeds of `tests/formal/test_reset_rewrites.py` answered `unknown`;
  B120's leak hid most cases (leaked blocks are never overwritten), and
  fixing B120 alone made it frequent. Fixed with B120: cached templates
  live in their own `rewrite_arena`.
- **B120: s-expression arena leaked on every reset — FIXED 2026-10-12 (memory leak, ~80 KB per Verilator randomize).**
  `sexpr_arena_reset` rewound to the first block, but allocation never
  moved on to the blocks after it: when the first filled, a new block was
  linked over the rest of the chain, leaking it. Verilator mode deep-copies
  every declaration into the session arena, so each `(reset)` leaked about
  80 KB; 800 resets peaked at 16.9 MB RSS (now flat at 4.6 MB). The
  per-command parse arena leaked the same way for commands over 8 KB.
  Fixed: allocation walks the kept blocks before adding one at the tail
  (`smt2_parser.c`). Test `tests/formal/test_reset_rewrites.py`. Found by
  the malloc survey.
- **B119: a refused incremental add led to wrong `unsat` — FIXED 2026-10-12 (WRONG UNSAT, SMT2 incremental sessions).**
  When the CDCL context refuses an assertion (a variable wider than 64 bits
  declared after the compile), later checks are meant to bit-blast from the
  retained builder. Two defects broke that:
  (1) `_flush_aux` ended retention on the refused hand-off as on any other,
  so past the retention cap (16 MB) the builder was reset and the refused
  assertions were in neither the context nor the builder;
  (2) `dvs_solver_add_constraint` refused the wide variable part-way through
  setting up the add's new variables, and expressions naming a variable no
  problem declared (an id gap, or a variable of the refused add) read an
  all-zero slot, a variable with domain {0}. SMT-LIB problems skip SV
  elaboration, which catches undeclared names for the others.
  Pre-existing; reachable only with a builder past the cap, so found by
  running `test_bb_session_fuzz.py` with `DV_RETAIN_MAX_BYTES=1024` (new
  override, for tests): seeds 1486 and 1226 answered unsat where z3 said
  sat on f04af10-equivalent code. Fixed: a refused hand-off keeps the
  builder whole; an add checks its widths, and that every variable its
  items name exists, before it changes anything (`DVS_COMPILE_BAD_VAR` /
  `DVS_COMPILE_UNSUPPORTED_WIDTH`). 2,000 seeds clean at caps 1 KB, 4 KB
  and the default. Tests `tests/formal/test_retention_end.py`,
  `tests/c/test_add_var_ids.c`.
- **B118: an incremental add read a "new" variable as a compiler-internal one — FIXED 2026-10-12 (WRONG UNSAT, C/Python API).**
  The compile numbers the variables it makes for itself (aux variables for
  sums with constants, ITE results, reification guards) right after the
  ones the problem declared, and nothing in the API said where they end.
  `dvs_solver_add_constraint` treated any declared id below the context's
  `n_vars` as an existing variable and ignored its declaration. So base
  `x2 == x1 + 1000` (aux for `x1 + 1000` at id 3), then an add declaring
  "new" x3 with `x3 == x0 + 3`, constrained the aux (domain [1000, 1200])
  and answered unsat. Same on f04af10. The SMT2 front end was not affected
  (it numbers from the context's `n_vars`); the C and Python APIs were, for
  the obvious "next id". `dvs_solver_add_array_vars` could likewise
  re-initialise an existing variable. Fixed: variables a problem declares
  carry `VAR_DECLARED`; an add that declares an id without it, or
  re-declares at another width, returns `DVS_COMPILE_BAD_VAR` before
  changing anything; `dvs_solver_add_array_vars` takes new ids only. New
  `dvs_solver_n_vars()` / `SolveCtx.n_vars` give the id to number from.
  Tests `tests/c/test_add_var_ids.c`, `tests/unit/test_add_var_ids.py`.
  Found 2026-10-12 writing the problem-lifetime test for B117.
- **B117: incremental SMT2 sessions hold O(flushes x problem size) memory — FIXED 2026-10-12, de07a9c (memory/perf; no wrong answer).**
  While the builder is retained after the CDCL compile (`cdcl_retained`,
  up to 16 MB), every hand-off to the context (`_flush_aux`, at each
  check-sat or push after an assert) calls `dvs_builder_finalize_since`,
  which copies the whole builder and then cuts its lists to the new items.
  Each copy is kept in `fe->aux_problems` until its scope pops, for the
  post-solve model validation. 4,000 rounds of `assert; check-sat` on one
  32-bit variable peak at 447 MB (1,000: 32 MB; 2,000: 115 MB), with or
  without a push per round. The old 32-push cap did not bound it: plain
  incremental asserts reach it. Found 2026-10-12 stress-testing deep pushes
  (dynamic pool follow-ups). Fix: the retained builder is finalized once
  and kept current with `dvs_builder_finalize_append`; a hand-off cuts its
  lists in place (`dvs_problem_cut_since`/`dvs_problem_uncut`) and keeps no
  copy. 4,000 rounds: 447 MB -> 6.6 MB; 10,000 nested pushes 2.76 GB -> 13 MB.
- **B116: a duplicate `declare-fun` of an uninterpreted-sort constant is accepted — FIXED 2026-10-12 (conformance; no wrong answer seen).**
  `(declare-sort S 0) (declare-fun s0 () S) (declare-fun s0 () S)` gives no
  error; z3 reports "constant 's0' ... already declared". The answer that
  follows matches z3's. Bit-vector redeclarations are rejected
  (`cmd_redeclared.smt2`); this is the sort-typed path. Pre-existing (same on
  f04af10). Found 2026-10-11 by `test_parser_fuzz.py::test_mutated_input[21]`:
  removing the B83 xfail lines from the conformance corpus shifted which line
  that seed repeats.
  Fixed: `_declared` also checks the sort-typed constant and function
  tables, and those tables (with the declared sort names) are now scoped by
  push/pop like `define-fun` (B82); before, a sort-typed declaration made
  inside a push outlived the pop, and redeclaring it after the pop with a
  different sort read the stale entry. Conformance cases
  `cmd_redeclared_sort_const.smt2`, `ok_sort_decls_popped.smt2`.
- **B115: clause-notify reentrancy flag can stick, and is process-wide (FIXED
  2026-10-10, uncommitted; perf only).** `dvs_clause_prop.c` `clause_notify_lb/ub` set the file-static
  `_in_clause_prop = 1` and then return early, still set, when `var_id >=
  db->n_watch_vars`. From then on every learnt-clause notification in the
  process is skipped. The flag is also shared by all threads and contexts.
  Learnt clauses are implied by the problem, so skipping them loses pruning,
  not correctness. Found 2026-10-10 while auditing recursion for the stack
  guards. The watch lists are sized once, so any variable created later (an
  auxiliary variable from an incremental assert) took that path. Fixed: the
  flag is `ClauseDB.in_notify`, set only after the range check.
  `tests/c/test_clause_notify.c` fails on the old code.
- **B111: the field split slows three real classes (perf). PARTLY FIXED 2026-10-09 (uncommitted).**
  svcorpus (OpenTitan), split on vs off, median / p90 per solve:
  hmac_base 80 / 116 us -> 134 / 156 us; kmac_base 230 / 1480 -> 415 / 1240;
  dma_seq_item 94 / 130 -> 70 / 1570 (a better median, a 12x tail).
  - hmac (FIXED): `$countones` written as a sum of single-bit slices split
    4- and 6-bit words into 1-bit fields. A slice that is only a term of
    arithmetic (+ - * / %) now records its cuts without asking for a split
    (dvs_sv.c `_fd_walk`, `term`): 88 / 112 us; nothing else in the corpus or
    field_bench moved. A minimum split width is the wrong rule: it also loses
    aes_message_item's gain (39 -> 23 us, spread 0.76 -> 0.09), which comes
    from splitting a 4-bit error-type word whose bits gate constraints.
  - dma (OPEN): chunk_data_size (32 bits) is split by `chunk_data_size[1:0]
    == 0` under implications, and is also compared whole against other
    variables (`chunk_data_size < total_data_size`, `<= mem_range_limit -
    mem_range_base`). Not splitting such words restores the tail (93 / 130
    us) but loses the median gain, and costs field_bench's NoC mesh shape
    1.7 -> 40 us; so not that rule either. Not understood yet.
  - kmac (OPEN): not investigated.
- **B109: a solve after a timed-out solve, with no reset, answered unsat on a satisfiable problem (wrong unsat). ✅ FIXED 2026-10-09 (uncommitted).**
  A solve seals its start as the root (ctx->search_base) and returns with its
  search still on the trail: a model's decisions (get_value reads them), or,
  at the wall-clock deadline, a search in mid-flight. The next solve with no
  reset sealed that state as its root; a dead partial search has no
  extension, so `(y & x) == 31` (16 bits) read as unsat after a clause-learning
  solve hit its deadline. Likewise a pin after a model was made against the
  model's singletons, so any other value was refused. Predates this session
  (HEAD 6b412a6). Found by the wide campaign's new builder[again] check (seed
  707: 5 failures in 2000 problems, all `expect=sat got=unsat`). FIX:
  ctx->search_sealed marks a solve's search on the trail; solve, pin,
  checkpoint, add_constraint, exclude_value and propagate_only first
  backtrack to search_base (dvs_search_discard); reset and restore clear it.
  A model stays readable until the next such call. Gate:
  tests/formal/test_conflict_requeue.py (solve after a deadline timeout, pin
  after a model, model readable); shrunk regressions r_b76c5db806,
  r_516ecf415a, r_824d60b21b, r_09d67b5328, r_7d680ff935. Mutants (no discard
  in solve; in pin) both caught.
- **B110: with clause learning, `(y & x) == 31` (16 bits) runs to the deadline (perf). OPEN.**
  `solve(seed, use_lcg=True)` times out at the default 10 s (and in 5
  restarts), where the plain search solves it at once. Found with B109.
  Learning on a bitwise-and result seems to steer the search away from
  solutions; not investigated.
- **B107: `inside` with any non-literal element is CompileIncomplete (coverage gap, sound). OPEN — the most common of the svcorpus gaps.**
  `expr_in_range` / `expr_in_ranges` / `expr_in_set` compile only when every
  bound and member is a constant leaf: `x inside {[1:y]}`, `[1 : 512-y]`, even
  `[1 : 4-1]`, and `x inside {y, 3}` raise CompileIncompleteError, while the
  same constraint written `1 <= x && x <= y` compiles. Real SV writes these
  all the time (`addr inside {[base : base+size-1]}`); svcorpus hit it in
  OpenTitan entropy_src_env_cfg (worked around by folding constant bounds)
  and flash_ctrl_otf_base (`num_pages`, `ctrl_info_num`, `addr`). Fix idea: in
  the common rewrite pass (dvs_sv_rewrite_masks), lower a membership with a
  non-constant element to its comparisons, in SV sizing.
- **B108: `$countones` compiles only as `var == countones(var)` (coverage gap, sound). OPEN.**
  `expr_countones` needs a plain variable for both result and operand: a
  constant result (`$countones(x) == 2`), a use under `||`, or an expression
  operand raise CompileIncompleteError. svcorpus (tl_seq_item's a_mask,
  hmac_base) writes it as a sum of bits instead. Fix idea: materialise the
  result and operand as aux variables at compile.
- **B106: no variables wider than 64 bits on the builder path, so packed structs cannot be solved (coverage gap). OPEN.**
  `CompileUnsupportedError: ... a variable wider than 64 bits`. OpenTitan
  flash_ctrl_otf_base randomizes a 91-bit `rand_op` struct and 92-bit
  `rand_regions[i]`. The SMT2 path has wide (<= 128-bit) bit-blast support;
  the builder has none. A front end could split a packed struct into its
  members (they are what the constraints read), which the field split does
  for words <= 64 bits.
- **B105: 64-bit unsigned division of a widened 32-bit variable is CompileIncomplete on the builder path (coverage gap, sound). OPEN.**
  Found by the svcorpus (tests/perf/svcorpus), OpenTitan uart_base_vseq's
  NCO check: `(longint'(baud) * 2**20) / clk_hz < 65536`. Through the builder,
  `cast64(x) / u64(K)` and `(cast64(x) * u64(2^20)) / u64(K)` raise
  CompileIncompleteError; the same with signed 64-bit operands compiles and
  solves (x = 1116337). The CDCL path declines 64-bit unsigned arithmetic (a
  value >= 2^63 is negative in its int64 domain, B9) and the builder has no
  bit-blast fallback, so a native front end gets no answer for this class.
  Repro: perf-out/svcorpus/udiv64b.py (gitignored); case
  tests/perf/svcorpus/cases/uart_base.py.
- **B104: a solve after an unsat solve, or after a refused pin, returned a model that broke the constraints (wrong model). ✅ FIXED 2026-10-09 (uncommitted).**
  `dvs_solver_propagate` dequeues a propagator, fires it, and on a conflict
  returned with the propagator off the queue. A backtrack restores the bounds
  and clears the queue, so the search never noticed; but with no backtrack --
  a conflict at the solve's root, or a `pin` that propagation refused -- the
  bounds stay contradictory, and if the failing propagator emptied no domain
  (`x > x`, a constant-false comparison, an ite whose result fits neither
  branch, `~x == -x`) the next solve's level-0 propagation found nothing
  queued, its empty-domain scan found nothing empty, and it searched from the
  contradictory state to a model that broke the constraint. So a second
  `solve()` with no `reset()` after an unsat answer, or a `solve()` after
  `pin()` returned False, answered sat with a bad model. Predates this session
  (HEAD 6b412a6: 25 failures in 1500 problems). Neither door reached it: the
  builder door reset before every solve and the protocol door restored after
  a refused pin. FIX: the conflicting propagator stays queued, so the next
  propagation finds the conflict again (dvs_propagate.c). Gate:
  tests/formal/test_conflict_requeue.py; the campaign's builder door solves
  once more with no reset (`builder[again]`) and the protocol door solves
  after a refused pin (`solved anyway`); eight shrunk regressions
  (r_9e2d51726b, r_45b05f14a3, r_ca9fa37135, r_8a93117e3d, r_6c64346b23,
  r_793049dd23, r_4b2109af30, r_cb95cbb4d0). Mutant (no re-queue) caught.
- **B103: a pin that starts a non-converging propagation read as a conflict (wrong `unsat`). ✅ FIXED 2026-10-09 (uncommitted).**
  Found by the field-split wide campaign (seed 707, protocol door): `ite(c,
  x, y) < y` on 32-bit-or-wider words, x pinned to a large value. The ite's
  hull takes y's lower bound, `r < y` raises it by one, and the pair climb
  until the B61 trail cap aborts propagation. `dvs_solver_pin_var` and
  `dvs_solver_propagate_only` returned that abort as a conflict, so the pin
  read as unsat; and `prop_aborted` stayed set, so every later pin and
  propagation failed until the next solve. Also, the abort dropped the
  propagator it had just dequeued. Predates this session (reproduces at
  6b412a6). FIX: pin and propagate_only treat an abort as not refuted (clear
  the flag; the solve continues the propagation under its deadline), and the
  abort re-queues its propagator. Gate: tests/formal/test_propagation_guard.py
  (pin at 32/33/64 bits, propagate_only, no poisoning) + regression
  r_b90cfa954d.json; 2 of 3 mutants caught (the re-queue is defensive: the
  dropped propagator re-fires anyway here). The perf half is closed too
  (2026-10-09): compile rewrites `ite(c, a, b) op b` -- one side the same as a
  branch, also through the same extension on both sides -- to `c && a op b`
  (irreflexive op) or `!c || a op b` (reflexive), so the pinned solve is sat
  in microseconds instead of a timeout (dvs_sv.c `_mrw_ite_self`; gate
  tests/formal/test_ite_self.py, 8 of 8 mutants caught). A climb through a
  second variable (`ite(c, x, y) < v <= y`) still reaches the cap and times
  out (B30 family); test_propagation_guard.py now uses that shape.
- **B102: 64-bit variable with low bits forced to zero is drawn with a step at 2^63 (distribution, models valid). OPEN.**
  `x[k-1:0] == 0` (also `(x & (2^k-1)) == 0`, which the mask rewrite turns
  into it) on an unsigned 64-bit x: values with bit 63 set come up ~20% less
  often. 8000 seeded `fair_pick` draws, top nibble of x: k=4 `[605, 556, 598,
  555, 555, 523, 536, 535 | 429, 485, 466, 460, 417, 446, 442, 392]`,
  chi-square p=1e-20; k=1 p=0.006, k=8 p=5e-24; 63-bit k=4 p=3e-4; 48 bits and
  below, and a 64-bit x with no constraint, uniform. Predates the field split.
  Goes through the conflict path: `DVS_SPLIT_RANDOM=0` makes it far worse;
  `DVS_KEEP_DRAWS=0` does not change it; the bound-tightening and learnt-literal
  helpers checked so far are sign-aware. Next: trace one solve that conflicts
  at an upper-half value through `lcg_analyze_conflict` and the 64-bit
  bit-slice explain. Matters for 64-bit aligned addresses (PCIe, AXI, DMA).
- **B101: alignment to a random size was slow or skewed (perf + distribution, sound). ✅ FIXED 2026-10-09 (uncommitted).**
  AXI testbenches align to a random `size` three ways. The slice form `size==k
  -> addr[k-1:0]==0` drew 29 µs and skewed size (JSD 0.31); the mask `(addr &
  ((1<<size)-1)) == 0` and modulo `addr % (1<<size) == 0` forms were 12–16 µs
  but as skewed, and 123 µs once the address was split into fields (the
  alignment then failed after the field draw). FIX: `_mrw_align` in
  `dvs_sv_rewrite_masks` rewrites the mask and modulo forms, s an unsigned
  variable of at most 4 bits whose largest shift stays in the word, to the
  slice form; the split handles that. All three: 10–11 µs, JSD 0.017. Gate:
  tests/formal/test_align_rewrite.py (exhaustive); 3 of 5 mutants caught, 2
  equivalent.
- **B100: set or ordering on a >32-bit slice of a ≥40-bit word timed out (perf, sound). ✅ FIXED 2026-10-09 (uncommitted) for words up to 64 bits.**
  `x[35:0] inside {0, 5}` or `x[35:0] < 5` on a 40-, 48-, 62-, 63- or 64-bit
  x: SOLVE_TIMEOUT every seed; `x[15:0]` and equalities were fine. Found by
  the 64-bit field-split test. FIX: field decomposition (now including 64-bit
  words, tied to their parts by bit slices, and the split word made aux so its
  fields are decided first). Gate: tests/formal/test_field_split.py
  `test_random_fields_64`; the aux mutant is caught there.
- **B99: concatenation used as a value was CompileIncomplete on CDCL (coverage). ✅ FIXED 2026-10-09 (uncommitted).**
  Only `var == {hi, lo}` compiled; `{y, x[3:0]} < k`, `{x[7:4], x[3:0]} != k`
  and RISC-V's `{i[31], i[7], i[30:25], i[11:8]} != 0` declined. FIX:
  `_value_to_var` materialises a concat as an aux variable through the concat
  propagator (parts unsigned and exactly their widths, result < 64 bits), and
  `_expr_width` knows a concat's width. Gate: tests/formal/test_concat_value.py
  (150 random concats exhaustive, none may decline); builder-door unknowns in
  the soundness campaign fell from ~800 to ~100 per run.
- **B98: multi-bit or signed variable read as a truth value under `!` / `||` / `&&` (wrong `unsat` and wrong `sat`). ✅ FIXED 2026-10-09 (uncommitted).**
  Found by the negation-pushdown tests. Three sites assumed a truth-valued
  variable is 0/1 or non-negative: (1) `_bool_to_var` returned the variable
  itself as a guard, so a 2-bit `s = 2` made `!s` (encoded `1 - g`) come out
  -1: `!s || z[2:1] == 2` wrong-unsat at s=2; (2) a root `!s`, and `!s` under
  `!(a || b)`, compiled to `s <= 0`: `!(s || f)` wrong-sat for a signed s = -4;
  (3) a root `s` compiled to `s >= 1`, rejecting negative signed values. Any
  front end that lets a multi-bit value stand as a condition (builder, SV, DPI)
  can reach these; SMT-LIB2 cannot, since its Bool is 1-bit. FIX: a guard for a
  variable whose domain is not inside [0,1] is `g <-> s != 0`; `!s` tightens
  both bounds to 0; a signed root `s` is `s != 0`. Gate:
  tests/formal/test_bool_guard_width.py (exhaustive, widths 1/2/8 unsigned and
  3 signed, 12 forms); 3 of 4 mutants caught, the fourth site is now bypassed by
  the negation pushdown in `dvs_sv_rewrite_masks` and kept as a guard.
- **B95: Verilator 5.046 parity asserts misdetected as 5.052+ enumeration → diversification off (distribution regression, models valid). ✅ FIXED 2026-10-09 (uncommitted).**
  `_is_vlt_enum`'s hash pattern `(= #b… (bvxor <bits>…))` (added 5064e4b,
  2026-10-04) also matched solveDiversityXor's parity round, so the first
  parity assert of every randomize() set `vlt_enum` and the remaining rounds
  routed `bitblast (enumeration)` with `set_diversify(0)`. rand-core under the
  default Verilator protocol: range_1000 excess JSD 0.000 → 0.609, a_lt_b
  0.004 → 0.566, packet 0.004 → 0.726. FIX: a lone XOR counts as enumeration
  only with UniGen2's `#b0` chain seed (`_vlt_xor_seeded`); concat hashes and
  blocking clauses unchanged, riscv-dv 5052 tests pass. Gate:
  tests/unit/test_verilator_mode_sampling.py (parity-vs-enumeration route,
  chi-square under the protocol); mutant caught.
- **B97: bound decisions in a seeded solve skewed gappy domains (distribution, models valid). ✅ FIXED 2026-10-09 (uncommitted).**
  Found bisecting the residue after B95 (first bad: merge 111a909 of 4fcd7e2's
  `_bound_decision` with b7cfbb0's Verilator CDCL sampling). A wide variable
  with conflict activity was decided `x <= v` / `x >= v`; a failed bound drops
  a side chosen by interval width, not solution count, and VSIDS activity
  outlives a reset, so in a randomize() loop it applied from the first
  decision. ot_aon_wkup / disjoint_ranges / sum_eq_const under the protocol
  failed chi-square. FIX: a seeded solve makes value decisions (uniform
  rejection) until DVS_BOUND_DECISION_TRIES (32) of them failed for that var in
  this solve; unseeded solves unchanged; test_lcg_lifted_sum (NVMe product) and
  test_reified_or (power-of-two) still pass. Gate as B95; mutant caught.
- **B96: CDCL `(x & M) == V` cost doubles per masked bit (perf cliff, sound). ✅ FIXED 2026-10-09 (uncommitted) for single-mask shapes.**
  `bounds_band` has no backward rule, so the word was drawn and rejected by
  search: 32-bit `(a & (2^k-1)) == 0` 25 µs at k=8, 6.8 ms at k=16, timeout at
  k≥20. FIX: `dvs_sv_rewrite_masks` (dvs_sv.c, internal, hidden symbol) runs in
  every CDCL compile after elaboration and rewrites constant-mask comparisons
  into bit-field comparisons: now 2–10 µs at any k, scattered masks 10 µs and
  uniform, 64-bit masks compile in the API. Tests:
  tests/formal/test_mask_rewrite.py (exhaustive 4-bit equivalence, 4/5 mutants
  caught, the 5th equivalent); soundness generator gained mask shapes (6000
  problems, 2 campaigns, 0 failures). Later the same day the pass also sees
  through bit selects (`(x[11:0] & 'hffc) == V`) and pushes `!` down to
  flipped comparisons, so `!(x[31:20] inside {..})` and `!(a && b)` over
  fields compile on CDCL (before: CompileIncomplete). The remaining multi-field
  shapes (OpenTitan's TPM address: 1.8–2.6 ms, skewed) are fixed by field
  decomposition, same day: TPM 18 µs, PCIe page edge 1.4–1.8 ms → 2–3 µs
  (`docs/bitfield_linearization_research.md` §10–11).

- **B38: builder API has no consistent width rule for arithmetic vs constants — wrong `sat` and `unsat` — ✅ FIXED 2026-10-01 (d86438b, 804e474).**
  `binop == const` compiles through the pin-var path at 32 bits/integers
  (u4 `x + 3 == 0` → unsat), while `< <= > >=` against a const wrap at the
  operand var's width (u8 `x + 3 < 10` → sat at x=254), and out-of-width
  consts raise CompileUnsatError (u8 `x + 200 > 290`). The validator uses
  "widest sized operand, consts unsized" -- a third rule -- so neither class
  was caught. Survey: 159/2868 in-width disagreements (all `==`), hundreds
  out-of-width. DECISION (owner, 2026-10-01): SystemVerilog sizing/signedness
  (unsized const = 32-bit, context-determined width, unsigned-if-any-unsigned);
  SMT2 front end gets SIZED constants (ExprConst.width) to stay modular.
  Fix: new elaboration pass src/c/zsp_sv.c rewrites builder problems into an
  explicit SV form (sized consts, EXPR_SV_CAST) before CDCL compile and
  bitblast; zsp_validate.c is an independent SV evaluator; SMT2 consts are
  sized and its problems are flagged explicit. Fixed along the way: 64-bit
  `x != 0` wrong unsat (ne_32 on 64-bit vars), `x == y` alias skipped domain
  merge for u32/64-bit, `&`/`|` bounds unsound for negatives, modular
  add/sub/mul/shl no-ops at width 64, consts truncated into int32 slots,
  contradiction probe used out-of-range consts. `>>` is now SV logical (was
  floor for signed in 82cfb7f). 128-bit math via src/c/zsp_i128.h
  (MSVC-portable; -DZSP_NO_INT128 forces the portable path).
  Regression: tests/unit/test_sv_width_semantics.py (1485 disagreements
  before, 0 after). Docs: concepts/problem-model.md now describes SV sizing.
  FOLLOW-UP DONE 2026-10-01: BIN_ASHR = 18 (SV `>>>`) added in 5674f8c, docs
  a13dc62; SMT2 bvashr now supported. zuspec-dataclasses native_backend.py
  RShift -> 18 edited locally (uncommitted, separate repo); that checkout's
  tests/solver has 95 pre-existing API-skew failures unrelated to this.

- **B39: signed DIV/MOD/RSHIFT family — ✅ FIXED 2026-10-01 (82cfb7f).**
  `(x % 3) == -2` unsat / `== 1` sat for x=-8 (mod/div keyed signedness off
  the result pin var only); `r == x >> 1` unsat for every r with x negative;
  SIGFPE on INT64_MIN / -1 and % -1; 32-bit pin var wrapped 64-bit `x + 1 == 0`;
  negative consts materialised unsigned beside signed operands; `bounds_ne/lt`
  `v±1` overflow at INT_MIN/MAX → no-op bound (wrong sat); `explain_bounds_mod`
  omitted r. Also resolves B28 (its two strict-xfail tests now pass).
  Regression: tests/unit/test_signed_arith_semantics.py.

- **B36: CDCL answer reported the previous bit-blast model — ✅ FIXED 2026-09-30.**
  Default mode: `QF_ABV` solve (bitblast) → `(reset)` → `QF_BV` solve (CDCL)
  → `sat` with the OLD bit-blast values (`x=#x11` under `(assert (= x #x22))`).
  `bb_solver` outlives its answer (the Verilator identity cache carries it
  across reset) and `_fe_get_var_value` read it whenever it existed. Fix:
  `fe->bb_model_valid`, cleared at each check-sat, set only by the bit-blast
  paths; readback gates on it. The Verilator-mode check-sat-assuming reuse path
  deliberately keeps the prior model. Regression: `test_stale_model.py`.

- **B37: assertion inside a popped scope still enforced → wrong `unsat` — ✅ FIXED 2026-09-30.**
  `(assert (bvugt x #x10)) (check-sat) (push 1) (assert (bvult x #x05)) (pop 1)
  (check-sat)` → `unsat` (z3: sat). push flushes, so the scope's assert sat
  pending in the builder; pop never discarded it and the next check-sat's flush
  handed it to the ctx. Fix: pop resets the builder when compiled.
  Regression: `test_incremental_escalation.py`.

- **Escalation after incremental asserts — ✅ FIXED 2026-09-30.** Asserts made
  after the CDCL compile became aux problems the bitblast fallback couldn't see,
  so a CDCL `unknown` (or a bitblast-routed instance) after them answered
  `unknown` — EVERY stock-Verilator-5.046 randomize() warned "Solver error:
  unknown" (its XOR-hash asserts come after the first check-sat). The builder is
  now kept after compile (`cdcl_retained`, ≤16 MB; `builder_finalize_since`
  hands CDCL only the new items) and the fallback solves a full copy
  (`fe->bb_problem`, owned with `bb_solver`). pop ends retention.

- **B31: reified strict compare of two ranges → wrong `unsat` — ✅ FIXED 2026-09-30 (e938aeb).**
  `x > #x10 ∧ (= #b1 (ite (bvult x (bvor y #x01)) #b1 #b0))` → CDCL `unsat`
  (sat: x=#x11, y=#xff). Verilator's `(= #b1 (__Vbv P))` form hits it. Root
  cause: `_fire_reification_32/64` (`zsp_prop_templates.c`), guard=0 branch
  (`x > y`), tightened `x.lb := y.hi+1` / `y.ub := x.lo-1` — entailment, not
  propagation — instead of `y.lo+1` / `x.hi-1`. Invisible against a constant
  (`y.lo == y.hi`), so only composed RHS or var-var compares failed; every
  strict / negated compare goes through this branch via `_reify_cmp_var_var`.
  Found by the fuzzer once it generated `bvneg`. Same commit also fixed:
  (a) LCG clause conflicts seeded analysis from a STALE `conflict_prop_ref`
  → bogus learnt unit → wrong `unsat` (now `ctx->conflict_clause_idx`);
  (b) reification explainers cited only the guard; (c) tautological /
  duplicate learnt clauses livelocked the search → chronological fallback;
  (d) conflict-loop deadline checked every 1024 conflicts (10 s budget ran to
  ~38 s) → every 16. Regression: `tests/formal/test_reified_compare.py`.
  **Side effect:** at width 32, reified self-compares (`y > (y|x)`,
  `bvslt y y`, `y < y*1`) now stall like their unreified forms (= B30); the
  old wrong rule happened to answer a few instantly. No wrong answers.

- **B35: `clause_notify_lb/ub` leave `_in_clause_prop` set on early return.**
  `zsp_clause_prop.c`: the reentrancy flag is set, then the function returns
  early when `var_id >= n_watch_vars` without clearing it, so clause
  notifications stay off for the rest of the process. Perf only (clauses
  still checked elsewhere); found by the B31 investigation, not fixed.

- **B34: `bvneg` was an unwrapped integer negation on CDCL → wrong `unsat` — ✅ FIXED 2026-09-30.**
  `(= (bvneg x) K)` returned `unsat` for EVERY constant K at every width
  (e.g. `(= (bvneg x) #xff)`, satisfied by x=1). The front end lowered `bvneg`
  to `UN_NEG`, which CDCL (`prop_add_unary_neg_32`) and the model validator
  (`zsp_validate.c`, `r = -a`) treat as integer negation with no mod-2^w wrap.
  Fix: `smt2_frontend.c` lowers `bvneg x` as `bvsub 0 x` (the wrap-aware sub
  path). Why nothing caught it: the fuzzer never generated `bvneg` (now it
  does). Regression: `tests/formal/test_bvneg.py` (78 cases vs z3, both
  engines; 26 fail without the fix). Cost: CDCL can't see that
  `(bvneg (bvneg x))` is x, so `(distinct (bvneg (bvneg x)) x)` at 16 bits
  burns the ~10 s CDCL budget before bitblast proves it (correct, slow).
  `UN_NEG` itself is still unwrapped for the C/Python builder API, where it
  means SV integer negation — audit whether that is right for unsigned vars.

- **B32: `get-value` / `get-model` print Bool variables as `(_ BitVec 1)` — ✅ FIXED 2026-10-07** (fw-hdl R5; `Smt2Var.is_bool`, Bool terms print `true`/`false` too; `tests/formal/test_smt2_conformance.py`).
  Found 2026-09-30 while writing the SMT-LIB2 docs. `(declare-const p Bool)`
  … `(get-value (p))` → `((p #b0))`; SMT-LIB requires `((p false))`, and
  `get-model` emits `(define-fun p () (_ BitVec 1) (_ bv0 1))`. Root cause:
  `Smt2Var` has no Bool flag, so the printers can't tell a Bool from a 1-bit
  BV (`_pad[2]` has room for one). Not a soundness issue: values are right,
  only the rendering is. Verilator is unaffected (it wraps Bools as 1-bit BVs
  via `__Vbv`); a tool that parses `true`/`false` (yosys-smtbmc's
  `get_net_bool`, z3-style drivers) would misread them. Fix changes output
  that callers parse, so check interop when fixing.

- **B75: a stray `)` silently ended batch input — ✅ FIXED 2026-10-07 (prover soundness).**
  fw-hdl R2. `sexpr_parse` returned NULL for an unmatched `)` and the batch
  loop read NULL as end of input, so every command after it (including
  `check-sat`) was dropped, with exit 0 and no output. A wrapper that reads
  "no `sat`" as "proven" would report a proof. Interactive mode lost frame
  sync instead (depth went to -1). Now every parse error is an
  `(error "line L column C: ...")` on stdout, the parser resynchronises, the
  session is poisoned (check-sat → `unknown`), and the exit status is 4.
  Regression: `tests/formal/test_smt2_conformance.py`.

- **B76: every diagnostic went to `/tmp/dv-solve.log` when stdout was not a tty — ✅ FIXED 2026-10-07.**
  fw-hdl R2/R4. Added for yosys-smtbmc (which merges stderr into the protocol
  stream), it hid every error from scripted use, and `DV_LOG=1` wrote a file
  named `1`. Now errors are `(error ...)` responses on stdout; warnings and
  diagnostics are silent unless `--stats`, `-v` or `DV_LOG` (stderr, or the
  file when `DV_LOG` is a path containing `/` or `.`). Also fixed alongside:
  unknown commands (`define-sort`, `declare-datatype`) were ignored with a
  logged warning and the run answered anyway; `echo` doubled the quotes; the
  interactive reader counted parens inside `|quoted symbols|`; `12ab` and
  `#xFG` lexed as two tokens; get-value printed `(? v)` keys and `#b0`
  placeholders for terms it could not evaluate.

- **B77: `reset-assertions` kept every assertion in force — ✅ FIXED 2026-10-07 (wrong unsat).**
  Found writing the fw-hdl model check (plan 2.5). It was a no-op that only
  cleared the cached result, so `(assert (= x #x1)) (check-sat)
  (reset-assertions) (assert (= x #x2)) (check-sat)` answered `unsat`; z3
  answers `sat`. Assertions made inside `push` levels also survived. Now it is
  a fast `(reset)` followed by a replay of the level-0 `set-logic`,
  `set-option`, `set-info`, declarations and definitions, which are recorded as
  they succeed. Regression: `tests/formal/test_smt2_conformance.py`
  (`test_reset_assertions_*`).

- **B78: a `define-fun` body's `let` captured the caller's argument — ✅ FIXED 2026-10-08 (wrong sat and wrong unsat, both engines).**
  Found by the fwhdl-core SECDED family (`tests/formal/gen_fwhdl.py`), whose
  let names, like fw-hdl's (`|xor.4|`), repeat between a function body and
  the assertion that calls it. Function application was dynamically scoped:
  arguments were expanded lazily inside the callee, under its lets, and a
  caller's let could also capture a global the body used. Minimal:
  `(define-fun f ((p (_ BitVec 4))) (_ BitVec 4) (let ((t #x0)) (bvadd p t)))`
  `(assert (let ((t x)) (not (= (f t) x))))` answered `sat` (z3: `unsat`);
  `(let ((t x)) (= (f t) x))` with `x = 5` answered `unsat` (z3: `sat`). The
  Phase 2 model check turned the wrong `sat` into `unknown` by default; the
  wrong `unsat` it cannot see. Now the substitution stack holds linked
  environments: a body sees only its parameters and the globals, and an
  argument is translated in the caller's environment at the call. Regression:
  `tests/formal/test_eval_fuzz.py::test_translation_matches_eval` (let names
  from a shared 3-name pool; the solver must agree with the evaluator on every
  term) and `tests/formal/test_smt2_conformance.py::test_define_fun_scoping`.

- **B79: `(set-info :smt-lib-version 2.6)` was an input error — ✅ FIXED 2026-10-08 (regression from 094e5cd).**
  The Phase 1 lexer rejected `2.6` as an invalid numeral, which poisoned the
  session: every SMT-LIB benchmark file starting with that line answered
  `unknown`. Decimals are now one token, accepted as attribute values; in a
  term they are an error (no Real sort here).

- **B80: a subterm that failed to translate was re-reported as an input error — ✅ FIXED 2026-10-08.**
  `select`/`store`/`as const`/`=` reported "index must be a BV" and similar
  when an operand had already failed (an unsupported construct), turning an
  honest `unknown` into an input error that poisoned the rest of the session.
  Now only an operand of the wrong kind (array for scalar) is an error.

- **B81: a term wider than 255 bits crashed the translator — ✅ FIXED 2026-10-08 (SIGSEGV).**
  `((_ sign_extend 192) x)` on a 64-bit `x` passed a 256-bit width through
  `uint8_t` builder parameters. Any extension, concat or repeat wider than
  `SMT2_MAX_BV_BITS` (192) now answers `unknown`.

- **B82: a `define-fun` made inside a `push` survived the `pop` — ✅ FIXED 2026-10-08 (wrong unsat).**
  Found by the conformance corpus. Definitions were never scoped: after
  `(push 1) (define-fun f () (_ BitVec 4) #x1) (pop 1) (define-fun f () (_ BitVec 4) #x2)
  (assert (= f #x2))` the lookup found the popped `f` first and answered
  `unsat` (z3: `sat`); a popped `f` also stayed usable. `push` now saves the
  define-fun and define-sort counts and `pop` restores them, as for variables.
  Alongside: a second `set-logic`, a redeclared name, and operands of
  `=`/`distinct`/`ite`/bit-vector operators with different widths (`(= x b)`
  with a 4-bit `x` and an 8-bit `b` answered `sat`) are now input errors, as
  in z3; `:print-success`, `get-option`, `get-assertions` (with
  `:produce-assertions`, or whenever the model check keeps them) are
  implemented; an unsupported `get-*` query is an error that no longer
  poisons the session. Regression: `tests/formal/test_conformance_corpus.py`.

- **B83: `push`/`pop` before the first declaration left the next `check-sat` `unknown` — ✅ FIXED 2026-10-11 (completeness, honest unknown).**
  `(push 1) (pop 1) (declare-const x (_ BitVec 8)) (assert (= x #xff)) (check-sat)`
  answered `unknown`. The cause was not `dvs_builder_finalize_since` as first
  guessed: the push compiled an empty problem, which left the context with no
  per-variable tables (capacity 0), so the first incremental add had no room
  for `x` and failed. Fixed as a side effect of the dynamic pool work
  (docs/dynamic_pool_plan.md P2a, commit 21fdc95): an add now grows the
  tables. `ok_push_before_declarations.smt2` and `ok_redeclared_after_pop.smt2`
  are no longer xfail.

- **B84: a `declare-sort` value passed on through nested functions hung the front end — ✅ FIXED 2026-10-08 (hang).**
  Found reviewing the define-fun memo. `_resolve_sym`, which turns a
  parameter into the argument it stands for (for sort-function applications,
  constant array indices), looked each argument up in the callee's
  environment instead of the caller's. In Yosys' usual output,
  `(define-fun f ((state S)) .. (g state))`, `state` then resolved to itself
  and the loop never ended. It now follows `arg_env`, as translation has since
  B78. Regression: `tests/formal/conformance/ok_sort_fun_through_params.smt2`.

- **B85: a symbolic `select` indexed by the first declared variable read a non-node — ✅ FIXED 2026-10-08 (honest unknown; undefined behavior).**
  Found by the define-fun memo fuzzer. A dense array from `declare-const` was
  `calloc`ed without setting `store_idx_varid = UINT32_MAX` and
  `store_val = EXPR_NULL`, so the R1 rewrite in `_array_select`
  (`select(store(a, i, v), i) = v`) took any select whose index is var 0 for
  a store at that index and returned ref 0, which points into the pool
  header. Seen as `[dvs_bbsolver] error: unknown ExprKind <n>` and `unknown`
  where z3 answers; the value read depended on the header, so it could in
  principle have been a wrong answer. Arrays declared before the index
  variable were not affected, which is why the array fuzzers missed it.
  Regression: `tests/formal/conformance/value_select_by_first_var.smt2`. The
  bit-blaster's error now prints the kind it did not recognize.

- **B87: a `push` after a >64-bit declaration dropped its scope; the next `pop` removed the enclosing one — ✅ FIXED 2026-10-09 (wrong `sat`).**
  Found by the piece 6.1 session fuzzer (`tests/formal/test_bb_session_fuzz.py`).
  When a variable wider than 64 bits was declared after the CDCL context was
  compiled, the next `push` failed to flush it into the context
  (`dvs_solver_add_constraint` takes no >64-bit variable) and returned without
  recording a scope. The matching `pop` then removed the enclosing scope with
  its assertions: `push; x = 7; declare q (70 bits); push; pop; x = 8;
  check-sat` answered `sat`. A failed flush now keeps the scope and switches
  the session to bit-blasting (`needs_bitblast`), in `push`, `check-sat` and
  `check-sat-assuming` alike: the context missed a constraint that was handed
  off all the same, so it no longer stands for the assertion set. Same commit:
  `push` skips the CDCL compile when the problem must bit-blast anyway (it
  re-finalized and failed to compile the whole problem at every push).
  Regressions: `test_push_after_a_wide_declaration_keeps_the_scope`
  (`tests/formal/test_incremental_protocol.py`).

- **B88: `check-sat-assuming` solved a CDCL context whose compile had proved the assertions unsat — ✅ FIXED 2026-10-09 (wrong `sat`, raw; the model check turned it into `unknown`).**
  Found by the session fuzzer. A compile that proves unsat keeps the context
  with `compiled == 0`; `_ensure_compiled` then returned 0 ("ready") on every
  later call, so `check-sat-assuming` pinned and solved a context that was
  never compiled. A context that is not compiled now reports unsat each time,
  and a `pop` below the depth where the compile ran drops it (the verdict
  went with the scope). Two neighbours, same root (conflicts at the context
  root): a flush conflict found by one `check-sat-assuming` was forgotten by
  the next, whose checkpoint lost it (`flush_unsat` now records the depth),
  and `push` flushed into a context still holding the last model, so
  asserting `p` after a model with `p = false` looked like a conflict (it now
  resets first, as `check-sat` does). With B89's validation net in place these
  answer `unknown` at worst, so their regressions run with the net off
  (`test_check_sat_assuming_cdcl_without_the_net`, and
  `test_check_sat_assuming_cdcl` in `tests/formal/test_incremental_protocol.py`).

- **B89: CDCL `check-sat-assuming` had no model-validation net — ✅ FIXED 2026-10-09 (wrong `sat`, raw; masked by the model check).**
  Found by the session fuzzer. The compile has no arm for `v == not(x < c)`
  and leaves it out of the context; `check-sat` catches that by validating
  the model against the whole problem and escalating to bit-blast,
  `check-sat-assuming` did not, so `p = (not (bvult x #x3))` under the
  assumption `(not p)` answered `sat` with `x = 4`. It now validates and
  escalates to the bit-blast session with the assumptions in a scope. The
  missing compile arm (`var == not(cmp)`) stays: the validation net and the
  escalation are the designed response to an uncompiled constraint.

- **B90: `get-model` printed the low 64 bits of a variable wider than 64 bits — ✅ FIXED 2026-10-09 (wrong model output).**
  Found while starting piece 6.2. `_cmd_get_model` read every variable with
  the 64-bit accessor and printed `(_ bvN W)`: a 100-bit all-ones `x` came out
  as 2^64 - 1. `get-value` was right (it reads wide values by limbs), and no
  test or fuzzer read `get-model`, which is how it went unseen. Wide values now
  print as `#b` literals. The session fuzzer now asks `get-model` after every
  `sat` and checks it lists each visible name with `get-value`'s value.
  Regression: `test_get_model_wide_values`
  (`tests/formal/test_incremental_protocol.py`).

- **B91: `|x|` and `x` were different symbols; user names could collide with internal ones — ✅ FIXED 2026-10-09 (valid input rejected).**
  SMT-LIB 2.6 §3.1: a quoted symbol whose body is a simple symbol is that
  symbol. The lexer kept the bars, so `(declare-fun |c| ...)` followed by `c`
  was an "unknown variable" input error. The parser now strips the bars from
  such symbols and marks them `quoted`, and printing puts them back, so a
  `get-value` key still echoes as written. Same commit: the front end named
  its auxiliary variables `__auxN` and `__rdN`, so a user `__aux3` was
  "already declared" (or, declared first, could clash with a later
  auxiliary), and `get-model` hid any user name starting `__aux`. Internal
  names now start with a backslash, which no SMT-LIB symbol can contain.
  Regression: `test_quoted_and_simple_symbols_are_one_symbol`.

- **B92: `check-sat` forgot a flush conflict in a live scope — ✅ FIXED 2026-10-09 (wrong `sat` from CDCL, raw; validation made it `unsat` via bit-blast).**
  Found by the session fuzzer once CDCL's validation failures were checked.
  `(push 1) (assert (distinct a a)) (check-sat)` answered `unsat` from the
  flush; `(assert (= a #x01)) (check-sat)` then reset the context, which lost
  the root conflict, and CDCL answered `sat`. B88 fixed the same thing for
  `check-sat-assuming`; `check-sat` now also answers `unsat` while
  `flush_unsat` is set. Regression: `test_cdcl_incremental_without_the_net`.

- **B93: an incremental assert at the root lost its bounds at the next reset — ✅ FIXED 2026-10-09 (wrong `sat` from CDCL, raw; validation made it right via bit-blast).**
  Found by the session fuzzer. `(assert (bvult a b)) (check-sat)
  (assert (= a #x5)) (check-sat) (check-sat)`: the second `check-sat` saw
  `a = 5`, the third `a = 0`. A constraint compiled to a bound tightening has
  no propagator to re-derive it; inside a checkpoint the scope log keeps such
  bounds (B67's neighbour), but at the root nothing did, and
  `dvs_solver_reset` restored the variable from the compile snapshot. Every
  incremental CDCL session with a unit or bound assertion after its first
  `check-sat` hit this, so every one of them escalated to bit-blast after a
  failed validation. `dvs_solver_add_constraint` now writes the bounds it sets
  at the root into the snapshot (tier-0 and tier-1). Losing a constraint only
  loosens the problem, so this was never a wrong `unsat`. Regression:
  `test_cdcl_incremental_without_the_net` (4- and 40-bit).

- **B94: a seeded SMT-LIB script gave the same model for every seed — ✅ FIXED 2026-10-09 (no diversity; models were right).**
  Found by piece 6.4 while turning fw-hdl's 60-seed check into a benchmark.
  Phase 3 (5179d8e) routed bit-level QF_BV to the bit-blast engine, whose
  exhaustive simulation stopped at the lowest satisfying input; the
  flip-check that follows only moves to a neighbouring solution, and a
  hash-like filter has none. fw-hdl's check (`m` in 'A'..'Z',
  `crc32(m)[1:0] == 0`) went from all 6 letters to 4; `countones == 4` and
  `a + b == 100` gave one model in 2000 seeds. A seeded sweep now goes
  through every assignment and keeps one uniformly (reservoir sampling), and
  a seeded one-shot QF_BV script goes to bit-blast, whose simulation samples
  uniformly where CDCL skewed orderings (`a < b`: `a == 0` at half its
  share). Regression: `tests/formal/test_seed_diversity.py`; benchmark: the
  `dv-seed` arm of `rand-core`.

- **B86: exit status `unknown` after a `push`, `pop` or `assert` that followed the last `check-sat` — ✅ FIXED 2026-10-09 (wrong exit status).**
  Found by the piece 5.3 incremental runs. The exit status (D3) was read from
  `has_result`, which `push`, `pop` and `assert` clear because they discard
  the model, so a session ending `(check-sat) (pop 1)` printed `sat` and
  exited 2. The front end now records each `check-sat`'s answer separately
  (`answered`, set in `_check_sat_checked`; a `check-sat` that fails early
  counts as `unknown`). Regression: three cases in `test_exit_status`
  (`tests/formal/test_smt2_conformance.py`).

- **Coverage: standard QF_BV operators answered `unknown` (fixed 2026-10-08).**
  `bvnand`, `bvnor`, `bvxnor`, `bvcomp`, `bvredor`, `bvredand`, `rotate_left`,
  `rotate_right`, the overflow predicates (`bvnego`, `bvuaddo`, `bvsaddo`,
  `bvumulo`, `bvsmulo`, `bvusubo`, `bvssubo`, `bvsdivo`) and signed compares
  wider than 64 bits had no lowering. They are now rewritten into supported
  operators by their SMT-LIB definitions (`_rewrite_text` in
  `smt2_frontend.c`); the translator fuzz checks them. Overflow predicates on
  operands wider than 96 bits need a product wider than 192 and stay `unknown`.

- **z3 5.1 is wrong on constant-array equality over a small index sort (oracle hazard, not our bug).**
  Found 2026-10-07 by `tests/formal/test_eval_fuzz.py`. z3's `get-value`
  compares arrays structurally (`false` for equal arrays), and z3 answers
  `sat` for `(assert (= A B))` with constant-array stores over a 1- or 2-bit
  index where A and B differ at an index no store covers. Element by element,
  z3 itself shows they differ. `rewriter.expand_store_eq=true` fixes the
  2-bit case but not the 1-bit one. The evaluator fuzzer rewrites array
  equalities into element equalities before asking z3. `DV_ORACLE` and other
  z3 cross-checks can report false mismatches on such terms.

- **B33: `(get-unsat-core)` is unsupported and produces no reply on stdout — ✅ FIXED 2026-09-30.**
  CONFIRMED with stock Verilator 5.046: an unsat randomize() hung forever. Now implemented: replies with the `:named` assertions in scope (valid, non-minimal core; push/pop-scoped, cleared by reset), or an `(error ...)` line on stdout — never silence. Verilator then prints UNSATCONSTR for each and randomize() returns 0, as with z3. Regression: `tests/formal/test_unsat_core.py`. Follow-up: minimize the core (Verilator warns on every listed constraint).
  Original report:
  Found 2026-09-30. The `(error "unsupported: …")` line goes to the diagnostic
  log (whenever stdout is not a tty), so a driver waiting for a reply blocks.
  Bundled Verilator 5.046 sends `(get-unsat-core)` after an `unsat`
  randomize() (named-assert re-check in `VlRandomizer::next`) and reads one
  line → **probable hang on an unsatisfiable randomize()** under
  `VERILATOR_SOLVER=dv-solve-smt2`. No-reply behaviour confirmed 2026-09-30;
  the Verilator hang itself is unconfirmed — check in docs phase D2. Related
  nit: `(echo "x")` prints `""x""` (quotes doubled).
  Fixed alongside (2026-09-30, `smt2_main.c`): `--engine=cdcl` cleared
  `DV_ENGINE` (= auto) instead of forcing CDCL; interactive mode exited 2
  whenever input ended in whitespace (i.e. every file-backed pipe).

- **B26 — FIXED 2026-08-18.** Moved to the FIXED section below. The
  originally-recorded root cause (an empty `explain_disj_clause` explanation)
  was **wrong**; the real defect was signed int64 ordering in
  `literal_is_true`/`literal_is_false`. The empty explanation is real but
  benign here — that conflict never reaches the explain path.

- **B30: a cyclic strict inequality between two vars never terminates.**
  `(assert (bvult a b)) (assert (bvult b a))` — trivially `unsat` — runs
  forever at width >= 32 in the **default** engine (width 8 is fine). Found
  2026-08-18 while building the B29 mirror test; **verified pre-existing**
  against a pristine HEAD build, so it is not a side effect of the B26/B29
  compares. Not a wrong answer — a hang — but a 2-line trivially-unsat input
  that never returns is a bad failure mode, and the shape (`a < b` and `b < a`
  reachable through a chain) is not exotic in real constraint sets.
  Likely mechanism: bounds propagation converges one unit per round over the
  whole domain instead of detecting the cycle, and neither the wall-clock bail
  nor the escalation path fires. Fix direction: detect the mutual-strict cycle
  at compile time, or make the bail actually reachable from this loop. Not
  pinned as a test — a hanging test is worse than none; add one once the bail
  works.

- **B28: `bvadd`/`bvsub`/`bvmul`/`bvshl` do not wrap at width 64 under pure CDCL — ✅ FIXED 2026-10-01 (82cfb7f, see B39).**
  `(assert (= (bvadd v (_ bv18446744073709551615 64)) (_ bv0 64)))` — satisfiable
  at `v == 1` by wrapping — answers `unsat` with `DV_NO_BITBLAST=1`. The
  wrap-aware `bv*_64` propagators cover widths 1..63 only; at width 64 the
  compile falls through to `bounds_add_64`, which is plain **integer**
  arithmetic. Not reachable in the default engine (width >= 64 arithmetic
  auto-routes to bitblast, the B9 fix), so this is the same severity class as
  B23: real, but audit-mode-only. Fix direction: decline the compile at width 64
  for these ops rather than using the non-modular propagator — but measure
  first, `bounds_add_64` is on a common path.
  Widths 1..63 were the *production-reachable* half of this bug and are fixed;
  see B27 below.

- **B23: tier-2 (>64-bit) comparisons are wrong `unsat` under forced pure CDCL.**
  A lone `(assert (bvult (_ bv5 65) v))` on a 65-bit `v` answers `unsat`; so do
  `(= v (_ bv5 65))`, and the same at widths 96 and 128. The CDCL engine reasons
  through `var_lo64`/`var_hi64`, which cannot represent a tier-2 domain (they
  read a `WideBounds64` for what is a `WideBoundsN` var); instead of declining,
  it computes with those bounds and "proves" the constraint false.
  **Not reachable in production today** — width > 64 auto-routes to bitblast, so
  `--engine=cdcl` and `--mode=verilator` both answer `sat` correctly; only
  `DV_NO_BITBLAST=1` (a test-only override) exposes it. Still a latent
  wrong-`unsat` in an engine we are routing more traffic to. Found while gating
  Phase 2; **not** a disjunction bug and not caused by the hull.
  Pinned: `tests/formal/test_disj_hull.py::test_b23_tier2_comparison_wrong_unsat`
  (xfail strict — fixing B23 fails it, and it must then become a positive
  assertion). Fix direction: make the CDCL compile decline width > 64 operands
  outright (uncompiled → validation → `unknown`), as B5/B8 did for concat.

---

## Found 2026-08-17 — UniGen2 protocol + randomization-quality sweep (B14–B21)

Found while evaluating Verilator PR#8042 (UniGen2 near-uniform sampler,
`verilator/verilator#8042`), which drives the external solver with `push`/`pop`,
XOR hash constraints, and a **blocking-clause model-enumeration loop** (BSAT).
None of these are UniGen2-specific — they are all plain SMT-LIB that any
incremental consumer can hit. See `docs/randomness_benchmark_plan.md`.

Repro note: real Verilator emits `(set-logic QF_ABV)` (37/37 captured
transcripts), which auto-routes to **bitblast**, not CDCL
(`_engine_is_bitblast`, smt2_frontend.c). Engine choice is by declared logic
only — `--mode=verilator` does not affect it.

### B14. Bitblast: any `(assert)` after the first `(check-sat)` was SILENTLY DROPPED → wrong `sat` — ✅ FIXED 2026-08-17
- **Symptom (soundness, wrong verdict):**
  ```smt2
  (set-logic QF_ABV)
  (declare-fun a () (_ BitVec 3))
  (assert (bvult a #b101))
  (check-sat)                    ; sat
  (assert (bvugt a #b110))       ; contradicts the above
  (check-sat)                    ; dv-solve: sat   <-- WRONG, z3: unsat
  ```
  Scope: **every bitblast-routed solve** — `QF_ABV` on all engine settings
  (arrays force `needs_bitblast`), and `QF_BV` under `--engine=bitblast`.
  Pure-CDCL `QF_BV` was correct. Not interactive-specific (batch file mode too).
- **Root cause:** `_cmd_assert` (and the 6 other builder-mutating sites) recorded
  a pending addition with `if (fe->compiled) fe->has_aux = 1;`. `fe->compiled` is
  the **CDCL**-compiled flag, and the bitblast path deliberately never calls
  `_ensure_compiled` — so `has_aux` stayed 0. `_ensure_problem` then early-returned
  on `if (fe->problem) return 0;`, and `_flush_aux` early-returned on
  `!fe->compiled`. Net effect: post-first-solve asserts accumulated in
  `fe->builder` and were **never** folded into `fe->problem`, which is what
  `_check_sat_bitblast` bit-blasts. CDCL was correct only because it has the
  `has_aux`/`_flush_aux` fold; bitblast had no equivalent.
- **Fix (`smt2_frontend.c` / `.h`):**
  - New `_builder_touched(fe)` replaces all 7 `if (fe->compiled) fe->has_aux = 1;`
    sites; it additionally sets `fe->problem_dirty` when `fe->problem` exists.
  - `_ensure_problem` now rebuilds `fe->problem` (free + re-finalize) when dirty,
    and no longer `builder_reset`s — the builder is retained so a re-finalize
    reproduces the **full** constraint set. Var identity is safe because
    `Smt2Var` stores a stable `var_id`, not a pool-relative `ExprRef`.
  - `fe->builder_retained` guards the exotic case: any path that DOES reset the
    builder (`_ensure_compiled`, `_flush_aux`, `_check_sat_array`) clears it, and
    a rebuild request with the flag clear returns **-2 → honest `unknown`**
    rather than re-finalizing a partial set (which would reintroduce this very
    bug). Sound in every case: correct verdict or `unknown`, never wrong.
  - `(reset)`/`soft_reset` already `builder_reset`s and memsets the struct, so
    the Verilator randomize loop still reclaims builder memory per call.
- **Verified:** all 6 logic×engine combinations now `sat unsat` (agree with z3);
  a 61-model BSAT blocking-clause enumeration returns 61/61 **distinct** models
  on `ot_aon_wkup` (0.35 ms/model) and `array_many_elem` (0.14 ms/model), where
  before it returned 1 model and then repeated it.
- **Known cost (accepted):** a dirty rebuild re-finalizes the whole builder, so a
  long incremental session is O(n²) in the number of asserts — measured
  50/100/200/400/800/1600 asserts → 2/3/9/28/98/384 ms. Acceptable, and it is
  the price of not silently dropping constraints (the old path was fast only
  because it was wrong). Note `_check_sat_bitblast` **already** rebuilt the
  bbsolver from scratch per call (`zsp_bbsolver_new`), so the dominant quadratic
  predates this fix; the re-finalize rides on top of it. The real fix for both
  is true incremental bit-blasting (sub-phase 5c, held — see
  `docs/phase5_incremental_bitblast_scope.md`), which would also let push/pop
  scope incremental adds properly.
- Regression guard: `tests/formal/test_incremental_protocol.py`
  `test_b14_incremental_assert_reaches_solver` (6 cases: {QF_BV, QF_ABV} ×
  {cdcl, bitblast, auto}) and
  `test_b14_blocking_clause_enumeration_yields_distinct_models`.

### B15. CDCL: `(not (and ...))` asserted after a `(check-sat)` → `unknown` — 🟥 OPEN
- `QF_BV` + `--engine=cdcl` only (so **not** on the Verilator path). The
  logically-equivalent De Morgan form `(or (not …) (not …))` works, and the same
  `(not (and …))` asserted *before* the first `check-sat` works.
- Likely the B10 conjunction-spine flattening not being wired into the
  incremental-compile path. Safe (`unknown`, not a wrong verdict), but it is
  exactly the blocking-clause syntax PR#8042 emits.

### B16. Unary `(and x)` / `(or x)` / `(xor x)` → `unknown` — ✅ FIXED 2026-08-17
- **Symptom:** `(assert (and (= a #b000)))`, `(assert (or …))`, `(assert (xor …))`
  and `(assert (not (and (= a #b000))))` all returned `unknown`. Both `QF_BV` and
  `QF_ABV`, before or after a `check-sat`, every engine.
- **Why it mattered:** SMT-LIB permits `and`/`or`/`xor` with arity ≥ 1 (z3
  accepts the unary form; it rejects only the *nullary* one). UniGen2 emits the
  unary form whenever there is a **single** rand element — the scalar
  `randomize()` case, which is the common one — because a blocking clause over
  one variable is literally `(not (and (= x V)))`. With B14 fixed, this was the
  last thing stopping a BSAT enumeration loop on single-variable problems.
  Safe-but-silent: `unknown`, never a wrong verdict.
- **Root cause:** all three translator arms in `_translate_list_tagged`
  (`smt2_frontend.c`) opened with `if (s->list.count < 3) return TAGGED_NULL;` —
  i.e. they demanded ≥2 operands. The accumulator loops underneath were already
  fully arity-agnostic, so the guard was the entire bug.
- **Fix:** relax the three guards to `count < 2` (≥1 operand). A unary `and`/`or`
  then falls out of the existing loop as the identity, and nullary `(and)` /
  `(or)` stay rejected, matching z3. The `_eval_sexpr` get-value evaluator
  already handled any arity and needed no change.
- **Verified:** all 9 unary shapes (incl. nested `(and (or x))`) agree with z3 on
  both logics; the blocking-clause shape agrees on all 3 engines; nullary still
  refused. End-to-end, `$countones(x)==4` over 8 bits now enumerates
  **exhaustively to exactly 70 distinct models** (= C(8,4), the ground truth),
  every one with 4 bits set, then correctly reports `unsat` — complete and sound.
- Regression guard: `tests/formal/test_boolean_arity.py` (24 cases, differential
  vs z3).

### B17. n-ary `concat` with arity ≥ 3 → `unknown` — 🟥 OPEN
- `(concat x y)` works; `(concat x y z)` and wider return `unknown`. Both logics.
- SMT-LIB's `concat` is formally binary but n-ary is widely accepted and widely
  emitted. UniGen2's hash constraint is a `concat` of up to 10 `bvxor` chains, so
  every hash with ≥3 bits is rejected today.
- Fix: left-fold n-ary `concat` into nested binary applications.

### B18. Seeded CDCL burned the full time budget on holed/disjunctive domains — ✅ FIXED 2026-08-17
- **Symptom:** any *nonzero* solve seed (i.e. the randomization path) on a domain
  with a hole made CDCL run to its wall-clock deadline and return `unknown`.
  Seed 0 solved the same input in 2-3 ms. Users normally saw `sat` anyway —
  supplied by **bitblast escalation**, which masked the failure and made every
  such sample cost a full 10 s CDCL timeout first.
  ```
  $countones(x)==4         seed 0 -> sat 2 ms | seed 1 -> 10132 ms
  x in {[10:20],[100:150]} seed 0 -> sat 3 ms | seed 5 ->  5113 ms
  (both, DV_NO_BITBLAST=1: unknown -- CDCL genuinely never solved them)
  ```
- **Root cause:** the phase-saving shortcut in `_pick_value` (`zsp_search.c`)
  returns the saved value after checking only the *coarse* `[lo,hi]` bounds plus
  one `_is_hole()` probe — unlike the randomized path below it, which runs its
  pick through `_pick_avoiding_holes()`. The diversity mechanism seeded every
  var's phase to a *random* in-domain value, so on a union-of-ranges domain that
  value routinely landed **in the gap**: it passed the bounds check, was returned
  as an infeasible decision, and — since the saved phase never changed — was
  re-picked after every restart until the budget expired.
- **Fix (two parts):**
  1. Skip phase saving entirely for a diversity solve (`opts->seed != 0`), so
     value choice goes through the hole-aware randomized path. Seed 0 keeps
     phase saving byte-for-byte as before. Seeded solves also no longer clobber
     the seed-0 saved phases.
  2. Drop the random phase initialization altogether — it was the fuel for the
     bug and was never needed for diversity: `_pick_value`'s `_rand_range64`
     path is seeded from `ctx->rng_state` and yields fully distinct models per
     seed (verified 20/20).
- **Result:** countones seeded **10132 ms -> 3 ms** and pure-CDCL `unknown -> sat`;
  disjoint ranges **5113 ms unknown -> 2 ms sat**. Diversity preserved.
- Regression guard: `tests/formal/test_sampler_properties.py`. Verified to BITE:
  reconstructing the original code fails 3 of the 8 guards and takes 360 s
  instead of 0.03 s.

### B19. CDCL re-randomization was a POINT MASS within one process — ✅ FIXED 2026-08-17
- **Symptom:** in one process, N different `(set-option :seed …)` values returned
  N *identical* models; across N separate processes the same seeds returned N
  distinct models. On `range_1000` the in-process arm scored **0.1 % coverage and
  JSD 0.9943 at every budget** (25 000 draws collapsing to a single value) while
  the fresh-process arm sat at the uniform bias floor.
- **Root cause:** the random initial-phase seeding lived *inside* the
  first-call-only allocation guard `if (… && !ctx->phase_save && …)`
  (`zsp_search.c`). Once `phase_save` was non-NULL the block was skipped, so
  every later solve reused the first solve's phases and the new seed never
  reached value selection. `ctx->rng_state` was updated but nothing consumed it,
  because the phase-save shortcut short-circuited first.
- **Fix:** separate allocation from initialization (allocate once, `ps_fresh`
  flag), and — via the B18 fix — bypass the phase-save shortcut entirely on
  seeded solves so the seeded RNG path drives value choice on every solve.
- **Result:** 20 seeds in one process now give 20 distinct models (was 1);
  in-process and fresh-process arms score identically. Seed 0 still
  deterministic within and across runs.
- Regression guard: `test_sampler_properties.py::test_b19_varies_within_one_process`.

### B21. Sequential seeds produced correlated samples (a linear sweep, not draws) — ✅ FIXED 2026-08-17
- **Symptom:** with sequential seeds the "sampler" walked an arithmetic
  progression. On `a + b == 100` (8-bit, 256 solutions) `a` came back as
  **65, 130, 195, 4, 69, 134, 199, 8, …** — exactly `+65 (mod 256)`, cycling
  through all 256 values once with **zero repeats in 64 draws** (iid sampling
  would collide with p ≈ 99.9 %).
- **Why it mattered / how it hid:** this *flattered* every metric. It scored
  99.6 % coverage at the 1x budget and JSD 0.0000 — i.e. **better than a true
  uniform sampler**, which is impossible for iid draws (coupon collector caps
  1x coverage near 63 %). A benchmark run would have reported dv-solve's CDCL
  sampler as near-perfect. Consecutive samples were in fact almost fully
  determined by their predecessor. It also short-cycled elsewhere: with
  `fair_pick` on, 2000 draws covered only **121 of 256** values.
  **Verilator mode hits this directly — it seeds with `++div_counter`, i.e.
  1, 2, 3, …**
- **Root cause:** `ctx->rng_state = opts->seed` fed a raw sequential integer
  straight into xorshift64, whose low bits stay strongly correlated across
  nearby seeds. Every consumer of `ctx->rng_state` inherited the correlation.
- **Fix:** avalanche the incoming seed through a SplitMix64 finalizer
  (`_seed_mix64`, `zsp_search.c`) before it becomes `rng_state`. One line at the
  single point where seeds enter, so it fixes all consumers at once.
- **Result:** sequences look iid (collisions appear at the birthday rate);
  `fair_pick` coverage 121 -> **256 of 256**. All CDCL arms then land
  statistically indistinguishable from the uniform bias floor (χ² p = 0.26–0.98)
  on all three shaped benchmarks.
- **Lesson:** a sampler scoring *better* than the uniform bias floor is a bug
  signal, not a win. Without the floor arm in the harness this would have
  shipped as a headline result.
- Regression guards: `test_sampler_properties.py::test_b21_*` (stride-dominance
  and birthday-collision checks). Both verified to fail without the fix.

### B20. Bitblast `_diversify` heavily over-weights boundary values on shaped constraints — 🟥 OPEN
- Quantified against z3-enumerated ground truth (`tests/formal/sample_quality.py`).
  On `ot_aon_wkup` (`thold ∈ {0} ∪ [5,171]`, `cnt` tied to `thold`, K = 1003):
  **~50 % of all samples are the single solution `thold=0, cnt=0`** — a
  501× over-representation of a 1/1003 branch. Next most common is `thold=5`
  (19.5 %). The mass concentrates on 0, 5, 64, 128, 129 — boundary/round bit
  patterns.
- Consistent with the mechanism: `_diversify` (`zsp_bbsolver.c:1138`) flips each
  free bit toward a random target and keeps the flip only if every assertion
  still holds. Under a range constraint most high-bit flips are rejected, so the
  model collapses back toward small/boundary values instead of spreading over
  the feasible range.
- Coverage vs the numbers PR#8042 publishes for the same benchmark
  (coverage is directly comparable; their JSD estimator is not — see below):

  | | 1x | 25x |
  |---|---|---|
  | uniform (bias floor) | 64.8 % | 100 % |
  | UniGen2 (PR#8042) | 62.91 % | 100 % |
  | Verilator master (PR#8042) | 29.81 % | 92.32 % |
  | **dv-solve bitblast** | **12.1 %** | **40.6 %** |

  On `countones` (K=70) we are 48.6 % / 98.6 % vs master 54.29 % / 100 %.
  On the plain-range control `range_1000` we sit **at the bias floor** — ties
  everyone, matching the PR's own observation that samplers only separate on
  shaped constraints.
- **Note on JSD:** PR#8042 reports UniGen2 at JSD 17.12 on `ot_aon_wkup` @1x,
  but a *perfect* uniform sampler scores 23.46 under normalized-JSD×100 at that
  sample count. Their figure is below the achievable floor, so their estimator
  is definitely not normalized JSD×100. **Do not compare JSD across the two
  reports until their estimator is replicated** (open item in
  `docs/randomness_benchmark_plan.md` §3). Coverage is safe to compare.

---

### B10. SIGSEGV (crash) on large plain-QF_BV inputs — ✅ FIXED 2026-07-20
- **Root cause (NOT memory corruption — it was stack overflow from unbounded
  recursion):** deeply-nested `(and c1 (and c2 (and c3 ...)))` conjunction spines
  (thousands of conjuncts, as large asp graph-colouring / edge-matching / sudoku
  instances build) recursed one C-stack frame per conjunct and overflowed the
  stack. ASAN pinpointed THREE separate recursion sites, all now flattened to an
  iterative heap worklist (O(1) C-stack on the spine):
  1. `zsp_bbsolver.c` `bb_binary` BIN_AND/BIN_OR (bitblast walk). **NOTE: a naive
     iterative *flatten* here was WRONG — it dissolved DAG-shared sub-conjunctions
     (the recursive path memoizes each AND/OR node via `bb_expr`), changing the CNF
     (+951 clauses on mcm/70) and REGRESSING the heavy-tail solve (mcm/70
     40s→timeout). Replaced with an iterative MEMOIZED post-order (helper
     `bb_is_andor_node`; `bb_binary` now takes the node's ExprRef) that reproduces
     the recursion's EXACT AIG — verified byte-identical CNF (588093 clauses) and
     mcm/70 back to 39.5s.**
  2. `zsp_compile.c` `_compile_constraint` BIN_AND (CDCL compile) — plain flatten is
     fine (compiles constraints to propagators, builds no shared gates). Also added
     the missing `#include <stdlib.h>` (realloc/free were implicitly declared).
  3. `zsp_bbsolver.c` `collect_substs_from` BIN_AND (bitblast equality-subst pre-pass)
     — plain flatten is fine (order-preserving; substs=35352 unchanged).
  AND/OR are associative+commutative and the subst walk preserves left-to-right
  leaf order, so all three are verdict/behavior-preserving (soundness gate: 204
  cross-check + 201 fuzz differential vs z3 green; ASAN-clean past the former
  crash). graph-colouring{130,140}, sudoku.in3 AND edge-matching-10x10 (ran past
  its former ~547s crash to the 700s cap) now all run to timeout/verdict (no
  signal 11) on BOTH engines. Regression guard:
  tests/formal/test_deep_conjunction_no_crash.py (4 cases, real fixtures).
- **Residual:** other deep-nest shapes (deep ITE / arithmetic trees) are NOT
  flattened — no corpus instance exercises them, but `subst_reaches_var` and the
  general `bb_expr` operand recursion remain depth-bounded by expression nesting.
  Revisit only if a future instance overflows there. **UPDATE 2026-07-20: now
  partly reachable — see B11/B12.**

### B11. Frontend: deeply-nested `let` silently truncated to `unknown` — ✅ FIXED 2026-07-20
- **Root cause:** `SMT2_MAX_SUBST` was a hard 64 and `subst_stack` a fixed 64-entry
  array in the (stack-allocated) `Smt2Frontend`. A `(let ...)` nest deeper than 64
  active bindings overflowed → `TAGGED_NULL` → tainted assert → silent `unknown`
  (no diagnostic even with DV_LOG). `let` with deep shared subterms is how virtually
  every tool (incl. Verilator) emits SMT-LIB, so a large class of real formulas
  silently returned `unknown`. Corpus: uum32 (nest 3430), fft/Sz1024 (4483),
  brummayerbiere maxxor032 (416)/minandmaxor256 (3071), Sage2/15955 (2369) all
  instant-bailed at 0.0s.
- **Fix:** `subst_stack` is now heap-backed and grown geometrically on demand
  (`_subst_reserve`, smt2_frontend.c); the per-`let` `let_vals`/`let_names` temp
  buffers are sized by binding count (8-inline / heap fallback) and freed BEFORE the
  phase-2 body recursion so the C-stack frame stays tiny. `SMT2_MAX_SUBST` is now
  just the initial capacity. Freed in destroy + soft_reset. Soundness: 229 regression
  + 355 differential-vs-z3 (300 fuzz) green, 0 disagreements; let shadowing/parallel
  semantics cross-checked vs z3. uum32/fft/Sage2/15955 now translate + attempt the
  solve instead of instant-`unknown` (uum32/fft/Sage2 currently time out, matching
  bitwuzla — the coverage bail is gone; hardness is separate).
- **Second bug found + fixed while closing this (was the "~113 chained-let limit"):**
  after `subst_stack` went heap-backed, the `let` push set every `Smt2Subst` field
  EXCEPT `expanding` — and realloc'd memory is uninitialised (unlike the old zero-init
  fixed array). A garbage `expanding` byte made `_subst_lookup` SKIP that binding
  ("unknown variable xNN" past index 64). This was a latent SOUNDNESS hole, not just a
  bail: a skipped binding falls through to older same-named entries, so a SHADOWED let
  (reused name) could resolve to the WRONG outer binding → wrong verdict. Fix: init
  `e->expanding = 0` in the let push (define-fun push already did). Now deep chained AND
  shadowed lets solve correctly to depth 1000+; **brummayer032 closes: unsat @78.5s,
  agrees with bitwuzla.** Regression guard: tests/formal/test_deep_let.py (12 cases,
  unique+shadowed × depths straddling 64, differential vs z3). Full re-verify: 527
  soundness tests + deep shadowed-let vs z3 all green.

### B12. Deeply-nested S-expression (≥~2000) → SIGSEGV in translator recursion — ✅ FIXED 2026-07-21
- **Root cause:** the `_translate_tagged` ⇄ `_translate_list_tagged` expression walk
  recurses one C frame per nesting level (~8.6 KB each, measured), so a single
  expression nested ≥~1490 deep (`(bvadd (bvadd … ))`, `let`-chained or not) overflows
  the default C stack → SIGSEGV. Confirmed by gdb: the crash is the *translator*
  recursion, not the parser (`_parse_one` survived depth 2000). Deep `let` chains crash
  at the same threshold. **Pre-existing** — exactly the residual B10/B11 flagged.
- **Fix (two parts, both in `src/c/smt2/`):**
  1. **Crash-proof depth guard** (`smt2_frontend.c`): `_translate_tagged` is now a thin
     wrapper around `_translate_tagged_impl` that counts recursion depth
     (`Smt2Frontend.translate_depth`) and, past `_translate_depth_limit()`, sets
     `incomplete` and returns `TAGGED_NULL` → check-sat prints `unknown` instead of
     crashing. The limit is *derived from the actual stack size* (70% of it at a
     conservative 16 KB/level), so it sits safely below the true overflow point on any
     stack. **Sound: `unknown`, never a wrong verdict, never a crash.**
  2. **Coverage boost** (`smt2_main.c`): the solve now runs on a pthread with an
     explicit **1 GiB stack** (a reliably-reserved region — runtime `setrlimit` raises
     are unreliable: Linux may refuse to grow the *main* stack past its initial
     reservation, which reproduced a crash at depth 3000 under an 8 MB main stack). The
     frontend guard is sized from that known stack via `g_smt2_translate_stack_bytes`,
     so it only bails past ~45 K depth. Falls back to inline + RLIMIT_STACK-sized guard
     if thread setup fails (still crash-proof).
- **Verified:** no crash at any depth 500…500 000 across default / 8 MB / unlimited
  stacks (`bvadd` and `let` shapes); depths ≤~8000 now *solve* (were crashes); ≥~50 K
  cleanly `unknown`. ASAN clean (depth 2000 solves, 200 000 → unknown). deep_let 12/12,
  SMT2 formal 308/308, full formal 1144 passed (4 unrelated pre-existing `bitwuzla`-ref
  baseline failures; dv-solve passes those benches). The `~113 chained-let limit` was a
  *separate* earlier fix (the uninitialised-`expanding` bug — see B11).
- **Note (unrelated, pre-existing at HEAD d4b1279):** `test_smt2_frontend.c::test_declare_array_addr_too_wide`
  failed (`n_array_vars==1, expected 0`) — confirmed failing on a clean HEAD checkout,
  independent of B12. **Resolved 2026-07-21: it was a STALE TEST, not a bug.** A later
  Verilator-hardening commit replaced "reject too-wide arrays" with SPARSE arrays (M >
  SMT2_MAX_ARRAY_ADDR_BITS registers a lazy-per-index sparse var) but didn't update the
  test. Verified sound differentially vs z3 (concrete-index selects solve exactly;
  stores / symbolic indices taint to `unknown`; 0 sat-vs-unsat disagreements). Fixed the
  test to assert the sparse-array contract; added `tests/formal/test_sparse_array_sound.py`
  (6 differential cases) as the durable soundness guard.

### B13. Bitblast/cube encode: `ZSP_BB_UNKNOWN` guard collided with the "ready" sentinel → bogus SAT — ✅ FIXED 2026-07-21
- **Symptom:** the A-4 soundness guards (bit-blaster does not encode AllDifferent
  or signed div/mod) fired correctly — set `S->last_result = ZSP_BB_UNKNOWN` and
  `return ZSP_BB_UNKNOWN` — yet `check()` returned **SAT (10)**, silently dropping
  the hard constraint. Reproduced by `tests/unit/test_bvsat.py::test_guard_all_different_defers`
  and `test_guard_signed_divmod_defers[3,4]` (all three had been failing).
- **Root cause:** `_bb_encode()` overloaded the return value `0` to mean BOTH
  "fully encoded, ready to solve" (`return 0`) AND "deferred" (`ZSP_BB_UNKNOWN`,
  which **is** `0`). All three call sites — `zsp_bbsolver_check` (bbsolver.c),
  `zsp_bbsolver_prepare` → `zsp_cube.c` (cube engine) — tested `if (enc != 0) return enc;`,
  so a deferral (`0`) looked identical to "ready" and the solver proceeded WITHOUT
  the unsupported hard constraint → bogus SAT. `ZSP_BB_ERROR` (-1) escaped the bug
  only because it is non-zero.
- **Fix:** introduced `ZSP_BB_ENCODE_READY` (=1, distinct from every `ZSP_BB_*`
  verdict) as the encode "ready" sentinel; `_bb_encode` now returns it on success
  and `ZSP_BB_UNKNOWN`/`ZSP_BB_ERROR` on defer/error. All three call sites test
  `enc != ZSP_BB_ENCODE_READY`. Header `zsp_bbsolver.h` documents why "ready" must
  not be 0. This is engine-wide (one-shot AND cube), soundness-preserving.
- **Verified:** all 4 `guard` tests pass (incl. `not_over_deferred` — unsigned
  div/mod still solves); full bvsat 43/43; cube suite 202/202; unit 620/620;
  fuzz-vs-z3 161/161; end-to-end signed `bvsdiv` agrees with z3 (sat where the
  buggy unsigned readback would have said unsat). Was an in-flight regression from
  the A-4 guard work (the guards were written but the return-value collision made
  them no-ops); now genuinely sound.

### B10-OLD. (original report, kept for context) SIGSEGV on large plain-QF_BV `asp/GraphColouring` inputs
- `build/dv-solve-smt2 <f>` exits 139 (SIGSEGV, core dumped) on
  `asp/GraphColouring/graph-colouring-nodes=130-density=0.1-instance=3.smt2`
  and `...nodes=140...instance=2.smt2` from the SMT-COMP 2025 parallel corpus.
  Both engines (CDCL default AND `DV_ENGINE=bitblast`) crash. Input is plain
  `QF_BV` (Bool + `(_ BitVec 4)`, ~9.3k lines) — no arrays/UF. This is a
  robustness crash, not a wrong answer, but it's a hard failure on valid input.
  Related: `sudoku.in3.smt2` also SIGSEGVs (exit 139, core dumped) at ~77s under
  `DV_ENGINE=bitblast` — same crash class (earlier logged as "hangs"; corrected
  2026-07-20, it crashes). Repro corpus: `tests/formal/corpus/smtcomp2025_parallel/`. Needs a
  minimized repro + backtrace (build with ASAN). Found during Task-A baseline.
  - **Same crash class, confirmed NOT OOM:** `asp/EdgeMatching/edge-matching-
    width=10-height=10-colours=13.smt2` (246k vars) under `DV_ENGINE=bitblast`
    SIGSEGVs (signal 11) after 547s CPU with peak RSS only ~1.06 GB — so this is
    a genuine memory-safety bug on large QF_BV bitblast, not resource
    exhaustion. Pattern: dv-solve crashes on large QF_BV inputs. Prioritize an
    ASAN run over these three (graph-colouring ×2, edge-matching).

### B5. CDCL `r == sign_extend(a)` into a wide unsigned var → spurious `unsat`  — ✅ FIXED 2026-07-09
- `zsp_compile.c` bounded the result var to the SIGNED range and linked with a
  plain `bounds_eq`. Sound ONLY for a SIGNED result var; for an UNSIGNED wide
  result var the negative lb wraps → spurious compile-time UNSAT.
- Fix: native-compile sign-extend ONLY when the result var is `VAR_SIGNED`
  (correct; keeps the sign-extend unit tests passing). For an unsigned result var,
  leave it UNCOMPILED (return 0) → CDCL drops it, the validation net re-checks it,
  and unknown escalates to bitblast. CDCL is now sound (correct-or-unknown).

### B6. CDCL: extract of bit 63 / MSB of a 64-bit var → spurious `unsat`  — ✅ FIXED 2026-07-10
- `(assert (= ((_ extract 63 63) v) #b1))` for a 64-bit `v` was `unsat` on CDCL
  (z3 `sat`), on the DEFAULT path too (extract isn't routed to bitblast).
- Root cause: `_bit_slice_backward` (zsp_prop_templates.c) bailed on
  `alo < 0 || ahi < 0`. For a full-range 64-bit *unsigned* var, `ahi = var_hi64`
  = 2^64-1 whose int64 pattern is -1 → guard tripped → backward propagation
  skipped → `v` never forced to have bit 63 set, and the search (capped at the
  2^63 cliff for decisions) never tried a bit-63-set value → wrong `unsat`.
- Fix: the slice math is purely unsigned (`_slice_min_ge/_max_le` take uint64_t),
  so for an unsigned width>=64 source only reject a genuinely inverted domain
  (`(uint64_t)alo > (uint64_t)ahi`); keep the old signed-bound bail for signed /
  narrow sources. A real CDCL fix (correct-or-unknown), not a bitblast route.
  Guarded: `cdcl64_extract_{hibit,hi32}_sat` (test_gap_coverage), the exhaustive
  `test_bitslice_backward.py`, and the pure-CDCL fuzz sweep (`DV_NO_BITBLAST`).
- NOTE: 64-bit *arithmetic* wrap (B9) is a distinct mechanism, still routed to
  bitblast (out of Phase-G scope per the `var_b_lt` note); B6 was a bounds/slice
  issue fixable inside the existing sign-aware framework.

### B2. dv-solve `unknown` on nested / wide-index / symbolic-index arrays  (feature gap, safe)
- Honest `unknown` (z3 solves). Flat QF_ABV works.
  - Nested arrays `(Array _ (Array _ _))` → unknown (`_parse_array_sort` rejects).
  - 128-bit (wide) array index → unknown.
  - Symbolic (variable) `select` index into a sparse array → unknown.
- Fixtures (still `unknown` in the corpus): `t_constraint_struct_complex`,
  `t_constraint_dyn_queue_basic`, `t_constraint_assoc_arr_basic`,
  `t_constraint_rand_array_index`.
- Not soundness bugs — coverage. See `nested-array-unknown` memory.

### B3. CDCL returns silently-wrong models for composed materialized exprs  (soundness, mitigated)
- An `ite` of unsigned divisions (from signed div/rem/mod lowering) leaves the
  result var unpinned on the CDCL bounds engine → arbitrary in-range value
  (verified: `bvsdiv(1,15)` → 0). Bitblast is exact.
- Mitigated: signed div/rem/mod set `fe->needs_bitblast` to force bitblast.
- Underlying issue remains: any future frontend lowering that materialises a
  multi-level composed expr may hit it. Validate the *model*, not just sat/unsat,
  or force bitblast. Related to plan §8.E (why we did NOT add nested-expr support
  to the CDCL compile).

---

## 🟧 WORKED-AROUND (latent defect remains)

### B4. Shared unflattened ITE-tree sub-DAG → inconsistent bitblast values
- Referencing a raw composed ITE-tree ExprRef at multiple use sites produced
  wrong, memo-dependent values (bvsmod's remainder used 3×: `2` by default, `15`
  under `DV_BB_NO_MEMO`; z3 says `0`).
- Worked around: `bvsmod` `_flatten_to_var`'s the shared remainder to a single
  aux var before reuse. `_signed_divrem_expr` results used once are fine.
- Root cause not fixed — the bitblast memoization/sharing of ITE sub-DAGs is
  suspect. Any lowering that reuses a raw composed sub-expr should flatten it.

---

## ✅ FIXED THIS EFFORT (listed for context / regression pointers)

### 2026-10-10 — translator stack work (B112–B114, uncommitted)

Found while reducing the SMT-LIB2 translator's stack per level and moving its
guard to the thread's real stack (docs/translator_stack_depth.md). The old
fixed cap (~46,000 levels on the CLI, 358 in a library) had hidden B112.

- **B112: builder pool offset wrapped past 4 GiB (FIXED).**
  `dvs_builder_alloc` kept `virtual_used` in 32 bits without an overflow check.
  A problem bigger than 4 GiB (a 400,000-deep `store` chain under `select`)
  wrapped it, so blocks overlapped and `dvs_builder_finalize` crashed with
  SIGSEGV in `memcpy`. The allocation now fails (EXPR_NULL), and the
  check-sat answers `unknown`. Affects every front end.
- **B113: SMT-LIB2 parser recursion had no guard (FIXED).** `_parse_tok`
  recursed at 336 B a level. Library use of the front end on a 1 MB thread
  crashed at about 3,000 levels. It is now iterative, with a heap stack of open
  lists. The CLI was protected by its 1 GiB stack.
- **B114: `_sexpr_deep_copy` left `quoted` uninitialized (FIXED).** The copy
  was arena memory (malloc, not zeroed), so a copied symbol (define-fun body,
  `:produce-assertions` record, unsat-core replay) could print as `|x|`. The
  copy is now iterative and copies the flag.
- **Core passes now guard their own stack (FIXED, same day).** The open
  follow-up: `_compile_constraint`, `_value_to_var`, `bb_expr`, the cycle
  checks, the model validator, the oracle emitter and the elaborator each stop
  at the thread's stack (`dvs_stack_low`, dvs_stackinfo.h) and decline, so the
  translator's 2 KB-a-level budget is gone. Also: the get-value term printers
  are iterative, and the bit-blaster's width/signedness helpers report
  "unsupported" past their depth limit instead of returning a default (a
  latent hazard; the probe, an 8-bit var equal to a 5,001-deep 16-bit negation
  chain, answers unsat on old and new code because elaboration makes the
  width explicit). docs/translator_stack_depth.md.
### 2026-10-01 — found while writing internals/ (B43–B49)

Found by checking the old `docs/propagators.md` against the code. All verified
against a HEAD (13b3b4d) build; regressions in `tests/unit/test_wide_watch.py`
and `tests/unit/test_countones_clog2_exhaustive.py` fail on it.

- **B43: DisjClause overran its 4-slot watch section (FIXED c1ace8e).** An OR of 5+
  comparisons wrote up to 32 watched ids into `PropWatchSect` (4 slots),
  aliasing `var_ids[4..]` onto the watcher chains and the clause data. Wrong
  MODELS in the default CDCL path (Python/C/DPI): e.g. a 6- and a 9-way OR
  plus two `!=` gave `v2 == v3` in ~1 solve in 40. SMT-LIB2 masked it (model
  validation → unknown → bitblast). Now a `PROP_FLAG_WIDE_WATCH` layout, each
  variable watched once.
- **B44: explainers read wide-watch propagators with the 4-slot layout
  (FIXED c1ace8e).** `explain_all_different`, `explain_sum_eq`, `explain_disj_clause`
  and the LCG propagator-conflict path read `PROP_WS` on wide propagators:
  off by one slot, citing the capacity field as a var id and dropping the last
  variable. Explanations were also silently TRUNCATED at 30 literals. Both make
  learnt clauses too strong → wrong unsat/pruned solutions under LCG (SMT-LIB2
  default; C `use_lcg=1`). New `prop_watched_vars()` accessor; explainers now
  refuse (return -1 → chronological backtrack) instead of truncating.
- **B45: AllDifferent explanation cited only singletons (FIXED c1ace8e).** Its
  pigeonhole / Hall-interval rules narrow on non-singleton bounds. Now cites
  every other var's bounds.
- **B46: `x == y` merged variables bypassed (FIXED c1ace8e).** Compile merges
  `x == y` into one var; all-different, `dvs_solver_pin_var`,
  `dvs_solver_exclude_value` and `dist` used the caller's id: `x0==x2` +
  `all_different(x0,x1,x2)` returned `[0,2,0]`; pinning the merged-away var
  returned 0 and was ignored (DPI pin too); dist on it ignored.
- **B47: `$countones` lower bound unsound (FIXED c1ace8e).** Took bits set in both
  `lo` and `hi` as forced: x in [3,7], `$countones(x)==1` → wrong unsat
  (4 = 0b100). Backward rules also bounded signed operands as unsigned. Now
  common-prefix bits only, and value rules only for non-negative domains.
- **B48: `$clog2` forced operand > 0 and read it as signed (FIXED c1ace8e).**
  `$clog2(0)==0` (IEEE: 0) → wrong unsat; negative signed operands (SV reads
  the pattern unsigned) → wrong unsat.
- **B49: Python `_SolveOpts` lacked `time_limit_ms` (FIXED c1ace8e).** C read 4 bytes
  past the ctypes struct as the time limit; 31 test-local copies had the same
  gap. `test_solve_opts_matches_header` now checks field order against
  `dv_solve.h`.
- Also: an all-different over >16 vars or a >32-bit var returned
  `DVS_COMPILE_NOMEM`; now `DVS_COMPILE_UNSUPPORTED_WIDTH` (validation does not
  check all-different, so leaving it uncompiled would go unenforced). One with
  <2 vars is trivially true (was an error).
- **OPEN question (not reproduced):** now B50, below.

### 2026-10-01 — found by the soundness coverage work (B50–B52)

See `docs/soundness_coverage_plan.md` §8–§9 for what each says about coverage.
Regressions: `tests/formal/test_lcg_soundness.py`,
`tests/formal/test_signed_ops.py::test_divrem_of_variables_matches_z3`. The
step-checker build (`-DDVS_STEP_CHECK=ON`) rejects the first bad learnt clause
of B50 on HEAD 6487e77.

- **B50: explainers omit the narrowed variable's own previous bound — wrong
  `unsat` (FIXED).** `x != y` with y fixed at v raises x.lo from v to v+1 only
  because x.lo was v; `explain_bounds_ne` cited only y, so the learnt clause
  claimed the bound from y alone. Repro (4-bit): `(distinct (bvsub z x) z)`,
  `(bvugt (bvsub z x) (_ bv14 4))` → `unsat` (sat: x=1, z=0). Also via bvand +
  distinct. The analysis also attributed a literal to the latest trail entry
  for its variable (not the one that made it true), kept the first-stored
  bound for a repeated slot, explained the current bound instead of the needed
  one, and silently dropped literals on clause-buffer overflow. Fix: rewrite of
  the analysis in `dvs_lcg.c` — every explainer not on a short whitelist gets
  the own-bound literal; literals are attributed to their maker; overflow
  bails.
- **B51: stale culprit propagator — wrong `unsat` (FIXED).** A conflict made by
  the search itself (exhausted split, decision tightening) left
  `conflict_prop_ref` from an earlier conflict, and analysis seeded from that
  propagator's non-conflicting bounds. Repro (4-bit): `(= (bvsub z x) (_ bv12
  4))`, `(bvult (bvadd x y) y)`, `(bvult x z)` → `unsat` (it is sat). Exposed
  once B50's fix changed the search. Fix: cleared at every search tightening
  and at propagate entry (`dvs_search.c`, `dvs_propagate.c`).
- **(B50 fix follow-up) learning livelock.** Taking the domain-emptying
  propagation as the UIP learnt a clause already true at the root; 7 of 3000
  brute-force problems hung. Now always resolved. And `_lit_maker` scanned the
  trail per literal (quadratic; one sweep seed went 3 s → 49 s); now a
  per-analysis index with binary search.
- **B52: `bvudiv`/`bvurem` by zero unconstrained on CDCL — wrong `sat`/model
  (FIXED).** SMT-LIB: `a/0 = ~0`, `a%0 = a`. The CDCL DIV/MOD propagators leave
  the result free at a zero divisor (SystemVerilog `x`), and model validation
  skips zero-divisor terms. `(bvuge z (bvudiv z z))` gave `z=0`. The signed
  forms were right only because they force bitblast. Fix: the SMT-LIB2
  frontend guards the divisor unless it is a nonzero constant. The old
  exhaustive division test used constant operands, which fold.
- **B53: constraint-level `ite` and soft constraints compiled ungated — wrong
  `unsat` (FIXED).** Branches were compiled with `_compile_constraint` and only
  the propagators found afterwards were gated (the last one; or one off by
  one), so root tightening, `x == y` merges and a `false` branch's `-1`
  applied regardless of the condition; an else-branch compile failure was
  ignored. `(ite (= x y) false true)` → `unsat`. Soft constraints had the same
  fallback (the noted soft-`!=` residual). Fix: `_compile_gated_constraint`
  reifies the body and adds `guard -> g`; every ite condition shape is
  reified; soft bodies go through it; alias resolution added there. Also:
  LCG adds `guard >= 1` for guard-gated propagators (explainers never cited
  the guard). Regressions: `tests/formal/test_bool_ite_constraint.py`,
  `tests/unit/test_gated_constraints.py`.
- **B54: `explain_sum_eq` cited the summands' same-direction bounds for a
  summand — invalid learnt clauses (FIXED).** A summand's ub follows from
  r.ub and the others' lb. Circular explanations (`v3<=1` by `v0<=1`, and
  back). Found by the clause checker in `test_wide_watch.py` (answers right).
  Duplicated summands now keep their other occurrence.
- **B55: reification with guard 0 accepted `x > y` at the range edge — wrong
  model (FIXED).** `_fire_reification_32/64` skipped both prunings when `y.lo`
  was at x's max or `x.hi` at y's min, and returned no conflict. Also
  `bounds_mul_32` ignored a zero factor (r=1, a=b=0 accepted).
- **B56: explainers read bounds made after the step they explain — invalid
  learnt clauses (FIXED).** At a conflict the current bounds include ones
  derived from the explained step: `v2 <= 45` explained by `v8 <= 12` and
  `v8 <= 12` by `v2 == 45`. HEAD has it. Fix: analysis rewinds the domains to
  just before each explained step (`_rewind_before`), restored on return.
- **B58: asserts after a partly compiled first check-sat were dropped — wrong
  `sat` (FIXED).** `_ensure_compiled` set `fe->compiled` only for rc == 0; with
  rc > 0 (some constraints uncompiled) the ctx existed but `_flush_aux` was a
  no-op, so every later assert (and push/pop scope) never reached CDCL or
  validation. `(assert (xor true (= x #x0)))(check-sat)(assert false)` → sat.
  Also made popped asserts look enforced on bitblast-routed problems. Found by
  the campaign's incremental door. Regression:
  `test_incremental_protocol.py::test_asserts_after_a_partly_compiled_check_are_enforced`.
- **B60: `ite_value` entailed with the branch operand still a range — wrong
  model with `use_lcg` (FIXED).** Entailment compared r with the branch's lower
  bound only; a learnt clause propagated inside the firing narrowed r further
  than the branch, the propagator retired with a = [8,9], r = 8, and a = 9 was
  later unenforced. Builder/C API with learning on. Pinned by the step
  checker's new fixed-point-at-sat check. Regression:
  `tests/unit/test_lcg_builder.py`, `tests/formal/soundness/regressions/`.
- **B61: self-referential strict compare at >=31 bits exhausted memory (FIXED;
  subsumes B30's hang).** `x < x`, `(x+y) > (x+y)`, `x > (x|y)` climb one value
  per propagation round; 2^31+ rounds inside one propagate, no deadline check,
  trail grows until OOM. Fix: same-var compare/ne/reif_eq decided outright;
  propagation checks the solve deadline + an 8M-entry trail cap and sets
  ctx->prop_aborted, which every consumer treats as timeout (no unsat, no
  learning, no shave pruning). Regression: `tests/formal/test_propagation_guard.py`.
- **B62–B65 (FIXED 2026-10-02, nightly trial):** B62 `x <s x` spent the CDCL
  budget (frontend folds same-var compares); B63 push/pop with >64-bit vars kept
  popped asserts (builder marks + dvs_builder_rewind on pop; failed compile
  releases its ctx); B64 `_bool_to_var` folded 64-bit unsigned consts as
  signed (and the negated-compare fold); B65 `_fire_ite_value_64` signed
  min/max on unsigned 64-bit domains. Regressions: test_wide_boundaries.py,
  test_incremental_protocol.py (wide push/pop), soundness/regressions/.
- **B68 (FIXED 5c50665 2026-10-02, Verilator-mode sampling collapse):**
  `_bool_to_var` had no arm for a Boolean ite, so Verilator's
  `(= #b1 (__Vbv (ite c a b)))` went uncompiled in CDCL; every seeded solve
  was downgraded to unknown by validation and --mode=verilator fell back to
  bit-blast re-diversification (ot_aon_wkup: 5 distinct of 300). Quality,
  not soundness: validation caught every bad model. Regression:
  tests/unit/test_verilator_mode_sampling.py.
- **B69 (OPEN, latent, needs env overrides to reach):** with
  `DV_ENGINE=cdcl DV_NO_BITBLAST=1`, problems using SMT2 bvsdiv/bvsrem/bvsmod
  get wrong `sat` (bad models, and sat on unsat) from CDCL, and the
  model-validation net does NOT catch them, so the validator evaluates the
  frontend's signed-div/rem lowering (`_signed_divrem_expr`) the same wrong
  way. Normal routing sets needs_bitblast for these ops, so default and
  --mode=verilator are clean (0 of 192 re-solves wrong). Fix the validator so
  the net is independent; then decide whether CDCL can own the lowering.
- **B67 (FIXED 2026-10-02, incremental reset, WRONG UNSAT):**
  `dvs_solver_reset` restored only the variables of the original compile;
  the auxiliaries an incremental `(assert)` created kept the values the last
  search left (the last model, or wherever a timed-out search stopped), so
  the next solve was confined to them. `(assert (bvult (bvadd a b) #x80))`
  sat with a=b=0, then `a=5, b=7` -> unsat; `y >=s 0` sat, then `y = 1` ->
  unsat. 78 of 1000 random step-wise sessions wrong on the default CDCL path.
  Found by the R3 swizzle driver: Verilator's XOR-hash asserts after a
  check-sat ran CDCL into a 10 s timeout and then a wrong unsat
  (--mode=verilator, 32-bit address). Fix: dvs_compile.c
  `_snapshot_added_vars` extends the reset snapshot with each add's new
  variables. The campaign's incremental door only ever added constraints in
  one round, so it could not see this; new door `smt2-steps` (one assert +
  check-sat + get-value at a time). Regressions:
  soundness/regressions/ (B67), tests/unit/test_vlt_protocol.py.
- **B66 (FIXED 2026-10-02, array-select stimulus):** builder `r = a[idx]`
  compiled element i as var `base + i` without `_resolve`, so an element
  merged away by `a1 == a2` was never constrained: wrong models and a wrong
  `sat` on an unsat problem, with or without learning (B46 alias-bypass
  class; the elements are implicit ids, so B46's audit missed them).
  Regression: tests/unit/test_lcg_array_select.py (1,600 brute-forced problems,
  also the stimulus that catches the lcg-no-guard-literal mutant).
- OPEN (completeness, sound): builder inequality against a `concat` operand
  is uncompiled (CompileIncompleteError); bitblast-routed SMT-LIB2 answers
  `unknown` after `pop`.
- **B57: `explain_ite_value` explained every narrowing as r's (FIXED).** Wrong
  for a, b (from r) and c (r disjoint from a branch), and the hull case
  omitted r's own range. Hidden while explainers could read their own
  conclusion (B56). Variables in two roles are refused.

### 2026-10-01 — found while writing the C/SV reference (B40–B42)

- **B40: DPI repeat solves returned the same solution (FIXED d30d9bf).**
  `dvs_dpi_solve_h` never undid the previous search, so a reused handle
  returned the first solution whatever the seed; every
  `dvs_randomizer::randomize_obj()` gave the same object, and a pin after a
  solve was checked against the solved values. Also: `dvs_dpi_restore_h`
  consumed its checkpoint though `dvs_dpi.h` said it didn't, and the base64
  decoder read bytes above `z` as `A` (and segfaulted on some malformed
  input). Regression: `tests/unit/test_dpi_shim.py`.
- **B41: undeclared variable read as 0 (FIXED f10e24f).** An expression naming
  an undeclared variable compiled and solved with that variable == 0;
  duplicate ids left a slot undeclared. Now `DVS_COMPILE_BAD_VAR` (-4) /
  Python `ValueError`. Regression: `tests/unit/test_problem_validation.py`.
- **B42: SV-elaboration failure fell back to the raw problem (FIXED f10e24f).**
  `dvs_solver_compile`, `dvs_solver_add_constraint` and the bit-blaster
  ignored `dvs_sv_elaborate`'s error and compiled the un-elaborated problem
  (different sizing rules) on OOM, >255-bit expressions or >20000-deep
  nesting. Now refused. Note: a builder expression nested >20000 deep is
  therefore unsupported; SMT-LIB2 input is unaffected (already explicit).

### 2026-08-18 — the 2^63 cliff, third and fourth instances (B26, B29)

Both are **wrong `unsat` reachable in the default engine**, both **verified
pre-existing** against a pristine HEAD build, and both are the same root defect
B22 fixed once already: *a bound stored as `int64_t` is a bit PATTERN for an
unsigned variable, so ordering it with bare signed operators reads a
width-64 domain's top (2^64-1) as -1.* B22 fixed the var-const branch of one
propagator. It is now clear that was a point fix on a systemic problem — the
same compare was still wrong in the clause database (B26) and in the var-var
branch of the very same propagator (B29).

**Standing rule from here: any comparison of two bound values must go through
`var_b_lt`/`var_b_gt` (one variable) or `_pair_lt` (two).** A bare `<`/`>` on
`var_lo64`/`var_hi64` results is a bug until proven otherwise. Grep for it when
touching this code.

- **B29: a var-var disjunct is "definitely false" for every operand at width
  64 → wrong `unsat`.** `(or (bvult a b) (bvugt a b))` on two 64-bit vars —
  satisfiable by any two distinct values — answered `unsat` on `--engine=auto`.
  The var-var branch of `_fire_disj_clause` compared `l_lo`/`l_hi` against
  `r_lo`/`r_hi` with bare signed operators, so with both domains full
  (`lo=0, hi=-1`) `l_lo >= r_hi` (0 >= -1) held and *every* disjunct was
  falsified at once → `n_false == n` → level-0 conflict → UNSAT.
  Fixed with a new `_pair_lt(lv, rv, a, b)`: unsigned ordering when both
  operands are unsigned and at least one is width >= 64, signed otherwise (so a
  genuinely negative bound still reads as below the domain).
  The var-var *enforcement* arm below it does `r_lo + 1` / `l_hi - 1` without
  saturating at the representable edge. That direction only ever loses
  strength (a no-op tightening), never soundness, so it is left as-is.
  Regression: `tests/formal/test_disj_hull.py::test_b29_var_var_disjunct_wrong_unsat`
  (4 shapes x 4 widths, both engines; widths 8/32/63 are the control that
  always passed) plus `test_b29_mirror_var_var_still_unsat`.

### 2026-08-18 — B26: learnt-clause literals were compared with SIGNED int64

- **B26: any learnt clause over an unsigned width-64 variable is read as
  falsified at level 0 → wrong `unsat`. Production-reachable** (`--engine=auto`
  picks CDCL for QF_BV). Repro:
  `(declare-const v (_ BitVec 64)) (assert (or (bvuge v (_ bv3 64)) (distinct v v)))`
  → `unsat`; bitblast and z3 say `sat`.

  **Root cause.** `literal_is_true`/`literal_is_false` (`zsp_lcg.h`) ordered a
  literal's bound against the variable's bounds with bare `<`/`>` on `int64_t`.
  For an unsigned variable of width >= 64 the stored bounds are *bit patterns*,
  so a full domain `[0, 2^64-1]` has `hi == -1`. Trace of the repro: the search
  decides `v = 0`, both disjuncts are legitimately false, conflict analysis
  learns the correct unit clause `v >= 1` and backjumps to level 0 — and then
  `clause_propagate` evaluates `v >= 1` against `hi == -1`, concludes `-1 < 1`,
  calls the clause falsified at level 0, and the search loop's
  "conflict at level 0 means UNSAT" rule fires. This is the **same 2^63 cliff
  as B22**, one layer up: B22 fixed it in the DisjClause propagator, this fixes
  it in the clause database. Fixed by ordering through `var_b_lt`/`var_b_gt`,
  which select unsigned ordering exactly for unsigned width >= 64.

  The `already_tight` short-circuits in `zsp_clause_prop.c` (both the
  event-driven and the full-scan path) had the same bare-signed compare, and
  are fixed the same way. That half is a *missed propagation*, not a wrong
  answer, and is not what the repro needed — fixed alongside because it is the
  identical defect on the identical values.

  **The originally-filed root cause was wrong.** `explain_disj_clause` really
  can return an empty explanation for a single-variable disjunction, but this
  conflict is seeded by the propagator-conflict path in `lcg_analyze_conflict`
  (watch-var bounds), which never calls `explain`. The `[lcg-trace]` dump shows
  exactly one conflict, one correct learnt clause, and then UNSAT — which is
  what pointed at clause evaluation rather than at explanation. Lesson: confirm
  which code path a conflict actually takes (`DV_LCG_TRACE=1`) before sizing a
  fix around it. The empty-explanation hole remains a latent weakness worth
  closing on its own merits, but it is not a known wrong-answer source.

  Regression: `tests/formal/test_aux_var_tiers.py` — `test_b26_self_neq_disjunct`
  (3 ops, was xfail-strict, now a positive assertion on both engines),
  `test_b26_model_is_correct` (the model must land in `[3,5]`, not merely
  report `sat`), and `test_b26_mirror_still_unsat` (a genuinely-unsat width-64
  range must still be `unsat`, so the fix cannot pass by never concluding
  `unsat`).

### 2026-08-17 — the aux-variable tier family (B24, B25, B27)

All three are **wrong `unsat` reachable in the DEFAULT engine** — the worst
class we have, because the model-validation net only guards `sat`. All three
were **pre-existing** (verified against a pristine HEAD build, not assumed) and
all three were found by the **fuzzer**, only after `fuzz_bv_smt2.py` was taught
to emit Verilator's reified `(= #b1 (__Vbv ...))` form. The generic Boolean
shapes it emitted before never reached these paths. That is the same lesson as
B22: *when you add or touch a code path, check whether the fuzzer can reach it.*

B24 and B25 share one root cause: **an auxiliary variable created with
`_init_tier0` at a hard-coded or default width.** Tier-0 stores bounds as
`int32`, and `VAR_IS_TIER0` is also the flag propagators read to decide they may
use their 32-bit fire function. So a mis-tiered aux var is wrong twice over: the
number is truncated, *and* the wrong propagator runs on it. The fix is a shared
`_init_aux_tiered()` that applies the same tier policy as a declared variable
(unsigned 32-bit promotes to tier-1), verifies the value FITS the declared
width, and declines (-> uncompiled -> `unknown` -> escalation) rather than
mis-encoding.

- **B24: reified comparison against a constant >= 2^31 → wrong `unsat`.**
  `(assert (= #b1 (__Vbv (bvuge (_ bv2147483648 64) (_ bv1 64)))))` → `unsat`.
  `_bool_to_var`/`_value_to_var` materialised the comparison's constant as a
  tier-0 AUX var, so `_fire_reification_32` compared the raw `int32` fields and
  read 2147483648 back as -2147483648: `-2147483648 >= 1` is false. This is the
  B22 cliff one tier down — a **2^31** cliff instead of 2^63. A second instance
  of the same call put a 96-bit constant in a width-32 var (a variable that is
  simply a lie), which the bounds engine then "proved" impossible.
  Fixed at 6 call sites in `zsp_compile.c`.

- **B25: `bvand`/`bvor`/... with a constant operand wider than 32 bits → wrong
  `unsat`.** `(assert (bvugt (bvand v #xffffffffffff) #x800000000000))` on a
  48-bit `v` → `unsat`. The const-operand materialisation in the
  comparison-over-a-binary-op route hard-coded width 32, so the mask became
  2^32-1 and the result could never exceed 2^47. Fixed by using the result
  variable's width (BV binary operands share it).

- **B27: `bvadd` did not wrap for widths 32..63 → wrong `unsat`.**
  `(assert (= (bvadd v #xffffffff) #x00000000))` — satisfiable at `v == 1` —
  answered `unsat`. `_compile_binary`'s primary route has used the wrap-aware
  `bv*_64` propagators for widths 1..63 since they landed; the **second** route
  (a comparison whose operand is a binary op) was never updated and fell
  straight through to the non-modular `bounds_add_32/64`. Widths < 32 happened
  to take the primary route, which is why this looked width-32-specific. Fixed
  by mirroring the modular routing. Width 64 remains broken in pure CDCL — see
  **B28** in OPEN.

- **B22: DisjClause hit the "2^63 cliff" → wrong `unsat` on width-64 unsigned.**
  `(or (bvult (_ bv5 64) v) (bvuge v (_ bv9 64)))` on a 64-bit `v` returned
  `unsat`. For an unsigned var of width >= 64 the stored `lo`/`hi` are *bit
  patterns*, so a width-64 domain's top (2^64-1) reads back as `-1`.
  `_clause_definitely_false` compared them with bare signed operators, so
  `hi <= c` was true for every constant: both disjuncts were falsified at once
  and the propagator raised a conflict. `zsp_ctx.h` documents this hazard and
  provides the sign-aware `var_b_*` helpers — this path had never been
  converted. Constant magnitude is irrelevant (constants 5 and 9 reproduce it);
  32- and 16-bit are unaffected.
  Fix (`zsp_prop_templates.c`): route `_clause_definitely_false` through
  `var_b_lt`/`var_b_gt`; clamp `_enforce_clause`'s `c±1` at `var_repr_min/max`
  (they wrapped around the domain at the edges); refuse the propagator entirely
  for width > 64 vars, whose domains `var_lo64/hi64` cannot represent.
  **Found by the fuzzer, not by inspection** — extending
  `tests/formal/fuzz_bv_smt2.py` with a `disj_over_var` production (the generic
  `or` production builds disjunctions over arbitrary *terms*, which essentially
  never reduce to the `var op const` shape `_flatten_or` accepts, so the
  DisjClause path was effectively unfuzzed) surfaced it on 4 of 2500 seeds.
  Pre-existing: reproduces with the Phase 2 hull disabled.
  Regression: `tests/formal/test_disj_hull.py` (`_B22` cases). Post-fix:
  2501/2501 fuzz seeds agree with z3.

- **BUG-1** countones/clog2 tier-1 wrong models — tier-aware propagators.
  (`test_sysfunc.py`)
- **BUG-2** backward bit-slice spurious `unsat` — bounds-consistent propagator.
  (`test_bitslice_backward.py`, exhaustive)
- **BUG-3** search domain-bisection never explored the upper half → false
  `unsat` for under-propagating constraints — reversible two-way split.
  (`test_search_completeness.py`)
- **Array-select tagged as const → wrong `unsat`** — select handler tagged
  element *variables* `leaf_kind=const`, corrupting the zero_extend/boolean
  const-fold. Now `leaf_kind=var`. (`smt2/verilator/rt_array_zext_select.smt2`)
- **Signed `bvsdiv`/`bvsrem`/`bvsmod`** — SMT-LIB lowering, force-routed to
  bitblast, 256/256 vs z3. (`test_signed_ops.py`)
- **B10: CDCL hang (no wall-clock budget) → now degrades to `unknown`** — the
  search bounded *work* (conflict/restart counters) but not *time*: a hard shape
  churned a Luby-growing conflict budget for minutes, and the LCG learn/propagate
  loop (`clause_propagate` at `zsp_search.c:529`, `continue`s to itself) could
  cycle without progress, never reaching the outer budget check — both read as a
  hang (`t_constraint_unpacked_array`, `t_randomize_inside_cond`; masked in
  production by bitblast). Fix: wall-clock deadline in `_solver_solve_core`
  (default 10s, `DV_CDCL_TIME_LIMIT` overrides, <=0 disables), checked cheaply
  (every ~1024 iters) in BOTH the decision loop and the conflict loop → returns
  `SOLVE_TIMEOUT` → `unknown` → (outside `DV_NO_BITBLAST`) escalates to bitblast.
  Production unaffected (these route straight to bitblast, 0.01s). Permanent
  guard: `tests/formal/test_cdcl_termination.py` (pure-CDCL, short budget, asserts
  a verdict within a subprocess ceiling — fails on hang).
- **B11: CDCL signed `bvsgt`/`bvsge` wrong-model→`unknown`, and slow signed-cmp
  conjunctions** — signed compares lower via the MSB-flip identity
  `a <s b <=> (a^2^(w-1)) <u (b^2^(w-1))`. Two defects: (1) the frontend built
  the compare over raw xor-exprs — the variable side's xor was never flattened,
  so its `bxor` binding wasn't compiled; the reifier couldn't materialise it and
  the invert branch (bvsgt/bvsge) produced a model that failed validation →
  `unknown`. Fix: `_translate_signed_cmp` flattens the variable side's xor to a
  var and folds a constant operand's flip into a plain const (var-vs-const, which
  `_bool_to_var` reifies). (2) Even once correct, a conjunction of signed compares
  blind-enumerated: the `bxor` propagator only did singleton propagation, severing
  the bounds link between the var and its flipped copy. Fix: `_fire_bounds_bxor_64`
  now does sign-bit-flip (single-bit / power-of-two constant) bounds propagation —
  when a range lies in one 2^m-aligned block, xor with 2^m is an order-preserving
  shift, propagated both directions. Recovered foreach, inheritance, nested_class
  (pure-CDCL sat); all bvsgt/bvsge/bvslt/bvsle now correct. Regression:
  `rt_signed_gt.smt2`. (64-bit sign bit 2^63 excluded — stays sound `unknown`.)
- **B9: CDCL infinite propagation hang on wide two-var `!=` / comparison** — the
  negated two-var compare compile site (`zsp_compile.c`, `(not (= x y))` etc.)
  installed the **32-bit** bounds propagators unconditionally, with no width
  check — unlike the sibling non-negated site. On 64-bit / tier-1-promoted vars
  the 32-bit propagator truncated the bounds and never reached fixpoint, so
  `solver_propagate` spun forever, never reaching the conflict/time budget → a
  hard hang (worse than a wrong answer: it blocks, and default mode never
  escalates because escalation needs `SOLVE_TIMEOUT`, not a live spin). Fix:
  make that site width-aware (`_var_needs_wide` → `_ne_64`/`_le_64`/... ), mirror
  of the non-negated dispatch. Regression: `rt_ne_wide_vars.smt2`. Found via a
  64-bit `(not (= x y))` probe (both vars free) that hung >6s.
  NB: a live propagation spin is invisible to the conflict-budget escape — any
  new propagator must guarantee fixpoint (bounds tighten monotonically at the
  correct width).
- **B8: CDCL wrong `unsat` on 64-bit `concat` disequality/inequality** — delta-
  debugged `t_constraint_operators` to a single assert: `(not (= (concat c b) K))`
  with `c,b` 32-bit (a 64-bit concat). Root cause: `_fire_bounds_concat_64`
  reasons in signed int64 (`hi << lo_w` overflows, arithmetic `r >> lo_w`, `>= 0`
  guards). A full 64-bit result range [0, 2^64-1] lands in the upper half
  (>= 2^63), where that signed arithmetic manufactures spurious empty-domain
  conflicts → wrong `unsat` (repro: `r == concat(c,b)` with `r > K`, or `c != 0`;
  lower-half cases like `r < K`, `r == K` were fine). The range isn't
  representable in signed-int64 bounds. Fix: decline native compile for a
  full-width (>= 64-bit) concat result (`zsp_compile.c`) → uncompiled → the
  model-validation net + bitblast escalation make it sound (`unknown` in pure
  CDCL, `sat`/`unsat` via bitblast in production). t_constraint_operators now
  pure-CDCL `unknown` / production `sat`. Removed from the soundness-audit xfail
  list (no known wrong-answer CDCL bugs remain). NB: the width-aware `var NEQ
  const` fix at the negated-compare site (mirror of B9) also landed here.
- **B7: CDCL `(var >= 0)` reification wrong `unsat`** — `_bool_to_var` reified
  `var >= c` as `NOT(var <= c-1)`; for `c==0` the `-1` threshold wraps to `2^w-1`
  on an unsigned var → guard forced false → wrong `unsat`. Fix: fold inequalities
  that are trivially true/false at the unsigned range boundary in `_bool_to_var`
  (`zsp_compile.c`). Fixed 3 corpus fixtures (randc_wide, union, solve_before).
- **B1: `sign_extend` to 64 bits wrong `unsat`** — the CDCL compile mis-lowered
  it (see B5) AND ran even for bitblast-routed solves. Two-part fix: (1) the
  bitblast path now skips the CDCL compile entirely (`smt2_frontend.c`); (2)
  `sign_extend` to ≥64b force-routes to bitblast. `t_constraint_operators`
  un-quarantined (now sat=sat). Regression:
  `smt2/verilator/rt_sign_extend_64.smt2`.

---

## Pre-existing / external (not ours)

- 4 `bitwuzla` tier3 BMC fixtures return `unknown` (external solver). Present
  before this effort; unrelated. (`test_tier3_baseline.py::...bitwuzla...`)

### B70 — DV_ARRAY lazy engine: spurious sat on a later incremental check (FIXED a9cf2f1, 2026-10-02)
Only the first check-sat ran `_check_sat_array`; later checks fell through to the
standard path with abstract reads as free vars and no lemmas. `i == j; check-sat;
a[i] != a[j]; check-sat` answered sat (z3: unsat). Now answers unknown. Opt-in
engine only. Real fix = incremental refinement (correctness plan step 1).
  → Real fix landed with correctness-plan step 1 (2026-10-02): every check-sat with
  abstract arrays re-solves from the retained builder; push/pop scope the
  abstract-array state.

Found by the new incremental array session fuzzer (`generate_session`,
`test_fuzz_array_session`, auto + DV_ARRAY=1), 2026-10-02. B71/B72 are DEFAULT-path
wrong answers with no arrays needed; all four pre-date step 1 (reproduced on a9cf2f1).

### B71 — push after a conflicting flush dropped the wrong scope (wrong sat) (FIXED 2026-10-02)
`x == 3; push; x == 4; push; pop; check-sat` answered sat. The second push's
`_flush_aux` hit the conflict (-2), `_cmd_push` returned an error without recording
a frame, so the pop removed the `x == 4` scope. Now a flush conflict records dead
frames (no checkpoint) like a compile-time UNSAT; the crc==-2 path also now records
n frames for `(push n)`, not 1.

### B72 — declarations after a compile shared one var id (wrong unsat) (FIXED 2026-10-02)
`_next_var_id` = max(fe->n_vars, ctx->n_vars) skips ids a compile used internally,
but `_add_var` advanced n_vars by one slot, so the next declaration got the SAME id.
After any compile with internal aux vars (e.g. an ite), `declare z; declare w;
assert z != w` was unsat, and all elements of a dense array declared then were one
var. Fix: `_add_var` bumps n_vars past the id (`_bump_n_vars`).

### B73 — sparse array kept element vars from a popped scope (crash) (FIXED 2026-10-02)
A large-address array declared before a push kept the element vars made inside it
in its sparse table; the pop rewound the ids, a later select of that index read a
dangling var → SIGSEGV. Pop now drops entries with var id >= n_vars.

### B74 — vars minted in the problem left the name table short (crash) (FIXED 2026-10-02)
The lazy engine's in-problem read/witness vars raised n_vars past vars_cap with no
name-table slots; the next `_find_var` read off the end (SIGSEGV in interactive
sessions after a check-sat). `_bump_n_vars` grows + blanks the skipped slots;
`_find_var`/get-model bound by vars_cap. Also: transient dense array values
(`_make_array_value`) left `is_abstract` uninitialized — latent until arrays could
be abstract outside DV_ARRAY; now zeroed.
