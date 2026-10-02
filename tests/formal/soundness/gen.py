"""Constrained-random problem generator; records the stimulus bins it hits.

`Gen(rng, widths, builder_safe)` draws one Problem. With `builder_safe`, only
operators whose SystemVerilog meaning (the Python/C builder's) equals their
SMT-LIB meaning are used: no division (SV gives x for a zero divisor), no
arithmetic shift right, no signed comparisons.
"""
from __future__ import annotations

import random

from .ir import BV_BIN, BV_UN, CMP, Problem, width

_BUILDER_BIN = ["bvadd", "bvsub", "bvmul", "bvand", "bvor", "bvxor", "bvshl", "bvlshr"]
_BUILDER_CMP = ["=", "distinct", "bvult", "bvule", "bvugt", "bvuge"]

# Variable layouts, all within ~12 free bits so enumeration stays cheap.
WIDTHS = [{"x": 4, "y": 4, "z": 4}, {"x": 3, "y": 3, "z": 6}, {"x": 6, "y": 6},
          {"a": 1, "x": 5, "y": 5}, {"x": 8, "y": 4}, {"x": 2, "y": 2, "z": 2, "u": 2}]

# Wide layouts: the 2^31 / 2^32 / 2^63 / 2^64 boundaries and the >64-bit
# (bitblast) path. Too wide to enumerate; z3 is the oracle.
WIDE_WIDTHS = [{"x": 16, "y": 16}, {"x": 32, "y": 32}, {"x": 33, "y": 33, "z": 32},
               {"x": 63, "y": 63}, {"x": 64, "y": 64}, {"x": 64, "y": 64, "z": 64},
               {"x": 65, "y": 65}, {"a": 1, "x": 64, "y": 32}]


WIDTH_CLASSES = ["1-8", "9-16", "17-32", "33-63", "64", "65+"]


def width_class(w: int) -> str:
    """Bucket a term width for the operator x width cross."""
    if w <= 8: return "1-8"
    if w <= 16: return "9-16"
    if w <= 32: return "17-32"
    if w <= 63: return "33-63"
    return "64" if w == 64 else "65+"


class Gen:
    def __init__(self, rng: random.Random, widths: dict, builder_safe: bool = False):
        self.rng = rng
        self.widths = widths
        self.bin_ops = _BUILDER_BIN if builder_safe else BV_BIN
        self.cmp_ops = _BUILDER_CMP if builder_safe else CMP
        self.pool: list = []      # built terms, for sharing
        self.bins: set = set()

    def const(self, w):
        r, m = self.rng.random(), (1 << w) - 1
        if r < 0.15: v, b = 0, "zero"
        elif r < 0.3: v, b = m, "max"
        elif r < 0.4: v, b = 1 << (w - 1), "sign-min"
        elif r < 0.5: v, b = (1 << (w - 1)) - 1, "sign-max"
        elif r < 0.6: v, b = 1, "one"
        else: v, b = self.rng.randrange(1 << w), "interior"
        self.bins.add("const:" + b)
        return ("const", v, w)

    def var(self, w):
        names = [n for n, vw in self.widths.items() if vw == w]
        return ("var", self.rng.choice(names), w) if names else None

    def bv(self, w, depth):
        rng = self.rng
        same = [t for t in self.pool if width(t) == w]
        if same and rng.random() < 0.15:
            self.bins.add("shape:shared-subterm")
            return rng.choice(same)
        if depth <= 0 or rng.random() < 0.35:
            v = self.var(w)
            if v is not None and rng.random() < 0.75:
                return v
            return self.const(w)
        r = rng.random()
        if r < 0.55:
            op = rng.choice(self.bin_ops)
            a = self.bv(w, depth - 1)
            b = a if rng.random() < 0.08 else self.bv(w, depth - 1)
            if a is b:
                self.bins.add("shape:same-operand")
            t = (op, w, a, b)
        elif r < 0.65:
            t = (rng.choice(BV_UN), w, self.bv(w, depth - 1))
        elif r < 0.75:
            t = ("ite", w, self.boolean(depth - 1), self.bv(w, depth - 1), self.bv(w, depth - 1))
        elif r < 0.85:
            iw = rng.choice(sorted(set(self.widths.values())))
            if iw == w:
                return self.bv(w, depth - 1)
            if iw < w:
                t = (rng.choice(["zero_extend", "sign_extend"]), w, self.bv(iw, depth - 1))
            else:
                lo = rng.randrange(iw - w + 1)
                t = ("extract", w, lo + w - 1, lo, self.bv(iw, depth - 1))
        else:
            if w < 2:
                return self.bv(w, depth - 1)
            hw = rng.randrange(1, w)
            t = ("concat", w, self.bv(hw, depth - 1), self.bv(w - hw, depth - 1))
        self.bins.add("op:" + t[0])
        self.bins.add(f"opw:{t[0]}:{width_class(w)}")
        self.pool.append(t)
        return t

    def boolean(self, depth):
        rng = self.rng
        if depth <= 0 or rng.random() < 0.7:
            w = rng.choice(sorted(set(self.widths.values())))
            op = rng.choice(self.cmp_ops)
            self.bins.add("cmp:" + op)
            return (op, "bool", self.bv(w, depth), self.bv(w, depth))
        r = rng.random()
        if r < 0.35:
            n = rng.choice([2, 2, 3, 5])
            self.bins.add(f"bool:or{n}")
            return ("or", "bool", *[self.boolean(depth - 1) for _ in range(n)])
        if r < 0.55:
            self.bins.add("bool:not")
            return ("not", "bool", self.boolean(depth - 1))
        if r < 0.75:
            self.bins.add("bool:=>")
            return ("=>", "bool", self.boolean(depth - 1), self.boolean(depth - 1))
        if r < 0.9:
            # Boolean ite; x == K and x == y conditions take the guarded
            # compile paths (B53).
            self.bins.add("bool:ite")
            if rng.random() < 0.4:
                w = rng.choice(sorted(set(self.widths.values())))
                v = self.var(w)
                c = ("=", "bool", v, self.const(w) if rng.random() < 0.6 else (self.var(w) or v))
                self.bins.add("bool:ite-eq-cond")
            else:
                c = self.boolean(depth - 1)
            return ("ite", "bool", c, self.boolean(depth - 1), self.boolean(depth - 1))
        if r < 0.95:
            self.bins.add("bool:xor")
            return ("xor", "bool", self.boolean(depth - 1), self.boolean(depth - 1))
        self.bins.add("bool:and")
        return ("and", "bool", self.boolean(depth - 1), self.boolean(depth - 1))

    def problem(self) -> Problem:
        rng = self.rng
        depth = rng.randrange(1, 4)
        cons = [self.boolean(depth) for _ in range(rng.randrange(1, 7))]
        names = list(self.widths)
        if len(names) > 1 and rng.random() < 0.15:
            a, b = rng.sample(names, 2)
            if self.widths[a] == self.widths[b]:
                self.bins.add("shape:alias")
                cons.append(("=", "bool", ("var", a, self.widths[a]), ("var", b, self.widths[b])))
        self.bins.add(f"size:{len(cons)}")
        return Problem(dict(self.widths), cons, set(self.bins))
