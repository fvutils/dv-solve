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


# ---- --verilator-hash=ignore ------------------------------------------------

_PRE = ("(set-logic QF_ABV)(declare-fun x () (_ BitVec 8))"
        "(assert (= #b1 (ite (bvult x #x10) #b1 #b0)))(check-sat)(get-value (x))")
_HASH1 = "(assert (= #b1 (bvxor ((_ extract 0 0) x) ((_ extract 1 1) x))))(check-sat)(get-value (x))"
_HASH0 = "(assert (= #b0 (bvxor ((_ extract 0 0) x) ((_ extract 1 1) x))))(check-sat)"


def _run(script: str, *opts) -> list:
    r = subprocess.run([str(_EXE), "--interactive", *opts], input=script, capture_output=True,
                       text=True, timeout=60)
    return r.stdout.split()


def test_hash_ignore_keeps_the_model():
    out = _run(_PRE + _HASH1 + _HASH0, "--mode=verilator", "--verilator-hash=ignore")
    assert out.count("sat") == 3 and "unsat" not in out
    vals = [w for w in out if w.startswith("((x")] + [w for w in out if w.startswith("#b")]
    assert len(set(vals)) <= 2      # one model, printed twice


def test_hash_honor_is_the_default():
    for opts in (("--mode=verilator",), ("--mode=verilator", "--verilator-hash=honor")):
        out = _run(_PRE + _HASH1 + _HASH0, *opts)
        assert out[-1] == "unsat"   # the two parity asserts contradict


def test_hash_ignore_enforces_any_other_assert():
    # After skipped hashes a real constraint is solved for, and a constraint
    # that merely looks bitwise but is not Verilator's shape is not skipped.
    out = _run(_PRE + _HASH1 + "(assert (= x #x07))(check-sat)(get-value (x))"
               + "(assert (= #b1 (bvxor ((_ extract 0 0) x) #b1)))(check-sat)",
               "--mode=verilator", "--verilator-hash=ignore")
    assert out[-1] == "unsat"       # x == 7 has bit 0 set: (1 ^ 1) == 1 is false
    assert "((x" in " ".join(out) and "#b00000111))" in out


def test_hash_option_needs_verilator_mode():
    r = subprocess.run([str(_EXE), "--verilator-hash=ignore"], input="", capture_output=True,
                       text=True, timeout=60)
    assert r.returncode == 2


# ---- a large array under the Verilator protocol ------------------------------

_ARR = ("(declare-fun data () (Array (_ BitVec 32) (_ BitVec 8)))"
        "(declare-fun idx () (_ BitVec 2))(declare-fun sel () (_ BitVec 8))"
        "(assert (= sel (select data ((_ zero_extend 30) idx))))"
        + "".join(f"(assert (and (bvuge (select data #x0000000{k}) #x0a)"
                  f" (bvule (select data #x0000000{k}) #x32)))" for k in range(4)))
_ARR_GET = "(get-value (idx sel " + " ".join(
    f"(select data #x0000000{k})" for k in range(4)) + "))"
_ARR_VAL = re.compile(r"\(\(?(idx|sel|select data #x0000000(\d))\)? #b([01]+)\)")


def test_verilator_mode_randomizes_a_large_array():
    """A 32-bit-address array with a symbolic index takes the word-level array
    engine. Each randomize must be a valid model, and they must vary -- not one
    model repeated, nor only the free index varying."""
    one = _ARR + "(check-sat)" + _ARR_GET + "(reset)"
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"],
                       input="(set-logic QF_ABV)(check-sat)(reset)" + one * 100,
                       capture_output=True, text=True, timeout=120)
    ms = []
    for block in r.stdout.split("((idx")[1:]:
        m = {}
        for name, k, v in _ARR_VAL.findall("((idx" + block):
            m[f"d{k}" if k else name] = int(v, 2)
        ms.append(m)
    assert len(ms) == 100, r.stdout[-400:]
    for m in ms:
        assert all(10 <= m[f"d{k}"] <= 50 for k in range(4)), m
        assert m["sel"] == m[f"d{m['idx']}"], m
    assert len({tuple(sorted(m.items())) for m in ms}) > 40
    assert len({m["d0"] for m in ms}) > 5
    assert len({m["idx"] for m in ms}) == 4
