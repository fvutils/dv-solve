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
import math

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
def EXT(t, hi, lo): return ("extract", hi - lo + 1, hi, lo, t)  # noqa: E704


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
# `bins` is (key, truth): key(model) names a bin, truth maps every bin to its
# share of the solution space.

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


def _cond_weight():
    """mode != 3; len < 4/256/4096 by mode (item 1: condition-variable weight)."""
    md, ln = V("mode", 2), V("len", 16)
    cons = [P("distinct", md, C(3, 2)),
            ITE(P("=", md, C(0, 2)), P("bvult", ln, C(4, 16)),
                ITE(P("=", md, C(1, 2)), P("bvult", ln, C(256, 16)),
                    P("bvult", ln, C(4096, 16))))]
    t = 4 + 256 + 4096
    key = lambda m: m["mode"]                              # noqa: E731
    return ({"mode": 2, "len": 16}, cons, None,
            "`mode != 3`; `len < 4/256/4096` by mode: uniform gives "
            "P(mode) = 4 : 256 : 4096",
            (key, {0: 4 / t, 1: 256 / t, 2: 4096 / t}))


def _order_chain():
    """a[10] 8-bit strictly increasing (item 5: pure ordering chain)."""
    a = [V(f"a{i}", 8) for i in range(10)]
    cons = [P("bvult", a[i], a[i + 1]) for i in range(9)]
    tot = math.comb(256, 10)
    key = lambda m: m["a0"]                                # noqa: E731
    return ({f"a{i}": 8 for i in range(10)}, cons, None,
            "`a[10]` 8-bit, `a[i] < a[i+1]`: C(256,10) solutions, binned by "
            "a0 (skewed low)",
            (key, {k: math.comb(255 - k, 9) / tot for k in range(247)}))


def _linear_sys():
    """16-bit a+b+c+d==1000; a==b+c; 2c+d<=500 (item 0: linear-system arm)."""
    a, b, c, d = V("a", 16), V("b", 16), V("c", 16), V("d", 16)
    cons = [P("=", B("bvadd", 16, B("bvadd", 16, B("bvadd", 16, a, b), c), d), C(1000, 16)),
            P("=", a, B("bvadd", 16, b, c)),
            P("bvule", B("bvadd", 16, B("bvmul", 16, C(2, 16), c), d), C(500, 16))]

    def _solutions():
        for bb in range(501):
            for cc in range(501):
                dd = 1000 - 2 * bb - 2 * cc
                if dd < 0:
                    break
                if 2 * cc + dd <= 500 and bb + cc < 65536:
                    yield bb, cc

    cnt: dict = {}
    for _, cc in _solutions():
        cnt[cc] = cnt.get(cc, 0) + 1
    tot = sum(cnt.values())
    key = lambda m: m["c"]                                 # noqa: E731
    return ({"a": 16, "b": 16, "c": 16, "d": 16}, cons, None,
            "16-bit `a+b+c+d == 1000; a == b+c; 2*c+d <= 500`: binned by c",
            (key, {k: v / tot for k, v in cnt.items()}))


def _mod_crt():
    """32-bit x%7==3; x%11==5; x%13==2 (item 3: CRT congruence facts)."""
    x = V("x", 32)
    cons = [P("=", B("bvurem", 32, x, C(m, 32)), C(k, 32)) for m, k in ((7, 3), (11, 5), (13, 2))]
    r = next(v for v in range(1001) if v % 7 == 3 and v % 11 == 5 and v % 13 == 2)
    cnt: dict = {}
    for v in range(r, 1 << 32, 1001):
        cnt[v >> 24] = cnt.get(v >> 24, 0) + 1
    tot = sum(cnt.values())
    key = lambda m: m["x"] >> 24                           # noqa: E731
    return ({"x": 32}, cons, None,
            "32-bit `x%7==3; x%11==5; x%13==2` (CRT, ~4.3M solutions), "
            "binned by x>>24",
            (key, {k: v / tot for k, v in cnt.items()}))


def _xor_split():
    """(x^y)=='hDEADBEEF; (x&y)==0 (item 4 neighbour: bit-level split, same
    free-bit shape as $countones)."""
    k_const = 0xDEADBEEF
    x, y = V("x", 32), V("y", 32)
    cons = [P("=", B("bvxor", 32, x, y), C(k_const, 32)),
            P("=", B("bvand", 32, x, y), C(0, 32))]
    n1 = bin(k_const).count("1")
    key = lambda m: bin(m["x"]).count("1")                 # noqa: E731
    return ({"x": 32, "y": 32}, cons, None,
            "`(x ^ y) == 'hDEADBEEF; (x & y) == 0`: 2^24 solutions, binned "
            "by $countones(x) (binomial)",
            (key, {k: math.comb(n1, k) / 2 ** n1 for k in range(n1 + 1)}))


def _pop32():
    """$countones(x)==16; x[3:0]=='hA (item 4: exact countones draw)."""
    x = V("x", 32)
    acc = ZX(BIT(x, 0), 6)
    for i in range(1, 32):
        acc = B("bvadd", 6, acc, ZX(BIT(x, i), 6))
    cons = [P("=", acc, C(16, 6)), P("=", EXT(x, 3, 0), C(0xA, 4))]
    tot = math.comb(28, 14)
    key = lambda m: m["x"] >> 28                           # noqa: E731
    return ({"x": 32}, cons, None,
            "`$countones(x) == 16; x[3:0] == 'hA` (32-bit): C(28,14) "
            "solutions, binned by x[31:28]",
            (key, {t: math.comb(24, 14 - bin(t).count("1")) / tot for t in range(16)}))


def _align64():
    """64-bit addr[11:0]==0 in [2^40, 2^48) (item 0: 64-bit timing arm)."""
    lo, hi = 1 << 40, 1 << 48
    a = V("addr", 64)
    cons = [P("=", EXT(a, 11, 0), C(0, 12)),
            P("bvuge", a, C(lo, 64)), P("bvult", a, C(hi, 64))]
    key = lambda m: m["addr"] >> 40                        # noqa: E731
    return ({"addr": 64}, cons, None,
            "64-bit `addr[11:0] == 0`, in [2^40, 2^48): binned by addr>>40 "
            "(255 equal bins)",
            (key, {k: 1 / 255 for k in range(1, 256)}))


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


ENUMERABLE = {
    "countones": _countones, "ot_aon_wkup": _ot_aon_wkup, "range_1000": _range_1000,
    "disjoint_ranges": _disjoint_ranges, "sum_eq_const": _sum_eq_const,
    "a_lt_b": _a_lt_b, "dep_range": _dep_range, "bit_mask": _bit_mask,
    "crc_filter": _crc_filter,
}
WIDE = {
    "addr_aligned": _addr_aligned, "packet": _packet,
    "cond_weight": _cond_weight, "order_chain": _order_chain,
    "linear_sys": _linear_sys, "mod_crt": _mod_crt, "xor_split": _xor_split,
    "pop32": _pop32, "align64": _align64,
}


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
