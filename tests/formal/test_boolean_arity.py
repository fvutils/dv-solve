"""Boolean connective arity: `(and x)` / `(or x)` / `(xor x)` are legal SMT-LIB.

B16 regression (docs/solver_bug_backlog.md).  The translator required arity >= 2
(`s->list.count < 3`), so the unary form tainted the assert and the solve came
back `unknown`.  That silently killed any single-variable model-enumeration
loop, because a blocking clause over one variable is exactly
`(not (and (= x V)))` -- the form Verilator PR#8042's UniGen2 sampler emits for
scalar `randomize()`.

Every case is cross-checked against z3 so the expected verdicts are not
hand-asserted.  Nullary `(and)` / `(or)` stay rejected: z3 rejects them too.

Usage:
    direnv exec . pytest tests/formal/test_boolean_arity.py -v
"""
from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_DV_SOLVE = _REPO_ROOT / "build" / "dv-solve-smt2"

_PRELUDE = "(declare-fun a () (_ BitVec 3))\n(assert (bvult a #b101))\n"

# (assert ...) bodies whose unary connective used to yield `unknown`.
_UNARY_CASES = [
    "(and (= a #b000))",
    "(or (= a #b000))",
    "(xor (= a #b000))",
    "(not (and (= a #b000)))",
    "(not (or (= a #b000)))",
    "(and (bvult a #b011))",
    "(or (bvugt a #b011))",
    # nested: unary inside a binary connective
    "(and (and (= a #b001)) (or (= a #b001)))",
    "(not (and (or (= a #b000))))",
]


def _script(body: str, logic: str) -> str:
    return f"(set-logic {logic})\n{_PRELUDE}(assert {body})\n(check-sat)\n(exit)\n"


def _verdict(out: str) -> str | None:
    for line in out.splitlines():
        if line.strip() in ("sat", "unsat", "unknown"):
            return line.strip()
    return None


def _dv(body: str, logic: str, engine: str | None = None) -> str | None:
    if not _DV_SOLVE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    args = [str(_DV_SOLVE), "--interactive"]
    if engine:
        args.append(f"--engine={engine}")
    p = subprocess.run(args, input=_script(body, logic),
                       capture_output=True, text=True, timeout=30)
    return _verdict(p.stdout)


def _z3(body: str, logic: str) -> str | None:
    if not shutil.which("z3"):
        pytest.skip("z3 not available for differential check")
    p = subprocess.run(["z3", "-in"], input=_script(body, logic),
                       capture_output=True, text=True, timeout=30)
    return _verdict(p.stdout)


@pytest.mark.parametrize("body", _UNARY_CASES)
@pytest.mark.parametrize("logic", ["QF_BV", "QF_ABV"])
def test_unary_connective_matches_z3(body: str, logic: str) -> None:
    """Unary and/or/xor must produce a real verdict, and the same one as z3."""
    got = _dv(body, logic)
    assert got != "unknown", f"{body} under {logic} regressed to unknown (B16)"
    assert got == _z3(body, logic), f"{body} under {logic}: dv={got}"


@pytest.mark.parametrize("engine", ["cdcl", "bitblast", "auto"])
def test_unary_connective_all_engines(engine: str) -> None:
    """The blocking-clause shape specifically, on every engine."""
    body = "(not (and (= a #b000)))"
    got = _dv(body, "QF_BV", engine)
    assert got == "sat", f"engine={engine}: {got}"


@pytest.mark.parametrize("body", ["(and)", "(or)"])
def test_nullary_connective_rejected(body: str) -> None:
    """Nullary is NOT valid SMT-LIB; z3 errors on it, so we must not answer sat."""
    assert _dv(body, "QF_BV") != "sat"


def test_single_var_blocking_clause_enumeration() -> None:
    """End-to-end: enumerate a 1-variable domain with `(not (and (= x V)))`.

    This is the UniGen2 BSAT loop for a scalar rand var.  Before B16 the first
    blocking clause returned `unknown` and enumeration stopped at one model.
    """
    if not _DV_SOLVE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    cmds = ["(set-logic QF_ABV)", "(declare-fun x () (_ BitVec 3))",
            "(assert (bvult x #b101))"]
    for v in range(5):
        cmds += ["(check-sat)", f"(assert (not (and (= x #b{v:03b}))))"]
    cmds += ["(check-sat)", "(exit)"]
    p = subprocess.run([str(_DV_SOLVE), "--interactive"],
                       input="\n".join(cmds) + "\n",
                       capture_output=True, text=True, timeout=30)
    res = [l.strip() for l in p.stdout.splitlines()
           if l.strip() in ("sat", "unsat", "unknown")]
    assert res == ["sat"] * 5 + ["unsat"], res
