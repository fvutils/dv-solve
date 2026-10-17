"""The bitblast flip-check's memoized loop draws exactly what the plain loop draws.

`_diversify` (dvs_bbsolver.c) memoizes, per assignment of the candidate
inputs, whether the assignment satisfies every assertion, when the candidate
bits cover few distinct inputs but visit them many times (builder
intermediates alias the bits of the variables they are built from). The
accept/reject sequence must not change: the same seed gives the same model.
crc_filter in Verilator mode is that shape (8 inputs, ~2800 visits each) and
is routed to bitblast by the shape router. DV_DIVERSIFY_MEMO=0 runs the plain
loop for comparison.
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

_N = 60


def _transcript(b: Bench) -> str:
    log = []
    s = vlt_protocol.Session([str(_EXE), "--interactive", "--mode=verilator"],
                             b.widths, b.smt_vlt(), log=log)
    rng = random.Random(7)
    for _ in range(_N):
        s.randomize(rng)
    s.close()
    return "".join(log) + "(exit)\n"


def _replay(script: str, memo: bool) -> str:
    env = {**os.environ, "DV_DIVERSIFY_MEMO": "1" if memo else "0"}
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"], input=script,
                       capture_output=True, text=True, timeout=300, env=env)
    return r.stdout


def test_memo_loop_matches_plain_loop():
    b = Bench("crc_filter")
    script = _transcript(b)
    memo, plain = _replay(script, True), _replay(script, False)
    assert memo == plain
    # And the draws are solutions, several of them distinct.
    vals = [int(v, 2) for v in re.findall(r"\(m #b([01]+)\)", memo)]
    assert len(vals) >= _N
    assert all(all(ev(c, {"m": v}) for c in b.cons) for v in vals)
    assert len(set(vals)) > 1
