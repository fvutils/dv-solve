"""The canonical shape class over the svcorpus cases (solve stats plan, S1).

For every case:
- invariance: renumbered variable ids, shuffled constraint order and small
  constants changed within their class keep the class hash;
- mutation: changing any one binary operator changes it.
Across the corpus, no two cases, and no mutants of two different cases, share
a hash (the collision check).
"""
from __future__ import annotations

import ctypes
import random

import pytest

from tests.perf.svcorpus import case as C

dv = pytest.importorskip("dv_solve")

from dv_solve.builder import SolveProblemBuilder  # noqa: E402
from dv_solve.problem import (BIN_ADD, BIN_AND, BIN_BAND, BIN_BOR, BIN_EQ,  # noqa: E402
                              BIN_LT, BIN_LTE, BIN_NEQ, BIN_OR, BIN_SUB)

# One structural mutation per operator: another operator of the same kind.
_FLIP = {BIN_LT: BIN_LTE, BIN_LTE: BIN_LT, BIN_EQ: BIN_NEQ, BIN_NEQ: BIN_EQ,
         BIN_ADD: BIN_SUB, BIN_SUB: BIN_ADD, BIN_BAND: BIN_BOR, BIN_BOR: BIN_BAND,
         BIN_AND: BIN_OR, BIN_OR: BIN_AND}
# Small constants that are not 0, 1, a power of two or all-ones at any width
# a case uses: one class.
_SMALL = [3, 5, 6, 7, 9, 10, 11, 12, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23]


class _Class(ctypes.Structure):
    _fields_ = [("hash", ctypes.c_uint64), ("inst", ctypes.c_uint64),
                ("n_nodes", ctypes.c_uint32), ("truncated", ctypes.c_uint32),
                ("rest", ctypes.c_uint8 * 4096)]  # features, tracked vars, and room


def _check_size(lib):
    """The library's dvs_class_t fits in _Class (it writes all of it)."""
    lib.dvs_class_sizeof.restype = ctypes.c_size_t
    assert lib.dvs_class_sizeof() <= ctypes.sizeof(_Class)


class _Builder(SolveProblemBuilder):
    """Records or perturbs the case's builder calls."""

    def __init__(self, shuffle=None, flip_at=None, small_map=None):
        super().__init__()
        self.shuffle, self.flip_at, self.small_map = shuffle, flip_at, small_map
        self.n_binary = 0
        self.flipped = False
        self.held = []

    def expr_binary(self, op, lhs, rhs):
        if self.n_binary == self.flip_at and op in _FLIP:
            op, self.flipped = _FLIP[op], True
        self.n_binary += 1
        return super().expr_binary(op, lhs, rhs)

    def expr_const(self, value, is_signed=False, width=0):
        if self.small_map and value in self.small_map and (width == 0 or width > 5):
            value = self.small_map[value]
        return super().expr_const(value, is_signed, width)

    def add_constraint(self, root):
        if self.shuffle is None:
            return super().add_constraint(root)
        self.held.append(root)
        return 0

    def flush(self):
        if self.shuffle is not None:
            self.shuffle.shuffle(self.held)
            for r in self.held:
                super().add_constraint(r)


def _classify(case, perm=None, **kw):
    """(class hash, binary operators seen, mutated?) of `case` built by _Builder."""
    b = _Builder(**kw)
    names = [n for n, _, _ in case.VARS]
    order = perm or list(range(len(names)))
    v = {n: order[i] for i, n in enumerate(names)}
    for n, w, s in sorted(case.VARS, key=lambda t: v[t[0]]):
        lo, hi = (-(1 << (w - 1)), (1 << (w - 1)) - 1) if s else (0, (1 << w) - 1)
        b.add_var(v[n], width=w, is_signed=s, lo=lo, hi=hi)
    case.build(b, v)
    b.flush()
    for n, entries in getattr(case, "DIST", {}).items():
        b.add_dist(v[n], [{"lo": lo, "hi": hi, "weight": wt, "is_per_value": pv}
                          for lo, hi, wt, pv in entries])
    buf, _ = b.finalize()
    lib = b._lib
    lib.dvs_class_compute.restype = ctypes.c_int
    lib.dvs_class_compute.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Class)]
    _check_size(lib)
    out = _Class()
    assert lib.dvs_class_compute(ctypes.cast(buf, ctypes.c_void_p), ctypes.byref(out)) == 0
    assert not out.truncated
    return out.hash, b.n_binary, b.flipped


@pytest.mark.parametrize("name", C.names())
def test_invariant(name):
    case = C.load(name)
    base = _classify(case)[0]
    rng = random.Random(name)
    n = len(case.VARS)
    for trial in range(3):
        perm = list(range(n))
        rng.shuffle(perm)
        assert _classify(case, perm=perm)[0] == base, (name, "renumbered", trial)
        assert _classify(case, shuffle=random.Random(trial))[0] == base, (name, "shuffled", trial)
        shifted = _SMALL[trial + 1:] + _SMALL[:trial + 1]
        assert _classify(case, small_map=dict(zip(_SMALL, shifted)))[0] == base, \
            (name, "constants", trial)


def test_mutations_and_collisions():
    """Every operator mutation changes the class, and no two cases (or mutants
    of different cases) share one. Mutants of one case may share a class: a
    case that repeats a block (flash_ctrl_otf_base's 8 regions) gives the same
    problem, up to renaming, whichever copy is mutated."""
    owner = {}
    missed, collisions, n_mutants = [], [], 0
    for name in C.names():
        case = C.load(name)
        h, n_bin, _ = _classify(case)
        hashes = {h}
        for k in range(n_bin):
            m, _, flipped = _classify(case, flip_at=k)
            if not flipped:
                continue
            n_mutants += 1
            if m == h:
                missed.append((name, k))
            hashes.add(m)
        for x in hashes:
            if owner.setdefault(x, name) != name:
                collisions.append((owner[x], name))
    assert n_mutants > 300, n_mutants
    assert not missed, f"mutations that kept the class: {missed}"
    assert not collisions, f"cases sharing a class: {collisions}"
