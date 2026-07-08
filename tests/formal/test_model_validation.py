"""Value-satisfaction cross-check (plan §4.3).

test_cross_check.py compares only the sat/unsat *verdict*. This test goes one
step further on the `sat` cases: it takes the concrete model dv-solve returns,
pins every variable to its assigned value, and asks z3 whether that assignment
satisfies the original constraints. A disagreement means dv-solve returned a
*silently-wrong model* — it said `sat` but produced an assignment that does not
actually satisfy the problem. Verdict comparison alone cannot catch that.

Scope: scalar-variable fixtures (declared `(_ BitVec N)`). Fixtures that declare
array-sorted variables are skipped — an array model can't be pinned with a
single equality, and dv-solve punts to `unknown` on the nested-array shapes
anyway (see the verilator/t_constraint_struct_complex gap).
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

from .harness.dv_solve_smt2_solver import DvSolveSMT2Solver
from .harness.z3_solver import Z3Solver

_HERE = Path(__file__).resolve().parent
_DIRS = [
    _HERE / "smt2" / "verilator",
    _HERE / "smt2" / "tier2",
    _HERE / "smt2" / "tier3",
]
_TIMEOUT = 10.0

_DECL_SCALAR = re.compile(r"^\(declare-fun\s+(\S+)\s+\(\)\s+\(_\s+BitVec\s+\d+\)\s*\)")
_DECL_ANY = re.compile(r"^\(declare-fun\s+(\S+)\s+\(\)")
# One model binding: (name #bXXXX) or (name #xXXXX) or (name (_ bvV W)).
# The name class excludes parens so the model's outer "((name ..." doesn't get
# captured as "(name".
_BIND = re.compile(r"\(\s*([^\s()]+)\s+(#b[01]+|#x[0-9a-fA-F]+|\(_\s+bv\d+\s+\d+\))\s*\)")


def _fixtures():
    files = []
    for d in _DIRS:
        if d.is_dir():
            files.extend(sorted(d.glob("*.smt2")))
    return files


_FILES = _fixtures()
_DV = DvSolveSMT2Solver()
_Z3 = Z3Solver()


def _scalar_and_array_vars(text: str):
    scalar, has_array = [], False
    for line in text.splitlines():
        m = _DECL_SCALAR.match(line.strip())
        if m:
            scalar.append(m.group(1))
            continue
        if _DECL_ANY.match(line.strip()):
            has_array = True   # a declare-fun that isn't a scalar BitVec
    return scalar, has_array


def _dv_model(fixture: Path, scalar_vars):
    """Run dv-solve with (get-value ...) appended; return (verdict, {var: bits})."""
    body = fixture.read_text()
    gv = body + "\n(get-value (%s))\n" % " ".join(scalar_vars)
    exe = _HERE.parents[1] / "build" / "dv-solve-smt2"
    proc = subprocess.run([str(exe)], input=gv, capture_output=True, text=True,
                          timeout=_TIMEOUT)
    out = proc.stdout
    verdict = out.strip().split("\n", 1)[0].strip() if out.strip() else "error"
    model = {v: b for v, b in _BIND.findall(out)}
    return verdict, model


@pytest.fixture(scope="module")
def z3_solver():
    if not _Z3.is_available():
        pytest.skip("z3 not on PATH")
    return _Z3


@pytest.mark.skipif(not _FILES, reason="no fixtures")
@pytest.mark.parametrize("fixture", _FILES,
                         ids=[f"{f.parent.name}/{f.stem}" for f in _FILES])
def test_model_satisfies(fixture, z3_solver):
    if not (_HERE.parents[1] / "build" / "dv-solve-smt2").exists():
        pytest.skip("dv-solve-smt2 not built")

    body = fixture.read_text()
    scalar, has_array = _scalar_and_array_vars(body)
    if has_array:
        pytest.skip("array-sorted variables cannot be pinned")
    if not scalar:
        pytest.skip("no scalar variables to validate")

    verdict, model = _dv_model(fixture, scalar)
    if verdict != "sat":
        pytest.skip(f"dv-solve non-sat verdict: {verdict}")
    # Every scalar var should appear in the model.
    missing = [v for v in scalar if v not in model]
    if missing:
        pytest.skip(f"dv-solve model missing vars: {missing}")

    # Build: original problem (minus its check-sat) + pin each var + check-sat.
    pins = "\n".join(f"(assert (= {v} {model[v]}))" for v in scalar)
    lines = [ln for ln in body.splitlines() if ln.strip() != "(check-sat)"]
    augmented = "\n".join(lines) + "\n" + pins + "\n(check-sat)\n"

    res = _Z3.solve_text(augmented, timeout_s=_TIMEOUT) if hasattr(_Z3, "solve_text") else None
    if res is None:
        # Fall back to a direct z3 subprocess if the harness lacks solve_text.
        z3_bin = _HERE.parents[1] / "packages" / "python" / "bin" / "z3"
        proc = subprocess.run([str(z3_bin), "-smt2", "-in"], input=augmented,
                              capture_output=True, text=True, timeout=_TIMEOUT)
        verdict_z3 = proc.stdout.strip().split("\n", 1)[0].strip()
    else:
        verdict_z3 = res.result

    assert verdict_z3 == "sat", (
        "dv-solve returned a model that z3 rejects (silently-wrong model) for "
        f"{fixture.name}: z3={verdict_z3}\nmodel={model}"
    )
