""">64-bit (tier-2) variable support: constraint solving and full-width model
read-back on the bitblast engine.

The CDCL engine's get_value() returns only the low 64 bits (var_lo64), so exact
read-back of a wide value must go through the bitblast engine's value_wide(),
which returns the full bit pattern (little-endian limbs, assembled to a Python
int by the wrapper). Bitwise/concat nodes are native to bitblast, so wide values
are naturally built by concatenating narrower fields.

Two builder-API rules for the bitblast engine, learned the hard way and pinned
here so future tests don't relearn them:

  * DECLARE A WIDE VAR'S FULL RANGE WITH hi = 2**63 - 1 (INT64_MAX). For width
    >= 64, assert_var_bounds treats the natural upper bound as INT64_MAX (the
    int64 lo/hi simply cannot express 2**width - 1), so hi = INT64_MAX asserts
    no cap. Any smaller hi is enforced as a REAL upper-bound constraint and will
    make a larger wide value UNSAT.
  * A var's declared [lo,hi] is only enforced when the var is REFERENCED by some
    constraint. A free (unreferenced) var ignores its declared bounds and gets a
    seeded-random fill over its full width -- deliberate bitblast behavior
    (unreferenced == unconstrained). In the Verilator/SMT2 path vars are declared
    full-width and constrained by explicit assertions, so this never bites there.

Maps to Verilator tests: t_constraint_assoc_arr_wide, t_randomize_queue_wide,
t_randomize_unpacked_wide (wide element read-back).
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ, BIN_LTE, BIN_GTE
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT

_FULL = (1 << 63) - 1   # INT64_MAX: "full range" sentinel for width >= 64


def _bb_wide(build, width, is_signed=False, seed=1):
    b = SolveProblemBuilder()
    build(b)
    buf, _sz = b.finalize()
    ctx = BVSatCtx(buf)
    rc = ctx.check(seed=seed)
    val = ctx.value_wide(0, width, is_signed) if rc == BVSAT_SAT else None
    ctx.destroy()
    return (rc == BVSAT_SAT), val


# ------------------------------------------------------------------ #
# Wide value built by concatenation                                  #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("xw,ew,a,b", [
    (96, 48, 0x1234_5678_9ABC, 0x0FED_CBA9_8765),
    (128, 64, 0xDEAD_BEEF_CAFE_0001, 0x0123_4567_89AB_CDEF),
    (72, 36, 0xABCDEF01, 0x123456789),
    (65, 33, 0x1_2345_6789 & ((1 << 33) - 1), 0x1_ABCD & ((1 << 33) - 1)),
])
def test_wide_concat_readback(xw, ew, a, b):
    """x == {b, a}: a genuinely >64-bit model read back exactly via value_wide."""
    def build(bb):
        bb.add_var(0, xw, False, 0, _FULL)          # wide, full range
        bb.add_var(1, ew, False, a, a)              # low field pinned
        bb.add_var(2, ew, False, b, b)              # high field pinned
        bb.add_constraint(
            bb.expr_binary(BIN_EQ, bb.expr_var(0),
                           bb.expr_concat(bb.expr_var(2), bb.expr_var(1), ew)))

    sat, x = _bb_wide(build, xw)
    assert sat, "wide concat should be SAT"
    assert x == (b << ew) | a, "x=%#x expected %#x" % (x, (b << ew) | a)


# ------------------------------------------------------------------ #
# Wide equality / range via explicit constraints                     #
# ------------------------------------------------------------------ #

def test_wide_eq_small_const():
    """A 96-bit var pinned to a small value via explicit EQ reads back exactly."""
    def build(bb):
        bb.add_var(0, 96, False, 0, _FULL)
        bb.add_constraint(bb.expr_binary(BIN_EQ, bb.expr_var(0), bb.expr_const(5)))

    sat, x = _bb_wide(build, 96)
    assert sat and x == 5


def test_wide_range_low_limb():
    """A 96-bit var constrained to [900, 1000] via explicit LTE/GTE."""
    def build(bb):
        bb.add_var(0, 96, False, 0, _FULL)
        bb.add_constraint(bb.expr_binary(BIN_GTE, bb.expr_var(0), bb.expr_const(900)))
        bb.add_constraint(bb.expr_binary(BIN_LTE, bb.expr_var(0), bb.expr_const(1000)))

    sat, x = _bb_wide(build, 96)
    assert sat and 900 <= x <= 1000


def test_wide_high_bit_set():
    """Force a bit above 64 to be set (via the high field) and read it back."""
    ew = 40
    def build(bb):
        bb.add_var(0, 80, False, 0, _FULL)
        bb.add_var(1, ew, False, 0, 0)              # low = 0
        bb.add_var(2, ew, False, 1 << 39, 1 << 39)  # high MSB set -> bit 79 of x
        bb.add_constraint(
            bb.expr_binary(BIN_EQ, bb.expr_var(0),
                           bb.expr_concat(bb.expr_var(2), bb.expr_var(1), ew)))

    sat, x = _bb_wide(build, 80)
    assert sat
    assert x == (1 << 79), "x=%#x expected bit 79 set" % x
    assert x >> 64, "value must exceed 64 bits"


# ------------------------------------------------------------------ #
# Signed wide read-back                                              #
# ------------------------------------------------------------------ #

def test_wide_signed_negative_readback():
    """A 72-bit signed var pinned to -1 reads back as -1 via value_wide(signed).

    The signed full range [-2**71, 2**71-1] cannot be expressed in the int64
    lo/hi, so declare with the INT64_MIN/MAX sentinels (which assert_var_bounds
    reads as 'no cap' for width>=64) and pin the value with an explicit EQ."""
    def build(bb):
        bb.add_var(0, 72, True, -(1 << 63), (1 << 63) - 1)   # INT64_MIN/MAX sentinels
        bb.add_constraint(
            bb.expr_binary(BIN_EQ, bb.expr_var(0), bb.expr_const(-1, is_signed=True)))

    sat, x = _bb_wide(build, 72, is_signed=True)
    assert sat and x == -1, "expected -1, got %s" % (x,)
