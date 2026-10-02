"""--mode=verilator sampling on constraints in Verilator's own encoding.

Verilator lifts every constraint to a bit (`(assert (= #b1 (__Vbv c)))`). A
Boolean if-then-else inside that wrapper, as `if (thold == 0) cnt == 0; else
...` produces, used to go uncompiled in the CDCL engine: every seeded solve
was downgraded to `unknown` by model validation, the instance was routed to
bit-blast re-diversification, and 300 randomize() calls returned 5 distinct
solutions. These pin the CDCL compile of a Boolean ite and the spread that
depends on it.
"""
from __future__ import annotations

import os
import re
import subprocess
from pathlib import Path

import pytest

from tests.formal.soundness.ir import ev
from tests.perf import vlt_protocol
from tests.perf.rand_benches import Bench

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")

_VAL = re.compile(r"\((\w+) #b([01]+)\)")


def _models(text: str, names: list) -> list:
    out, cur = [], {}
    for n, v in _VAL.findall(text):
        cur[n] = int(v, 2)
        if len(cur) == len(names):
            out.append(dict(cur))
            cur = {}
    return out


def test_wrapped_boolean_ite_solves_under_any_seed():
    """Pure CDCL, no escalation: sat with a valid model for every seed."""
    b = Bench("ot_aon_wkup")
    for seed in range(1, 21):
        script = (vlt_protocol.randomize_script(b.widths, b.smt_vlt())
                  + f"(set-option :seed {seed})\n(check-sat)\n(get-value (cnt thold))\n")
        r = subprocess.run([str(_EXE), "--interactive"], input=script, capture_output=True,
                           text=True, timeout=60,
                           env={**os.environ, "DV_ENGINE": "cdcl", "DV_NO_BITBLAST": "1",
                                "DV_FAIR_PICK": "1"})
        assert r.stdout.split()[0] == "sat", (seed, r.stdout)
        m = _models(r.stdout, b.names)[0]
        assert all(ev(c, m) for c in b.cons), (seed, m)


def test_verilator_mode_spreads_a_boolean_ite_problem():
    b = Bench("ot_aon_wkup")
    one = vlt_protocol.randomize_script(b.widths, b.smt_vlt()) + \
        "(check-sat)\n(get-value ( cnt thold))\n(reset)\n"
    script = "(set-logic QF_ABV)\n(check-sat)\n(reset)\n" + one * 300
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"], input=script,
                       capture_output=True, text=True, timeout=120)
    ms = _models(r.stdout, b.names)
    assert len(ms) == 300
    assert all(all(ev(c, m) for c in b.cons) for m in ms)
    # 1003 solutions, 300 draws: a uniform sampler sees about 260 distinct;
    # the bit-blast fallback saw 5.
    assert len({(m["cnt"], m["thold"]) for m in ms}) > 150
    assert max(m["thold"] for m in ms) - min(m["thold"] for m in ms) > 100
