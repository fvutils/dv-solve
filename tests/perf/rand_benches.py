"""Randomization benchmarks, written once in the soundness campaign's IR.

Every arm solves the same problem from one definition: the SMT-LIB2 arms get
Verilator-form constraints (`smt_vlt`), the dv-solve API arm gets builder
calls (tests/formal/soundness/doors.py `_lower`), and the ground truth comes
from the IR's own bit-exact evaluator, which shares no code with any solver.

Two kinds of benchmark:

- **Enumerable** (at most 16 free bits): the whole solution space is the
  ground truth, so coverage, JSD and chi-square are exact.
- **Wide** (realistic 32-bit fields): the space is far too large to list.
  These measure latency, and distribution through `bins`: a projection of a
  solution to one of a few hundred bins whose exact sizes are known. A
  sampler that repeats itself or ignores whole regions shows there just as
  it does on a small space.
"""
from __future__ import annotations

import hashlib
import json

from tests.formal.soundness.ir import smt


def V(n, w): return ("var", n, w)                     # noqa: E704
def C(v, w): return ("const", v, w)                   # noqa: E704
def B(op, w, a, b): return (op, w, a, b)              # noqa: E704
def P(op, a, b): return (op, "bool", a, b)            # noqa: E704
def AND(*a): return ("and", "bool", *a)               # noqa: E704
def OR(*a): return ("or", "bool", *a)                 # noqa: E704
def ITE(c, a, b): return ("ite", "bool", c, a, b)     # noqa: E704
def NOT(a): return ("not", "bool", a)                 # noqa: E704
def ZX(t, w): return ("zero_extend", w, t)            # noqa: E704
def BIT(t, i): return ("extract", 1, i, i, t)         # noqa: E704


def _countones():
    x = V("x", 8)
    acc = ZX(BIT(x, 0), 8)
    for i in range(1, 8):
        acc = B("bvadd", 8, acc, ZX(BIT(x, i), 8))
    return {"x": 8}, [P("=", acc, C(4, 8))], None, \
        "x has exactly four bits set: `$countones(x) == 4`, 70 solutions"


def _ot_aon_wkup():
    t, c = V("thold", 8), V("cnt", 8)
    return ({"thold": 8, "cnt": 8},
            [OR(P("=", t, C(0, 8)), AND(P("bvuge", t, C(5, 8)), P("bvule", t, C(0xab, 8)))),
             ITE(P("=", t, C(0, 8)), P("=", c, C(0, 8)),
                 AND(P("bvuge", c, B("bvsub", 8, t, C(5, 8))), P("bvule", c, t)))],
            lambda m: m["thold"] == 0,
            "an OpenTitan timer shape: `thold == 0` is one solution of 1003")


def _range_1000():
    r = V("r", 16)
    return {"r": 16}, [P("bvult", r, C(1000, 16))], None, \
        "a plain range, `r < 1000`: every sampler should do well"


def _disjoint_ranges():
    x = V("x", 8)
    return ({"x": 8},
            [OR(AND(P("bvuge", x, C(10, 8)), P("bvule", x, C(20, 8))),
                AND(P("bvuge", x, C(100, 8)), P("bvule", x, C(150, 8))))],
            lambda m: m["x"] <= 20,
            "`x inside {[10:20], [100:150]}`: the small band is 11 of 62 solutions")


def _sum_eq_const():
    a, b = V("a", 8), V("b", 8)
    return {"a": 8, "b": 8}, [P("=", B("bvadd", 8, a, b), C(100, 8))], None, \
        "`a + b == 100` (8-bit, wrapping): 256 solutions"


def _a_lt_b():
    a, b = V("a", 6), V("b", 6)
    return {"a": 6, "b": 6}, [P("bvult", a, b)], lambda m: m["a"] == 0, \
        "`a < b` (6-bit): 2016 solutions, an ordering"


def _dep_range():
    a, b = V("a", 8), V("b", 8)
    return ({"a": 8, "b": 8},
            [P("bvule", a, C(80, 8)), P("bvuge", b, a),
             P("bvule", b, B("bvadd", 8, a, C(10, 8)))],
            lambda m: m["a"] == 0,
            "`b inside {[a:a+10]}`, `a <= 80`: a range that depends on another variable")


def _bit_mask():
    x = V("x", 8)
    return {"x": 8}, [P("=", B("bvand", 8, x, C(0x0F, 8)), C(5, 8))], None, \
        "`(x & 'hf) == 5`: 16 solutions spread by the high bits"


def _crc_filter():
    """fw-hdl's check (docs/fw_hdl_requests.md, "Not a request: sampling
    diversity"): a letter whose CRC-32 has its two low bits clear. A hash-like
    filter: neighbouring letters are rarely both solutions, so a sampler that
    walks from one solution by single-bit flips finds no others."""
    m = V("m", 8)
    c = B("bvxor", 32, C(0xFFFFFFFF, 32), ZX(m, 32))      # one byte, table-less
    for _ in range(8):
        mask = ("bvneg", 32, B("bvand", 32, c, C(1, 32)))
        c = B("bvxor", 32, B("bvlshr", 32, c, C(1, 32)), B("bvand", 32, C(0xEDB88320, 32), mask))
    c = ("bvnot", 32, c)
    return ({"m": 8},
            [P("bvuge", m, C(ord("A"), 8)), P("bvule", m, C(ord("Z"), 8)),
             P("=", B("bvand", 32, c, C(3, 32)), C(0, 32))],
            None, "`m inside {[\"A\":\"Z\"]}` with `crc32(m)[1:0] == 0`: 6 solutions")


# ---- wide benchmarks: binned ------------------------------------------------
# `bins` is (key, truth[, draw]): key(model) names a bin, truth maps every bin
# to its share of the solution space, and draw(rng), when given, is an exact
# uniform draw for the reference arm where rejection over the bits is too slow.

def _addr_aligned():
    """A bus address: word-aligned, in a 1 GiB window. 2^28 solutions."""
    a = V("addr", 32)
    lo, hi, n = 0x1000_0000, 0x5000_0000, 256
    cons = [P("=", B("bvand", 32, a, C(3, 32)), C(0, 32)),
            P("bvuge", a, C(lo, 32)), P("bvult", a, C(hi, 32))]
    key = lambda m: (m["addr"] - lo) * n // (hi - lo)      # noqa: E731
    return ({"addr": 32}, cons, None,
            "a word-aligned 32-bit address in a 1 GiB window: about 268 million "
            "solutions, in 256 equal bins",
            (key, {i: 1 / n for i in range(n)}))


def _packet():
    """A packet header: a length that depends on the kind, and a payload tag."""
    k, ln, tag = V("kind", 2), V("len", 8), V("tag", 32)
    cons = [P("distinct", k, C(3, 2)),
            P("bvuge", ln, C(1, 8)), P("bvule", ln, C(64, 8)),
            ITE(P("=", k, C(0, 2)), P("bvule", ln, C(16, 8)), ("true",)),
            P("distinct", tag, C(0, 32))]
    # Every (kind, len) pair has the same 2^32 - 1 tags: the pairs are equally
    # likely.
    pairs = [(kd, n) for kd in range(3) for n in range(1, 65) if kd != 0 or n <= 16]
    key = lambda m: (m["kind"], m["len"])                  # noqa: E731
    return ({"kind": 2, "len": 8, "tag": 32}, cons, None,
            "a packet header: `kind != 3`, `len` in 1..64 but at most 16 when "
            "`kind == 0`, `tag != 0`; binned by (kind, len), 144 bins",
            (key, {p: 1 / len(pairs) for p in pairs}))


# ---- accuracy benchmarks: shapes a sampler gets wrong -------------------------
# From docs/accuracy_closure_plan.md. Each has far fewer solutions under some
# values than others, so drawing a value uniformly, or deciding variables one at
# a time, shows up as a skew. Sums use 32-bit arithmetic, as SV does with an
# unsized constant.

def _cond_weight():
    """A kind that selects a length limit: uniform gives P(kind) = 4 : 256 : 4096.

    Variables are declared in name order, so the condition is `kind` (before
    `len`), as an SV class would declare it. The bias depends on that order.
    """
    md, ln = V("kind", 2), V("len", 16)
    cons = [P("distinct", md, C(3, 2)),
            ITE(P("=", md, C(0, 2)), P("bvult", ln, C(4, 16)),
                ITE(P("=", md, C(1, 2)), P("bvult", ln, C(256, 16)),
                    P("bvult", ln, C(4096, 16))))]
    t = 4 + 256 + 4096
    return ({"kind": 2, "len": 16}, cons, None,
            "`kind != 3`; `len` < 4, 256 or 4096 by kind: binned by kind, "
            "true shares 4 : 256 : 4096 of 4356",
            (lambda m: m["kind"], {0: 4 / t, 1: 256 / t, 2: 4096 / t},
             lambda r: (lambda i: {"kind": 0, "len": i} if i < 4 else
                        {"kind": 1, "len": i - 4} if i < 260 else
                        {"kind": 2, "len": i - 260})(r.randrange(t))))


def _order_chain(k):
    """`a[0] < a[1] < ... < a[9]`, 8-bit: C(256,10) solutions, binned by a[k]."""
    a = [V(f"a{i}", 8) for i in range(10)]
    cons = [P("bvult", a[i], a[i + 1]) for i in range(9)]
    from math import comb
    tot = comb(256, 10)
    tr = {v: comb(v, k) * comb(255 - v, 9 - k) / tot for v in range(256)}
    tr = {v: p for v, p in tr.items() if p > 0}
    return ({f"a{i}": 8 for i in range(10)}, cons, None,
            f"`a[10]` 8-bit, `a[i] < a[i+1]`: C(256,10) solutions, binned by a[{k}]",
            (lambda m: m[f"a{k}"], tr,
             lambda r: dict(zip((f"a{i}" for i in range(10)), sorted(r.sample(range(256), 10))))))


def _ksubsets_by_sum(vals, k, s):
    dp = [[0] * (s + 1) for _ in range(k + 1)]
    dp[0][0] = 1
    for v in vals:
        for j in range(k, 0, -1):
            for t in range(s, v - 1, -1):
                dp[j][t] += dp[j - 1][t - v]
    return dp[k][s]


def _draw_sum_unique(r):
    while True:                         # a random ordered 8-subset, kept if it sums
        a = r.sample(range(1, 33), 8)
        if sum(a) == 100:
            return {f"a{i}": v for i, v in enumerate(a)}


def _draw_sum3(r):
    a = r.choices(range(201), weights=[201 - v for v in range(201)])[0]
    b = r.randrange(201 - a)
    return {"a": a, "b": b, "c": 200 - a - b}


def _sum_unique():
    """`a[8]` in [1:32], all different, `a.sum() == 100`: binned by a[0]."""
    a = [V(f"a{i}", 8) for i in range(8)]
    cons = [AND(P("bvuge", x, C(1, 8)), P("bvule", x, C(32, 8))) for x in a]
    cons += [P("distinct", a[i], a[j]) for i in range(8) for j in range(i + 1, 8)]
    acc = ZX(a[0], 32)
    for x in a[1:]:
        acc = B("bvadd", 32, acc, ZX(x, 32))
    cons.append(P("=", acc, C(100, 32)))
    full = range(1, 33)
    tot = _ksubsets_by_sum(full, 8, 100)
    tr = {}
    for v in full:
        n = _ksubsets_by_sum([u for u in full if u != v], 7, 100 - v)
        if n:
            tr[v] = n / (8 * tot)
    return ({f"a{i}": 8 for i in range(8)}, cons, None,
            "`a[8]` in [1:32], `unique {a}`, `a.sum() == 100`: binned by a[0]",
            (lambda m: m["a0"], tr, _draw_sum_unique))


def _linear_sys():
    """16-bit `a+b+c+d == 1000; a == b+c; 2*c+d <= 500`: binned by c."""
    a, b, c, d = (V(n, 16) for n in "abcd")
    z = lambda x: ZX(x, 32)                                # noqa: E731
    cons = [P("=", B("bvadd", 32, B("bvadd", 32, B("bvadd", 32, z(a), z(b)), z(c)), z(d)),
              C(1000, 32)),
            P("=", a, B("bvadd", 16, b, c)),
            P("bvule", B("bvadd", 32, B("bvmul", 32, C(2, 32), z(c)), z(d)), C(500, 32))]
    cnt = {}
    for bb in range(501):
        for cc in range(501):
            dd = 1000 - 2 * bb - 2 * cc
            if dd < 0:
                break
            if 2 * cc + dd <= 500:
                cnt[cc] = cnt.get(cc, 0) + 1
    tot = sum(cnt.values())
    sols = [(bb, cc) for bb in range(501) for cc in range(501)
            if 0 <= 1000 - 2 * bb - 2 * cc and cc + (1000 - 2 * bb - 2 * cc) <= 500 - cc]
    return ({"a": 16, "b": 16, "c": 16, "d": 16}, cons, None,
            "16-bit `a+b+c+d == 1000; a == b+c; 2*c+d <= 500`: binned by c",
            (lambda m: m["c"], {k: v / tot for k, v in cnt.items()},
             lambda r: (lambda bc: {"a": bc[0] + bc[1], "b": bc[0], "c": bc[1],
                                    "d": 1000 - 2 * bc[0] - 2 * bc[1]})(r.choice(sols))))


def _sum3_const():
    """`a + b + c == 200`, 8-bit, no wrap: P(a) is proportional to 201 - a."""
    a, b, c = V("a", 8), V("b", 8), V("c", 8)
    s = B("bvadd", 32, B("bvadd", 32, ZX(a, 32), ZX(b, 32)), ZX(c, 32))
    tot = sum(201 - v for v in range(201))
    return ({"a": 8, "b": 8, "c": 8}, [P("=", s, C(200, 32))], None,
            "`a + b + c == 200` (8-bit, 32-bit sum): binned by a, P(a) ~ 201 - a",
            (lambda m: m["a"], {v: (201 - v) / tot for v in range(201)}, _draw_sum3))


ENUMERABLE = {
    "countones": _countones, "ot_aon_wkup": _ot_aon_wkup, "range_1000": _range_1000,
    "disjoint_ranges": _disjoint_ranges, "sum_eq_const": _sum_eq_const,
    "a_lt_b": _a_lt_b, "dep_range": _dep_range, "bit_mask": _bit_mask,
    "crc_filter": _crc_filter,
}
WIDE = {"addr_aligned": _addr_aligned, "packet": _packet,
        "cond_weight": _cond_weight,
        "order_chain_a0": lambda: _order_chain(0),
        "order_chain_a5": lambda: _order_chain(5),
        "order_chain_a9": lambda: _order_chain(9),
        "sum_unique": _sum_unique, "linear_sys": _linear_sys,
        "sum3_const": _sum3_const}


class Bench:
    def __init__(self, name: str):
        self.name = name
        if name in ENUMERABLE:
            self.widths, self.cons, self.thin, self.about = ENUMERABLE[name]()
            self.bins = None
        else:
            self.widths, self.cons, self.thin, self.about, self.bins = WIDE[name]()
        self.names = sorted(self.widths)

    def smt_vlt(self) -> list:
        """Verilator-form constraints: 1-bit expressions."""
        return [f"(__Vbv {smt(c)})" for c in self.cons]

    def sha(self) -> str:
        doc = json.dumps({"widths": self.widths, "cons": [smt(c) for c in self.cons],
                          "bins": sorted(map(str, self.bins[1])) if self.bins else None}, sort_keys=True)
        return hashlib.sha256(doc.encode()).hexdigest()[:16]

    def enumerable(self) -> bool:
        return self.bins is None
