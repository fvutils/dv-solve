# fw-hdl sessions: one bit-blast instance across check-sats (plan piece 6.1)

Measured 2026-10-09 on the development machine, one solver per core, 60 s per
run. Plan: `docs/fw_hdl_implementation_plan.md` §4 Phase 6 and §6.

## Question

fw-hdl asks many questions of one design: every property at a known latency,
or one property at each depth of a latency search. Before this piece, a
session that kept asserting re-built its whole bit-blast encoding at every
`check-sat`, and a `check-sat-assuming` on a design with state wider than
64 bits answered `unknown` (only the CDCL engine took assumptions, and it takes
no variable wider than 64 bits). Yosys `-stbv` packs the whole state into one
bit-vector, so in practice every fw-hdl design is wider than 64 bits.

## What changed

- **One instance per session.** `src/c/dvs_bbsolver.c` gained a session mode
  (`dvs_bbsolver_sess_*`): one CaDiCaL instance that takes the constraints in
  batches. The front end (`_check_sat_session`, `src/c/smt2/smt2_frontend.c`)
  adds only what was asserted since the last `check-sat`.
- **Scopes as selectors.** Each `push` is a fresh AIG input `sel`. Assertions
  inside the scope are added as clauses `(-sel | a)`, and solving assumes the
  live selectors. `pop` adds `-sel` and forgets the scope's variables, whose
  ids the front end reuses.
- **Substitution inside a session.** A defining equality `v == e` is
  substituted (the bit-blaster's main lever) only while nothing has read `v`,
  and inside a scope only for that scope's own variables. Each batch is taken
  newest first, as the one-shot pass does. Oldest first left 2048 of
  crc32x16's transition equalities to the GF(2) pass at every query, which
  then cost 4 ms each. Newest first, every property folds to a constant
  before SAT, as in a one-shot run.
- **`check-sat-assuming`.** On a problem that bit-blasts, the assumptions go
  in one more scope of the session. It stays open until the next command that
  changes the assertion stack, because `get-value` needs the model. When CDCL
  answers and its model fails validation, the query escalates there too (B89).
- **When the session starts.** At the first `check-sat` inside a `push`, the
  second `check-sat` overall, or any `check-sat-assuming`. A lone
  `check-sat` stays one-shot (kissat and the whole-problem passes), so
  single-query files are unchanged. `DV_BB_SESSION=0` turns sessions off.
  Verilator mode and abstract arrays keep their own paths.
- **Fixed per-query costs.**
  - `dvs_builder_finalize_append` extends the session's problem in place
    instead of copying the whole pool at each `check-sat`.
  - The cycle check stops at variables that are already built.
  - A sync walks only the variables added since the last one.
  - `push` no longer tries a CDCL compile on a problem that must bit-blast
    (that cost a full finalize and failed compile per `push`).
  - `define-fun` lookup uses a hash index, like variable lookup since 5.3.

## Results

### Latency search (`tests/perf/latency_incr.py`)

16 depth queries; dv-solve, seconds:

| design (L) | separate files | session before (5.3) | session now |
|---|---|---|---|
| crc32x16 (8) | 1.02 | 0.79 | **0.28** |
| addtree8x16 (3) | 0.07 | 0.04 | 0.02 |
| rotl32 (5) | 0.05 | 0.02 | 0.03 |
| mac16 (3) | 0.07 | 0.06 | 0.02 |
| fir4 (3) | 0.05 | 0.02 | 0.01 |

For crc32x16, bitwuzla's session takes 4.77 s and z3's times out
(`docs/fw_hdl_latency_scaling.md`).

### Many properties (`tests/perf/property_session.py`)

One design at its latency, one property per output bit (`p_i`: bit i of the
output equals bit i of the specification), every answer `unsat`. `assume` is
one session of `(check-sat-assuming ((not p_i)))`, `push` one session of
`push`/`assert`/`check-sat`/`pop`, `mono` one file per property. Seconds:

`assume` session:

| design (L, properties) | dv-solve | boolector | bitwuzla | yices | cvc5 | z3 |
|---|---|---|---|---|---|---|
| crc32x16 (8, 32) | **0.18** | 0.22 | 18.0 | >60 | >60 | >60 |
| addtree8x16 (3, 19) | **0.01** | 0.32 | 0.43 | 53.9 | 0.15 | 0.17 |
| rotl32 (5, 32) | 0.03 | 0.03 | 0.02 | **0.01** | 0.12 | 0.02 |
| mac16 (3, 32) | **0.01** | 0.10 | **0.01** | **0.01** | 0.02 | 0.11 |
| fir4 (3, 18) | **0.00** | 0.08 | 0.03 | 0.01 | 0.03 | 0.08 |

The `push` session costs the same as `assume` for every solver except yices
(addtree8x16 times out). dv-solve: crc32x16 0.20 s.

One file per property, summed (`n/32`: the number solved within 60 s each):

| design | dv-solve | boolector | bitwuzla | yices | cvc5 | z3 |
|---|---|---|---|---|---|---|
| crc32x16 | **2.82** | 4.06 | 188 | 13/32 | 3/32 | 0/32 |
| addtree8x16 | **0.05** | 25.1 | 11.3 | 12/19 | 1.89 | 14.5 |
| rotl32 | 0.10 | 0.43 | 0.23 | **0.06** | 0.50 | 0.31 |
| mac16 | **0.07** | 0.30 | 0.43 | 0.10 | 0.56 | 1.01 |
| fir4 | **0.03** | 0.69 | 0.55 | 0.12 | 0.58 | 5.11 |

dv-solve is fastest or tied on every design except rotl32, where yices is
20 ms ahead. dv-solve's first query there is a 9 ms SAT proof over the rotate
network; the remaining 31 take 0.7 ms each. No solver gave a wrong answer.

Before this piece, dv-solve answered every `assume` query of all five designs
`unknown`.

## Soundness

`tests/formal/test_bb_session_fuzz.py` generates sessions that mix:
- nested `push`/`pop`, and declarations inside scopes;
- names redeclared at another width after a `pop`;
- defining equalities (substituted), including for a variable declared
  before the `push` and defined inside it, whose negation is asserted after
  the `pop`;
- slice definitions, `let`, Bool definitions and `check-sat-assuming`.

Each script runs in z3 as well. Every sat/unsat must agree, and every
dv-solve model is checked against the assertions in scope. The model check is
off, so a wrong answer shows as wrong rather than `unknown`. Most seeds force
the bit-blast engine; every fourth keeps the default routing, so narrow
problems go through CDCL. 20000 seeds clean on the final build.

Mutants, all caught (the CDCL-side ones by the protocol tests run with
validation off):
- scope assertions not guarded;
- selectors not assumed;
- substitution below the scope floor;
- substitution of an already built variable;
- `pop` keeping the scope's variables, or its entries in the simulation's
  assertion list;
- a wrong fast path in the cycle check;
- the appending finalize skipping bytes;
- no refill after a `pop`;
- `check-sat-assuming` without validation;
- the flush conflict ignored;
- an uncompiled context served as compiled;
- `push` flushing before its reset.

The fuzzer found three bugs, all from before this piece (in
`docs/solver_bug_backlog.md`):

- **B87 (wrong `sat`).** A `push` after a >64-bit declaration could not
  flush into the CDCL context, returned without a scope, and the next `pop`
  removed the enclosing scope with its assertions.
- **B88 (wrong `sat` from CDCL; the model check made it `unknown`).** These
  all involved conflicts at the context root:
  - a context whose compile proved unsat was served to `check-sat-assuming`
    as if compiled;
  - a flush conflict was forgotten by the next `check-sat-assuming`;
  - `push` flushed into a context still holding the last model.
- **B89 (same).** CDCL `check-sat-assuming` had no model-validation net, so
  a constraint the compile left out (`p == not(x < 3)`) was not enforced.

## Regression checks

- fwhdl-core A/B against 7eac036 (`perf-out/p61/fwhdl_ab.log`): 377 files,
  no verdict changes, 5.72 s against 6.33 s.
- sat-core A/B: 237 files, no verdict changes, 0.74 s against 0.73 s.
- `pytest -q`: 9100 passed; the same 35 environment failures as before
  (packages and tools missing from this machine).

## Limits

- Sessions solve with CaDiCaL, which can take assumptions; one-shot runs
  keep kissat. A hard query inside a session is not measured against kissat
  here.
- The GF(2) pass still runs over the whole assertion set at every query. With
  substitution folding the transitions this is cheap (0.3 ms per crc32x16
  property), but a session whose base equalities do not fold pays it each time.
- Narrow (≤ 64-bit) push/pop sessions compile CDCL at the first `push`, so
  their `check-sat`s stay on CDCL, as before.
