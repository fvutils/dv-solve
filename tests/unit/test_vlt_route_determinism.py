"""--mode=verilator picks an engine per check-sat without consulting the clock.

The engine decides how a model is sampled (CDCL near-uniform over solutions,
bitblast by flipping one model), so a route that followed wall-clock time made
a seeded run unrepeatable: on a loaded machine one slow check-sat locked a
shape to bitblast, and the same script gave other models
(docs/deterministic_routing_plan.md). Routing now weighs deterministic work
counts. These replay Verilator-shaped scripts once as fast as they run and once
with every check-sat stalled (DV_VLT_STALL_US, as a loaded machine stalls it,
far past the old 3 ms "slow shape" mark), and require the same routes and the
same answers.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest

pytest.importorskip("resource")

from tests.perf.route_calib import _HANDSHAKE, _bench, _divisibility, _illegal_instr

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")

STALL_US = 20000


def _run(script: str, tmp_path: Path, stall: bool):
    log = tmp_path / ("stall.log" if stall else "fast.log")
    env = {"DV_LOG": str(log), "PATH": "/usr/bin:/bin"}
    if stall:
        env["DV_VLT_STALL_US"] = str(STALL_US)
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"],
                       input=script + "(exit)\n", capture_output=True, text=True,
                       timeout=600, env=env)
    lines = [l.split() for l in log.read_text(encoding="utf-8").splitlines()
             if l.startswith("vlt-time")]
    return r.stdout, [l[2] for l in lines], [int(l[1]) for l in lines]


@pytest.mark.parametrize("name", ["illegal_instr", "divisibility", "ot_aon_wkup",
                                  "crc_filter", "linear_sys"])
def test_route_and_models_ignore_the_clock(tmp_path, name):
    if name == "illegal_instr":
        script = _HANDSHAKE + _illegal_instr(6)
    elif name == "divisibility":
        script = _HANDSHAKE + _divisibility(6)
    else:
        script = _bench(name, str(_EXE), 8)
    out_fast, routes_fast, _ = _run(script, tmp_path, stall=False)
    out_slow, routes_slow, us_slow = _run(script, tmp_path, stall=True)
    # The stall is real: every check-sat took far longer than the 3 ms that
    # used to mark a shape slow.
    assert min(us_slow) >= STALL_US, us_slow
    assert routes_slow == routes_fast
    assert out_slow == out_fast


def test_slow_shape_still_moves_to_bitblast(tmp_path):
    # The calibration target the old timing rule was written for: riscv-dv's
    # riscv_illegal_instr costs CDCL several times what bitblast does, so after
    # two CDCL solves and two bitblast trials the shape stays on bitblast.
    _, routes, _ = _run(_HANDSHAKE + _illegal_instr(6), tmp_path, stall=False)
    assert all(re.match(r"cdcl/", r) for r in routes[1:3]), routes
    assert routes[3:] == ["bitblast/cdcl-slow"] * 4, routes
