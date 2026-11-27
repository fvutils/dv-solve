"""`address % 16 == 0` with address's bits read elsewhere: no bounds crawl.

Reading address[0] and address[31] splits address into fields,
{a31, {a30_1, a0}}, which the search decides. The remainder was linked to
address's direct low slice only, so a0 stayed free; a decided a0 == 1 then
moved the low part's bounds 16 at a time between the remainder and the
inner concat, 2^27 rounds, until the trail cap gave the solve up:
"could not solve" (DVS_RANDOMIZE_ERROR) for about a third of the seeds.
A soft constraint keeps the problem on the CDCL engine, which bit-blast
otherwise took over from. Verilator's t_constraint_implication_set:

    (mode == 1) -> { if (txn_type inside {TXN_READ}) address % (1 << 4) == 0; }
    (mode == 2) -> { address[0] == 1'b0; address[31] == 1'b0; }
    (mode == 7) -> soft b == 4'hd;
    randomize() with { mode == 1; txn_type == TXN_READ; }

A low slice of a low slice is now linked too (prop_add_lowslice_mod_links),
and an even modulus 2^t * odd fixes the slice's low t bits (`% 12` against
a decided address[1]).
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import RANDOMIZE_OK, SolveHandle
from dv_solve.problem import BIN_EQ, BIN_MOD, BIN_OR, UN_NOT

_N = 100


def _handle(modulus, bits, soft=True):
    b = SolveProblemBuilder()
    b.add_var(0, width=4, is_signed=False, lo=0, hi=15)              # mode
    b.add_var(1, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)  # address
    b.add_var(2, width=4, is_signed=False, lo=0, hi=15)              # b
    mode, addr = b.expr_var(0), b.expr_var(1)

    def eq(x, y):
        return b.expr_binary(BIN_EQ, x, y)

    def implies(c, e):
        return b.expr_binary(BIN_OR, b.expr_unary(UN_NOT, c), e)

    b.add_constraint(eq(mode, b.expr_const(1, width=4)))
    for bit in bits:
        b.add_constraint(implies(eq(mode, b.expr_const(2, width=4)),
                                 eq(b.expr_extract(addr, bit, bit), b.expr_const(0, width=1))))
    rem = b.expr_binary(BIN_MOD, addr, b.expr_const(modulus, width=32))
    b.add_constraint(implies(eq(mode, b.expr_const(1, width=4)),
                             eq(rem, b.expr_const(0, width=32))))
    if soft:
        b.add_soft_constraint(implies(eq(mode, b.expr_const(7, width=4)),
                                      eq(b.expr_var(2), b.expr_const(13, width=4))), 0)
    return SolveHandle(b.finalize_bytes(), vars=[(4, False), (32, False), (4, False)])


@pytest.mark.parametrize("modulus", [16, 2, 1 << 20, 12])
@pytest.mark.parametrize("bits", [(31, 0), (0, 31), (30, 0), (31, 1), (31, 2), (5, 3)])
def test_mod_with_extracted_bits_solves(modulus, bits):
    h = _handle(modulus, bits)
    seen = set()
    for i in range(_N):
        rc, vals, _ = h.randomize(0x9E3779B97F4A7C15 * (i + 1) & (2**64 - 1))
        assert rc == RANDOMIZE_OK, f"seed {i}: rc={rc}"
        assert vals[0] == 1
        assert vals[1] % modulus == 0, vals
        seen.add(vals[1])
    assert len(seen) > _N // 2
