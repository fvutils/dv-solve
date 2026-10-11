"""Words split into field variables at their extracts' boundaries.

A packed word whose fields are constrained one by one -- TPM's register
offset from a set in x[11:2] next to a locality below 5 in x[15:12] -- left
the engine a wide word to draw, each field reaching it through a bit-slice
propagator: ~2 ms a solve and a skewed spread. Compile now splits such a word
into hidden field variables, x == {f_k, ..., f_0}, and each extract that is
exactly one field becomes it (dvs_sv_rewrite_masks, every front end). See
docs/bitfield_linearization_research.md section 9.

The split must be an equivalence. Seeded random field constraints on an 8-bit
word, next to a 3-bit variable they may link to, are checked exhaustively: every
assignment pinned, sat/unsat compared with the IR's own evaluator, and unpinned
models checked against the constraints.
"""
from __future__ import annotations

import random

import pytest

from tests.formal.soundness.doors import _lower
from tests.formal.soundness.ir import ev

dv = pytest.importorskip("dv_solve")
from dv_solve.builder import SolveProblemBuilder            # noqa: E402
from dv_solve.ctx import (CompileIncompleteError, CompileUnsatError,  # noqa: E402
                          SOLVE_OK, SOLVE_UNSAT, SolveCtx)

WIDTHS = {"x": 8, "y": 3}
IDS = {"x": 0, "y": 1}
X, Y = ("var", "x", 8), ("var", "y", 3)


def _build(cons, dist=None):
    b = SolveProblemBuilder()
    for n, w in WIDTHS.items():
        b.add_var(IDS[n], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in cons:
        b.add_constraint(_lower(b, c, IDS))
    if dist:
        b.add_dist(IDS["x"], dist)
    return b.finalize()[0]


def _check(cons, dist=None, declines=True):
    prob = _build(cons, dist)
    truth = {(xv, yv): all(ev(c, {"x": xv, "y": yv}) for c in cons)
             for xv in range(256) for yv in range(8)}
    try:
        ctx = SolveCtx(prob)
    except CompileUnsatError:
        assert not any(truth.values()), cons
        return
    except CompileIncompleteError:
        assert declines, cons
        return                      # declined: not a wrong answer
    with ctx:
        for (xv, yv), want in truth.items():
            ctx.reset()
            cp = ctx.checkpoint()
            pinned = ctx.pin(0, xv) and ctx.pin(1, yv)
            rc = ctx.solve(seed=1) if pinned else SOLVE_UNSAT
            ctx.restore(cp)
            assert rc in (SOLVE_OK, SOLVE_UNSAT), (cons, xv, yv, rc)
            assert (rc == SOLVE_OK) == want, \
                (cons, (xv, yv), "sat" if rc == SOLVE_OK else "unsat")
        for seed in range(1, 25):
            ctx.reset()
            rc = ctx.solve(seed=seed)
            assert (rc == SOLVE_OK) == any(truth.values()), (cons, seed, rc)
            if rc == SOLVE_OK:
                m = (ctx.get_value(0) & 0xFF, ctx.get_value(1) & 7)
                assert truth[m], (cons, seed, m)


def _ex(h, l):
    return ("extract", h - l + 1, h, l, X)


def _c(v, w):
    return ("const", v, w)


def _field_con(rng, h, l):
    """One constraint on x[h:l]: a set, its negation, an exclusion, an
    ordering, a link to y, or an equality."""
    w = h - l + 1
    f = _ex(h, l)
    k = rng.randrange(1 << w)
    r = rng.randrange(7)
    if r == 0:
        vals = rng.sample(range(1 << w), min(1 << w, rng.randint(1, 4)))
        return ("or", "bool", *[("=", "bool", f, _c(v, w)) for v in vals]) \
            if len(vals) > 1 else ("=", "bool", f, _c(vals[0], w))
    if r == 1:
        vals = rng.sample(range(1 << w), min(1 << w, rng.randint(1, 3)))
        return ("not", "bool", ("or", "bool", *[("=", "bool", f, _c(v, w)) for v in vals])
                if len(vals) > 1 else ("=", "bool", f, _c(vals[0], w)))
    if r == 2:
        return ("distinct", "bool", f, _c(k, w))
    if r == 3:
        return (rng.choice(["bvult", "bvule", "bvugt", "bvuge"]), "bool", f, _c(k, w))
    if r == 4:
        if w == 3:
            return ("=", "bool", f, Y)
        if w < 3:
            return ("=", "bool", f, ("extract", w, w - 1, 0, Y))
        return ("bvuge", "bool", f, ("zero_extend", w, Y))
    return ("=", "bool", f, _c(k, w))


def _fields(rng):
    """The (hi, lo) of x's fields: a random partition of its bits, often with
    some runs of adjacent fields named too (nested extracts, x[3:0] around
    x[1:0]), or, one time in five, intervals drawn freely (which may cross,
    and then are not split)."""
    r = rng.random()
    if r < 0.2:
        out = []
        for _ in range(4):
            lo = rng.randrange(8)
            out.append((rng.randrange(lo, min(8, lo + 4)), lo))
        return out
    cuts = sorted(rng.sample(range(1, 8), rng.randint(1, 4)))
    bounds = [0] + cuts + [8]
    out = [(bounds[i + 1] - 1, bounds[i]) for i in range(len(bounds) - 1)]
    if r < 0.6 and len(out) > 2:
        for _ in range(rng.randint(1, 2)):       # runs: nested, never crossing
            i = rng.randrange(len(out) - 1)
            j = rng.randrange(i + 1, len(out))
            run = (out[j][0], out[i][1])
            if all(lo > run[0] or hi < run[1] or (lo >= run[1] and hi <= run[0])
                   or (run[1] >= lo and run[0] <= hi) for hi, lo in out):
                out.append(run)
    return out


def _case(seed):
    rng = random.Random(seed)
    fields = _fields(rng)
    cons = []
    for _ in range(rng.randint(2, 4)):
        hi, lo = rng.choice(fields)
        c = _field_con(rng, hi, lo)
        r = rng.random()
        if r < 0.15 and cons:                    # under Boolean structure
            c = ("or", "bool", c, cons.pop())
        elif r < 0.25:
            c = ("=>", "bool", ("bvult", "bool", Y, _c(rng.randrange(8), 3)), c)
        cons.append(c)
    if rng.random() < 0.2 and len(fields) > 1:  # fields gathered, out of order
        (h1, l1), (h2, l2) = rng.sample(fields, 2)
        w = h1 - l1 + h2 - l2 + 2
        cat = ("concat", w, _ex(h1, l1), _ex(h2, l2))
        cons.append((rng.choice(["distinct", "bvult", "bvugt"]), "bool", cat,
                     _c(rng.randrange(1 << w), w)))
    if rng.random() < 0.3:                       # the whole word too
        cons.append(("bvuge", "bool", X, _c(rng.randrange(128), 8)))
    return cons


@pytest.mark.parametrize("seed", range(120))
def test_random_fields(seed):
    _check(_case(seed))


# TPM's shape, scaled to 8 bits: x[1:0] == 0, a locality x[7:6] < 3 and an
# offset x[5:2] from a set, next to a window on the whole word.
TPM = [("=", "bool", _ex(1, 0), _c(0, 2)),
       ("bvult", "bool", _ex(7, 6), _c(3, 2)),
       ("or", "bool", *[("=", "bool", _ex(5, 2), _c(v, 4)) for v in (0, 1, 2, 5, 9, 15)]),
       ("bvuge", "bool", X, _c(0x14, 8))]


def test_tpm_shape():
    _check(TPM)


def test_tpm_shape_is_split():
    """The split happens: field variables follow the user's, lowest first."""
    with SolveCtx(_build(TPM)) as ctx:
        for seed in range(1, 20):
            ctx.reset()
            assert ctx.solve(seed=seed) == SOLVE_OK
            x = ctx.get_value(0) & 0xFF
            assert [ctx.get_value(2 + i) for i in range(3)] == \
                [x & 3, (x >> 2) & 15, x >> 6]


def test_tpm_shape_spread():
    """Fields constrained independently: the draw over the fields is uniform
    over the solutions. (With TPM's window, which removes half of locality
    0's offsets, it is uniform per field, not per solution: locality 0's
    offsets come up twice as often.)"""
    cons = TPM[:3]
    sols = [x for x in range(256) if all(ev(c, {"x": x, "y": 0}) for c in cons)]
    n = 200 * len(sols)
    seen = {}
    with SolveCtx(_build(cons)) as ctx:
        for seed in range(1, n + 1):
            ctx.reset()
            assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
            x = ctx.get_value(0) & 0xFF
            seen[x] = seen.get(x, 0) + 1
    assert set(seen) == set(sols)
    assert max(seen.values()) < 2 * min(seen.values()), seen


# PCIe's shape, scaled: a page x[7:4] from a set, an offset x[3:0] below a
# bound, and dword alignment x[1:0] == 0 inside it.
PCIE = [("or", "bool", *[("=", "bool", _ex(7, 4), _c(v, 4)) for v in (1, 6, 15)]),
        ("bvult", "bool", _ex(3, 0), _c(13, 4)),
        ("=", "bool", _ex(1, 0), _c(0, 2))]


def test_nested_shape():
    _check(PCIE, declines=False)


def test_nested_shape_is_split():
    """Nested extracts split too: x[3:0] becomes a hidden word over the
    fields x[3:2] and x[1:0]."""
    with SolveCtx(_build(PCIE)) as ctx:
        for seed in range(1, 20):
            ctx.reset()
            assert ctx.solve(seed=seed) == SOLVE_OK
            x = ctx.get_value(0) & 0xFF
            assert [ctx.get_value(2 + i) for i in range(4)] == \
                [x & 3, (x >> 2) & 3, x >> 4, x & 15]


def test_nested_high_word_is_split():
    """A hidden word above bit 0: x[7:2] around x[3:2] and x[7:6]."""
    cons = [("bvult", "bool", _ex(7, 2), _c(50, 6)),
            ("distinct", "bool", _ex(3, 2), _c(1, 2)),
            ("or", "bool", ("=", "bool", _ex(7, 6), _c(0, 2)),
             ("=", "bool", _ex(7, 6), _c(2, 2)))]
    _check(cons, declines=False)
    with SolveCtx(_build(cons)) as ctx:
        for seed in range(1, 20):
            ctx.reset()
            assert ctx.solve(seed=seed) == SOLVE_OK
            x = ctx.get_value(0) & 0xFF
            assert [ctx.get_value(2 + i) for i in range(5)] == \
                [x & 3, (x >> 2) & 3, (x >> 4) & 3, x >> 6, x >> 2]


def test_overlapping_extracts():
    """x[4:2] and x[6:4] share bit 4: not split, still exact."""
    _check([("distinct", "bool", _ex(4, 2), _c(0, 3)),
            ("or", "bool", ("=", "bool", _ex(6, 4), _c(3, 3)),
             ("=", "bool", _ex(6, 4), _c(4, 3))),
            ("bvult", "bool", _ex(1, 0), _c(2, 2))])


def test_extract_of_whole_word():
    _check([("or", "bool", ("=", "bool", _ex(7, 0), _c(3, 8)),
             ("=", "bool", _ex(7, 0), _c(200, 8))),
            ("bvugt", "bool", _ex(7, 4), _c(0, 4))])


def test_distributed_word_is_left():
    """A dist names the word itself: no split, still exact."""
    _check([("bvult", "bool", _ex(7, 4), _c(9, 4)),
            ("distinct", "bool", _ex(3, 0), _c(2, 4))],
           dist=[{"lo": 0, "hi": 100, "weight": 1}, {"lo": 101, "hi": 255, "weight": 3}])


def test_field_in_soft():
    """A soft constraint on a field: the split keeps the soft's meaning."""
    b = SolveProblemBuilder()
    b.add_var(0, width=8, is_signed=False, lo=0, hi=255)
    b.add_var(1, width=3, is_signed=False, lo=0, hi=7)
    b.add_constraint(_lower(b, ("bvult", "bool", _ex(7, 4), _c(4, 4)), IDS))
    b.add_constraint(_lower(b, ("distinct", "bool", _ex(3, 0), _c(0, 4)), IDS))
    b.add_soft_constraint(_lower(b, ("=", "bool", _ex(3, 0), _c(5, 4)), IDS))
    b.add_soft_constraint(_lower(b, ("=", "bool", _ex(7, 4), _c(9, 4)), IDS))
    with SolveCtx(b.finalize()[0]) as ctx:
        for seed in range(1, 20):
            ctx.reset()
            assert ctx.solve(seed=seed) == SOLVE_OK
            x = ctx.get_value(0) & 0xFF
            assert x >> 4 < 4 and x & 15 == 5, hex(x)


# 64-bit words are tied to their parts by bit slices, not a concat (the
# concat propagator reasons in int64). Too wide to enumerate: each case pins x
# to values composed from the constraints' own constants (and their
# neighbours), the 2^63 boundary and random words, and checks unpinned models.

def _case64(seed):
    rng = random.Random(seed)
    x = ("var", "x", 64)
    cuts = sorted(rng.sample(range(1, 64), rng.randint(1, 5)))
    if rng.random() < 0.5 and 63 not in cuts:
        cuts.append(63)                                  # the sign bit alone
    b = [0] + sorted(set(cuts)) + [64]
    fields = [(b[i + 1] - 1, b[i]) for i in range(len(b) - 1)]
    if len(fields) > 2 and rng.random() < 0.5:
        i = rng.randrange(len(fields) - 1)
        fields.append((fields[i + 1][0], fields[i][1]))  # a nested run
    cons, picks = [], []
    for _ in range(rng.randint(2, 4)):
        h, l = rng.choice(fields)
        w = h - l + 1
        f = ("extract", w, h, l, x)
        vals = [rng.choice([0, (1 << w) - 1, 1 << (w - 1), rng.randrange(1 << w)])
                for _ in range(rng.randint(1, 3))]
        picks.append((h, l, vals))
        r = rng.randrange(4)
        if r == 0:
            c = ("or", "bool", *[("=", "bool", f, _c(v, w)) for v in vals]) \
                if len(vals) > 1 else ("=", "bool", f, _c(vals[0], w))
        elif r == 1:
            c = ("distinct", "bool", f, _c(vals[0], w))
        elif r == 2:
            c = (rng.choice(["bvult", "bvule", "bvugt", "bvuge"]), "bool", f, _c(vals[0], w))
        else:
            c = ("not", "bool", ("=", "bool", f, _c(vals[0], w)))
        cons.append(c)
    if rng.random() < 0.4:
        cons.append((rng.choice(["bvult", "bvuge"]), "bool", x,
                     _c(rng.choice([1 << 63, (1 << 63) - 1, rng.getrandbits(64)]), 64)))
    cands = {0, (1 << 64) - 1, 1 << 63, (1 << 63) - 1}
    cands |= {rng.getrandbits(64) for _ in range(40)}
    for _ in range(160):
        v = rng.getrandbits(64)
        for h, l, vals in rng.sample(picks, rng.randint(1, len(picks))):
            fv = (rng.choice(vals) + rng.choice([0, 0, 1, -1])) % (1 << (h - l + 1))
            m = ((1 << (h - l + 1)) - 1) << l
            v = (v & ~m) | (fv << l)
        cands.add(v)
    return cons, sorted(cands)


@pytest.mark.parametrize("seed", range(60))
def test_random_fields_64(seed):
    cons, cands = _case64(seed)
    b = SolveProblemBuilder()
    b.add_var(0, width=64, is_signed=False, lo=0, hi=(1 << 64) - 1)
    for c in cons:
        b.add_constraint(_lower(b, c, {"x": 0}))
    truth = {v: all(ev(c, {"x": v}) for c in cons) for v in cands}
    try:
        ctx = SolveCtx(b.finalize()[0])
    except CompileUnsatError:
        assert not any(truth.values()), cons
        return
    with ctx:
        for v, want in truth.items():
            ctx.reset()
            cp = ctx.checkpoint()
            rc = ctx.solve(seed=1) if ctx.pin(0, v - (1 << 64) if v >> 63 else v) else SOLVE_UNSAT
            ctx.restore(cp)
            assert rc in (SOLVE_OK, SOLVE_UNSAT), (cons, hex(v), rc)
            assert (rc == SOLVE_OK) == want, (cons, hex(v), rc)
        for seed in range(1, 25):
            ctx.reset()
            rc = ctx.solve(seed=seed)
            if any(truth.values()):
                assert rc == SOLVE_OK, (cons, seed, rc)
            if rc == SOLVE_OK:
                m = ctx.get_value(0) & ((1 << 64) - 1)
                assert all(ev(c, {"x": m}) for c in cons), (cons, seed, hex(m))
