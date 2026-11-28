"""Variables over 64 bits through a problem handle (DVS_REF_WORDS).

A dvs_ref_t of layout DVS_REF_WORDS holds a value as ceil(width / 32)
32-bit words, least significant first: Verilator's VlWide (word 0 is bits
31:0). A handle whose problem declares a variable over 64 bits solves it on
bit-blast, the STATE values pinned by equalities, and reads and writes the
wide values through such refs. Narrow variables may use the layout too.
"""
from __future__ import annotations

import ctypes

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.handle import (REF_SCALAR, REF_WORDS, RANDOMIZE_ERROR, RANDOMIZE_OK,
                             RANDOMIZE_UNSAT, SolveHandle, _Ref)
from dv_solve.problem import (BIN_ADD, BIN_EQ, BIN_LT, BIN_NEQ, BIN_RSHIFT, VAR_RANDC,
                              VAR_STATE)

_FULL = (1 << 63) - 1   # the declared range of a variable of 64 bits or more


def _mask(w):
    return (1 << w) - 1


def test_wide_rand_below_wide_state():
    """rand bit [127:0] x; bit [99:0] s; constraint { x < s; }: s is read
    through words, x written through words, for several values of s."""
    b = SolveProblemBuilder()
    b.add_var(0, 128, False, 0, _FULL)
    b.add_var(1, 100, False, 0, _FULL)
    b.set_var_kind(1, VAR_STATE)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0),
                                   b.expr_extend(b.expr_var(1), 100, 128)))
    h = SolveHandle(b.finalize_bytes(), vars=[(128, False), (100, False)])
    for i, s in enumerate([(1 << 99) + 12345, 1 << 70, 3, (1 << 99) + 12345]):
        for k in range(5):
            rc, v, _ = h.randomize(100 * i + k + 1, state={1: s})
            assert rc == RANDOMIZE_OK
            assert v[0] < s
            assert v[1] == s     # a STATE ref is not written


def test_wide_state_pin_exact():
    """x == s + 1 over 96 bits: the pin is the whole value, not its low 64."""
    b = SolveProblemBuilder()
    b.add_var(0, 96, False, 0, _FULL)
    b.add_var(1, 96, False, 0, _FULL)
    b.set_var_kind(1, VAR_STATE)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                                   b.expr_binary(BIN_ADD, b.expr_var(1),
                                                 b.expr_const_wide(1, 96))))
    h = SolveHandle(b.finalize_bytes(), vars=[(96, False), (96, False)])
    for seed, s in enumerate([0xFFFF_FFFF_FFFF_FFFF, 0xABCD_0123_4567_89AB_CDEF_0011,
                              _mask(96), 0]):
        rc, v, _ = h.randomize(seed + 1, state={1: s})
        assert rc == RANDOMIZE_OK
        assert v[0] == (s + 1) & _mask(96)


def test_every_word_is_random():
    """rand bit [159:0] x; constraint { x[3:0] == 5; }: every 32-bit word,
    not only the low 64 bits, takes values all over its range."""
    w = 160
    b = SolveProblemBuilder()
    b.add_var(0, w, False, 0, _FULL)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_extract(b.expr_var(0), 3, 0),
                                   b.expr_const(5, width=4)))
    h = SolveHandle(b.finalize_bytes(), vars=[(w, False)])
    n = 400
    ones = [0] * w
    words = [set() for _ in range(w // 32)]
    for i in range(n):
        rc, v, _ = h.randomize(7919 * i + 3)
        assert rc == RANDOMIZE_OK
        x = v[0]
        assert x & 0xF == 5
        for k in range(w):
            ones[k] += (x >> k) & 1
        for j in range(w // 32):
            words[j].add((x >> (32 * j)) & 0xFFFF_FFFF)
    for j in range(w // 32):
        assert len(words[j]) > n * 0.95, f"word {j}: {len(words[j])} distinct of {n}"
        lo = 4 if j == 0 else 32 * j
        for k in range(lo, 32 * (j + 1)):
            assert 0.38 < ones[k] / n < 0.62, f"bit {k} is 1 in {ones[k]} of {n}"


def test_wide_unsat_then_sat():
    """x < s with s == 0 has no solution; with s == 5 it has."""
    b = SolveProblemBuilder()
    b.add_var(0, 72, False, 0, _FULL)
    b.add_var(1, 72, False, 0, _FULL)
    b.set_var_kind(1, VAR_STATE)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(0), b.expr_var(1)))
    h = SolveHandle(b.finalize_bytes(), vars=[(72, False), (72, False)])
    assert h.randomize(1, state={1: 0})[0] == RANDOMIZE_UNSAT
    assert h.randomize(2, state={1: 0})[0] == RANDOMIZE_UNSAT
    rc, v, _ = h.randomize(3, state={1: 5})
    assert rc == RANDOMIZE_OK and v[0] < 5


def test_wide_shift_by_state():
    """(prot >> amount) == 0 with prot 100 bits and amount STATE
    (t_constraint_shift_width)."""
    b = SolveProblemBuilder()
    b.add_var(0, 100, False, 0, _FULL)
    b.add_var(1, 7, False, 0, 127)
    b.set_var_kind(1, VAR_STATE)
    b.add_constraint(b.expr_binary(
        BIN_EQ, b.expr_binary(BIN_RSHIFT, b.expr_var(0), b.expr_extend(b.expr_var(1), 7, 100)),
        b.expr_const_wide(0, 100)))
    h = SolveHandle(b.finalize_bytes(), vars=[(100, False), (7, False)])
    for i, amt in enumerate([90, 64, 33, 1, 0, 99]):
        rc, v, _ = h.randomize(i + 11, state={1: amt})
        assert rc == RANDOMIZE_OK
        assert v[0] >> amt == 0


def test_signed_wide_and_narrow_ref_sign_extends():
    """A signed 70-bit x == s, s read through a 16-bit signed SCALAR ref:
    the STATE value is sign-extended to 70 bits, and x is written negative."""
    b = SolveProblemBuilder()
    b.add_var(0, 70, True, -_FULL - 1, _FULL)
    b.add_var(1, 70, True, -_FULL - 1, _FULL)
    b.set_var_kind(1, VAR_STATE)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_var(1)))
    h = SolveHandle(b.finalize_bytes(), vars=[(70, True), (16, True, REF_SCALAR)])
    rc, v, _ = h.randomize(1, state={1: -3 & 0xFFFF})
    assert rc == RANDOMIZE_OK
    assert v[0] == -3


def _raw_call(h, specs, init):
    """dvs_randomize with hand-made refs: `specs` (width, signed, layout,
    nbytes) per var, cells pre-filled from `init` bytes. Returns (rc, cells)."""
    refs = (_Ref * len(specs))()
    cells = []
    for vid, (w, sg, lay, nb) in enumerate(specs):
        cell = ctypes.create_string_buffer(init[vid], nb)
        cells.append(cell)
        refs[vid].addr = ctypes.cast(cell, ctypes.c_void_p)
        refs[vid].width = w
        refs[vid].is_signed = sg
        refs[vid].layout = lay
    rc = h._lib.dvs_randomize(h._h, refs, None, 5)
    return rc, cells


def test_words_top_word_masked_and_word_order():
    """A 72-bit value written through words: word 0 is bits 31:0, word 2
    bits 71:64 with the bits above 71 cleared; a negative signed value is
    not sign-filled past its width."""
    k = 0xA5_1234_5678_9ABC_DEF0
    b = SolveProblemBuilder()
    b.add_var(0, 72, False, 0, _FULL)
    b.add_var(1, 72, True, -_FULL - 1, _FULL)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const_wide(k, 72)))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(1),
                                   b.expr_const_wide(-2, 72, is_signed=True)))
    h = SolveHandle(b.finalize_bytes(), vars=[(72, False), (72, True)])
    rc, cells = _raw_call(h, [(72, 0, REF_WORDS, 12), (72, 1, REF_WORDS, 12)],
                          [b"\xff" * 12, b"\xff" * 12])
    assert rc == RANDOMIZE_OK
    w = [int.from_bytes(cells[0].raw[4 * i:4 * i + 4], "little") for i in range(3)]
    assert w == [0x9ABC_DEF0, 0x1234_5678, 0xA5]
    w = [int.from_bytes(cells[1].raw[4 * i:4 * i + 4], "little") for i in range(3)]
    assert w == [0xFFFF_FFFE, 0xFFFF_FFFF, 0xFF]


@pytest.mark.parametrize("width", [8, 33, 64])
def test_words_ref_on_a_narrow_problem(width):
    """A narrow problem (CDCL) reads and writes WORDS refs: x == s + 1."""
    b = SolveProblemBuilder()
    b.add_var(0, width, False, 0, _mask(width) if width < 64 else _FULL)
    b.add_var(1, width, False, 0, _mask(width) if width < 64 else _FULL)
    b.set_var_kind(1, VAR_STATE)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0),
                                   b.expr_binary(BIN_ADD, b.expr_var(1),
                                                 b.expr_const(1, width=width))))
    h = SolveHandle(b.finalize_bytes(),
                    vars=[(width, False, REF_WORDS), (width, False, REF_WORDS)])
    s = _mask(width) - 5 if width < 64 else 0x1_0000_0000
    rc, v, _ = h.randomize(1, state={1: s})
    assert rc == RANDOMIZE_OK
    assert v[0] == (s + 1) & _mask(width)


def test_wide_mixed_with_narrow_vars():
    """A wide variable beside narrow ones (t_randomize_method_constraints):
    u > 0 over 80 bits, x inside {1, 2, 4, 5} over 48 bits."""
    b = SolveProblemBuilder()
    b.add_var(0, 80, False, 0, _FULL)
    b.add_var(1, 48, False, 0, _mask(48))
    b.add_var(2, 23, False, 0, _mask(23))
    b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_var(0), b.expr_const_wide(0, 80)))
    b.add_constraint(b.expr_in_set(b.expr_var(1), [b.expr_const(k, width=48)
                                                   for k in (1, 2, 4, 5)]))
    b.add_constraint(b.expr_in_range(b.expr_var(2), b.expr_const(14, width=23),
                                     b.expr_const(20, width=23)))
    h = SolveHandle(b.finalize_bytes(), vars=[(80, False), (48, False), (23, False)])
    seen = set()
    for i in range(40):
        rc, v, _ = h.randomize(i + 1)
        assert rc == RANDOMIZE_OK
        assert v[0] != 0 and v[1] in (1, 2, 4, 5) and 14 <= v[2] <= 20
        seen.add(v[0] >> 64)
    assert len(seen) > 20    # the bits above 64 vary


def _wide_with(kind):
    b = SolveProblemBuilder()
    b.add_var(0, 100, False, 0, _FULL)
    b.add_var(1, 8, False, 0, 255)
    if kind == "randc":
        b.set_var_kind(1, VAR_RANDC)
    elif kind == "soft":
        b.add_soft_constraint(b.expr_binary(BIN_EQ, b.expr_var(1), b.expr_const(3, width=8)))
    b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_var(0), b.expr_const_wide(0, 100)))
    return SolveHandle(b.finalize_bytes(), vars=[(100, False), (8, False)])


@pytest.mark.parametrize("kind", ["randc", "soft"])
def test_wide_with_unsupported_features_is_an_error(kind):
    """randc and soft constraints are not solved beside a wide variable yet:
    the call fails rather than ignore them."""
    assert _wide_with(kind).randomize(1)[0] == RANDOMIZE_ERROR


def test_narrow_ref_on_a_wide_var_reads_and_writes_its_width():
    """A 64-bit SCALAR ref of a 100-bit RAND variable gets its low 64 bits."""
    k = (0x5 << 96) | 0x0123_4567_89AB_CDEF
    b = SolveProblemBuilder()
    b.add_var(0, 100, False, 0, _FULL)
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(0), b.expr_const_wide(k, 100)))
    h = SolveHandle(b.finalize_bytes(), vars=[(64, False, REF_SCALAR)])
    rc, v, _ = h.randomize(1)
    assert rc == RANDOMIZE_OK and v[0] == k & _mask(64)


def test_queue_of_wide_elements():
    """rand bit [65:0] q[$]; constraint { q[0] == 0; q[1] == 1; q[2] == 2;
    foreach (q[i]) i > 2 -> q[i] != 0; } (t_randomize_queue_wide): elements
    of 66 bits, three 32-bit words each, solved together."""
    from dv_solve.problem import BIN_GT
    w = 66
    b = SolveProblemBuilder()
    b.add_array(0, w, False, 0)
    for k in range(3):
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_elem(0, b.expr_const(k)),
                                       b.expr_const_wide(k, w)))
    b.foreach_begin(0, 0)
    i = b.expr_loop_index(0)
    from dv_solve.problem import BIN_OR
    b.add_constraint(b.expr_binary(
        BIN_OR, b.expr_unary(1, b.expr_binary(BIN_GT, i, b.expr_const(2))),
        b.expr_binary(BIN_NEQ, b.expr_elem(0, i), b.expr_const_wide(0, w))))
    b.foreach_end()
    h = SolveHandle(b.finalize_bytes(), vars=[], arrays=[(w, False)])
    hi = set()
    for s in range(20):
        rc, _, elems = h.randomize(s + 1, arrays=[[0] * 6])
        assert rc == RANDOMIZE_OK
        q = elems[0]
        assert q[:3] == [0, 1, 2]
        assert all(0 < x < (1 << w) for x in q[3:])
        hi.update(x >> 64 for x in q[3:])
    assert hi == {0, 1, 2, 3}    # bits 65:64 are drawn too


def test_wide_elements_one_at_a_time():
    """rand bit [99:0] a[]; bit [99:0] lim; foreach (a[i]) a[i] < lim: each
    element is solved alone (the element-local path)."""
    w = 100
    b = SolveProblemBuilder()
    b.add_var(0, w, False, 0, _FULL)
    b.set_var_kind(0, VAR_STATE)
    b.add_array(0, w, False, 0)
    b.foreach_begin(0, 0)
    b.add_constraint(b.expr_binary(BIN_LT, b.expr_elem(0, b.expr_loop_index(0)),
                                   b.expr_var(0)))
    b.foreach_end()
    h = SolveHandle(b.finalize_bytes(), vars=[(w, False)], arrays=[(w, False)])
    lim = (1 << 97) + 3
    rc, _, elems = h.randomize(9, state={0: lim}, arrays=[[0] * 8])
    assert rc == RANDOMIZE_OK
    assert all(x < lim for x in elems[0])
    assert len(set(elems[0])) == 8
