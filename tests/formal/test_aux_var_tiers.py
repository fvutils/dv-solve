"""Auxiliary variables must be created at a tier that can hold their value.

The CDCL compile materialises a lot of anonymous helper variables: a
comparison's constant, a bitwise op's constant operand, an ITE's result, an
extract's result. Several of those call sites used `_init_tier0` with a
hard-coded or defaulted width. That is wrong twice over:

  * tier-0 stores bounds as `int32`, so any value outside that range becomes a
    DIFFERENT NUMBER (2147483648 reads back as -2147483648); and
  * `VAR_IS_TIER0` is the flag propagators read to decide they may run their
    32-bit fire function, so a mis-tiered variable also gets the wrong
    propagator.

The result in every case was a **wrong `unsat`, reachable in the default
engine** — the one failure mode with no downstream net, since the
model-validation pass only guards `sat`. B24, B25 and B27; all pre-existing,
all found by the fuzzer once `fuzz_bv_smt2.py` learned to emit Verilator's
reified `(= #b1 (__Vbv ...))` form.

Every case here is a TAUTOLOGY or a trivially satisfiable constraint, checked
in both the pure-CDCL and default engines. Nothing is sampled: the whole point
is that the compile must not decide `unsat` on its own, and that is a
yes/no property of each shape.

Usage:
    direnv exec . pytest tests/formal/test_aux_var_tiers.py -v
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": "20"}
_DEFAULT = {"DV_CDCL_TIME_LIMIT": "20"}
_PREAMBLE = ("(set-logic QF_BV)\n"
             "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n")


@pytest.fixture(autouse=True)
def _need_binary():
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")


def _solve(body: str, pure: bool) -> str:
    src = _PREAMBLE + body + "\n(check-sat)\n"
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=src, capture_output=True, text=True, timeout=60,
                       env={**os.environ, **(_PURE if pure else _DEFAULT)})
    return next((l.strip() for l in p.stdout.splitlines()
                 if l.strip() in ("sat", "unsat", "unknown")), "none")


def _assert_never_unsat(body: str, why: str) -> None:
    """`unknown` is an honest punt; `unsat` on a satisfiable input is not."""
    for pure in (True, False):
        got = _solve(body, pure)
        eng = "pure-CDCL" if pure else "default"
        assert got != "unsat", f"{why} [{eng}]: wrong unsat"
        assert got in ("sat", "unknown"), f"{why} [{eng}]: {got}"


# ------------------------------------------------- B24: reified compare consts

_B24 = [
    # (label, body) -- every one is a TRUE comparison between two constants
    ("2^31 exactly",
     "(assert (= #b1 (__Vbv (bvuge (_ bv2147483648 64) (_ bv1 64)))))"),
    ("2^32",
     "(assert (= #b1 (__Vbv (bvuge (_ bv4294967296 64) (_ bv1 64)))))"),
    ("3e9 vs 2e9",
     "(assert (= #b1 (__Vbv (bvuge (_ bv3000000000 64) (_ bv2000000000 64)))))"),
    ("the seed that found it (96-bit)",
     "(assert (= #b1 (__Vbv (bvuge (_ bv9051007824559996559 96)"
     " (_ bv8326720516999599348 96)))))"),
    ("mirror arm: bvule with the big constant on the right",
     "(assert (= #b1 (__Vbv (bvule (_ bv1 64) (_ bv2147483648 64)))))"),
    ("width 32, constant above INT32_MAX",
     "(assert (= #b1 (__Vbv (bvuge (_ bv2147483648 32) (_ bv1 32)))))"),
]


@pytest.mark.parametrize("label,body", _B24, ids=[c[0] for c in _B24])
def test_b24_reified_constant_comparison(label, body) -> None:
    """A reified comparison whose constant exceeds int32 must not go `unsat`."""
    _assert_never_unsat(body, f"B24 {label}")


@pytest.mark.parametrize("value", [2147483647, 2147483648, 4294967295,
                                   4294967296, 2**40, 2**47])
def test_b24_boundary_sweep(value) -> None:
    """Walk across the 2^31 and 2^32 boundaries; both sides must behave."""
    _assert_never_unsat(
        f"(assert (= #b1 (__Vbv (bvuge (_ bv{value} 64) (_ bv0 64)))))",
        f"B24 sweep {value}")


def test_b24_false_comparison_is_still_unsat() -> None:
    """The mirror of the above: a genuinely false constant compare IS unsat.

    Without this the fix could 'pass' by never concluding `unsat` at all.
    """
    for pure in (True, False):
        got = _solve(
            "(assert (= #b1 (__Vbv (bvuge (_ bv1 64) (_ bv2147483648 64)))))",
            pure)
        assert got == "unsat", f"expected unsat, got {got}"


# --------------------------------------------- B25: wide constant op operands

@pytest.mark.parametrize("width", [33, 40, 48, 63])
def test_b25_bitwise_constant_wider_than_32(width) -> None:
    """`(bvand v <all-ones>) > 2^(w-1)` is satisfiable for every width.

    The mask was built as a width-32 tier-0 variable, so the bounds engine saw
    it as 2^32-1 and could 'prove' the result too small.

    Stops at 63: at width 64 the mask is 2^64-1, whose stored bound is the
    pattern -1, and the bitwise propagators hit the 2^63 cliff. That is the
    B28/B23 family (audit-mode only -- the default engine routes >= 64-bit
    arithmetic to bitblast and answers correctly), pinned separately below.
    """
    allones = (1 << width) - 1
    half = 1 << (width - 1)
    _assert_never_unsat(
        f"(declare-const v (_ BitVec {width}))\n"
        f"(assert (bvugt (bvand v (_ bv{allones} {width}))"
        f" (_ bv{half} {width})))",
        f"B25 bvand w{width}")


def test_b25_original_fuzz_repro() -> None:
    _assert_never_unsat(
        "(declare-const v (_ BitVec 48))\n"
        "(assert (bvugt (bvand (_ bv259120173051071 48) v)"
        " (_ bv175941109208861 48)))",
        "B25 fuzz seed 3499")


# ------------------------------------------------------ B27: modular bvadd

@pytest.mark.parametrize("width", [4, 8, 16, 24, 31, 32, 33, 40, 48, 63])
def test_b27_bvadd_wraps(width) -> None:
    """`v + (2^w - 1) == 0` has the solution v == 1, by wrapping.

    Plain integer arithmetic has no solution, so a non-modular add propagator
    reports `unsat`. Widths 32..63 took a compile route that never got the
    wrap-aware propagators.
    """
    allones = (1 << width) - 1
    _assert_never_unsat(
        f"(declare-const v (_ BitVec {width}))\n"
        f"(assert (= (bvadd v (_ bv{allones} {width})) (_ bv0 {width})))",
        f"B27 wrap-add w{width}")


def test_b27_original_fuzz_repro() -> None:
    _assert_never_unsat(
        "(declare-const v (_ BitVec 32))\n"
        "(assert (= #b1 (bvand #b1 (__Vbv (not (bvult (_ bv1764318416 32)"
        " (bvadd v (_ bv2356268516 32))))))))",
        "B27 fuzz seed 2077")


@pytest.mark.parametrize("width", [8, 32, 48])
def test_b27_bvsub_wraps(width) -> None:
    """The same routing covers bvsub: `0 - 1 == 2^w - 1`."""
    allones = (1 << width) - 1
    _assert_never_unsat(
        f"(declare-const v (_ BitVec {width}))\n"
        f"(assert (= v (_ bv0 {width})))\n"
        f"(assert (= (bvsub v (_ bv1 {width})) (_ bv{allones} {width})))",
        f"B27 wrap-sub w{width}")


# ------------------------------------------------------------- still open

@pytest.mark.xfail(strict=True, reason="B28: the wrap-aware bv* propagators "
                                       "cover widths 1..63; width 64 falls "
                                       "through to non-modular bounds_add_64")
def test_b28_bvadd_wraps_at_width_64() -> None:
    got = _solve("(declare-const v (_ BitVec 64))\n"
                 "(assert (= (bvadd v (_ bv18446744073709551615 64))"
                 " (_ bv0 64)))", pure=True)
    assert got != "unsat", "B28: wrong unsat on a wrapping 64-bit add"


@pytest.mark.xfail(strict=True, reason="B28 family: at width 64 the bitwise "
                                       "propagators read the all-ones mask as "
                                       "the pattern -1 (the 2^63 cliff)")
def test_b28_bitwise_all_ones_mask_at_width_64() -> None:
    got = _solve("(declare-const v (_ BitVec 64))\n"
                 "(assert (bvugt (bvand v (_ bv18446744073709551615 64))"
                 " (_ bv9223372036854775808 64)))", pure=True)
    assert got != "unsat", "wrong unsat on a 64-bit all-ones mask"


# ---------------------------------------------- B26: width-64 literal ordering
#
# FIXED 2026-08-18. Root cause is NOT the empty explanation this bug was
# originally filed under: it is `literal_is_true`/`literal_is_false` (and the
# clause propagator's `already_tight` check) comparing a literal's bound to the
# variable's bounds with bare SIGNED int64 operators. On an unsigned width-64
# variable the top of the domain is the bit pattern -1, so the just-learnt
# clause `v >= 1` reads as FALSE against the full domain [0, 2^64-1] — a
# falsified clause at decision level 0, i.e. UNSAT. Now ordered with
# var_b_lt/var_b_gt, which switch to unsigned ordering for that case.

@pytest.mark.parametrize("op", ["bvuge", "bvugt", "="])
def test_b26_self_neq_disjunct(op) -> None:
    """A self-comparison disjunct forces a learnt clause on a full-width-64
    domain, which is what tripped the signed literal compare."""
    _assert_never_unsat(f"(declare-const v (_ BitVec 64))\n"
                        f"(assert (or ({op} v (_ bv3 64)) (distinct v v)))",
                        f"B26 {op}")


def test_b26_model_is_correct() -> None:
    """Not just `sat`: the model must land in the satisfying window. Guards
    against 'fixing' the compare by loosening it into an unsound direction."""
    src = (_PREAMBLE +
           "(declare-const v (_ BitVec 64))\n"
           "(assert (or (bvuge v (_ bv3 64)) (distinct v v)))\n"
           "(assert (bvule v (_ bv5 64)))\n"
           "(check-sat)\n(get-value (v))\n")
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=src, capture_output=True, text=True, timeout=60,
                       env={**os.environ, **_PURE})
    out = p.stdout
    assert "\nunsat" not in "\n" + out, f"B26: wrong unsat\n{out}"
    if "sat" not in out.split():
        pytest.skip("engine punted to unknown")
    bits = next((l.split("#b")[1].rstrip(")\n ")
                 for l in out.splitlines() if "#b" in l), None)
    assert bits is not None, f"no model reported\n{out}"
    assert 3 <= int(bits, 2) <= 5, f"model {int(bits, 2)} outside [3,5]"


@pytest.mark.parametrize("lo,hi", [("3", "1"), ("18446744073709551615", "0")])
def test_b26_mirror_still_unsat(lo, hi) -> None:
    """The fix must not buy `sat` by losing the ability to conclude `unsat` on
    a width-64 domain."""
    got = _solve(f"(declare-const v (_ BitVec 64))\n"
                 f"(assert (bvuge v (_ bv{lo} 64)))\n"
                 f"(assert (bvule v (_ bv{hi} 64)))", pure=True)
    assert got == "unsat", f"genuinely unsat width-64 range answered {got}"
