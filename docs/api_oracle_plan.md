# API-mode oracle check: dv-solve's C API answers checked by z3

Status: implemented (2026-10-07); first results in §10. Companion to the SMT-LIB2-mode oracle
(`DV_ORACLE`, `src/c/smt2/smt2_oracle.c`).

## 1. Goal

The smt2 oracle checks `dv-solve-smt2` (Verilator, yosys-smtbmc) against a
reference SMT solver. Clients of the **C API** — pyvsc above all, also the DPI
and zuspec paths — build problems with `dvs_builder_*`, compile them and call
`dvs_solver_solve()`. They never produce SMT-LIB, so they had no oracle check.

This adds one, configured through the API rather than environment variables, and
recording into the **same run-directory format**, so one report tool
(`python -m dv_solve.oracle report`) covers Verilator runs and pyvsc runs alike.

## 2. What is checked

Exactly two things: **does dv-solve's sat/unsat verdict agree with z3**, and
**do the values dv-solve produced satisfy the constraints, as z3 judges them**.

| dv-solve says | the oracle is asked | ok | failure | otherwise |
|---|---|---|---|---|
| `dvs_solver_solve` → OK | the problem + every variable pinned to dv-solve's value | sat | **bad-model** (unsat) | unchecked (unknown/timeout) |
| `dvs_solver_solve` → UNSAT | the problem | unsat | **bad-unsat** (sat; z3's model is kept) | unchecked |
| `dvs_solver_compile` → `DVS_COMPILE_UNSAT` | the problem | unsat | **bad-unsat** | unchecked |
| `dvs_solver_solve` → TIMEOUT | — (dv-solve claimed nothing) | | | skipped |

"The problem" is the context's *current* hard state:

* every variable, with its declared domain `[lo, hi]`;
* every hard constraint and all-different;
* constraints added with `dvs_solver_add_constraint()`;
* pins (`dvs_solver_pin_var`) and exclusions (`dvs_solver_exclude_value`).

Soft constraints never make a problem unsat and never make a model wrong, so
they are left out (their count is recorded). Distributions are sampling
weights, not domain restrictions, in dv-solve (a front end that wants SV's
`dist`-implies-`inside` emits that `inside` as a hard constraint), so they are
left out too.

The oracle never changes what dv-solve answers or the random stream it draws.

## 3. The independent semantics path

The API speaks SystemVerilog expression semantics (`dvs_sv.h`): unsized
literals, context-determined widths, signedness rules. The engines run on the
problem **after** `dvs_sv_elaborate()` has rewritten it into explicit form.

The oracle could emit that elaborated problem 1:1 into SMT-LIB, but then a bug
in elaboration would be invisible — z3 would faithfully solve the same wrong
problem. So the emitter (`src/c/dvs_smt2_emit.c`) translates the **original**
problem under the SV rules itself. It is a symbolic port of the model
validator's evaluator (`dvs_validate.c` `_vtype`/`_ev`), which is already the
second, independent reading of those rules; the emitter is the third. Where the
validator *skips* (arrays, sums, `$countones`, `$clog2`, values wider than 64
bits), the emitter still produces SMT-LIB, so those constructs are covered for
the first time.

Each query records its semantics class in `sem`:

* `sv` — everything was translated exactly.
* `sv+div` — the problem divides. SV leaves `x / 0` as X; SMT-LIB defines it
  (`bvudiv x 0 = ~0`, `bvurem x 0 = x`). A disagreement on such a problem may
  be that difference rather than a bug; the report groups these apart.
* `sv+agg` — the problem uses `sum`, `countones`, `clog2` or `array_select`.
  The emitter reads them as exact (non-wrapping) integer relations, and an
  array index outside the array as false.

What the oracle **cannot** see: a bug in the front end that built the problem
(pyvsc's translator, for one — the `c32 == a16 + b16` carry bug). The problem
it checks is the one it was handed. pyvsc's own XCHECK (`VSC_DVSOLVE_XCHECK`,
Boolector, model level) covers that layer; the two are complementary.

## 4. API

In `dv_solve.h`:

```c
typedef struct dvs_oracle_s dvs_oracle_t;

typedef struct {
    const char *solver;    /* "z3" (default) or "bitwuzla"                   */
    const char *bin;       /* the solver's executable; NULL = from PATH      */
    const char *command;   /* a whole command line instead (SMT-LIB2 on stdin) */
    const char *out_dir;   /* run directory; %t = UTC time, %p = pid;
                            * NULL = "dvs-oracle/%t-%p"                     */
    const char *tag;       /* free text recorded in run.json                 */
    double      timeout_s; /* per query; 0 = 10 s                           */
    int         keep_all;  /* 1: also write transcript.smt2 and every query's
                            * record; 0: records + failure bundles only     */
} dvs_oracle_opts_t;

dvs_oracle_t *dvs_oracle_create(const dvs_oracle_opts_t *opts, FILE *err);
void          dvs_oracle_destroy(dvs_oracle_t *o);      /* finalises run.json */
int           dvs_solver_set_oracle(dvs_ctx_t *ctx, dvs_oracle_t *o,
                                    const char *label);  /* before compile */
int           dvs_oracle_last_result(const dvs_oracle_t *o);
void          dvs_oracle_get_stats(const dvs_oracle_t *o, dvs_oracle_stats_t *st);
const char   *dvs_oracle_run_dir(const dvs_oracle_t *o);

int dvs_problem_write_smt2(const dvs_problem_t *p, FILE *out);
```

Design points:

* **One oracle, many contexts.** pyvsc builds a context per RandSet and
  caches them; all share one reference-solver process and one run directory.
  `label` names the context in every record (pyvsc passes the RandSet's
  field names), so a failure points back at the source.
* **No abort inside the library.** The smt2 binary may `exit(3)` on a P0
  (`DV_ORACLE_ON_FAIL=abort`); a library must not kill its host. The caller
  reads `dvs_oracle_last_result()` after each solve and decides — the Python
  wrapper raises `OracleMismatch` when asked to.
* **`dvs_problem_write_smt2()`** is the emitter on its own: any API problem as
  a stand-alone `.smt2` script. Useful for repro reports with no oracle at all.
* Each check sends `(reset)` and the whole script, as the smt2 oracle does (z3's
  incremental core, once it sees a `push`, is far slower on these queries). The
  script is also the bundle's repro: `z3 fail/q000012.smt2` replays it.
* POSIX only, like the smt2 oracle: on Windows `dvs_oracle_create` reports
  that and returns NULL.
* Not thread-safe: one thread drives an oracle and its contexts at a time.

## 5. State tracking

Per attached context the oracle keeps (`dvs_oracle.c`):

* the variable table (width, signedness) from the compiled problem, so an
  added problem that names variables without declaring them is typed;
* the base script (declarations, domains, constraints), emitted at compile —
  so the problem need not outlive the compile call;
* a list of entries (added constraints, pins, exclusions), each tagged with the
  checkpoint depth it was made at.

The context's rules, mirrored:

| call | effect on the oracle's state |
|---|---|
| `pin_var` | a pin at the current depth |
| `reset` | outside any checkpoint, drops every pin; inside one, nothing (dv-solve returns to the checkpoint and re-applies the scope's pins) |
| `exclude_value` | an exclusion; survives reset and restore |
| `add_constraint` | the constraints, at the current depth |
| `checkpoint` | depth + 1 |
| `restore(cp)` | drops entries made deeper than `cp`; depth = `cp` |

A failing `pin_var` / `exclude_value` / `add_constraint` adds nothing.

## 6. Records

The run directory is the smt2 oracle's: `run.json` (`kind:
dv-solve-oracle-run`, `mode: "api"`), `queries.jsonl`, `fail/qNNNNNN.{smt2,json}`,
and with `KEEP_ALL` a `transcript.smt2`. A query record:

```json
{"q": 7, "s": 3, "d": 0, "cmd": "api-solve", "sem": "sv",
 "dvs": {"verdict": "sat", "engine": "api", "ms": 0.41, "validate": -1},
 "orc": {"check": "model", "verdict": "sat", "ms": 3.2, "pinned": 4},
 "res": "ok", "label": "c.a c.b", "seed": 12345, "softs": 0}
```

`s` is the context number (one per attached context), `d` the checkpoint depth.

## 7. Code layout

* `src/c/dvs_oracle_core.{c,h}` — the reference-solver process, run directory,
  records and result classes, factored out of `smt2_oracle.c`, which now uses
  them. In `libdv_solve`.
* `src/c/dvs_smt2_emit.{c,h}` — problem → SMT-LIB2 under SV semantics.
* `src/c/dvs_oracle.c` — the public API and the per-context state; hooks in
  `dvs_solver_compile/solve/reset/pin_var/exclude_value/add_constraint/
  checkpoint/restore/destroy`.
* `src/dv_solve/oracle.py` — gains the `Oracle` class (ctypes) alongside the
  report tool; `SolveCtx` takes `oracle=`, and `dv_solve.set_default_oracle()`
  attaches one to every context created afterwards.

## 8. pyvsc

`vsc.set_solver_oracle(...)` (and `vsc.get_solver_oracle()`) create a
`dv_solve.oracle.Oracle` and install it as the default for every dv-solve
context pyvsc builds; `raise_on_fail=True` turns a P0 into an exception at the
`randomize()` that produced it. A whole regression is compared with:

```python
import vsc
vsc.set_solver_oracle(out_dir="oracle/%t-%p", tag="unit")
... run ...
vsc.set_solver_oracle(None)      # finalises run.json
```

then `python -m dv_solve.oracle report oracle/`.

## 9. Not covered (yet)

* `dvs_solver_solve_n`, `propagate_only`, `check_unsat`, `optimize` are not
  checked (`solve_n` calls `dvs_solver_solve`, so its solves are).
* The BVSat bit-blast path (`dv_solve.bvsat`, used by pyvsc's bvsat sampler)
  is a separate engine with its own API; it is not hooked.
* Out of scope by design: timeouts, soft-constraint optimality (were the right
  softs dropped?), distribution quality.

## 10. First results (2026-10-07)

**pyvsc regressions, oracle on** (`vsc.set_solver_oracle()` from a pytest
plugin, dv-solve backend):

| suite | tests | queries checked by z3 | ok |
|---|---|---:|---:|
| `ve/unit` | 587 passed | 28,994 | 28,994 |
| `ve/unit_dc` | 417 passed | 5,986 | 5,986 |

No disagreement: on everything pyvsc's own tests hand it, dv-solve's verdicts
and values agree with z3. z3 time was 9.3 s over the 29k queries.

**Random API problems** (1,534 problems over 1–64-bit signed/unsigned
variables and every expression kind, 2 seeds each; 3,235 queries): 166
disagreements. Each was triaged against dv-solve's own model validator
(`dvs_validate.c`, an independent SV evaluator): for a bad-unsat, z3's witness
was pinned into a variables-only context and validated against the original
problem; for a bad-model, dv-solve's model was validated. Outside the `/` `%`
class, **every disagreement is confirmed by the validator: these are dv-solve
bugs, not the oracle's translation.** Minimal repros:

| # | problem | dv-solve | right answer |
|---|---|---|---|
| A | `v` 1-bit **signed**, domain [-1, 0]; constraint `v` | compile UNSAT | sat, v = -1 |
| B | `v1` 64-bit **unsigned**, domain [0, 2⁶⁴-1] (hi = -1); `v0` 32-bit; `v0 < v1` | UNSAT | sat |
| C | `v0` 64-bit unsigned, full domain; `v0 inside {-3, 3, 19}` | UNSAT | sat, v0 = 3 |

Of the 79 non-division disagreements, 41 involve a 1-bit signed variable
(A), 9 a full-domain 64-bit unsigned variable (B, C), 5 both, and 7 neither
(e.g. `~((285 && v1) < (v1 != v0))` with an `extract` past a 3-bit variable's
width; casts to 1-bit signed). The `sv+div` class adds 15 validator-confirmed
bad-unsats and 5 validator-confirmed bad-models on top of the expected x/0
differences.

None of these shapes occur in pyvsc's suites (a pyvsc `bit[1]` is unsigned,
and full-range 64-bit unsigned fields are rare), which is consistent with the
clean pyvsc runs above — but they are reachable through the API.
