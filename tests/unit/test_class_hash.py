"""The canonical shape class (docs/solve_stats_capture_plan.md, S1).

dvs_class_compute gives two problems the same class hash when they differ only
in variable ids, the order constraints and commutative operands were written
in, or the values of constants within one constant class; any change of
structure gives a different hash. The instance hash tells every distinct
problem apart.
"""
import ctypes

import pytest

from dv_solve.builder import SolveProblemBuilder
from dv_solve.problem import (BIN_ADD, BIN_AND, BIN_BAND, BIN_EQ, BIN_GT, BIN_LT,
                              BIN_LTE, BIN_MUL, BIN_OR)


class _Class(ctypes.Structure):
    _fields_ = [("hash", ctypes.c_uint64), ("inst", ctypes.c_uint64),
                ("n_nodes", ctypes.c_uint32), ("truncated", ctypes.c_uint32),
                ("rest", ctypes.c_uint8 * 4096)]  # features, tracked vars, and room


def _check_size(lib):
    """The library's dvs_class_t fits in _Class (it writes all of it)."""
    lib.dvs_class_sizeof.restype = ctypes.c_size_t
    assert lib.dvs_class_sizeof() <= ctypes.sizeof(_Class)


def _classify(build):
    """(class hash, instance hash) of the problem `build(b)` makes."""
    b = SolveProblemBuilder()
    build(b)
    buf, _ = b.finalize()
    lib = b._lib
    lib.dvs_class_compute.restype = ctypes.c_int
    lib.dvs_class_compute.argtypes = [ctypes.c_void_p, ctypes.POINTER(_Class)]
    _check_size(lib)
    out = _Class()
    assert lib.dvs_class_compute(ctypes.cast(buf, ctypes.c_void_p), ctypes.byref(out)) == 0
    assert not out.truncated
    return out.hash, out.inst


def _vars(b, ids, width=8, signed=False):
    for i in ids:
        b.add_var(i, width, signed, 0, (1 << width) - 1)


def _lt(b, x, y):
    return b.expr_binary(BIN_LT, b.expr_var(x), b.expr_var(y))


def _lt_const(b, x, c):
    return b.expr_binary(BIN_LT, b.expr_var(x), b.expr_const(c))


def chain(ids=(0, 1, 2), order=(0, 1), c=10):
    """x < y, y < z, z < c over ids x, y, z; `order` permutes the first two."""
    x, y, z = ids

    def build(b):
        _vars(b, sorted(ids))
        cons = [lambda: _lt(b, x, y), lambda: _lt(b, y, z)]
        for i in order:
            b.add_constraint(cons[i]())
        b.add_constraint(_lt_const(b, z, c))
    return build


BASE = chain()

# Same class: each differs from BASE only in ids, order or a constant's value.
SAME = {
    "renumbered": chain(ids=(2, 0, 1)),
    "reordered": chain(order=(1, 0)),
    "constant in class": chain(c=20),
}


def _swapped_gt(b):
    _vars(b, (0, 1, 2))
    b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(1), b.expr_var(0)))
    b.add_constraint(_lt(b, 1, 2))
    b.add_constraint(_lt_const(b, 2, 10))


def _conjoined(b):
    _vars(b, (0, 1, 2))
    b.add_constraint(b.expr_binary(BIN_AND, _lt(b, 0, 1), _lt(b, 1, 2)))
    b.add_constraint(_lt_const(b, 2, 10))


SAME["y > x written for x < y"] = _swapped_gt
SAME["two constraints joined by &&"] = _conjoined


def _mutant(**kw):
    def build(b):
        width = kw.get("width", 8)
        signed = kw.get("signed", False)
        _vars(b, (0, 1, 2), width, signed)
        if "extra_var" in kw:
            _vars(b, (3,))
        op = kw.get("op", BIN_LT)
        b.add_constraint(b.expr_binary(op, b.expr_var(0), b.expr_var(1)))
        # sharing: y < z, or y < x (a cycle), or z alone
        second = kw.get("second", (1, 2))
        b.add_constraint(_lt(b, *second))
        if kw.get("soft"):
            b.add_soft_constraint(_lt_const(b, 2, 10))
        else:
            b.add_constraint(_lt_const(b, 2, kw.get("c", 10)))
        if "extra" in kw:
            b.add_constraint(kw["extra"](b))
        if kw.get("dist"):
            b.add_dist(0, [{"lo": 0, "hi": 3, "weight": 1}])
        if kw.get("alldiff"):
            b.add_all_different([0, 1, 2])
    return build


# Different class: one structural change each.
DIFFERENT = {
    "operator < to <=": _mutant(op=BIN_LTE),
    "wider variables": _mutant(width=16),
    "signed variables": _mutant(signed=True),
    "variables shared differently": _mutant(second=(2, 0)),
    "constant of another class": _mutant(c=1000),
    "constant zero": _mutant(c=0),
    "soft instead of hard": _mutant(soft=True),
    "an extra unused variable": _mutant(extra_var=True),
    "an extra constraint": _mutant(extra=lambda b: _lt(b, 0, 2)),
    "a dist": _mutant(dist=True),
    "an all-different": _mutant(alldiff=True),
}


def test_base_matches_its_mutant_form():
    assert _classify(BASE) == _classify(_mutant())


@pytest.mark.parametrize("name", sorted(SAME))
def test_same_class(name):
    assert _classify(SAME[name])[0] == _classify(BASE)[0], name


@pytest.mark.parametrize("name", sorted(DIFFERENT))
def test_different_class(name):
    assert _classify(DIFFERENT[name])[0] != _classify(BASE)[0], name


def test_all_mutants_distinct():
    hashes = {name: _classify(f)[0] for name, f in DIFFERENT.items()}
    assert len(set(hashes.values())) == len(hashes), hashes


def test_instance_hash_tells_constants_apart():
    a, b = _classify(chain(c=10)), _classify(chain(c=20))
    assert a[0] == b[0] and a[1] != b[1]


def test_commutative_operands():
    def build(order):
        def f(b):
            _vars(b, (0, 1, 2))
            x, y = (b.expr_var(0), b.expr_var(1)) if order else (b.expr_var(1), b.expr_var(0))
            for op in (BIN_ADD, BIN_MUL, BIN_BAND, BIN_OR):
                b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(op, x, y), b.expr_var(2)))
        return f
    assert _classify(build(True))[0] == _classify(build(False))[0]


def test_deep_nesting_is_iterative():
    """A deep left spine (as SMT-LIB tools emit) hashes without recursion."""
    def build(b):
        _vars(b, (0, 1))
        e = b.expr_var(0)
        for _ in range(100000):
            e = b.expr_binary(BIN_ADD, e, b.expr_var(1))
        b.add_constraint(b.expr_binary(BIN_LT, e, b.expr_const(5)))
    h, _ = _classify(build)
    assert h


# ---- feature vector and motifs (S1b) ---------------------------------------

_MOTIFS = ["var-cmp-const", "var-eq-const", "var-cmp-var", "var-eq-var", "range", "set",
           "ranges", "mod-eq", "mask-eq", "shift-eq", "linear", "nonlinear", "bitwise",
           "countones", "sum", "array", "guarded", "disjunction", "bool-var", "ite-value",
           "constant", "other"]


class _Features(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in
                ("n_vars", "n_conjuncts", "n_softs", "n_alldiffs", "n_dists", "n_ordered")] + \
               [("widths", ctypes.c_uint32 * 6)] + \
               [(n, ctypes.c_uint32) for n in
                ("n_components", "max_component", "max_depth", "guard_depth", "n_nodes",
                 "n_bit", "n_arith", "n_mul", "n_cmp", "n_eq", "n_bool", "n_other")] + \
               [("motifs", ctypes.c_uint32 * len(_MOTIFS))]


class _Track(ctypes.Structure):
    _fields_ = [("n", ctypes.c_uint32), ("seen", ctypes.c_uint32),
                ("var_id", ctypes.c_uint32 * 64), ("width", ctypes.c_uint8 * 64),
                ("free", ctypes.c_uint8 * 64)]


class _ClassF(ctypes.Structure):
    _fields_ = [("hash", ctypes.c_uint64), ("inst", ctypes.c_uint64),
                ("n_nodes", ctypes.c_uint32), ("truncated", ctypes.c_uint32),
                ("f", _Features), ("track", _Track)]


def _features(build):
    b = SolveProblemBuilder()
    build(b)
    buf, _ = b.finalize()
    lib = b._lib
    lib.dvs_class_compute.restype = ctypes.c_int
    lib.dvs_class_compute.argtypes = [ctypes.c_void_p, ctypes.POINTER(_ClassF)]
    lib.dvs_motif_name.restype = ctypes.c_char_p
    lib.dvs_motif_name.argtypes = [ctypes.c_int]
    assert [lib.dvs_motif_name(i).decode() for i in range(len(_MOTIFS))] == _MOTIFS
    lib.dvs_class_sizeof.restype = ctypes.c_size_t
    assert lib.dvs_class_sizeof() == ctypes.sizeof(_ClassF)
    out = _ClassF()
    assert lib.dvs_class_compute(ctypes.cast(buf, ctypes.c_void_p), ctypes.byref(out)) == 0
    f = out.f
    _features.last = out
    return f, {_MOTIFS[i]: f.motifs[i] for i in range(len(_MOTIFS)) if f.motifs[i]}


def test_motifs():
    from dv_solve.problem import BIN_BAND, BIN_MOD, BIN_NEQ, UN_NOT

    def build(b):
        _vars(b, (0, 1, 2), 8)
        b.add_var(3, 1, False, 0, 1)                           # g
        b.add_var(4, 16, False, 0, 65535)                      # w: its own component
        x, y, z, g, w = (b.expr_var(i) for i in range(5))
        c = b.expr_const
        b.add_constraint(b.expr_binary(BIN_LT, x, c(5)))                      # var-cmp-const
        b.add_constraint(b.expr_binary(BIN_EQ, x, y))                         # var-eq-var
        b.add_constraint(b.expr_in_range(y, c(1), c(30)))                     # range
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(BIN_MOD, x, c(4)), c(0)))   # mod-eq
        b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(BIN_BAND, z, c(15)), c(3)))  # mask-eq
        b.add_constraint(b.expr_binary(BIN_OR, b.expr_unary(UN_NOT, g),
                                       b.expr_binary(BIN_LT, y, z)))          # guarded + var-cmp-var
        b.add_constraint(b.expr_binary(BIN_LT, b.expr_binary(BIN_ADD, x, y), z))         # linear
        b.add_constraint(b.expr_binary(BIN_NEQ, b.expr_binary(BIN_MUL, x, y), z))        # nonlinear
        b.add_constraint(b.expr_binary(BIN_OR, b.expr_binary(BIN_LT, x, y),
                                       b.expr_binary(BIN_LT, z, y)))          # disjunction
        b.add_constraint(b.expr_binary(BIN_LT, w, c(100)))                    # var-cmp-const
    f, m = _features(build)
    assert m == {"var-cmp-const": 2, "var-eq-var": 1, "range": 1, "mod-eq": 1, "mask-eq": 1,
                 "guarded": 1, "var-cmp-var": 1, "linear": 1, "nonlinear": 1,
                 "disjunction": 1}, m
    assert f.n_vars == 5 and f.n_conjuncts == 10
    assert list(f.widths) == [1, 3, 1, 0, 0, 0]
    # x, y, z, g coupled; w alone
    assert f.n_components == 2 and f.max_component == 4
    assert f.guard_depth == 1 and f.max_depth >= 2


def test_motifs_ignore_ids_and_constants():
    """Motifs and features belong to the class: renaming and constants in
    class do not change them."""
    a, ma = _features(chain())
    b, mb = _features(chain(ids=(2, 0, 1), c=20))
    assert ma == mb == {"var-cmp-var": 2, "var-cmp-const": 1}
    assert (a.n_components, a.max_component) == (b.n_components, b.max_component) == (1, 3)


def test_tracked_variables():
    """The variables the stats follow: rand and randc only, widest first,
    with the pinned ones marked; the same widths in the same order whatever
    the ids and declaration order."""
    from dv_solve.problem import VAR_STATE

    def build(order):
        def f(b):
            ids = {"a": 0, "b": 1, "c": 2, "s": 3, "p": 4}
            decl = {"a": (8, 0, 255), "b": (32, 0, 1000), "c": (16, 0, 9),
                    "s": (64, 0, 5), "p": (12, 7, 7)}
            for n in order:
                w, lo, hi = decl[n]
                b.add_var(ids[n], w, False, lo, hi)
            b.set_var_kind(ids["s"], VAR_STATE)
            b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(ids["a"]), b.expr_var(ids["c"])))
        return f

    seen = set()
    for order in ("abcsp", "psbca", "cbaps"):
        _features(build(order))
        t = _features.last.track
        got = [(t.width[i], t.free[i]) for i in range(t.n)]
        assert got == [(32, 1), (16, 1), (12, 0), (8, 1)], got
        assert sorted(t.var_id[i] for i in range(t.n)) == [0, 1, 2, 4]
        seen.add(_features.last.hash)
    assert len(seen) == 1
