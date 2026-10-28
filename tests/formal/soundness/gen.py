"""Constrained-random problem generator; records the stimulus bins it hits.

`Gen(rng, widths, builder_safe)` draws one Problem. With `builder_safe`, only
operators whose SystemVerilog meaning (the Python/C builder's) equals their
SMT-LIB meaning are used: unsigned division and remainder by a nonzero
constant only (SV gives x for a zero divisor), no arithmetic shift right, no
signed comparisons.
"""
from __future__ import annotations

import random

from .ir import BV_BIN, BV_UN, CMP, Problem, width

_BUILDER_BIN = ["bvadd", "bvsub", "bvmul", "bvudiv", "bvurem", "bvand", "bvor", "bvxor",
                "bvshl", "bvlshr"]
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
        self.builder_safe = builder_safe
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
            if op in ("bvudiv", "bvurem") and self.builder_safe:
                # The builder shares SMT-LIB's meaning only for a nonzero
                # divisor; a constant one is what generated constraints use
                # (`slba % 8 == 0`).
                b = ("const", rng.randrange(1, 1 << w), w)
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

    def mask_const(self, w):
        """A mask: low bits, one run, a scattered pattern, or anything."""
        rng, m = self.rng, (1 << w) - 1
        r = rng.random()
        if r < 0.3:
            k = rng.randrange(1, w + 1)
            v, b = (1 << k) - 1, "low"
        elif r < 0.6:
            lo = rng.randrange(w)
            hi = rng.randrange(lo, w)
            v, b = ((1 << (hi + 1)) - 1) & ~((1 << lo) - 1), "run"
        elif r < 0.8:
            v, b = rng.choice([0x5555555555555555, 0x3333333333333333,
                               0x0F0F0F0F0F0F0F0F]) & m, "scattered"
        else:
            v, b = rng.randrange(1 << w), "any"
        self.bins.add("mask:" + b)
        return ("const", v, w)

    def mask_operand(self, w, depth):
        """What a mask is applied to: a variable, or one zero-extended,
        sliced or shifted right by a constant."""
        rng = self.rng
        r = rng.random()
        v = self.var(w)
        if r < 0.4 and v is not None:
            return v
        iws = sorted(set(self.widths.values()))
        if r < 0.6:
            iw = rng.choice(iws)
            if iw < w and self.var(iw) is not None:
                self.bins.add("mask-operand:zero_extend")
                return ("zero_extend", w, self.var(iw))
            if iw > w and self.var(iw) is not None:
                self.bins.add("mask-operand:extract")
                lo = rng.randrange(iw - w + 1)
                return ("extract", w, lo + w - 1, lo, self.var(iw))
        if r < 0.85 and v is not None:
            self.bins.add("mask-operand:lshr")
            return ("bvlshr", w, v, ("const", rng.randrange(w), w))
        return self.bv(w, depth)

    def mask_compare(self, depth):
        """(x & M) or (x | M) against a constant, or two ANDs with one mask:
        the shapes compile rewrites into bit-field comparisons."""
        rng = self.rng
        w = rng.choice(sorted(set(self.widths.values())))
        op = rng.choice(["bvand", "bvand", "bvor"])
        m = self.mask_const(w)
        x = self.mask_operand(w, depth)
        a = (op, w, x, m) if rng.random() < 0.7 else (op, w, m, x)
        if op == "bvand" and rng.random() < 0.15:
            self.bins.add("mask:masked-pair")
            b = ("bvand", w, self.mask_operand(w, depth), m)
            return (rng.choice(["=", "distinct"]), "bool", a, b)
        mv, r = m[1], rng.random()
        if r < 0.5:      # a value the masked operand can take
            v = (rng.randrange(1 << w) & mv) if op == "bvand" else (rng.randrange(1 << w) | mv)
        else:
            v = rng.randrange(1 << w)
        cmp = rng.choice(self.cmp_ops)
        self.bins.add(f"mask-cmp:{op}:{cmp}")
        c = ("const", v, w)
        return (cmp, "bool", a, c) if rng.random() < 0.7 else (cmp, "bool", c, a)

    def boolean(self, depth):
        rng = self.rng
        if rng.random() < 0.12:
            return self.mask_compare(depth)
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

    def field_word(self, depth):
        """Constraints on the fields of one word -- x[h:l] from a partition
        of x's bits, in a set, excluded, ordered or linked to other terms --
        the shape compile splits into field variables."""
        rng = self.rng
        names = [n for n, w in self.widths.items() if w >= 3]
        if not names:
            return []
        n = rng.choice(names)
        w = self.widths[n]
        x = ("var", n, w)
        cuts = sorted(rng.sample(range(1, w), min(w - 1, rng.randint(1, 4))))
        b = [0] + cuts + [w]
        fields = [(b[i + 1] - 1, b[i]) for i in range(len(b) - 1)]
        if rng.random() < 0.2:                  # one field across a cut
            self.bins.add("field:overlap")
            h, l = rng.choice(fields)
            fields.append((min(w - 1, h + 1), l))
        elif len(fields) > 2 and rng.random() < 0.5:   # a run of fields, nested
            self.bins.add("field:nested")
            i = rng.randrange(len(fields) - 1)
            j = rng.randrange(i + 1, len(fields))
            fields.append((fields[j][0], fields[i][1]))
        out = []
        for _ in range(rng.randint(2, 4)):
            h, l = rng.choice(fields)
            fw = h - l + 1
            f = ("extract", fw, h, l, x)
            r = rng.random()
            if r < 0.3:
                kind = "set"
                k = min(1 << fw, rng.randint(2, 5))
                if fw < 63:
                    vals = rng.sample(range(1 << fw), k)
                else:
                    # len(range(1 << 63)) overflows ssize_t
                    vals = set()
                    while len(vals) < k:
                        vals.add(rng.getrandbits(fw))
                    vals = sorted(vals)
                c = ("or", "bool", *[("=", "bool", f, ("const", v, fw)) for v in vals])
            elif r < 0.45:
                kind = "exclude"
                c = ("distinct", "bool", f, self.const(fw))
            elif r < 0.65:
                kind = "order"
                c = (rng.choice(["bvult", "bvule", "bvugt", "bvuge"]), "bool", f, self.const(fw))
            elif r < 0.8:
                kind = "link"
                c = (rng.choice(self.cmp_ops), "bool", f, self.bv(fw, depth))
            else:
                kind = "eq"
                c = ("=", "bool", f, self.const(fw))
            if rng.random() < 0.15:
                kind += "-neg"
                c = ("not", "bool", c)
            self.bins.add("field:" + kind)
            out.append(c)
        if rng.random() < 0.2 and len(fields) > 1:  # fields gathered, reordered
            self.bins.add("field:concat")
            (h1, l1), (h2, l2) = rng.sample(fields, 2)
            cw = h1 - l1 + h2 - l2 + 2
            cat = ("concat", cw, ("extract", h1 - l1 + 1, h1, l1, x),
                   ("extract", h2 - l2 + 1, h2, l2, x))
            out.append((rng.choice(self.cmp_ops), "bool", cat, self.const(cw)))
        if rng.random() < 0.3:
            self.bins.add("field:whole-word")
            out.append((rng.choice(self.cmp_ops), "bool", x, self.const(w)))
        return out

    def problem(self) -> Problem:
        rng = self.rng
        depth = rng.randrange(1, 4)
        cons = [self.boolean(depth) for _ in range(rng.randrange(1, 7))]
        if rng.random() < 0.15:
            cons += self.field_word(depth)
        names = list(self.widths)
        if len(names) > 1 and rng.random() < 0.15:
            a, b = rng.sample(names, 2)
            if self.widths[a] == self.widths[b]:
                self.bins.add("shape:alias")
                w = self.widths[a]
                cons.append(("=", "bool", ("var", a, w), ("var", b, w)))
                if rng.random() < 0.5:
                    # A guarded comparison on the variable compile merges away:
                    # the gated compile path must act on the kept one.
                    self.bins.add("shape:alias-guarded")
                    k = self.const(w)
                    cons.append(("ite", "bool", ("=", "bool", ("var", b, w), k),
                                 (rng.choice(["bvult", "bvugt", "="]), "bool", ("var", b, w),
                                  self.const(w)),
                                 self.boolean(0)))
        self.bins.add(f"size:{len(cons)}")
        return Problem(dict(self.widths), cons, set(self.bins))
