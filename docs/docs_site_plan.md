# dv-solve Documentation Site Plan

Status: D0, D1, D2 DONE and live (2026-09-30); D3 (Python) next. Comes before `ci_benchmark_publishing_plan.md`:
the benchmark pages become a "Results" section of this site later.

Goal: a user-facing doc set, published at `dvkit.org/fvutils/dv-solve/`, that
describes what the solver is, how to reach it from each of its front doors,
and the reference for every supported API.

---

## 1. Where things stand

- **No site.** `docs/` is ~60 flat working notes (20 tracked, the rest
  untracked): plans, session logs, perf sweeps, design studies. No `conf.py`,
  no index, no `docs.yml`.
- **Some user-facing material exists but is stale or unverified:**

  | File | Content | Problem |
  |---|---|---|
  | `solver_api.md` | C runtime API (`solver_create/compile/solve`…) | says "zuspec-solver"; not checked against current headers |
  | `builder_api.md` | C + Python builder API | same |
  | `dpi_integration.md` | SV DPI flow via generated classes | says "zuspec-solver"; build path refers to `packages/zuspec-solver` |
  | `soft_constraints.md`, `propagators.md` | feature / internals notes | reasonable starting points |
  | `sby_integration.md` (untracked) | `dv-solve-smt2` as a yosys-smtbmc backend | good starting point |
  | `constraint_debug_framework.md` | 466-line design doc | user-facing part (how to read an unsat diagnosis) needs extracting |
  | `README.md` | Python quick start + module map | the quick start is **broken** (`add_variable`, `ctx.compile()` don't exist); fixed version is `docs/examples/quickstart.py` |

- **The estate uses Sphinx.** `trlog` and `sphinx-systemverilog` use
  `sphinx_rtd_theme` + `myst_parser` + autodoc/napoleon; `pyvsc-main` has the
  reference `.forgejo/workflows/docs.yml`. There is an estate SystemVerilog
  autodoc extension (`sphinx-systemverilog`) we can use for the SV packages.
- **Python autodoc is cheap here:** `dv_solve` loads `libdv_solve.so` lazily
  (`lib._load_lib()`), so Sphinx can import the modules without a native build.

### Things the docs will force us to decide (see §6)

1. **Naming.** C symbols and SV packages are `zsp_*`; README says they will be
   renamed `dvs_*`. A C/SV reference published now publishes the old names.
2. **The public C API has no boundary.** CMake installs *every* header under
   `src/c/` as `include/dv_solve/`. The reference has to say which ones are
   supported; the rest are internal whether or not they ship.
3. **30 `DV_*` environment variables**, almost all internal diagnostics or
   experiment switches (`DV_KISSAT_LIGHT_MAXCLAUSES`, `DV_LCG_TRACE`, ...).
4. **`--mode=verilator` vs stock Verilator.** The CLI help says verilator mode
   "treats check-sat-assuming as a randomization diversity request". The
   bundled Verilator 5.046 never sends `check-sat-assuming`; it diversifies
   with random XOR-hash `assert`s + `check-sat`. The Verilator guide must be
   written against, and tested with, stock Verilator, which may turn up a
   product gap rather than just a doc fix.

---

## 2. Site outline

Sphinx source lives in `docs/site/`; tested example code in `docs/examples/`.
The working notes stay where they are in `docs/` (28 tracked files and many
notes reference them by path) and are outside the Sphinx source tree, so they
can never be published by accident. Pages are MyST Markdown unless autodoc
needs `.rst`.

```
index                      what dv-solve is; four front doors; "pick your path"
getting-started/
  install                  pip wheels (platform matrix from wheels.yml); build from source
  quickstart-python        README example, expanded
  quickstart-smt2          solve a .smt2 file; interactive session
  quickstart-verilator     VERILATOR_SOLVER, one randomize() example
concepts/
  problem-model            variables (width, signedness, domain), expressions, constraints,
                           supported operators
  randomization            seeds, determinism, what "random" means (per-solution uniformity
                           via CDCL); dist, randc, soft, unique
  soft-constraints         from soft_constraints.md
  engines                  CDCL/propagation, bit-blast, cube; auto-routing by logic;
                           when to force one
  soundness                sat/unsat are definitive; `unknown` is honest, never a guess;
                           the list of constructs that currently return unknown
  diagnosing-unsat         reading contradiction/MUS output (user-facing extract of
                           constraint_debug_framework.md)
guides/
  smt2-solver              dv-solve-smt2 in depth: logics (QF_BV, QF_ABV, QF_UFBV,
                           QF_AUFBV), supported commands and options, batch vs
                           interactive, limits (wide BV ≤128b, arrays)
  verilator                external-solver setup, --mode=verilator, what it changes,
                           known gaps
  yosys-sby                smtbmc backend (from sby_integration.md)
  systemverilog-dpi        dvs_dpi_pkg / dvs_randomizer_pkg flow (from dpi_integration.md)
  zuspec                   entry-point integration
  packaging                get_libdirs / get_incdirs / get_svdirs / get_dpi_lib /
                           resolve_report, for tools that link or load dv-solve
reference/
  python                   autodoc: builder, ctx, problem, top-level helpers
  c-api                    builder_* and solver_* (from builder_api.md, solver_api.md)
  sv-api                   sphinx-systemverilog autodoc of src/sv/*.sv
  cli                      dv-solve-smt2 flags, exit codes, output format
  environment              the few supported DV_* variables (§6.3)
  status-codes             SOLVE_OK/UNSAT/TIMEOUT, compile errors, CompileUnsatError &c.
internals/  (later)
  architecture             pipeline: front end → compile → engines → SAT back ends
  propagators              from propagators.md
project/
  changelog, license
  (later) results          ← benchmark plan plugs in here
```

**Python API scope (decided: minimal).** Public = `dv_solve.builder`
(`SolveProblemBuilder`), `dv_solve.ctx` (`SolveCtx`, status codes, compile
errors), the operator constants in `dv_solve.problem`, and the `dv_solve`
package discovery helpers. Everything else (`partitioner`, `icl`,
`structural_solver`, `stream_solver`, `flow_constraint_store`,
`buffer_inference`, `state_graph`, `scheduling_graph`, `bvsat`, and the legacy
fixed-buffer `SolveProblem` class) is internal and not documented; the README
module map shrinks to match.

---

## 3. Tooling

- **Sphinx 7 + `myst_parser` + `sphinx_rtd_theme`** (matches trlog and
  sphinx-systemverilog). `sphinx.ext.autodoc` + `napoleon` for Python.
- **C API: hand-written `c:function::` / `c:struct::` directives** to start.
  Doxygen + Breathe would need doxygen on the runner and good header comments;
  hand-written entries are what `builder_api.md`/`solver_api.md` already are,
  and they let us document only the public subset (§1.2).
- **SV API: `sphinx-systemverilog`** — the estate's own extension. Uses
  pyslang; confirm a py3.10 wheel exists for the runner.
- **Examples that can't rot.** Code on the quick-start and guide pages is
  pulled from `docs/examples/*.{py,smt2,sv}` with `literalinclude`, and a
  pytest module runs every example (Python examples execute; `.smt2` examples
  go through `dv-solve-smt2` and the expected output is checked). This lives in
  the normal test suite, not the docs build, so the docs build stays free of a
  native build.
- `docs/site/requirements.txt` pins the above.

---

## 4. Publishing

`.forgejo/workflows/docs.yml`, copied in shape from `pyvsc-main`:

- triggers: push (all branches — non-`main` stays in staging), PR, dispatch
- internal-identifier grep
- `pip install -r docs/site/requirements.txt` (with the `--break-system-packages`
  fallback)
- `python3 -m sphinx -W --keep-going -M html docs/site docs/site/_build` — `-W` so a
  broken cross-reference or failed autodoc import fails the build rather than
  shipping a hole
- sanity checks: `index.html` exists; page count ≥ a floor set well below the
  known-good count; `reference/python.html` contains at least one documented
  class (catches a silent autodoc collapse)
- upload artifact `docs-fvutils-dv-solve`, `path: docs/site/_build/html/**`

Docs are not a release, so no release-authority gate. No GitHub twin needed
for publishing; optionally add a GitHub build-only check later. Before the
first run, confirm the repo's Actions unit is on (`has_actions`), or the
workflow is ignored silently.

---

## 5. Phases

| Phase | Scope | Done when |
|---|---|---|
| **D0 — scaffold + publish** | `docs/site/` with `conf.py`, `index`, `install`, `quickstart-python`; `requirements.txt`; `docs.yml` | page live at dvkit.org/fvutils/dv-solve/ |
| **D1 — SMT-LIB2 front door** | `quickstart-smt2`, `guides/smt2-solver`, `reference/cli`, `reference/environment`, `concepts/soundness`, `concepts/engines`, `concepts/randomization-seeds`. `guides/yosys-sby` is deferred: yosys-smtbmc only drives solvers from a fixed list, so today it needs `tests/formal/sby/smtio_dvsolve.patch` applied to yosys — not publishable as a user guide | these are independent of the naming decision, and SMT2 is the most-used surface today |
| **D2 — Verilator** | `quickstart-verilator`, `guides/verilator`, `concepts/randomization`; resolve §1.4 against stock Verilator 5.046 | guide's example runs with bundled Verilator |
| **D3 — Python** | `concepts/problem-model`, `soft-constraints`, `diagnosing-unsat`, `reference/python` (autodoc), `reference/status-codes`, `guides/packaging`, `guides/zuspec` | public-module decision made; docstrings on public classes filled in |
| **D4 — C + SV** | `reference/c-api`, `reference/sv-api`, `guides/systemverilog-dpi` | after the naming decision and the public-header list |
| **D5 — hardening** | `docs/examples/` + test module; `internals/`; README links to the site | every code block on the site is executed by the test suite |

D0 is deliberately small: it proves the runner, the artifact and the
publishing path end to end before any real writing, the same way the
benchmark plan's P0 did.

---

## 6. Decisions (resolved 2026-09-30)

1. **`zsp_` → `dvs_` rename happens before D4.** D0–D3 don't expose C/SV
   symbol names, so they proceed now; the C/SV reference waits for the rename
   so we never publish names that are about to change. *Done 2026-10-01:*
   internals, files, CMake options and SV packages are `dvs_`; the public
   API is namespaced too (`dvs_builder_*`, `dvs_solver_*`, `dvs_ctx_t`,
   `dvs_expr_t`, `DVS_BIN_*`, `DVS_SOLVE_*`). The override variable is
   `DVS_SOLVER_PATH`, with `ZSP_SOLVER_PATH` still honoured.
2. **Public C API = one umbrella header**, `dv_solve/dv_solve.h`, declaring
   exactly the documented surface (builder + solver runtime). Other headers
   keep installing for now but are internal. *Done 2026-10-01:* the header
   mirrors the public Python surface; `soft_active` is left out (its index
   runs in reverse add order), as are `add_source`, `expr_array_select`,
   `solve_n` and the fixed-buffer `solve_problem_*` / `expr_*` API.
   `tests/c/test_public_api.c` exercises it through that header alone.
   Follow-up: the library still exports every internal symbol; building
   with `-fvisibility=hidden` needs the ctypes tests moved off internals.
3. **Environment variables: expose as few as possible.** Most `DV_*`
   variables are internal diagnostics and stay undocumented. The reference
   page lists only variables a user needs to control behaviour, starting from
   `DV_ENGINE` (same as `--engine=`) and admitting others only when a guide
   actually needs them. Documented = supported; anything undocumented may
   change or disappear.
4. **Python public surface: minimal** (§2).
