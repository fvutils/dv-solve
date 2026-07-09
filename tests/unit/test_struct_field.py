"""Unit tests for packed struct / union field constraints exercised by
Verilator (t_constraint_struct*, t_randomize_struct_sel, t_randomize_union).

A packed struct is a single bit-vector whose fields are bit-slices. The two
native IR forms (see zsp_compile.c) are:

  * `packed == concat(hi, lo)`  -- relate a packed var to its field vars
    (bidirectional: pin fields -> packed, or pin packed -> fields). This is the
    sound modeling used here for constraining struct fields.
  * `field == extract(packed, hi, lo)` -- read a slice OUT of a determined
    value. Correct in the forward direction (packed pinned -> field). The
    backward direction (field pinned, packed free) is BUG-2 (spurious UNSAT);
    see the xfail tracker at the end and docs/verilator_coverage_test_plan.md §8.

The equality's other side must be a plain VAR for native compilation --
`extract(x,..) == const` does NOT compile (CompileIncompleteError); introduce a
field var and pin it instead.
"""
from __future__ import annotations

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import BIN_EQ
from dv_solve.ctx import (
    SolveCtx, SOLVE_OK, SOLVE_UNSAT, CompileUnsatError,
)


def _solve(build, seed=1):
    b = SolveProblemBuilder()
    vids = build(b)
    buf, _sz = b.finalize()
    try:
        try:
            ctx = SolveCtx(buf)
        except CompileUnsatError:
            return SOLVE_UNSAT, {}
        rc = ctx.solve(seed=seed)
        vals = {v: ctx.get_value(v) for v in vids} if rc == SOLVE_OK else {}
        ctx.destroy()
        return rc, vals
    finally:
        b.destroy()


# packed 16-bit x = {a[15:8], b[7:0]}; vars: 0=x, 1=a, 2=b
def _packed_concat(b, a_lo, a_hi, b_lo, b_hi):
    b.add_var(0, 16, False, 0, 0xFFFF)
    b.add_var(1, 8, False, a_lo, a_hi)
    b.add_var(2, 8, False, b_lo, b_hi)
    b.add_constraint(
        b.expr_binary(BIN_EQ, b.expr_var(0),
                      b.expr_concat(b.expr_var(1), b.expr_var(2), 8))
    )


# ------------------------------------------------------------------ #
# Struct fields via concat (bidirectional)                           #
# ------------------------------------------------------------------ #

def test_struct_fields_pinned_compose():
    """Pin both fields -> the packed word is their concatenation."""
    def build(b):
        _packed_concat(b, 0xAB, 0xAB, 0xCD, 0xCD)
        return [0, 1, 2]

    rc, v = _solve(build)
    assert rc == SOLVE_OK
    assert v[0] == 0xABCD, "packed=%#x, expected 0xABCD" % v[0]


def test_struct_word_pinned_decompose():
    """Pin the packed word -> both fields follow."""
    def build(b):
        b.add_var(0, 16, False, 0xABCD, 0xABCD)
        b.add_var(1, 8, False, 0, 255)
        b.add_var(2, 8, False, 0, 255)
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(0),
                          b.expr_concat(b.expr_var(1), b.expr_var(2), 8))
        )
        return [1, 2]

    rc, v = _solve(build)
    assert rc == SOLVE_OK
    assert v[1] == 0xAB and v[2] == 0xCD, "fields=%#x/%#x" % (v[1], v[2])


def test_struct_neighbor_field_independent():
    """Constrain one field; the neighbor stays free but valid."""
    seen_b = set()
    for seed in range(1, 12):
        def build(b):
            # a pinned; b free over [0,255]
            _packed_concat(b, 0xAB, 0xAB, 0, 255)
            return [0, 1, 2]

        rc, v = _solve(build, seed=seed)
        assert rc == SOLVE_OK
        assert v[1] == 0xAB
        assert (v[0] >> 8) & 0xFF == 0xAB      # high byte is the pinned field
        assert v[0] & 0xFF == v[2]             # low byte tracks the free field
        seen_b.add(v[2])
    assert len(seen_b) > 1, "neighbor field never varied: %s" % sorted(seen_b)


# ------------------------------------------------------------------ #
# Field read via forward extract (packed pinned -> field)            #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("hi_bit,lo_bit,expected", [
    (15, 8, 0xAB),
    (7, 0, 0xCD),
    (11, 4, 0xBC),   # a straddling nibble slice of 0xABCD
])
def test_field_read_forward_extract(hi_bit, lo_bit, expected):
    """field == extract(packed, hi, lo) with packed pinned reads the slice."""
    def build(b):
        b.add_var(0, 16, False, 0xABCD, 0xABCD)          # packed pinned
        b.add_var(1, 8, False, 0, 255)                    # field
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(1),
                          b.expr_extract(b.expr_var(0), hi_bit, lo_bit))
        )
        return [1]

    rc, v = _solve(build)
    assert rc == SOLVE_OK
    assert v[1] == expected, "slice [%d:%d] = %#x, expected %#x" % (
        hi_bit, lo_bit, v[1], expected)


# ------------------------------------------------------------------ #
# Union: overlapping views of the same storage                       #
# ------------------------------------------------------------------ #

def test_union_byte_and_word_views_alias():
    """A 16-bit union with a word view and a two-byte view: constraining the
    bytes is visible through the word (they share storage via concat)."""
    def build(b):
        # storage word = {byte_hi, byte_lo}; word-view var equals storage.
        b.add_var(0, 16, False, 0, 0xFFFF)   # storage / word view
        b.add_var(1, 8, False, 0x12, 0x12)   # byte_hi pinned
        b.add_var(2, 8, False, 0x34, 0x34)   # byte_lo pinned
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(0),
                          b.expr_concat(b.expr_var(1), b.expr_var(2), 8))
        )
        return [0]

    rc, v = _solve(build)
    assert rc == SOLVE_OK
    assert v[0] == 0x1234, "word view %#x should alias byte views" % v[0]


# ------------------------------------------------------------------ #
# BUG-2 regression: backward extract (field pinned, packed free).     #
# The bit-slice propagator is now bounds-consistent (back-propagates a #
# fixed slice into the parent's bounds), so a satisfiable field-pinned #
# problem no longer returns spurious UNSAT. See §8 / _bit_slice_backward#
# and the exhaustive validator in test_bitslice_backward.py.          #
# ------------------------------------------------------------------ #

@pytest.mark.parametrize("hi,lo,fv", [
    (15, 8, 0xAB),   # top slice
    (7, 0, 0xCD),    # low slice
    (11, 4, 0xBC),   # middle slice
    (15, 8, 0x00),   # top slice, small value (was SAT even before the fix)
    (7, 0, 0xFF),    # low slice, all-ones
])
def test_backward_extract(hi, lo, fv):
    """field == extract(packed, hi, lo) with the field pinned and packed free:
    must be SAT with a model whose slice matches (was BUG-2 spurious UNSAT)."""
    def build(b):
        b.add_var(0, 16, False, 0, 0xFFFF)   # packed free
        b.add_var(1, 8, False, fv, fv)       # field pinned
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(1),
                          b.expr_extract(b.expr_var(0), hi, lo))
        )
        return [0]

    rc, v = _solve(build)
    assert rc == SOLVE_OK, "field slice [%d:%d]==%#x should be SAT" % (hi, lo, fv)
    assert (v[0] >> lo) & ((1 << (hi - lo + 1)) - 1) == fv, (
        "packed %#x slice [%d:%d] != %#x" % (v[0], hi, lo, fv))


def test_backward_extract_two_fields():
    """Both fields pinned via backward extract -> the packed word is determined."""
    def build(b):
        b.add_var(0, 16, False, 0, 0xFFFF)   # packed free
        b.add_var(1, 8, False, 0xAB, 0xAB)
        b.add_var(2, 8, False, 0xCD, 0xCD)
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(1),
                          b.expr_extract(b.expr_var(0), 15, 8)))
        b.add_constraint(
            b.expr_binary(BIN_EQ, b.expr_var(2),
                          b.expr_extract(b.expr_var(0), 7, 0)))
        return [0]

    rc, v = _solve(build)
    assert rc == SOLVE_OK and v[0] == 0xABCD, "packed=%#x" % v.get(0, -1)
