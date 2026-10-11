"""Named regression targets for docs/cdcl_construct_plan.md.

Each target is a fixture CDCL could not answer on its own when the plan was
written (2026-10-10). It runs under pure CDCL (DV_NO_BITBLAST=1) and must give
the known verdict within a time budget. A target is `xfail` until the phase
that should close it lands; remove the mark then (strict=True makes an
unexpected pass fail the run, so a target that starts passing is noticed).

    direnv exec . pytest tests/formal/test_cdcl_targets.py -v
"""
from __future__ import annotations

import os
import re
import subprocess
import time
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = Path(os.environ.get("DV_SMT2_EXE", _REPO / "build" / "dv-solve-smt2"))
_ENV = {**{k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"},
        "DV_NO_BITBLAST": "1"}
_F = _REPO / "tests" / "formal" / "smt2"


def _phase(n: int, why: str):
    return pytest.mark.xfail(reason=f"plan phase {n}: {why}", strict=True)


# (id, fixture, expected verdict, budget seconds, marks)
TARGETS = [
    ("qc_satadd_w4", _F / "fw_hdl/gen/quickcheck-trivial/qc_satadd_w4_signed1_s1.smt2",
     "unsat", 0.5, [_phase(3, "80% of conflicts bail out of analysis")]),
    ("t_constraint_operators", _F / "verilator/t_constraint_operators.smt2",
     "sat", 2.0, [_phase(5, "64-bit concat (5.2); the divides were phase 1")]),
    ("cdcl64_xor_highbit", _F / "gaps/cdcl64_xor_highbit_sat.smt2",
     "sat", 2.0, [_phase(5, "64-bit unsigned xor")]),
    ("r_ca9fa37135", _REPO / "tests/formal/soundness/regressions/r_ca9fa37135.smt2",
     "unsat", 2.0, []),
    ("inv0", _REPO / "tests/formal/soundness/lcg_stress/inv0.smt2", "sat", 2.0, []),
    ("gray_w8_decodeloop", _F / "fw_hdl/gen/affine-proof/gray_w8_decodeloop.smt2",
     "unsat", 2.0, []),
    ("crc_loop_w16", _F / "fw_hdl/gen/affine-proof/crc_loop_w16_bytes1_k2.smt2",
     "unsat", 2.0, [_phase(4, "GF(2) rows")]),
    ("uart_base", _REPO / "tests/perf/svcorpus/captures/uart_base/base.smt2",
     "sat", 2.0, [_phase(5, "64-bit unsigned divide")]),
]

# The masked-field probes from the analysis (a coverage agent's finding).
PROBES = [
    ("mask_and_bound", """(declare-const z (_ BitVec 32))
(assert (= (bvand z #x00ff0000) #x00120000))
(assert (bvugt z #x00ffffff))""", "sat", 0.02, [_phase(2, "known-bits domain")]),
    ("mask_or", """(declare-const z (_ BitVec 32))
(assert (= (bvor z #x0000000f) #x1234567f))""", "sat", 0.02, [_phase(2, "known-bits domain")]),
]


def _run(script: str, budget: float):
    t = time.monotonic()
    try:
        r = subprocess.run([str(_DV)], input=script, capture_output=True, text=True,
                           timeout=max(5.0, budget * 4), env=_ENV)
    except subprocess.TimeoutExpired:
        return "timeout", budget * 4
    words = [w for w in r.stdout.split() if w in ("sat", "unsat", "unknown")]
    return (words[0] if words else "error"), time.monotonic() - t


@pytest.fixture(autouse=True)
def _need_binary():
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 not built")


@pytest.mark.parametrize("path,want,budget", [
    pytest.param(p, w, b, id=i, marks=m) for i, p, w, b, m in TARGETS])
def test_target(path, want, budget):
    got, dt = _run(path.read_text(), budget)
    assert got == want, f"pure CDCL: {got} (want {want})"
    assert dt <= budget, f"{dt:.2f} s > {budget} s"


@pytest.mark.parametrize("body,want,budget", [
    pytest.param(b, w, t, id=i, marks=m) for i, b, w, t, m in PROBES])
def test_probe(body, want, budget):
    # Time the solve, not process start: subtract an empty problem's run.
    _, base = _run("(set-logic QF_BV)\n(check-sat)\n", 1.0)
    got, dt = _run(f"(set-logic QF_BV)\n{body}\n(check-sat)\n", budget)
    assert got == want, f"pure CDCL: {got} (want {want})"
    assert dt - base <= budget, f"{(dt - base) * 1e3:.1f} ms > {budget * 1e3:.0f} ms"


@pytest.mark.xfail(reason="plan phase 2: the CDCL probe declines this union", strict=True)
def test_randomize_union_stays_on_cdcl(tmp_path):
    """Verilator mode: t_randomize_union's only constraint is a tautology over
    a 12-bit slice, yet the CDCL probe runs out of its 50 ms and every later
    randomize() of the class goes to bit-blasting."""
    src = (_F / "verilator/t_randomize_union.smt2").read_text().rstrip()
    log = tmp_path / "route.log"
    cmds = []
    for _ in range(5):
        cmds += ["(reset)", src]
    cmds.append("(exit)")
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env["DV_LOG"] = str(log)
    subprocess.run([str(_DV), "--interactive", "--mode=verilator"],
                   input="\n".join(cmds) + "\n", capture_output=True, text=True,
                   timeout=60, env=env)
    routes = re.findall(r"vlt-route: (\w+) \(([\w-]+)\)", log.read_text())
    assert routes and all(eng == "cdcl" for eng, _ in routes), routes
