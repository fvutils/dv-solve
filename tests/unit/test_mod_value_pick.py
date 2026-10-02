"""A seeded solve draws a dividend with a remainder its `% c` still allows.

`(sz == 4096) -> (slba % 8 == 0)` fixes the remainder only once `sz` is
decided, and the modulo propagator then aligns only `slba`'s bounds. A
uniform draw between them met the rule one time in 8, and the conflicts the
other seven caused reshaped the distribution of the decisions made before
them: here, `sz` came out 4096 in 600 of 1000 solves, not about half.

The picker now keeps a draw's quotient and takes a remainder the modulo's
result still allows. While the guard is undecided the result spans every
remainder, so nothing is moved, and a 512-byte `slba` stays aligned one time
in 8.
"""
from __future__ import annotations

from collections import Counter

from dv_solve.ctx import SOLVE_OK, SolveCtx
from dv_solve.problem import (
    BIN_ADD, BIN_DIV, BIN_EQ, BIN_LT, BIN_LTE, BIN_MOD, BIN_MUL, BIN_NEQ, BIN_OR,
    SolveProblem,
)

SZ, NLB, NB, SLBA, FO, NP = range(6)


def _descriptor() -> SolveProblem:
    """An IO descriptor: sector size, length, byte count, start LBA, page
    offset and page count, with the rules that tie them."""
    sp = SolveProblem()
    sp.add_var(SZ, 16, False, 0, 0xFFFF)
    sp.add_var(NLB, 32, False, 0, 0xFFFFFFFF)
    sp.add_var(NB, 32, False, 0, 0xFFFFFFFF)
    sp.add_var(SLBA, 64, False, 0, (1 << 63) - 1)
    sp.add_var(FO, 16, False, 0, 0xFFFF)
    sp.add_var(NP, 16, False, 0, 0xFFFF)
    V, C, b = sp.expr_var, sp.expr_const, sp.expr_binary
    for e in (
        b(BIN_OR, b(BIN_EQ, V(SZ), C(512)), b(BIN_EQ, V(SZ), C(4096))),
        sp.expr_in_range(V(NLB), C(1), C(256)),
        b(BIN_EQ, V(NB), b(BIN_MUL, V(NLB), V(SZ))),
        b(BIN_LTE, V(NB), C(0x40000)),
        b(BIN_LTE, b(BIN_ADD, V(SLBA), V(NLB)), C(0x100000)),
        b(BIN_OR, b(BIN_NEQ, V(SZ), C(4096)),
          b(BIN_EQ, b(BIN_MOD, V(SLBA), C(8)), C(0))),
        b(BIN_EQ, b(BIN_MOD, V(FO), C(4)), C(0)),
        b(BIN_LT, V(FO), C(4096)),
        b(BIN_EQ, V(NP), b(BIN_DIV, b(BIN_ADD, b(BIN_ADD, V(FO), V(NB)), C(4095)),
                           C(4096))),
    ):
        sp.add_constraint(e)
    return sp


def _sample(n: int = 1000):
    """*n* seeded solves of one context, as a checkpoint/solve/restore user
    makes them: a short learning search first, then the full budget."""
    ctx = SolveCtx(_descriptor())
    out = []
    try:
        for s in range(1, n + 1):
            seed = s * 2654435761 % (1 << 32)
            cp = ctx.checkpoint()
            rc = ctx.solve(seed=seed, max_restarts=5, use_lcg=True)
            if rc != SOLVE_OK:
                rc = ctx.solve(seed=seed, max_restarts=10000)
            assert rc == SOLVE_OK, rc
            out.append([ctx.get_value(i) for i in range(6)])
            ctx.restore(cp)
    finally:
        ctx.destroy()
    return out


def test_a_guarded_modulo_does_not_skew_its_guard():
    got = _sample()
    for sz, nlb, nb, slba, fo, np_ in got:
        assert sz in (512, 4096) and 1 <= nlb <= 256 and nb == nlb * sz <= 0x40000
        assert slba + nlb <= 0x100000 and (sz != 4096 or slba % 8 == 0)
        assert fo % 4 == 0 and fo < 4096 and np_ == (fo + nb + 4095) // 4096
    sizes = Counter(v[SZ] for v in got)
    # Before: 600 of 1000.
    assert 450 <= sizes[4096] <= 550, sizes
    # Unguarded draws are not moved: a 512-byte slba is aligned 1 time in 8.
    small = [v[SLBA] for v in got if v[SZ] == 512]
    aligned = sum(1 for x in small if x % 8 == 0) / len(small)
    assert 0.08 <= aligned <= 0.18, aligned
    # The aligned values still spread over the range.
    big = [v[SLBA] for v in got if v[SZ] == 4096]
    assert 0.4 <= sum(1 for x in big if x >= 0x80000) / len(big) <= 0.6
