"""Assertions made after the first check-sat must reach every engine.

Verilator 5.x randomize(): after the first (check-sat) it asserts up to four
random XOR-parity "hash" constraints, each followed by (check-sat), for
diversity. On the CDCL-compiled path those arrive as incremental aux problems.
When CDCL could not decide the extended problem, the fallback to bitblast only
saw the problem as compiled -- without the hash asserts -- so it refused and
answered `unknown` on EVERY randomize() (Verilator: "Internal: Solver error:
unknown"). The builder is now retained after the compile, so the bit-blaster
gets the full assertion set.

Also B37: an assertion made inside a push/pop scope with no check-sat before
the pop was still enforced after the pop (wrong `unsat`).
"""
from __future__ import annotations

import re
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = _REPO / "build" / "dv-solve-smt2"


def _run(script: str, *args: str) -> list[str]:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    p = subprocess.run([str(_EXE), "--interactive", *args], input=script,
                       capture_output=True, text=True, timeout=60)
    return p.stdout.splitlines()


# First randomize() of docs/examples/verilator/packet.sv under Verilator 5.046,
# as captured from the solver pipe (the hash constraint is one of Verilator's).
_BASE = """(set-option :produce-models true)
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun kind () (_ BitVec 4))
(declare-fun len () (_ BitVec 8))
(assert (= #b1 (__Vbv (= ((_ extract 1 0) addr) #b00))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000010)))))
(assert (= #b1 (__Vbv (bvult (bvadd addr ((_ zero_extend 24) len)) #x00001000))))
(assert (= #b1 (__Vbv (not (= ((_ zero_extend 28) kind) #x00000000)))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= kind #x7))) (__Vbool (__Vbv (bvugt ((_ zero_extend 24) len) #x00000008)))))))
"""
_HASH = ("(assert (= #b0 (bvxor ((_ extract 4 4) addr) ((_ extract 9 9) addr) "
         "((_ extract 11 11) addr) ((_ extract 15 15) addr) ((_ extract 16 16) addr) "
         "((_ extract 18 18) addr) ((_ extract 20 20) addr) ((_ extract 21 21) addr) "
         "((_ extract 22 22) addr) ((_ extract 23 23) addr) ((_ extract 24 24) addr) "
         "((_ extract 26 26) addr) ((_ extract 30 30) addr) ((_ extract 0 0) kind) "
         "((_ extract 1 1) kind) ((_ extract 2 2) kind) ((_ extract 0 0) len) "
         "((_ extract 1 1) len) ((_ extract 2 2) len) ((_ extract 3 3) len) "
         "((_ extract 4 4) len) ((_ extract 6 6) len))))\n")
_VAL = re.compile(r"\((\w+) (#b[01]+)\)")


@pytest.mark.parametrize("mode", [["--mode=verilator"], []], ids=["verilator", "default"])
def test_hash_assert_after_check_sat_is_decided(mode):
    out = _run(_BASE + "(check-sat)\n" + _HASH + "(check-sat)\n"
               "(get-value (addr kind len))\n", *mode)
    assert out[:2] == ["sat", "sat"], out
    model = dict(_VAL.findall(" ".join(out[2:])))
    assert set(model) == {"addr", "kind", "len"}, out
    # The model must satisfy EVERY assertion, the hash included: pin it and
    # ask z3 (an independent oracle).
    z3 = shutil.which("z3")
    if z3 is None:
        pytest.skip("z3 not on PATH to validate the model")
    pins = "".join(f"(assert (= {k} {v}))\n" for k, v in model.items())
    chk = subprocess.run([z3, "-in"], input=_BASE + _HASH + pins + "(check-sat)\n",
                         capture_output=True, text=True, timeout=30)
    assert chk.stdout.strip() == "sat", f"model violates an assertion: {model}"


def test_pop_discards_assertions_never_solved_in_scope():
    out = _run("(set-logic QF_BV)\n(declare-fun x () (_ BitVec 8))\n"
               "(assert (bvugt x #x10))\n(check-sat)\n"
               "(push 1)\n(assert (bvult x #x05))\n(pop 1)\n"
               "(check-sat)\n(get-value (x))\n")
    assert out[:2] == ["sat", "sat"], out
    assert int(out[2].split("#b")[1].rstrip(")"), 2) > 0x10


def test_scoped_assertions_still_apply_inside_their_scope():
    out = _run("(set-logic QF_BV)\n(declare-fun x () (_ BitVec 8))\n"
               "(assert (bvugt x #x10))\n(check-sat)\n"
               "(push 1)\n(assert (bvult x #x05))\n(check-sat)\n(pop 1)\n"
               "(assert (bvult x #x12))\n(check-sat)\n(get-value (x))\n")
    assert out[:3] == ["sat", "unsat", "sat"], out
    assert out[3] == "((x #b00010001))", out
