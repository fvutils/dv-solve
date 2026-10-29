"""x urem (1 << s) bitblasts as x & ((1 << s) - 1).

The rewrite must keep SMT-LIB semantics when the shift reaches the width
(1 << s == 0, and x urem 0 == x), whether the divisor is written inline or
through a variable the bitblaster substitutes. For a pinned x and a free s,
r is satisfiable exactly for the values of x urem 2^s over every s.
DV_URW_POW2=0 turns the rewrite off.

    env -u LD_LIBRARY_PATH packages/python/bin/python3 -m pytest -q tests/formal/test_urem_pow2.py
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

_ROOT = Path(__file__).resolve().parents[2]
_BIN = _ROOT / "build" / "dv-solve-smt2"


def _expected(x: int, w: int, sw: int) -> set:
    out = set()
    for s in range(1 << sw):
        d = (1 << s) & ((1 << w) - 1)
        out.add(x % d if d else x)
    return out


def _script(x: int, w: int, sw: int, inline: bool) -> str:
    sh = f"(bvshl (_ bv1 {w}) ((_ zero_extend {w - sw}) s))"
    head = ["(set-option :produce-models true)", "(set-logic QF_BV)",
            f"(declare-fun x () (_ BitVec {w}))", f"(declare-fun s () (_ BitVec {sw}))",
            f"(declare-fun r () (_ BitVec {w}))", f"(declare-fun d () (_ BitVec {w}))"]
    if inline:
        head.append(f"(assert (= r (bvurem x {sh})))")
    else:
        head += [f"(assert (= d {sh}))", "(assert (= r (bvurem x d)))"]
    head.append(f"(assert (= x (_ bv{x} {w})))")
    body = []
    for rv in range(1 << w):
        body += ["(push 1)", f"(assert (= r (_ bv{rv} {w})))", "(check-sat)",
                 "(get-value (s))", "(pop 1)"]
    return "\n".join(head + body) + "\n"


def _run(tmp_path: Path, script: str, **envs) -> list:
    f = tmp_path / "u.smt2"
    f.write_text(script)
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(DV_ENGINE="bitblast", **envs)
    p = subprocess.run([str(_BIN), str(f)], capture_output=True, text=True,
                       env=env, timeout=120)
    return re.findall(r"^(sat|unsat|unknown)$|\(\(s #b([01]+)\)\)",
                      p.stdout, re.M)


def _answers(found: list) -> list:
    """[(verdict, s or None)] per check-sat."""
    out = []
    for verdict, sbits in found:
        if verdict:
            out.append([verdict, None])
        elif out:
            out[-1][1] = int(sbits, 2)
    return out


@pytest.fixture(scope="module", autouse=True)
def _built():
    if not _BIN.exists():
        pytest.skip(f"{_BIN} not built")


@pytest.mark.parametrize("inline", [True, False], ids=["inline", "subst"])
@pytest.mark.parametrize("x", [0, 5, 13, 15])
def test_urem_pow2_exact(tmp_path, inline, x):
    w, sw = 4, 3
    want = _expected(x, w, sw)
    ans = _answers(_run(tmp_path, _script(x, w, sw, inline)))
    assert len(ans) == 1 << w
    for rv, (verdict, s) in enumerate(ans):
        assert verdict == ("sat" if rv in want else "unsat"), (rv, verdict, want)
        if verdict == "sat":
            d = (1 << s) & ((1 << w) - 1)
            assert (x % d if d else x) == rv, (rv, s)


def test_urem_pow2_off_agrees(tmp_path):
    script = _script(13, 4, 3, False)
    assert _answers(_run(tmp_path, script)) is not None
    on = [v for v, _ in _answers(_run(tmp_path, script))]
    off = [v for v, _ in _answers(_run(tmp_path, script, DV_URW_POW2="0"))]
    assert on == off
