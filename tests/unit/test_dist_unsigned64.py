"""`dist` on an unsigned 64-bit var, with entries above INT64_MAX.

Bounds are int64 bit patterns; an unsigned 64-bit var's upper-half values
are negative as int64. The propagators order them per the var's signedness
(var_b_lt), but the dist value picker and the bit-blaster's dist encoding
clipped entries with a signed compare, so an upper-half entry never
overlapped the var's domain: the picker never drew it, and BV-SAT dropped it
from the dist's domain restriction.
"""
from __future__ import annotations

from dv_solve.builder import SolveProblemBuilder
from dv_solve.bvsat import BVSatCtx, BVSAT_SAT
from dv_solve.ctx import SolveCtx, SOLVE_OK

U64 = (1 << 64) - 1
TOP = 1 << 63


def _pat(v):
    """The int64 bit pattern of an unsigned 64-bit value."""
    return v - (1 << 64) if v >= TOP else v


def _entry(lo, hi, w, pv=True):
    return {"lo": _pat(lo), "hi": _pat(hi), "weight": w, "is_per_value": pv}


def _problem(entries):
    b = SolveProblemBuilder()
    b.add_var(0, 64, False, 0, _pat(U64))      # the full unsigned range
    b.add_dist(0, entries)
    buf, _ = b.finalize()
    return b, buf


def _draws(entries, n):
    b, buf = _problem(entries)
    out = []
    try:
        with SolveCtx(buf) as ctx:
            for seed in range(1, n + 1):
                ctx.reset()
                assert ctx.solve(seed=seed) == SOLVE_OK
                out.append(ctx.get_value(0) & U64)
    finally:
        b.destroy()
    return out


def test_picker_draws_upper_half_entry():
    vals = _draws([_entry(TOP, TOP + 9, 1), _entry(0, 9, 3)], 800)
    assert all(v <= 9 or TOP <= v <= TOP + 9 for v in vals), vals[:10]
    hi = sum(v >= TOP for v in vals) / len(vals)
    assert 0.18 < hi < 0.32, hi                    # weight 1 of 4


def test_picker_entry_straddling_int64_max():
    """[2^63-5, 2^63+4] is ordered unsigned; as int64 its lo > hi."""
    vals = _draws([_entry(TOP - 5, TOP + 4, 1)], 400)
    assert all(TOP - 5 <= v <= TOP + 4 for v in vals), vals[:10]
    assert any(v < TOP for v in vals) and any(v >= TOP for v in vals)


def test_bvsat_keeps_upper_half_entry():
    """The dist is BV-SAT's only constraint on the var, so the value must
    come from the upper-half entry."""
    for seed in range(8):
        b, buf = _problem([_entry(TOP + 100, TOP + 109, 1)])
        bb = BVSatCtx(buf)
        try:
            assert bb.check(seed=seed) == BVSAT_SAT
            v = bb.value(0) & U64
            assert TOP + 100 <= v <= TOP + 109, "seed %d: got %#x" % (seed, v)
        finally:
            bb.destroy()
            b.destroy()
