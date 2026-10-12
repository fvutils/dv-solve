"""B119: after the CDCL compile the SMT-LIB front end keeps its builder
("retention") so a bit-blast escalation sees every assertion, until the
builder passes a cap (16 MB; DV_RETAIN_MAX_BYTES lowers it here). Two
wrong `unsat`s on that path, both from an assertion the CDCL context refused
(a variable wider than 64 bits):

- the refused hand-off still ended retention and reset the builder, so the
  assertions were in neither the context nor the builder bit-blast solves;
- the refused add had set up some of its variables before failing, and a
  later add's constraints over the refused variables read zeroed slots
  (domain {0}).

Found by test_bb_session_fuzz.py at small caps (seeds 1486, 1714).
"""
import os
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
EXE = Path(os.environ.get("DV_SOLVE_SMT2", REPO / "build" / "dv-solve-smt2"))

CASES = {
    "refused_handoff_kept": (
        "(push 2)\n"
        "(declare-fun p5 () (_ BitVec 65))\n"
        "(declare-fun p4 () (_ BitVec 33))\n"
        "(declare-fun p2 () Bool)\n"
        "(assert (= p2 (bvule (bvsub p4 p4) (concat (_ bv728054 20) ((_ extract 17 5) p4)))))\n"
        "(declare-fun p3 () Bool)\n"
        "(assert (= p3 (bvult (_ bv748631766005250384 65) p5)))\n"
        "(declare-fun p1 () (_ BitVec 8))\n"
        "(assert (= p1 ((_ zero_extend 7) (bvmul ((_ extract 7 7) p5) (_ bv1 1)))))\n"
        "(push 2)\n"
        "(check-sat-assuming (p2))\n", {}),
    "refused_vars_not_zeroed": (
        "(declare-fun p4 () (_ BitVec 8))\n"
        "(declare-fun p1 () (_ BitVec 8))\n"
        "(push 1)\n"
        "(declare-fun p6 () Bool)\n"
        "(declare-fun p3 () (_ BitVec 65))\n"
        "(assert (= p3 (_ bv33369860171418398001 65)))\n"
        "(declare-fun p5 () (_ BitVec 8))\n"
        "(push 1)\n"
        "(assert (= p5 (_ bv66 8)))\n"
        "(declare-fun p0 () (_ BitVec 33))\n"
        "(assert (= p5 ((_ zero_extend 1) (ite p6 (bvneg (_ bv113 7)) ((_ extract 29 23) p0)))))\n"
        "(declare-fun p2 () (_ BitVec 33))\n"
        "(push 1)\n"
        "(check-sat-assuming ((not p6)))\n", {"DV_ENGINE": "bitblast", "DV_SIM": "0"}),
}


@pytest.mark.skipif(not EXE.exists(), reason="dv-solve-smt2 not built")
@pytest.mark.parametrize("cap", ["1024", "4096", ""])
@pytest.mark.parametrize("name", sorted(CASES))
def test_retention_end(name, cap):
    script, extra = CASES[name]
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update(extra)
    env["DV_CHECK_MODEL"] = "0"
    if cap:
        env["DV_RETAIN_MAX_BYTES"] = cap
    r = subprocess.run([str(EXE), "--interactive"], input=script, text=True,
                       capture_output=True, env=env, timeout=60)
    answers = [w for w in r.stdout.split() if w in ("sat", "unsat", "unknown")]
    assert answers and answers[-1] in ("sat", "unknown"), r.stdout + r.stderr
