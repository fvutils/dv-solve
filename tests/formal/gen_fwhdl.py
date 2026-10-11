"""Generate the fwhdl-core benchmark families (docs/fw_hdl_implementation_plan.md §3.2).

    python tests/formal/gen_fwhdl.py            # (re)write tests/formal/smt2/fw_hdl/gen/
    python tests/formal/gen_fwhdl.py --list     # print the instances, write nothing

The workload is fw-hdl's: proofs over unrolled bit-vector datapaths (CRCs,
LFSRs, checksums, hashes, ECC), counterexamples to wrong properties, and
inverse queries. Each family is written once in a small expression language
(`N` below) that both prints SMT-LIB, in the shape fw-hdl's printer emits (one
`define-fun` per function, its body a chain of `let`s), and evaluates on
concrete values. The generator uses the evaluation to check every instance
before writing it: a property claimed to hold must hold on a few thousand
samples, and a property claimed to fail must have a witness that falsifies it.
So `:status` comes from construction, and tests/formal/fwhdl_consensus.py
then confirms it with the pinned competitors.

Output, per instance: gen/<category>/<id>.smt2. gen/index.json lists every
instance with its family, knobs, split and expected status. The split is dev
(70 %) or held-out (30 %), by a hash of the instance id; tuning looks only at
dev (plan §3.4).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import random
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
OUT = _HERE / "smt2" / "fw_hdl" / "gen"
CATEGORIES = ("affine-proof", "mixed-proof", "refute", "inverse", "codegen-equiv",
              "quickcheck-trivial")


# ---------------------------------------------------------------------------
# Expression language. A node is a bit-vector (w > 0) or a Bool (w == 0).

class N:
    __slots__ = ("op", "args", "w", "p")

    def __init__(self, op, args, w, p=None):
        self.op, self.args, self.w, self.p = op, tuple(args), w, p

    # bit-vector operators; an int operand takes the other operand's width
    def _bin(self, op, o):
        o = _lift(o, self.w)
        assert self.w and self.w == o.w, (op, self.w, o.w)
        return N(op, (self, o), self.w)

    def __xor__(self, o): return self._bin("bvxor", o)
    def __and__(self, o): return self._bin("bvand", o)
    def __or__(self, o): return self._bin("bvor", o)
    def __add__(self, o): return self._bin("bvadd", o)
    def __sub__(self, o): return self._bin("bvsub", o)
    def __mul__(self, o): return self._bin("bvmul", o)
    def __rxor__(self, o): return _lift(o, self.w) ^ self
    def __rand__(self, o): return _lift(o, self.w) & self
    def __radd__(self, o): return _lift(o, self.w) + self
    def __rsub__(self, o): return _lift(o, self.w) - self
    def __invert__(self): return N("bvnot", (self,), self.w)
    def __neg__(self): return N("bvneg", (self,), self.w)
    def __rshift__(self, o): return self._bin("bvlshr", o)
    def __lshift__(self, o): return self._bin("bvshl", o)
    def urem(self, o): return self._bin("bvurem", o)
    def udiv(self, o): return self._bin("bvudiv", o)
    def bit(self, i): return self.ext(i, i)

    def ext(self, hi, lo):
        assert 0 <= lo <= hi < self.w
        return N("extract", (self,), hi - lo + 1, (hi, lo))

    def zext(self, w):
        assert w >= self.w
        return self if w == self.w else N("zero_extend", (self,), w, w - self.w)


def _lift(v, w):
    return v if isinstance(v, N) else const(v, w)


def const(v, w):
    return N("const", (), w, v & ((1 << w) - 1))


def var(name, w):
    return N("var", (), w, name)


def concat(*xs):                 # most significant first
    out = xs[0]
    for x in xs[1:]:
        out = N("concat", (out, x), out.w + x.w)
    return out


def eq(a, b):
    b = _lift(b, a.w)
    assert a.w == b.w
    return N("=", (a, b), 0)


def ult(a, b):
    b = _lift(b, a.w)
    return N("bvult", (a, b), 0)


def ite(c, a, b):
    assert c.w == 0
    b = _lift(b, a.w)
    a = _lift(a, b.w)
    assert a.w == b.w
    return N("ite", (c, a, b), a.w)


def lnot(a): return N("not", (a,), 0)
def land(*xs): return xs[0] if len(xs) == 1 else N("and", xs, 0)
def lor(*xs): return xs[0] if len(xs) == 1 else N("or", xs, 0)
def implies(a, b): return N("=>", (a, b), 0)
def ne(a, b): return lnot(eq(a, b))


class Fun:
    """A define-fun: `body(*params) -> N`, traced once over parameter nodes."""

    def __init__(self, name, params, body):
        self.name, self.params = name, params          # [(name, width)]
        self.pnodes = [N("param", (), w, n) for n, w in params]
        self.root = body(*self.pnodes)
        self.w = self.root.w

    def __call__(self, *args):
        assert len(args) == len(self.params)
        args = [_lift(a, w) for a, (_, w) in zip(args, self.params)]
        for a, (_, w) in zip(args, self.params):
            assert a.w == w, (self.name, a.w, w)
        return N("call", args, self.w, self)


# ---- evaluation -------------------------------------------------------------

def _sx(v, w):
    return v - (1 << w) if v >> (w - 1) & 1 else v


def evaluate(root, env, _memo=None):
    """Value of `root` (int, or bool for w == 0); env maps var and param names."""
    memo = {} if _memo is None else _memo
    stack = [root]
    while stack:
        n = stack[-1]
        if id(n) in memo:
            stack.pop()
            continue
        if n.op == "call":
            pend = [a for a in n.args if id(a) not in memo]
            if pend:
                stack += pend
                continue
            f = n.p
            sub = {pn.p: memo[id(a)] for pn, a in zip(f.pnodes, n.args)}
            memo[id(n)] = evaluate(f.root, sub)
            stack.pop()
            continue
        pend = [a for a in n.args if id(a) not in memo]
        if pend:
            stack += pend
            continue
        stack.pop()
        memo[id(n)] = _apply(n, [memo[id(a)] for a in n.args], env)
    return memo[id(root)]


def _apply(n, a, env):
    w, m = n.w, (1 << n.w) - 1 if n.w else 0
    op = n.op
    if op == "const":
        return n.p
    if op in ("var", "param"):
        return env[n.p]
    if op == "bvxor": return a[0] ^ a[1]
    if op == "bvand": return a[0] & a[1]
    if op == "bvor": return a[0] | a[1]
    if op == "bvnot": return ~a[0] & m
    if op == "bvneg": return -a[0] & m
    if op == "bvadd": return (a[0] + a[1]) & m
    if op == "bvsub": return (a[0] - a[1]) & m
    if op == "bvmul": return (a[0] * a[1]) & m
    if op == "bvlshr": return a[0] >> a[1] if a[1] < w else 0
    if op == "bvshl": return (a[0] << a[1]) & m if a[1] < w else 0
    if op == "bvurem": return a[0] % a[1] if a[1] else a[0]
    if op == "bvudiv": return a[0] // a[1] if a[1] else m
    if op == "extract":
        hi, lo = n.p
        return (a[0] >> lo) & ((1 << (hi - lo + 1)) - 1)
    if op == "zero_extend": return a[0]
    if op == "concat": return (a[0] << n.args[1].w) | a[1]
    if op == "=": return a[0] == a[1]
    if op == "bvult": return a[0] < a[1]
    if op == "ite": return a[1] if a[0] else a[2]
    if op == "not": return not a[0]
    if op == "and": return all(a)
    if op == "or": return any(a)
    if op == "=>": return (not a[0]) or a[1]
    raise ValueError(op)


# ---- printing ---------------------------------------------------------------

def _sort(w):
    return "Bool" if w == 0 else f"(_ BitVec {w})"


def _q(name):
    return name if name.replace("_", "a").isalnum() and not name[0].isdigit() else f"|{name}|"


def _atom(n):
    if n.op == "const":
        return f"(_ bv{n.p} {n.w})"
    if n.op in ("var", "param"):
        return _q(n.p)
    return None


def _head(n):
    if n.op == "extract":
        return f"(_ extract {n.p[0]} {n.p[1]})"
    if n.op == "zero_extend":
        return f"(_ zero_extend {n.p})"
    if n.op == "call":
        return _q(n.p.name)
    return n.op


def emit_lets(root, label):
    """`root` as a chain of lets, one per operator node in evaluation order,
    named <op>.<k> as fw-hdl's printer names them."""
    order, seen, stack = [], set(), [(root, False)]
    while stack:
        n, done = stack.pop()
        if _atom(n) is not None or id(n) in seen:
            continue
        if done:
            seen.add(id(n))
            order.append(n)
            continue
        stack.append((n, True))
        for a in reversed(n.args):
            stack.append((a, False))
    names = {}
    for k, n in enumerate(order):
        names[id(n)] = f"|{label(n)}.{k}|"

    def ref(n):
        return _atom(n) or names[id(n)]

    def term(n):
        return f"({_head(n)} {' '.join(ref(a) for a in n.args)})"

    if not order:
        return ref(root)
    lines = [f"  (let (({names[id(n)]} {term(n)}))" for n in order[:-1]]
    body = term(order[-1])
    return "\n".join(lines + [f"  {body}" + ")" * (len(order) - 1)])


def _label(n):
    return {"bvxor": "xor", "bvand": "and", "bvor": "or", "bvnot": "not", "bvneg": "neg",
            "bvadd": "add", "bvsub": "sub", "bvmul": "umul", "bvlshr": "shrl",
            "bvshl": "shll", "bvurem": "umod", "bvudiv": "udiv", "zero_extend": "zero_ext",
            "extract": "bit_slice", "call": "invoke", "=": "eq", "bvult": "ult",
            "=>": "implies"}.get(n.op, n.op)


def define_fun(f):
    ps = " ".join(f"({_q(n)} {_sort(w)})" for n, w in f.params)
    return f"(define-fun {_q(f.name)} ({ps}) {_sort(f.w)}\n{emit_lets(f.root, _label)})"


def _funs_of(root):
    """Functions reachable from root, callees first."""
    out, seen = [], set()

    def visit(n, vseen):
        stack = [n]
        while stack:
            x = stack.pop()
            if id(x) in vseen:
                continue
            vseen.add(id(x))
            if x.op == "call" and x.p.name not in seen:
                visit(x.p.root, set())
                if x.p.name not in seen:
                    seen.add(x.p.name)
                    out.append(x.p)
            stack.extend(x.args)
    visit(root, set())
    return out


def _vars_of(root):
    out, seen, stack = {}, set(), [root]
    while stack:
        x = stack.pop()
        if id(x) in seen:
            continue
        seen.add(id(x))
        if x.op == "var":
            out[x.p] = x.w
        stack.extend(x.args)
    return sorted(out.items())


# ---------------------------------------------------------------------------
# Instances

class Inst:
    def __init__(self, cat, family, knobs, prop, expect, about, seed=0,
                 witness=None, show=None):
        """prop: a Bool node. expect "unsat": prop holds for all inputs, and the
        file asserts its negation. expect "sat" with `show`: the file asserts
        prop itself (an inverse query); otherwise its negation (a refutation).
        witness: inputs that satisfy what the file asserts (sat only)."""
        self.cat, self.family, self.knobs = cat, family, dict(knobs)
        self.prop, self.expect, self.about, self.seed = prop, expect, about, seed
        self.witness, self.show = witness, show
        k = "_".join(f"{a}{b}" for a, b in self.knobs.items())
        self.id = f"{family}_{k}" + (f"_s{seed}" if seed else "")
        h = int(hashlib.sha256(self.id.encode()).hexdigest()[:8], 16)
        self.split = "held" if h % 10 < 3 else "dev"

    @property
    def asserted(self):
        return self.prop if self.show else lnot(self.prop)

    def text(self):
        vs = _vars_of(self.asserted)
        out = [f"; fwhdl-core {self.cat}: {self.family} " +
               " ".join(f"{a}={b}" for a, b in self.knobs.items()) +
               (f" seed={self.seed}" if self.seed else ""),
               f"; {self.about}",
               "; Generated by tests/formal/gen_fwhdl.py; do not edit.",
               "(set-info :smt-lib-version 2.6)",
               '(set-info :source |fw-hdl workload family, dv-solve tests/formal/gen_fwhdl.py|)',
               f"(set-info :status {self.expect})",
               "(set-option :produce-models true)",
               "(set-logic QF_BV)"]
        out += [define_fun(f) for f in _funs_of(self.asserted)]
        out += [f"(declare-const {_q(n)} {_sort(w)})" for n, w in vs]
        out.append(f"(assert\n{emit_lets(self.asserted, _label)})")
        out.append("(check-sat)")
        if self.expect == "sat":
            out.append(f"(get-value ({' '.join(_q(n) for n, _ in vs)}))")
        out.append("(exit)")
        return "\n".join(out) + "\n"

    def check(self, rng, samples=300):
        """Self-check against the construction; raises on a wrong claim."""
        vs = _vars_of(self.asserted)
        if self.expect == "sat":
            w = self.witness
            assert w is not None, f"{self.id}: sat instance without a witness"
            assert set(w) == {n for n, _ in vs}, (self.id, sorted(w))
            assert evaluate(self.asserted, w) is True, f"{self.id}: witness does not satisfy"
            return
        for i in range(samples):
            env = {n: _sample(rng, wd, i) for n, wd in vs}
            assert evaluate(self.prop, env) is True, f"{self.id}: property false at {env}"


def _sample(rng, w, i):
    edge = [0, (1 << w) - 1, 1, 1 << (w - 1)]
    if i < len(edge):
        return edge[i] & ((1 << w) - 1)
    return rng.getrandbits(w)


def find_witness(prop, vs, rng, extra=(), tries=20000):
    """Inputs making prop false, from edge values, `extra` candidates and
    random samples; None if none found."""
    for env in extra:
        if evaluate(prop, env) is False:
            return env
    for i in range(tries):
        env = {n: _sample(rng, w, i) for n, w in vs}
        if evaluate(prop, env) is False:
            return env
    return None


def refute(family, knobs, prop, about, rng, extra=(), seed=0, cat="refute"):
    vs = _vars_of(prop)
    w = find_witness(prop, vs, rng, extra)
    assert w is not None, f"{family} {knobs}: mutant has no counterexample found"
    return Inst(cat, family, knobs, prop, "sat", about, seed=seed, witness=w)


# ---------------------------------------------------------------------------
# Families

# reflected polynomials, init, xorout
CRCS = {8: (0x8C, 0x00, 0x00),                         # CRC-8/MAXIM
        16: (0xA001, 0x0000, 0x0000),                  # CRC-16/ARC
        32: (0xEDB88320, 0xFFFFFFFF, 0xFFFFFFFF),      # CRC-32
        64: (0xC96C5795D7870F42, (1 << 64) - 1, (1 << 64) - 1)}   # CRC-64/XZ


def crc_bitwise(W, nbytes, poly=None, tag=""):
    """Table-less CRC (Hacker's Delight crc32b), the fw-hdl crc32 example
    generalised: one define-fun per byte step, the 8-round loop unrolled."""
    p, init, xorout = CRCS[W]
    p = p if poly is None else poly

    def byte_step(byte, polynomial, crc):
        crc = crc ^ byte.zext(W)
        for _ in range(8):
            mask = -(crc & 1)
            crc = (crc >> 1) ^ (polynomial & mask)
        return crc
    step = Fun(f"crc{W}{tag}_one_byte", [("byte_", 8), ("polynomial", W), ("crc", W)], byte_step)

    def main(msg):
        crc = const(init, W)
        for i in range(nbytes):
            crc = step(msg.ext(8 * i + 7, 8 * i), p, crc)
        return crc ^ xorout
    return Fun(f"crc{W}{tag}_main", [("message", 8 * nbytes)], main)


def crc_nibble_table(W, nbytes):
    """Half-byte table CRC (16-entry table, a common firmware form)."""
    p, init, xorout = CRCS[W]

    def tbl(i):
        c = i
        for _ in range(4):
            c = (c >> 1) ^ (p if c & 1 else 0)
        return c

    def table(idx):
        out = const(tbl(15), W)
        for i in range(14, -1, -1):
            out = ite(eq(idx, i), const(tbl(i), W), out)
        return out
    T = Fun(f"crc{W}_nibble_table", [("index", 4)], table)

    def main(msg):
        crc = const(init, W)
        for i in range(nbytes):
            b = msg.ext(8 * i + 7, 8 * i)
            for nib in (b.ext(3, 0), b.ext(7, 4)):
                crc = (crc >> 4) ^ T((crc ^ nib.zext(W)).ext(3, 0))
        return crc ^ xorout
    return Fun(f"crc{W}_table_main", [("message", 8 * nbytes)], main)


def xor_all(xs):
    out = xs[0]
    for x in xs[1:]:
        out = out ^ x
    return out


def affine_prop(f, k, w_in, affine=True, drop_const=False):
    """f(x1)^...^f(xk) [^f(0) when k is even] == f(x1^...^xk): holds for an
    affine f. drop_const breaks it for a strictly affine f."""
    xs = [var(chr(ord("a") + i), w_in) for i in range(k)]
    lhs = [f(x) for x in xs]
    if affine and k % 2 == 0 and not drop_const:
        lhs.append(f(const(0, w_in)))
    return eq(xor_all(lhs), f(xor_all(xs)))


FIB_TAPS = {16: (15, 14, 12, 3), 32: (31, 21, 1, 0), 64: (63, 62, 60, 59)}


def lfsr(W, rounds, taps=None, tag=""):
    taps = taps or FIB_TAPS[W]

    def step(s):
        fb = xor_all([s.bit(t) for t in taps])
        return concat(s.ext(W - 2, 0), fb)
    st = Fun(f"lfsr{W}{tag}_step", [("state", W)], step)

    def main(s):
        for _ in range(rounds):
            s = st(s)
        return s
    return Fun(f"lfsr{W}{tag}_r{rounds}", [("state", W)], main)


XORSHIFT = {16: (7, 9, 8), 32: (13, 17, 5), 64: (13, 7, 17)}


def xorshift(W, rounds, triple=None, tag=""):
    a, b, c = triple or XORSHIFT[W]

    def step(x):
        x = x ^ (x << a)
        x = x ^ (x >> b)
        return x ^ (x << c)
    st = Fun(f"xorshift{W}{tag}_step", [("x", W)], step)

    def main(x):
        for _ in range(rounds):
            x = st(x)
        return x
    return Fun(f"xorshift{W}{tag}_r{rounds}", [("x", W)], main)


def gray(W, form, early=False):
    enc = Fun(f"gray{W}_encode", [("x", W)], lambda x: x ^ (x >> 1))
    if form == "log":
        def dec(g):
            sh = 1
            while sh < W:
                g = g ^ (g >> sh)
                sh *= 2
            return g
    else:
        def dec(g):
            n, m = g, g >> 1
            for _ in range(W - (2 if early else 1)):
                n = n ^ m
                m = m >> 1
            return n
    d = Fun(f"gray{W}_decode_{form}", [("g", W)], dec)
    x = var("x", W)
    return eq(d(enc(x)), x)


def parity(W, broken=False):
    def loop(x):
        p = x.bit(0)
        for i in range(1, W):
            p = p ^ x.bit(i)
        return p

    def fold(x):
        sh = W // 2
        while sh >= 1:
            if not (broken and sh == 2):
                x = x ^ (x >> sh)
            sh //= 2
        return x.bit(0)
    a = Fun(f"parity{W}_loop", [("x", W)], loop)
    b = Fun(f"parity{W}_fold", [("x", W)], fold)
    x = var("x", W)
    return eq(a(x), b(x))


def fletcher16(nbytes, fold_mutant=False):
    def red(x):                     # x mod 255
        if fold_mutant:             # end-around-carry fold without the final fix-up
            x = (x & 0xFF) + (x >> 8)
            return (x & 0xFF) + (x >> 8)
        return x.urem(255)

    def modular(msg):
        s1, s2 = const(0, 16), const(0, 16)
        for i in range(nbytes):
            s1 = red(s1 + msg.ext(8 * i + 7, 8 * i).zext(16))
            s2 = red(s2 + s1)
        return concat(s2.ext(7, 0), s1.ext(7, 0))

    def deferred(msg):
        s1, s2 = const(0, 32), const(0, 32)
        for i in range(nbytes):
            s1 = s1 + msg.ext(8 * i + 7, 8 * i).zext(32)
            s2 = s2 + s1
        return concat(s2.urem(255).ext(7, 0), s1.urem(255).ext(7, 0))
    tag = "_fold" if fold_mutant else ""
    a = Fun(f"fletcher16_modular{tag}_n{nbytes}", [("message", 8 * nbytes)], modular)
    b = Fun(f"fletcher16_deferred_n{nbytes}", [("message", 8 * nbytes)], deferred)
    return a, b


def adler32(nbytes):
    M = 65521

    def modular(msg):
        s1, s2 = const(1, 32), const(0, 32)
        for i in range(nbytes):
            s1 = (s1 + msg.ext(8 * i + 7, 8 * i).zext(32)).urem(M)
            s2 = (s2 + s1).urem(M)
        return concat(s2.ext(15, 0), s1.ext(15, 0))

    def deferred(msg):
        s1, s2 = const(1, 64), const(0, 64)
        for i in range(nbytes):
            s1 = s1 + msg.ext(8 * i + 7, 8 * i).zext(64)
            s2 = s2 + s1
        return concat(s2.urem(M).ext(15, 0), s1.urem(M).ext(15, 0))
    a = Fun(f"adler32_modular_n{nbytes}", [("message", 8 * nbytes)], modular)
    b = Fun(f"adler32_deferred_n{nbytes}", [("message", 8 * nbytes)], deferred)
    return a, b


FNV = {32: (0x811C9DC5, 0x01000193, (24, 8, 7, 4, 1, 0)),
       64: (0xCBF29CE484222325, 0x100000001B3, (40, 8, 7, 5, 4, 1, 0))}


def fnv1a(W, nbytes, form, drop_shift=None):
    off, prime, shifts = FNV[W]
    assert sum(1 << s for s in shifts) == prime

    def mul(h):
        if form == "mul":
            return h * prime
        return xor_free_sum([h << s if s else h for s in shifts if s != drop_shift])

    def main(msg):
        h = const(off, W)
        for i in range(nbytes):
            h = mul(h ^ msg.ext(8 * i + 7, 8 * i).zext(W))
        return h
    tag = form + (f"_no{drop_shift}" if drop_shift is not None else "")
    return Fun(f"fnv1a{W}_{tag}_n{nbytes}", [("message", 8 * nbytes)], main)


def xor_free_sum(xs):
    out = xs[0]
    for x in xs[1:]:
        out = out + x
    return out


def _fmix_params(W):
    s1 = W // 2
    s2 = max(1, (13 * W + 16) // 32)
    m = (1 << W) - 1
    return s1, s2, s1, (0x85EBCA6B & m) | 1, (0xC2B2AE35 & m) | 1


def fmix(W, even=False, consts=None):
    """murmur3's fmix32 finaliser, scaled to W bits (the multipliers truncated
    and kept odd, so it stays a bijection). even: first multiplier made even."""
    s1, s2, s3, c1, c2 = _fmix_params(W)
    if consts:
        c1, c2 = consts
    if even:
        c1 &= ~1

    def body(h):
        h = h ^ (h >> s1)
        h = h * c1
        h = h ^ (h >> s2)
        h = h * c2
        return h ^ (h >> s3)
    return Fun(f"fmix{W}" + ("_even" if even else "") + (f"_{c1:x}_{c2:x}" if consts else ""),
               [("h", W)], body)


def fmix_inverse(W):
    s1, s2, s3, c1, c2 = _fmix_params(W)
    m = 1 << W

    def unxs(h, s):                   # inverse of h ^= h >> s
        out, sh = h, s
        while sh < W:
            out = h ^ (out >> s)
            sh += s
        return out

    def body(h):
        h = unxs(h, s3)
        h = h * pow(c2, -1, m)
        h = unxs(h, s2)
        h = h * pow(c1, -1, m)
        return unxs(h, s1)
    return Fun(f"fmix{W}_inverse", [("h", W)], body)


def _fmix_eval(W, x, even=False):
    s1, s2, s3, c1, c2 = _fmix_params(W)
    if even:
        c1 &= ~1
    m = (1 << W) - 1
    x ^= x >> s1
    x = x * c1 & m
    x ^= x >> s2
    x = x * c2 & m
    return x ^ (x >> s3)


def _unxs_eval(W, h, s):
    out = h
    for _ in range(W):
        out = h ^ (out >> s)
    return out


def secded(d):
    """Extended Hamming SECDED over d data bits: decode(encode(x) ^ e) == x
    for every error e of weight at most one."""
    r = 1
    while (1 << r) < d + r + 1:
        r += 1
    n = d + r                                    # positions 1..n; bit 0 is overall parity
    data_pos = [p for p in range(1, n + 1) if p & (p - 1)][:d]
    assert len(data_pos) == d

    def encode(x):
        bits = {p: x.bit(i) for i, p in enumerate(data_pos)}
        for j in range(r):
            pp = 1 << j
            bits[pp] = xor_all([bits[p] for p in data_pos if p & pp])
        bits[0] = xor_all([bits[p] for p in range(1, n + 1)])
        return concat(*[bits[p] for p in range(n, -1, -1)])

    def decode(c):
        syn = concat(*[xor_all([c.bit(p) for p in range(1, n + 1) if p & (1 << j)])
                       for j in range(r - 1, -1, -1)])
        flip = ite(eq(syn, 0), const(0, n + 1), const(1, n + 1) << syn.zext(n + 1))
        c = c ^ flip
        return concat(*[c.bit(p) for p in reversed(data_pos)])
    E = Fun(f"secded{d}_encode", [("data", d)], encode)
    D = Fun(f"secded{d}_decode", [("codeword", n + 1)], decode)
    return E, D, n + 1


def double_errors(d, n, rng):
    """Candidate witnesses for a SECDED double-error refutation: random data,
    two flipped bits (random errors are almost never of weight <= 2)."""
    return [{"x": rng.getrandbits(d), "e": (1 << i) | (1 << j)}
            for i in range(n) for j in range(i + 1, n)]


def weight_at_most(e, k):
    x = e
    for _ in range(k):
        x = x & (x - 1)
    return eq(x, 0)


def popcount(W, bad_mask=False, bad_shift=False):
    lw = W.bit_length() + 1

    def naive(x):
        return xor_free_sum([x.bit(i).zext(lw) for i in range(W)])

    def rep(b):
        return int(b * (W // 8), 16)

    def swar(x):
        m1, m2, m4, h01 = rep("55"), rep("33" if not bad_mask else "3b"), rep("0f"), rep("01")
        x = x - ((x >> 1) & m1)
        x = (x & m2) + ((x >> 2) & m2)
        x = (x + (x >> 4)) & m4
        x = (x * h01) >> (W - 8 - (1 if bad_shift else 0))
        return x.ext(lw - 1, 0)
    a = Fun(f"popcount{W}_naive", [("x", W)], naive)
    b = Fun(f"popcount{W}_swar" + ("_bad" if bad_mask or bad_shift else ""), [("x", W)], swar)
    return a, b


def clz(W, no_zero_check=False):
    lw = W.bit_length()

    def priority(x):
        n = const(W, lw)
        for i in range(W):
            n = ite(eq(x.bit(i), 1), const(W - 1 - i, lw), n)
        return n

    def bsearch(x):
        n = const(0, lw)
        sh = W // 2
        while sh >= 1:
            top = eq(x >> (W - sh), 0)
            n = ite(top, n + sh, n)
            x = ite(top, x << sh, x)
            sh //= 2
        # after the loop every bit but the msb has been shifted past; x != 0
        return n if no_zero_check else ite(eq(x, 0), const(W, lw), n)
    a = Fun(f"clz{W}_priority", [("x", W)], priority)
    b = Fun(f"clz{W}_bsearch" + ("_nz" if no_zero_check else ""), [("x", W)], bsearch)
    return a, b


def satadd(W, signed, swap=False):
    mx = (1 << W) - 1

    if not signed:
        def a(x, y):
            s = x + y
            return ite(ult(s, x), const(mx, W), s)

        def b(x, y):
            return ite(ult(const(mx, W) - y, x), const(mx, W), x + y)
    else:
        smax, smin = (1 << (W - 1)) - 1, 1 << (W - 1)

        def a(x, y):
            s = x + y
            ovf = land(eq(x.bit(W - 1), y.bit(W - 1)), ne(s.bit(W - 1), x.bit(W - 1)))
            return ite(ovf, ite(eq(x.bit(W - 1), 1), const(smin, W), const(smax, W)), s)

        def b(x, y):
            xs, ys = concat(x.bit(W - 1), x), concat(y.bit(W - 1), y)
            s = xs + ys
            pos_ovf = land(eq(s.bit(W), 0), eq(s.bit(W - 1), 1))
            neg_ovf = land(eq(s.bit(W), 1), eq(s.bit(W - 1), 0))
            hi, lo = (const(smin, W), const(smax, W)) if swap else (const(smax, W), const(smin, W))
            return ite(pos_ovf, hi, ite(neg_ovf, lo, s.ext(W - 1, 0)))
    k = "s" if signed else "u"
    fa = Fun(f"sat{k}add{W}_carry", [("a", W), ("b", W)], a)
    fb = Fun(f"sat{k}add{W}_bound" + ("_swap" if swap else ""), [("a", W), ("b", W)], b)
    return fa, fb


def equiv(fa, fb, w_in, nargs=1):
    xs = [var(chr(ord("a") + i), w_in) for i in range(nargs)]
    return eq(fa(*xs), fb(*xs))


def injective(f, w_in):
    a, b = var("a", w_in), var("b", w_in)
    return implies(eq(f(a), f(b)), eq(a, b))


# ---------------------------------------------------------------------------
# The suite

def build(seed=1):
    rng = random.Random(seed)
    out = []

    def add(i):
        out.append(i)

    # -- affine-proof ------------------------------------------------------
    for W in (8, 16, 32, 64):
        for nb in (1, 2, 4, 8):
            for k in (2, 3, 4):
                f = crc_bitwise(W, nb)
                add(Inst("affine-proof", "crc_loop", {"w": W, "bytes": nb, "k": k},
                         affine_prop(f, k, 8 * nb), "unsat",
                         f"bit-serial CRC-{W} of a {nb}-byte message is affine over GF(2): "
                         f"the XOR of {k} CRCs equals the CRC of the XOR"))
    for W in (16, 32):
        for nb in (1, 2, 4):
            for k in (2, 3):
                f = crc_nibble_table(W, nb)
                add(Inst("affine-proof", "crc_table", {"w": W, "bytes": nb, "k": k},
                         affine_prop(f, k, 8 * nb), "unsat",
                         f"nibble-table CRC-{W} of a {nb}-byte message is affine over GF(2)"))
    for nb in (1, 2, 3, 4):
        f = crc_bitwise(32, nb)
        add(Inst("affine-proof", "crc_injective", {"w": 32, "bytes": nb}, injective(f, 8 * nb),
                 "unsat", f"CRC-32 is injective on {nb}-byte messages"))
    for W in (16, 32, 64):
        for r in (8, 16, 32, 64):
            f = lfsr(W, r)
            add(Inst("affine-proof", "lfsr", {"w": W, "rounds": r},
                     affine_prop(f, 2, W, affine=False), "unsat",
                     f"{r} steps of a {W}-bit Fibonacci LFSR are linear"))
    for W in (32, 64):
        for r in (1, 2, 4, 8):
            f = xorshift(W, r)
            add(Inst("affine-proof", "xorshift_linear", {"w": W, "rounds": r},
                     affine_prop(f, 2, W, affine=False), "unsat",
                     f"{r} rounds of xorshift{W} are linear"))
            add(Inst("affine-proof", "xorshift_injective", {"w": W, "rounds": r},
                     injective(f, W), "unsat", f"{r} rounds of xorshift{W} are a bijection"))
    for W in (8, 16, 32, 64, 128):
        for form in ("loop", "log"):
            add(Inst("affine-proof", "gray", {"w": W, "decode": form}, gray(W, form), "unsat",
                     f"Gray decode ({form} form) inverts Gray encode at {W} bits"))
    for W in (8, 16, 32, 64, 128, 256):
        add(Inst("affine-proof", "parity", {"w": W}, parity(W), "unsat",
                 f"bit-loop parity equals shift-fold parity at {W} bits"))

    # -- mixed-proof -------------------------------------------------------
    for nb in (2, 4, 8, 16):
        a, b = fletcher16(nb)
        add(Inst("mixed-proof", "fletcher16", {"bytes": nb}, equiv(a, b, 8 * nb), "unsat",
                 "Fletcher-16 reduced every step equals Fletcher-16 reduced once at the end"))
    for nb in (2, 4, 8):
        a, b = adler32(nb)
        add(Inst("mixed-proof", "adler32", {"bytes": nb}, equiv(a, b, 8 * nb), "unsat",
                 "Adler-32 reduced every step equals Adler-32 reduced once at the end"))
    for W, nbs in ((32, (1, 2, 4, 8)), (64, (1, 2, 4))):
        for nb in nbs:
            add(Inst("mixed-proof", "fnv1a", {"w": W, "bytes": nb},
                     equiv(fnv1a(W, nb, "mul"), fnv1a(W, nb, "shift"), 8 * nb), "unsat",
                     f"FNV-1a-{W} with the prime as a multiply equals its shift-add expansion"))
    for W in (8, 12, 16, 20, 24, 32):
        add(Inst("mixed-proof", "fmix_injective", {"w": W}, injective(fmix(W), W), "unsat",
                 f"murmur3 fmix scaled to {W} bits is injective"))
        f, g = fmix(W), fmix_inverse(W)
        x = var("x", W)
        add(Inst("mixed-proof", "fmix_inverse", {"w": W}, eq(g(f(x)), x), "unsat",
                 f"the inverse of fmix (modular-inverse multipliers, unrolled un-xorshift) "
                 f"undoes fmix at {W} bits"))
    for d in (4, 8, 16, 32, 64):
        E, D, n = secded(d)
        x, e = var("x", d), var("e", n)
        add(Inst("mixed-proof", "secded", {"data": d},
                 implies(weight_at_most(e, 1), eq(D(E(x) ^ e), x)), "unsat",
                 f"extended Hamming ({n},{d}) corrects every single-bit error"))
    for W in (16, 32, 64):
        a, b = popcount(W)
        add(Inst("mixed-proof", "popcount", {"w": W}, equiv(a, b, W), "unsat",
                 f"SWAR popcount equals a bit sum at {W} bits"))
    for W in (8, 16, 32, 64):
        a, b = clz(W)
        add(Inst("mixed-proof", "clz", {"w": W}, equiv(a, b, W), "unsat",
                 f"binary-search count-leading-zeros equals a priority scan at {W} bits"))
    for W in (8, 16, 32, 64):
        for sg in (False, True):
            a, b = satadd(W, sg)
            add(Inst("mixed-proof", "satadd", {"w": W, "signed": int(sg)}, equiv(a, b, W, 2),
                     "unsat", f"two forms of {'signed' if sg else 'unsigned'} saturating add agree"))

    # -- refute ------------------------------------------------------------
    for W, nb, k in ((8, 1, 2), (16, 2, 2), (32, 1, 2), (32, 4, 2), (32, 4, 4), (64, 8, 2)):
        if CRCS[W][1] == 0:          # a linear CRC has no constant to drop: flip a poly bit
            f, g = crc_bitwise(W, nb), crc_bitwise(W, nb, CRCS[W][0] ^ 1, tag="_wrongpoly")
            x = var("a", 8 * nb)
            add(refute("crc_wrongpoly", {"w": W, "bytes": nb}, eq(f(x), g(x)),
                       f"CRC-{W} against the same CRC with one polynomial bit flipped", rng))
        else:
            add(refute("crc_noconst", {"w": W, "bytes": nb, "k": k},
                       affine_prop(crc_bitwise(W, nb), k, 8 * nb, drop_const=True),
                       f"CRC-{W} is affine, not linear: without the CRC(0) term the "
                       f"XOR identity fails", rng))
    for W, r in ((16, 16), (32, 32), (64, 64)):
        taps = FIB_TAPS[W]
        f = lfsr(W, r)
        g = lfsr(W, r, taps=taps[:-1] + (taps[-1] + 1,), tag="_mistap")
        s = var("state", W)
        add(refute("lfsr_mistap", {"w": W, "rounds": r}, eq(f(s), g(s)),
                   f"a {W}-bit LFSR against one with a tap moved by one bit", rng))
    for W in (16, 32, 64):
        add(refute("gray_early_stop", {"w": W}, gray(W, "loop", early=True),
                   "Gray decode loop that stops one step early (only inputs with the top "
                   "bit set fail)", rng))
    for W in (16, 32, 64):
        add(refute("parity_missing_fold", {"w": W}, parity(W, broken=True),
                   "parity fold with the shift-by-2 step missing", rng))
    for nb in (2, 4, 8):
        a, b = fletcher16(nb, fold_mutant=True)
        add(refute("fletcher16_fold", {"bytes": nb}, equiv(a, b, 8 * nb),
                   "Fletcher-16 with mod 255 done by end-around-carry folding and no final "
                   "fix-up: a sum that is a nonzero multiple of 255 gives 255, not 0", rng))
    for W, nb in ((32, 2), (32, 4), (64, 2)):
        drop = FNV[W][2][2]
        add(refute("fnv1a_dropped_term", {"w": W, "bytes": nb},
                   equiv(fnv1a(W, nb, "mul"), fnv1a(W, nb, "shift", drop_shift=drop), 8 * nb),
                   "FNV-1a shift-add expansion missing one term of the prime", rng))
    for W in (8, 16, 24, 32):
        f = fmix(W, even=True)
        s1, _, _, _, _ = _fmix_params(W)
        a0 = rng.getrandbits(W)
        # an even multiplier cannot see the top bit: flip it after the first xorshift
        h = a0 ^ (a0 >> s1)
        b0 = _unxs_eval(W, h ^ (1 << (W - 1)), s1)
        assert _fmix_eval(W, a0, True) == _fmix_eval(W, b0, True) and a0 != b0
        add(refute("fmix_even", {"w": W}, injective(f, W),
                   "fmix with an even multiplier is not injective", rng,
                   extra=[{"a": a0, "b": b0}]))
    for d in (8, 16, 32, 64):
        E, D, n = secded(d)
        x, e = var("x", d), var("e", n)
        add(refute("secded_double", {"data": d}, implies(weight_at_most(e, 2), eq(D(E(x) ^ e), x)),
                   "SECDED does not correct double-bit errors", rng,
                   extra=double_errors(d, n, rng)))
    for W in (16, 32, 64):
        a, b = popcount(W, bad_mask=True)
        add(refute("popcount_bad_mask", {"w": W}, equiv(a, b, W),
                   "SWAR popcount with a wrong 0x33 mask", rng))
    for W in (16, 32, 64):
        a, b = clz(W, no_zero_check=True)
        add(refute("clz_no_zero_check", {"w": W}, equiv(a, b, W),
                   "binary-search clz without the zero check: only x = 0 differs", rng))
    for W in (8, 32, 64):
        a, b = satadd(W, True, swap=True)
        add(refute("satadd_swapped_clamp", {"w": W}, equiv(a, b, W, 2),
                   "signed saturating add clamping to the wrong end", rng))

    # -- inverse -----------------------------------------------------------
    def preimage(f, w_in):
        """f(x) == f(secret) for a random secret: (prop, witness, target)."""
        sec = rng.getrandbits(w_in)
        target = evaluate(f(const(sec, w_in)), {})
        return eq(f(var("x", w_in)), target), {"x": sec}, target

    for nb in (2, 4, 8):
        f = crc_bitwise(32, nb)
        prop, wit, t = preimage(f, 8 * nb)
        add(Inst("inverse", "crc32_preimage", {"bytes": nb}, prop, "sat",
                 f"find a {nb}-byte message with CRC-32 {t:#010x}", witness=wit, show=True))
    for nb in (5, 8):
        f = crc_bitwise(32, nb)
        a, b = var("a", 8 * nb), var("b", 8 * nb)
        # a collision: messages differing in a CRC-32 polynomial multiple
        a0 = rng.getrandbits(8 * nb)
        poly_full = (1 << 32) | int(f"{0xEDB88320:032b}"[::-1], 2)
        # CRC is linear in the reflected bit order: XOR in the reflected poly multiple
        delta = int(f"{poly_full:0{8 * nb}b}"[::-1], 2)
        b0 = a0 ^ delta
        assert evaluate(f(const(a0, 8 * nb)), {}) == evaluate(f(const(b0, 8 * nb)), {})
        add(Inst("inverse", "crc32_collision", {"bytes": nb},
                 land(ne(a, b), eq(f(a), f(b))), "sat",
                 f"find two different {nb}-byte messages with the same CRC-32",
                 witness={"a": a0, "b": b0}, show=True))
    for W in (12, 16, 20, 24, 32):
        f = fmix(W)
        prop, wit, t = preimage(f, W)
        add(Inst("inverse", "fmix_preimage", {"w": W}, prop, "sat",
                 f"invert {W}-bit fmix at {t:#x}", witness=wit, show=True))
    for nb in (2, 3, 4):
        f = fnv1a(32, nb, "mul")
        prop, wit, t = preimage(f, 8 * nb)
        add(Inst("inverse", "fnv1a32_preimage", {"bytes": nb}, prop, "sat",
                 f"find a {nb}-byte message with FNV-1a-32 hash {t:#010x}",
                 witness=wit, show=True))
    for nb in (2, 4, 8):
        a, b = fletcher16(nb)
        x, y = var("a", 8 * nb), var("b", 8 * nb)
        # bytes 0 and 255 are the same mod 255
        x0 = rng.getrandbits(8 * nb) & ~0xFF
        add(Inst("inverse", "fletcher16_collision", {"bytes": nb},
                 land(ne(x, y), eq(a(x), a(y))), "sat",
                 f"find two different {nb}-byte messages with the same Fletcher-16",
                 witness={"a": x0, "b": x0 | 0xFF}, show=True))
    for W, r in ((32, 16), (32, 64), (64, 16), (64, 64)):
        f = lfsr(W, r)
        prop, wit, t = preimage(f, W)
        add(Inst("inverse", "lfsr_state", {"w": W, "rounds": r}, prop, "sat",
                 f"recover the {W}-bit LFSR state {r} steps before {t:#x}",
                 witness=wit, show=True))

    # -- quickcheck-trivial: <= 16 input bits, constants drawn per seed ----
    for s in range(1, 11):
        r2 = random.Random(1000 + s)
        poly = r2.getrandbits(8) | 0x80
        f = crc_bitwise(8, 1, poly=poly, tag=f"_{poly:02x}")
        add(Inst("quickcheck-trivial", "qc_crc8_affine", {"poly": f"{poly:02x}"},
                 affine_prop(f, 2, 8), "unsat", "CRC-8 with a random polynomial is linear",
                 seed=s))
        W = r2.choice((4, 6, 8))
        a, b = satadd(W, bool(s & 1))
        add(Inst("quickcheck-trivial", "qc_satadd", {"w": W, "signed": s & 1},
                 equiv(a, b, W, 2), "unsat", "two saturating-add forms agree", seed=s))
        W = r2.choice((8, 16))
        add(Inst("quickcheck-trivial", "qc_gray", {"w": W}, gray(W, r2.choice(("loop", "log"))),
                 "unsat", "Gray decode inverts encode", seed=s))
        c1, c2 = r2.getrandbits(8) | 1, r2.getrandbits(8) | 1
        add(Inst("quickcheck-trivial", "qc_fmix8_injective", {"c": f"{c1:02x}{c2:02x}"},
                 injective(fmix(8, consts=(c1, c2)), 8), "unsat",
                 "8-bit fmix with random odd multipliers is injective", seed=s))
        tri = (r2.randint(1, 7), r2.randint(1, 7), r2.randint(1, 7))
        add(Inst("quickcheck-trivial", "qc_xorshift16_injective", {"t": "-".join(map(str, tri))},
                 injective(xorshift(16, 1, triple=tri, tag=f"_{'_'.join(map(str, tri))}"), 16),
                 "unsat", "xorshift16 with a random shift triple is a bijection", seed=s))
        if s == 1:
            a, b = popcount(16)
            add(Inst("quickcheck-trivial", "qc_popcount16", {"w": 16}, equiv(a, b, 16), "unsat",
                     "SWAR popcount equals a bit sum", seed=s))
        a, b = clz(r2.choice((8, 16)), no_zero_check=bool(s & 1))
        Wc = a.params[0][1]
        if s & 1:
            add(refute("qc_clz_nz", {"w": Wc}, equiv(a, b, Wc), "clz without the zero check",
                       r2, seed=s, cat="quickcheck-trivial"))
        else:
            add(Inst("quickcheck-trivial", "qc_clz", {"w": Wc}, equiv(a, b, Wc), "unsat",
                     "binary-search clz equals a priority scan", seed=s))
        E, D, n = secded(4)
        x, e = var("x", 4), var("e", n)
        k = 1 if s & 1 else 2
        p = implies(weight_at_most(e, k), eq(D(E(x) ^ e), x))
        if k == 1:
            add(Inst("quickcheck-trivial", "qc_secded4", {"errors": 1}, p, "unsat",
                     "Hamming (8,4) corrects a single error", seed=s))
        else:
            add(refute("qc_secded4", {"errors": 2}, p, "Hamming (8,4) cannot correct two errors",
                       r2, extra=double_errors(4, n, r2), seed=s, cat="quickcheck-trivial"))
        Wf = r2.choice((8, 16))
        a, b = fletcher16(1 if Wf == 8 else 2, fold_mutant=True)
        add(refute("qc_fletcher_fold", {"bytes": Wf // 8}, equiv(a, b, Wf),
                   "Fletcher-16 fold reduction without the fix-up", r2, seed=s,
                   cat="quickcheck-trivial"))
        W = r2.choice((8, 16))
        add(refute("qc_parity_missing_fold", {"w": W}, parity(W, broken=True),
                   "parity fold with a step missing", r2, seed=s, cat="quickcheck-trivial"))
        f = crc_bitwise(8, 1, poly=poly, tag=f"_{poly:02x}")
        g = crc_bitwise(8, 1, poly=poly ^ (1 << r2.randrange(8)), tag=f"_{poly:02x}_m")
        x = var("a", 8)
        add(refute("qc_crc8_wrongpoly", {"poly": f"{poly:02x}"}, eq(f(x), g(x)),
                   "CRC-8 against a one-bit-different polynomial", r2, seed=s,
                   cat="quickcheck-trivial"))

    # one instance per id: the trivial loop's random choices can collide
    uniq = {}
    for i in out:
        uniq.setdefault(i.id, i)
    return list(uniq.values())


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--samples", type=int, default=300, help="self-check samples per proof")
    ap.add_argument("--no-codegen", action="store_true",
                    help="skip codegen-equiv (it needs the pinned yosys) and keep its files")
    a = ap.parse_args(argv)
    insts = build()
    rng = random.Random(7)
    for i in insts:
        i.check(rng, a.samples)
    if not a.no_codegen:
        sys.path.insert(0, str(_HERE.parents[1]))
        import gen_codegen_equiv
        from tests.perf import tools
        insts += gen_codegen_equiv.build(tools.path("yosys"))
    if a.list:
        for i in insts:
            print(f"{i.cat:20} {i.split:5} {i.expect:6} {i.id}")
        print(f"{len(insts)} instances", file=sys.stderr)
        return 0
    out = Path(a.out)
    old = {}
    if (out / "index.json").exists():
        old = {e["id"]: e for e in json.loads((out / "index.json").read_text())["instances"]}
    for c in CATEGORIES:
        if c == "codegen-equiv" and a.no_codegen:
            continue
        (out / c).mkdir(parents=True, exist_ok=True)
        for p in (out / c).glob("*.smt2"):
            p.unlink()
    index = [e for e in old.values() if e["category"] == "codegen-equiv" and a.no_codegen]
    for i in insts:
        rel = f"{i.cat}/{i.id}.smt2"
        text = i.text()
        (out / rel).write_text(text)
        e = {"id": i.id, "path": rel, "category": i.cat, "family": i.family, "knobs": i.knobs,
             "seed": i.seed, "split": i.split, "status": i.expect,
             "sha": hashlib.sha256(text.encode()).hexdigest()[:16]}
        prev = old.get(i.id)
        if prev and prev.get("sha") == e["sha"] and "consensus" in prev:
            e["consensus"] = prev["consensus"]          # still the same file
            e["consensus_s"] = prev.get("consensus_s", {})
        index.append(e)
    (out / "index.json").write_text(json.dumps(
        {"_comment": "written by tests/formal/gen_fwhdl.py; consensus by "
                     "tests/formal/fwhdl_consensus.py",
         "instances": index}, indent=1) + "\n")
    print(f"{len(index)} instances in {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
