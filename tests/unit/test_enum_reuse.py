"""The enumeration tier draws from a kept solution list when constraints were only added.

`_enum_sample` (dvs_search.c) lists every solution of a small problem and
draws one. An assert after a check-sat (Verilator's XOR-hash asserts, up to
four per randomize) changes the root; when the root can only have lost
solutions against the list's (`_enum_covers`), the solve draws rows from the
list until one still holds, instead of listing again. A pop removes
constraints, so a list made inside the popped scope must not be drawn from.
DVS_ENUM_REUSE=0 lists again on every new root, for comparison.
"""
from __future__ import annotations

import os
import random
import re
import subprocess
from pathlib import Path

import pytest

pytest.importorskip("resource")

from tests.formal.soundness.ir import ev
from tests.perf import vlt_protocol
from tests.perf.rand_benches import Bench

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")

_OT = """(set-option :produce-models true)
(set-logic QF_BV)
(declare-fun cnt () (_ BitVec 8))
(declare-fun thold () (_ BitVec 8))
(assert (or (= thold #x00) (and (bvuge thold #x05) (bvule thold #xab))))
(assert (ite (= thold #x00) (= cnt #x00) (and (bvuge cnt (bvsub thold #x05)) (bvule cnt thold))))
"""


def _run(script: str, reuse: bool) -> tuple:
    env = {**os.environ, "DVS_ENUM_REUSE": "1" if reuse else "0", "DVS_ENUM_DEBUG": "1"}
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"], input=script,
                       capture_output=True, text=True, timeout=300, env=env)
    return r.stdout, r.stderr.count("[enum]")


def _responses(out: str) -> list:
    """One entry per command answer; a multi-line get-value is joined."""
    resp, cur = [], ""
    for line in out.splitlines():
        if not line.strip():
            continue
        if cur or line.startswith("(("):
            cur += line
            if cur.count("(") == cur.count(")"):
                resp.append(cur)
                cur = ""
        else:
            resp.append(line)
    return resp


def _check_models(b: Bench, script: str, out: str) -> list:
    """Every model against the bench and the hash asserts in force; the answers."""
    resp = iter(_responses(out))
    hashes, answers, last = [], [], None
    for cmd in re.findall(r"\((?:check-sat|get-value|reset|assert)[^\n]*", script):
        if cmd.startswith("(reset"):
            hashes = []
        elif cmd.startswith("(assert"):
            m = re.match(r"\(assert \(= #b([01]) \(bvxor (.*)\)\)\)\s*$", cmd)
            if m:
                bits = re.findall(r"\(\(_ extract (\d+) \d+\) (\w+)\)", m[2])
                hashes.append((int(m[1]), [(int(i), v) for i, v in bits]))
        elif cmd.startswith("(check-sat"):
            last = next(resp)
            answers.append(last)
        elif cmd.startswith("(get-value"):
            line = next(resp)
            if last != "sat":
                continue
            vals = {k: int(v, 2) for k, v in re.findall(r"\((\w+) #b([01]+)\)", line)}
            assert all(ev(c, vals) for c in b.cons), line
            for want, bits in hashes:
                assert sum((vals[v] >> i) & 1 for i, v in bits) % 2 == want, line
    return answers


def test_hash_asserts_draw_from_kept_list():
    b = Bench("ot_aon_wkup")
    log = []
    s = vlt_protocol.Session([str(_EXE), "--interactive", "--mode=verilator"],
                             b.widths, b.smt_vlt(), log=log)
    rng = random.Random(3)
    for _ in range(40):
        s.randomize(rng)
    s.close()
    script = "".join(log) + "(exit)\n"
    (kept, walks_kept), (relisted, walks_relisted) = _run(script, True), _run(script, False)
    assert _check_models(b, script, kept) == _check_models(b, script, relisted)
    # One walk per randomize (the base problem), not one per check-sat.
    assert walks_kept <= 41 < walks_relisted


def test_pop_does_not_draw_from_scoped_list():
    # The first list is made under `cnt != thold`, which narrows no bound.
    # After the pop, solutions with cnt == thold are back (about 1 in 6).
    script = _OT + "(push 1)\n(assert (distinct cnt thold))\n(check-sat)\n(get-value (cnt thold))\n(pop 1)\n"
    for i in range(48):
        script += f"(assert (bvuge (bvadd cnt thold) #x{i % 4:02x}))\n(check-sat)\n(get-value (cnt thold))\n"
    out, _ = _run(script, True)
    vals = re.findall(r"\(cnt #b([01]+)\)\s*\(thold #b([01]+)\)", out)
    assert len(vals) == 49
    assert vals[0][0] != vals[0][1]
    assert any(c == t for c, t in vals[1:])
