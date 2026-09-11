"""Compile-time exact domains for `x inside {a, b, c}` must not lose solutions.

Phase 3 of docs/cdcl_verilator_coverage_plan.md. `_disj_hull` tightens a
single-variable equality disjunction to the interval hull [min, max], but the
GAPS survive, and the search then enumerates them blind. Measured on five 8-bit
variables each pinned to {0x10,0x20,0x30,0x40,0x50}: >10 s and `unknown`, while
the same five over a contiguous {0x10..0x14} solve in 2 ms -- the gaps were the
entire cost (t_constraint_unpacked_array). solver_compile() now punches the
gaps out as holes, once, at level 0.

WHY THIS FILE EXISTS SEPARATELY FROM test_disj_hull.py
------------------------------------------------------
test_disj_hull.py probes membership by PINNING each candidate. That is the right
gate for the hull -- but it is blind to a bad hole, because a pinned variable
has a singleton domain and never reaches the hole-aware value picker. Holes are
only visible in a FREE solve. So the gates here are:

  1. Sampling. Solve the free problem under many seeds. Every model must be in
     the set (no under-punching) and every member must eventually appear (no
     over-punching -- a wrongly-punched value would simply never be returned).
  2. Forced solutions. Squeeze the problem down to exactly one legal value and
     require it. Over-punching that value turns a satisfiable problem into
     `unsat`/`unknown` with nothing downstream to catch it.
  3. Guarded disjunctions. A DisjClause under an ITE branch or a soft
     constraint only pins the variable WHEN THE GUARD HOLDS, so the install
     skips any propagator carrying a guard var. HONEST LIMIT: the two guarded
     tests below currently prove nothing. Deleting the guard check does not
     make them fail (verified by mutation), because no SMT2 shape tried --
     `(=> g (or ...))`, a Bool `ite`, `(or (= g 0) ...)` -- actually produces a
     GUARDED single-variable all-equality DisjClause. They are a forward guard
     for whoever creates one (the builder/DPI soft-constraint path can), not
     evidence that the check works.

MUTATION RESULTS (a green soundness gate proves nothing on its own):
  - punching one value past each gap end   -> 4 tests fail
  - dropping the sort before walking gaps  -> test_unsorted_set_also_terminates
    fails (it punches nothing, a silent loss of the optimisation rather than a
    wrong answer, so only a termination test catches it)
  - dropping the guard check               -> NOTHING fails; see above

Kill switch for A/B: DV_DISJ_HOLES=0.

Usage:
    direnv exec . pytest tests/formal/test_exact_domain.py -v
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": "20"}
_MODEL = re.compile(r"\(define-fun\s+(\S+)\s+\(\)\s+\(_ BitVec \d+\)\s+"
                    r"\(_ bv(\d+) \d+\)\)")

_SETS = [
    [0x10, 0x20, 0x30, 0x40, 0x50],   # the unpacked_array set
    [2, 9, 17, 30],                   # sparse, irregular gaps
    [0, 255],                         # both edges of an 8-bit domain
    [1, 2, 250],                      # one big gap
    [7, 8, 9],                        # contiguous: no holes to punch
    [30, 2, 17, 9],                   # DESCENDING-ish: the install must sort
    [0x50, 0x10, 0x30, 0x20],         # shuffled
]


@pytest.fixture(autouse=True)
def _need_binary():
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")


def _run(lines: list[str], seed: int | None = None,
         env: dict | None = None) -> tuple[str, dict[str, int]]:
    src = ""
    if seed is not None:
        src += f"(set-option :seed {seed})\n"
    src += "\n".join(lines) + "\n"
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=src, capture_output=True, text=True, timeout=60,
                       env={**os.environ, **_PURE, **(env or {})})
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    model = {n: int(v) for n, v in _MODEL.findall(p.stdout)}
    return verdict, model


def _inside(var: str, vals: list[int], w: int = 8) -> str:
    parts = " ".join(f"(= {var} (_ bv{v} {w}))" for v in vals)
    return f"(assert (or {parts}))"


# ------------------------------------------------------------------- sampling

@pytest.mark.parametrize("vals", _SETS, ids=lambda v: "_".join(map(str, v)))
def test_free_solve_samples_exactly_the_set(vals) -> None:
    """Over many seeds: every model is in the set, and the whole set shows up.

    A value punched by mistake is not reported as an error anywhere -- it just
    silently stops being reachable. Coverage over seeds is the only way to see
    that, so this asserts on the sampled set, not merely on `sat`.
    """
    seen = set()
    for seed in range(1, 61):
        verdict, model = _run(["(set-logic QF_BV)",
                               "(declare-fun x () (_ BitVec 8))",
                               _inside("x", vals),
                               "(check-sat)", "(get-model)"], seed=seed)
        assert verdict == "sat", f"seed {seed}: {verdict}"
        assert "x" in model, f"seed {seed}: no model"
        assert model["x"] in vals, \
            f"seed {seed}: x={model['x']} is outside {vals}"
        seen.add(model["x"])
    assert seen == set(vals), f"never sampled {sorted(set(vals) - seen)}"


@pytest.mark.parametrize("vals", _SETS, ids=lambda v: "_".join(map(str, v)))
def test_each_member_survives_as_the_only_solution(vals) -> None:
    """Squeeze to one legal value; it must still be found.

    This is the over-punching gate with no escape hatch: if the exact domain
    deleted `target`, the problem has no models left and the answer flips to
    `unsat` (or `unknown`) on an input that is plainly satisfiable.
    """
    for target in vals:
        lines = ["(set-logic QF_BV)", "(declare-fun x () (_ BitVec 8))",
                 _inside("x", vals)]
        lines += [f"(assert (distinct x (_ bv{v} 8)))"
                  for v in vals if v != target]
        lines += ["(check-sat)", "(get-model)"]
        verdict, model = _run(lines, seed=1)
        assert verdict == "sat", f"target {target}: {verdict}"
        assert model.get("x") == target, f"target {target}: got {model}"


def test_two_disjunctions_over_one_variable_intersect() -> None:
    """Two sets on the same variable: only the intersection may be returned.

    Each disjunction contributes its own holes, so the installed domain is the
    intersection. Under-punching here would let a value from one set but not
    the other through -- which the watched-literal rule would still catch, so
    this is really a check that the two hole sets compose rather than clobber.
    """
    a, b = [2, 9, 17, 30], [9, 17, 44]
    for seed in range(1, 21):
        verdict, model = _run(["(set-logic QF_BV)",
                               "(declare-fun x () (_ BitVec 8))",
                               _inside("x", a), _inside("x", b),
                               "(check-sat)", "(get-model)"], seed=seed)
        assert verdict == "sat", f"seed {seed}: {verdict}"
        assert model["x"] in (9, 17), f"seed {seed}: x={model['x']}"


# -------------------------------------------------------- conditional clauses

def test_guarded_disjunction_is_not_exactified() -> None:
    """A disjunction that only holds under a guard must NOT pin the variable.

    `(=> (= g 1) (x inside {3, 20}))` says nothing about x when g is 0. If the
    exact domain were installed unconditionally, x could never take any value
    but 3 or 20 -- deleting almost the whole solution space of a satisfiable
    problem. With g forced to 0, x must range freely.

    CAVEAT: this shape does not currently compile to a *guarded* DisjClause, so
    the test passes with the guard check removed. See the module docstring.
    """
    seen = set()
    for seed in range(1, 41):
        verdict, model = _run([
            "(set-logic QF_BV)",
            "(declare-fun x () (_ BitVec 8))",
            "(declare-fun g () (_ BitVec 1))",
            "(assert (=> (= g #b1)"
            " (or (= x (_ bv3 8)) (= x (_ bv20 8)))))",
            "(assert (= g #b0))",
            "(check-sat)", "(get-model)"], seed=seed)
        assert verdict == "sat", f"seed {seed}: {verdict}"
        seen.add(model["x"])
    assert seen - {3, 20}, \
        "x never left {3, 20} with the guard false -- a conditional " \
        "disjunction was installed as an unconditional exact domain"


def test_guarded_disjunction_still_binds_when_the_guard_holds() -> None:
    """The mirror of the above: with g forced to 1, the set must be enforced."""
    for seed in range(1, 21):
        verdict, model = _run([
            "(set-logic QF_BV)",
            "(declare-fun x () (_ BitVec 8))",
            "(declare-fun g () (_ BitVec 1))",
            "(assert (=> (= g #b1)"
            " (or (= x (_ bv3 8)) (= x (_ bv20 8)))))",
            "(assert (= g #b1))",
            "(check-sat)", "(get-model)"], seed=seed)
        assert verdict == "sat", f"seed {seed}: {verdict}"
        assert model["x"] in (3, 20), f"seed {seed}: x={model['x']}"


# ------------------------------------------------------------------- the win

def test_unpacked_array_shape_terminates() -> None:
    """The measurement that motivated the change: five independent `inside`s.

    Hull-only this does not finish in 10 s. It is the pure-BV reduction of
    t_constraint_unpacked_array (which reads the same set out of an array).
    """
    vals = [0x10, 0x20, 0x30, 0x40, 0x50]
    lines = ["(set-logic QF_BV)"]
    lines += [f"(declare-fun v{i} () (_ BitVec 8))" for i in range(5)]
    lines += [_inside(f"v{i}", vals) for i in range(5)]
    lines += ["(check-sat)", "(get-model)"]
    verdict, model = _run(lines, seed=1)
    assert verdict == "sat", verdict
    for i in range(5):
        assert model[f"v{i}"] in vals, f"v{i}={model[f'v{i}']}"


def test_unsorted_set_also_terminates() -> None:
    """Same shape, constants in scrambled order.

    The install sorts before walking the gaps. Without the sort it punches
    nothing (the gap loop runs backwards and does zero iterations), which is
    not a wrong answer -- it is a silent loss of the whole optimisation. Only a
    termination test catches that, so this case exists to make the sort
    load-bearing in the suite.
    """
    vals = [0x40, 0x10, 0x50, 0x20, 0x30]
    lines = ["(set-logic QF_BV)"]
    lines += [f"(declare-fun v{i} () (_ BitVec 8))" for i in range(5)]
    lines += [_inside(f"v{i}", vals) for i in range(5)]
    lines += ["(check-sat)", "(get-model)"]
    verdict, model = _run(lines, seed=1)
    assert verdict == "sat", verdict
    for i in range(5):
        assert model[f"v{i}"] in vals, f"v{i}={model[f'v{i}']}"


def test_wide_span_set_stays_correct_above_the_cap() -> None:
    """A sparse set over a wide span must stay CORRECT, punched or not.

    The install is capped so `x inside {1, 1000000}` cannot turn compile into a
    million-entry linear scan. Above the cap the hull alone carries the
    constraint, and pure CDCL then enumerates the hull and punts -- `unknown`
    here is a pre-existing completeness limit, unchanged by this work (HEAD
    answers `unknown` too; the default engine escalates and answers `sat`).

    What must never happen is `unsat`, and any model that IS returned must be
    in the set. Those are the properties asserted.
    """
    for seed in range(1, 4):
        verdict, model = _run([
            "(set-logic QF_BV)",
            "(declare-fun x () (_ BitVec 32))",
            "(assert (or (= x (_ bv1 32)) (= x (_ bv1000000 32))))",
            "(check-sat)", "(get-model)"], seed=seed)
        assert verdict in ("sat", "unknown"), f"seed {seed}: {verdict}"
        if verdict == "sat":
            assert model["x"] in (1, 1000000), f"seed {seed}: x={model['x']}"
