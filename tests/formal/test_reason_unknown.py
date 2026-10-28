"""(get-info :reason-unknown) names the cause of every `unknown`
(docs/fw_hdl_implementation_plan.md piece 6.3).

Each case forces one path to `unknown` and checks the reason: an input error,
a construct dv-solve does not implement (named), an engine that stopped
without a verdict (the bit-blast SAT budget, CDCL's search budget, a CDCL
model that failed validation with no bit-blast to fall back on). None may fall back to the bare
`incomplete`. The validation path has no case: since the CDCL compile covers
`bvnot`, no known input reaches it.
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="build/dv-solve-smt2 not built")

# 64-bit factoring: no engine decides it within a one-conflict budget.
_HARD = ("(declare-fun x () (_ BitVec 64))(declare-fun y () (_ BitVec 64))"
         "(assert (= (bvmul x y) #x6b8b4567327b23c7))"
         "(assert (bvugt x #x0000000000000001))(assert (bvugt y #x0000000000000001))"
         "(assert (bvult x y))(assert (bvult x #x00000000ffffffff))"
         "(assert (bvult y #x00000000ffffffff))")
# The same product without the 32-bit bounds: CDCL's word-level search stops
# on a five-conflict budget (with the bounds it now finds the factors).
_HARD_CDCL = ("(declare-fun x () (_ BitVec 64))(declare-fun y () (_ BitVec 64))"
              "(assert (= (bvmul x y) #x6b8b4567327b23c7))"
              "(assert (bvugt x #x0000000000000001))(assert (bvugt y #x0000000000000001))")


def _reason(body: str, env: dict | None = None) -> list[str]:
    script = ("(set-option :produce-models true)(set-logic QF_BV)" + body
              + "(check-sat)(get-info :reason-unknown)\n")
    e = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    e.update(env or {})
    p = subprocess.run([str(_EXE), "--interactive"], input=script, capture_output=True,
                       text=True, timeout=60, env=e)
    return [l for l in p.stdout.splitlines() if not l.startswith("(error")]


@pytest.mark.parametrize("body, env, want", [
    ("(declare-fun a () (_ BitVec 4))(assert (= a (bvfoo a)))", {},
     '"unsupported: operation \'bvfoo\'"'),
    ("(declare-fun a () (_ BitVec 4))(assert (= a ((_ foo 2) a)))", {},
     '"unsupported: indexed operator \'foo\'"'),
    ("(declare-fun a () (Array (_ BitVec 4) (Array (_ BitVec 4) (_ BitVec 8))))"
     "(declare-fun i () (_ BitVec 4))(assert (= (select (select a i) #x1) #x01))", {},
     '"unsupported: symbolic index into a nested array"'),
    ("(declare-fun a () (_ BitVec 4))(assert (= a #x1))(assert (bvult a)))", {},
     '"input error: '),
    (_HARD, {"DV_ENGINE": "bitblast", "DV_BB_MAX_CONFLICTS": "1"},
     '"incomplete: the SAT search stopped without a verdict (conflict limit)"'),
    (_HARD_CDCL, {"DV_NO_BITBLAST": "1", "DV_MAX_CONFLICTS": "5", "DV_MAX_RESTARTS": "1"},
     '"incomplete: CDCL search stopped: '),
])
def test_reason_unknown(body, env, want):
    out = _reason(body, env)
    assert out[0] == "unknown", out
    assert out[1].startswith("(:reason-unknown " + want), out


def test_reason_is_per_check():
    """A reason belongs to the check that answered unknown: a later check
    that answers does not report it."""
    out = _reason(_HARD + "(check-sat)(get-info :reason-unknown)(assert false)",
                  {"DV_ENGINE": "bitblast", "DV_BB_MAX_CONFLICTS": "1"})
    assert out[:2] == ["unknown", '(:reason-unknown "incomplete: the SAT search stopped '
                       'without a verdict (conflict limit)")'], out
    assert out[2:] == ["unsat", "(:reason-unknown incomplete)"], out
