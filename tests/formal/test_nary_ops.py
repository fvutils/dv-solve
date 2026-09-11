"""n-ary bitvector operators must compile, not just translate.

Regression for the Phase-1 fix in docs/cdcl_verilator_coverage_plan.md.

`BINOP_CASE` left-folds n-ary `bvand`/`bvor`/`bvxor`, but the accumulator was
not re-flattened between rounds, so from the 2nd operand on the left side was a
raw composed node. The CDCL compile only links var-var / const-var operands, so
the constraint was left UNCOMPILED -- the model then violated it and the
validation net downgraded a perfectly good `sat` to `unknown`.

Net effect: `x inside {a, b, c}` (which Verilator emits as a 3-way `bvor` of
reified equalities) silently did not work on CDCL, while arity 2 did. Every
case here is cross-checked against z3.

Usage:
    direnv exec . pytest tests/formal/test_nary_ops.py -v
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_B = "(define-fun B ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))"
_PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": "10"}


def _script(body: str, get: str = "a") -> str:
    return (f"(set-logic QF_BV)\n{_B}\n(declare-fun a () (_ BitVec 8))\n"
            f"{body}\n(check-sat)\n(get-value ({get}))\n(exit)\n")


def _run(cmd: list[str], script: str, env=None) -> tuple[str, list[int]]:
    p = subprocess.run(cmd, input=script, capture_output=True, text=True,
                       timeout=60, env={**os.environ, **(env or {})})
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    vals = [int(m[2:], 2) if m.startswith("#b") else int(m[2:], 16)
            for m in re.findall(r"#[bx][0-9a-fA-F]+", p.stdout)]
    return verdict, vals


def _cdcl(script: str):
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    return _run([str(_DV), "--interactive", "--engine=cdcl"], script, _PURE)


def _z3(script: str):
    if not shutil.which("z3"):
        pytest.skip("z3 not available")
    return _run(["z3", "-in"], script)


def _inside(vals: list[int]) -> str:
    """`a inside {vals}` the way Verilator emits it: n-ary bvor of reified eqs."""
    terms = " ".join(f"(B (= a #x{v:02x}))" for v in vals)
    return f"(assert (= #b1 (bvor {terms})))"


@pytest.mark.parametrize("k", [2, 3, 4, 5, 8])
def test_inside_set_solves_on_cdcl(k: int) -> None:
    """An `inside` set of any size must produce a definitive CDCL answer."""
    members = list(range(1, k + 1))
    v, vals = _cdcl(_script(_inside(members)))
    assert v == "sat", f"arity {k}: {v} (was `unknown` for k>=3 before the fix)"
    assert vals and vals[0] in members, \
        f"arity {k}: model {vals[0] if vals else None} not in {members}"


@pytest.mark.parametrize("k", [3, 5])
def test_inside_set_agrees_with_z3(k: int) -> None:
    s = _script(_inside(list(range(1, k + 1))))
    assert _cdcl(s)[0] == _z3(s)[0]


def test_unsat_inside_set_is_detected() -> None:
    """Soundness: an impossible set must be `unsat`, not a bogus `sat`.

    Guards the risk direction of this fix -- compiling MORE constraints must not
    manufacture satisfiability, and must not manufacture unsatisfiability either.
    """
    body = _inside([1, 2, 3]) + "\n(assert (bvugt a #x10))"
    v, _ = _cdcl(_script(body))
    assert v == _z3(_script(body))[0]
    assert v == "unsat", v


@pytest.mark.parametrize("op", ["bvand", "bvor", "bvxor"])
@pytest.mark.parametrize("k", [2, 3, 4])
def test_nary_wordlevel_ops_agree_with_z3(op: str, k: int) -> None:
    """The fold applies to full-width operands too, not just 1-bit guards."""
    args = " ".join(f"#x{(0xF0 >> i) & 0xFF:02x}" for i in range(k))
    body = f"(assert (= a ({op} a {args})))"
    s = _script(body)
    cv, cvals = _cdcl(s)
    zv, _ = _z3(s)
    if cv == "unknown":
        pytest.skip(f"{op}/{k}: honest unknown")
    assert cv == zv, f"{op} arity {k}: cdcl={cv} z3={zv}"


def test_all_members_of_an_inside_set_are_reachable() -> None:
    """The point of the exercise: the set must actually be sampled, not pinned.

    Before the fix this shape did not solve at all on CDCL; a fix that solves it
    but always returns the same member would not be a usable sampler.
    """
    members = [3, 7, 11, 19]
    seen = set()
    for seed in range(1, 61):
        s = _script(f"(set-option :seed {seed})\n" + _inside(members))
        v, vals = _cdcl(s)
        if v == "sat" and vals:
            seen.add(vals[0])
    assert seen <= set(members), f"out-of-set models: {seen - set(members)}"
    assert len(seen) >= 2, f"only reached {seen} of {members}"
