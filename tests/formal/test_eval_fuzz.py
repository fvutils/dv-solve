"""Differential fuzzer for the get-value evaluator (src/c/smt2/smt2_eval.c) vs z3.

Each seed declares a few bit-vector and Bool constants, pins every one to a
random value, defines two parameterized functions, and asks get-value for a
batch of random terms: every QF_BV operator (division by zero, shifts past the
width, signed and overflow operators), let with shadowing, define-fun
application, constant arrays with select/store/=, at widths from 1 to 128.
The terms are never asserted, so dv-solve answers them with the evaluator
alone. z3 must prove each value.

Seeds are fixed. Scale with EVAL_FUZZ_N (default 200), e.g.
``EVAL_FUZZ_N=10000 pytest tests/formal/test_eval_fuzz.py``. A failure prints
the script, for a one-command repro.
"""
from __future__ import annotations

import os
import random
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = _REPO / "build" / "dv-solve-smt2"
_Z3 = shutil.which("z3")
_N = int(os.environ.get("EVAL_FUZZ_N", "200"))
_PER_SEED = 24

pytestmark = [
    pytest.mark.skipif(not _EXE.is_file(), reason="build/dv-solve-smt2 not built"),
    pytest.mark.skipif(_Z3 is None, reason="z3 not on PATH"),
]

_WIDTHS = [1, 2, 3, 4, 7, 8, 12, 16, 31, 32, 33, 63, 64, 65, 96, 128]

_BV_BIN = ["bvand", "bvor", "bvxor", "bvadd", "bvsub", "bvmul", "bvudiv", "bvurem",
           "bvsdiv", "bvsrem", "bvsmod", "bvshl", "bvlshr", "bvashr", "bvnand",
           "bvnor", "bvxnor"]
_BV_NARY = ["bvand", "bvor", "bvxor", "bvadd", "bvmul"]
_CMP = ["bvult", "bvule", "bvugt", "bvuge", "bvslt", "bvsle", "bvsgt", "bvsge"]
_OVF = ["bvuaddo", "bvsaddo", "bvumulo", "bvsmulo", "bvusubo", "bvssubo", "bvsdivo"]


class _Gen:
    def __init__(self, seed: int, let_pool: int = 0):
        """let_pool > 0: let names come from a pool of that many names, shared
        by function bodies and the terms that call them (B78: a callee's let
        must not capture the caller's argument, nor a caller's let a name the
        body uses)."""
        self.r = random.Random(seed)
        self.let_pool = let_pool
        self.vars: dict[int, list[str]] = {}
        self.bools: list[str] = []
        self.funs: list[tuple[str, int, int]] = []   # name, param width, arity
        self.n_let = 0

    # -- literals ---------------------------------------------------------
    def value(self, w: int) -> int:
        r = self.r.random()
        if r < 0.15:
            return 0
        if r < 0.3:
            return (1 << w) - 1
        if r < 0.4:
            return 1 << (w - 1)                       # most negative
        if r < 0.5:
            return self.r.randrange(0, min(w + 3, 1 << w))   # small: shift amounts
        return self.r.getrandbits(w)

    def lit(self, w: int, v: int | None = None) -> str:
        v = self.value(w) if v is None else v
        form = self.r.random()
        if w % 4 == 0 and form < 0.4:
            return "#x" + format(v, "0%dx" % (w // 4))
        if form < 0.7:
            return "#b" + format(v, "0%db" % w)
        return "(_ bv%d %d)" % (v, w)

    # -- terms ------------------------------------------------------------
    def bv(self, w: int, d: int, env: dict) -> str:
        r = self.r
        names = [n for n, (k, ww) in env.items() if k == "bv" and ww == w] + self.vars.get(w, [])
        if d <= 0 or r.random() < 0.15:
            if names and r.random() < 0.7:
                return r.choice(names)
            return self.lit(w)
        c = r.random()
        if c < 0.08:
            return "(%s %s)" % (r.choice(["bvnot", "bvneg"]), self.bv(w, d - 1, env))
        if c < 0.38:
            op = r.choice(_BV_BIN)
            return "(%s %s %s)" % (op, self.bv(w, d - 1, env), self.bv(w, d - 1, env))
        if c < 0.44:
            op = r.choice(_BV_NARY)
            return "(%s %s)" % (op, " ".join(self.bv(w, d - 1, env) for _ in range(3)))
        if c < 0.50 and w >= 2:
            a = r.randrange(1, w)
            return "(concat %s %s)" % (self.bv(a, d - 1, env), self.bv(w - a, d - 1, env))
        if c < 0.56:
            src = r.choice([x for x in _WIDTHS if x >= w])
            lo = r.randrange(0, src - w + 1)
            return "((_ extract %d %d) %s)" % (lo + w - 1, lo, self.bv(src, d - 1, env))
        if c < 0.62 and w >= 2:
            src = r.randrange(1, w)
            op = r.choice(["zero_extend", "sign_extend"])
            return "((_ %s %d) %s)" % (op, w - src, self.bv(src, d - 1, env))
        if c < 0.65:
            ks = [k for k in range(1, w + 1) if w % k == 0 and k <= 8]
            k = r.choice(ks)
            return "((_ repeat %d) %s)" % (k, self.bv(w // k, d - 1, env))
        if c < 0.69:
            op = r.choice(["rotate_left", "rotate_right"])
            return "((_ %s %d) %s)" % (op, r.randrange(0, 2 * w + 2), self.bv(w, d - 1, env))
        if c < 0.76:
            return "(ite %s %s %s)" % (self.bool(d - 1, env), self.bv(w, d - 1, env),
                                       self.bv(w, d - 1, env))
        if c < 0.83:
            return self.let(lambda e: self.bv(w, d - 1, e), d, env)
        if c < 0.88:
            fs = [f for f in self.funs if f[1] == w]
            if fs:
                name, _, n = r.choice(fs)
                return "(%s %s)" % (name, " ".join(self.bv(w, d - 1, env) for _ in range(n)))
        if c < 0.92:
            return self.select(w, d, env)
        if w == 1:
            op = r.choice(["bvcomp", "bvredor", "bvredand"])
            if op == "bvcomp":
                ww = r.choice(_WIDTHS)
                return "(bvcomp %s %s)" % (self.bv(ww, d - 1, env), self.bv(ww, d - 1, env))
            return "(%s %s)" % (op, self.bv(r.choice(_WIDTHS), d - 1, env))
        return "(%s %s %s)" % (r.choice(_BV_BIN), self.bv(w, d - 1, env), self.bv(w, d - 1, env))

    def let(self, body, d: int, env: dict) -> str:
        r = self.r
        binds = []
        inner = dict(env)
        for _ in range(r.randrange(1, 3)):
            # Reuse a name sometimes: let shadows.
            if self.let_pool:
                name = "?t%d" % r.randrange(self.let_pool)
            elif env and r.random() < 0.3:
                name = r.choice(list(env))
            else:
                self.n_let += 1
                name = "?l%d" % self.n_let
            if r.random() < 0.25:
                binds.append("(%s %s)" % (name, self.bool(d - 1, env)))
                inner[name] = ("bool", 1)
            else:
                ww = r.choice(_WIDTHS)
                binds.append("(%s %s)" % (name, self.bv(ww, d - 1, env)))   # outer scope
                inner[name] = ("bv", ww)
        return "(let (%s) %s)" % (" ".join(binds), body(inner))

    def array(self, iw: int, ew: int, d: int, env: dict) -> str:
        a = "((as const (Array (_ BitVec %d) (_ BitVec %d))) %s)" % (iw, ew, self.bv(ew, d - 1, env))
        for _ in range(self.r.randrange(0, 4)):
            a = "(store %s %s %s)" % (a, self.bv(iw, d - 1, env), self.bv(ew, d - 1, env))
        return a

    def select(self, w: int, d: int, env: dict) -> str:
        iw = self.r.choice([1, 2, 3, 4, 8, 20])
        return "(select %s %s)" % (self.array(iw, w, d - 1, env), self.bv(iw, d - 1, env))

    def bool(self, d: int, env: dict) -> str:
        r = self.r
        names = [n for n, (k, _) in env.items() if k == "bool"] + self.bools
        if d <= 0 or r.random() < 0.15:
            if names and r.random() < 0.6:
                return r.choice(names)
            return r.choice(["true", "false"])
        c = r.random()
        w = r.choice(_WIDTHS)
        if c < 0.25:
            return "(%s %s %s)" % (r.choice(_CMP), self.bv(w, d - 1, env), self.bv(w, d - 1, env))
        if c < 0.35:
            n = r.randrange(2, 4)
            return "(%s %s)" % (r.choice(["=", "distinct"]),
                                " ".join(self.bv(w, d - 1, env) for _ in range(n)))
        if c < 0.55:
            op = r.choice(["and", "or", "xor", "=>", "=", "distinct"])
            n = r.randrange(2, 4)
            return "(%s %s)" % (op, " ".join(self.bool(d - 1, env) for _ in range(n)))
        if c < 0.62:
            return "(not %s)" % self.bool(d - 1, env)
        if c < 0.70:
            return "(ite %s %s %s)" % (self.bool(d - 1, env), self.bool(d - 1, env),
                                       self.bool(d - 1, env))
        if c < 0.78:
            op = r.choice(_OVF + ["bvnego"])
            if op == "bvnego":
                return "(bvnego %s)" % self.bv(w, d - 1, env)
            return "(%s %s %s)" % (op, self.bv(w, d - 1, env), self.bv(w, d - 1, env))
        if c < 0.86:
            return self.let(lambda e: self.bool(d - 1, e), d, env)
        if c < 0.94:
            # Array equality: small index widths are compared element by element.
            iw = r.choice([1, 2, 3, 20])
            ew = r.choice([1, 4, 8])
            return "(= %s %s)" % (self.array(iw, ew, d - 1, env), self.array(iw, ew, d - 1, env))
        return "(%s %s %s)" % (r.choice(_CMP), self.bv(w, d - 1, env), self.bv(w, d - 1, env))

    # -- the script ---------------------------------------------------------
    def script(self) -> tuple[str, list[str]]:
        r = self.r
        lines = ["(set-logic QF_ABV)"]
        pins = []
        for i in range(r.randrange(3, 7)):
            w = r.choice(_WIDTHS)
            name = "v%d" % i
            self.vars.setdefault(w, []).append(name)
            lines.append("(declare-const %s (_ BitVec %d))" % (name, w))
            pins.append("(assert (= %s %s))" % (name, self.lit(w)))
        for i in range(r.randrange(1, 3)):
            name = "b%d" % i
            self.bools.append(name)
            lines.append("(declare-const %s Bool)" % name)
            pins.append("(assert %s)" % (name if r.random() < 0.5 else "(not %s)" % name))
        for i in range(2):
            w = r.choice(_WIDTHS)
            n = r.randrange(1, 3)
            params = {"p%d" % j: ("bv", w) for j in range(n)}
            body = self.bv(w, 3, params)
            lines.append("(define-fun f%d (%s) (_ BitVec %d) %s)" % (
                i, " ".join("(%s (_ BitVec %d))" % (p, w) for p in params), w, body))
            self.funs.append(("f%d" % i, w, n))
        lines += pins
        lines.append("(check-sat)")
        terms = []
        for _ in range(_PER_SEED):
            if r.random() < 0.35:
                terms.append(self.bool(4, {}))
            else:
                terms.append(self.bv(r.choice(_WIDTHS), 4, {}))
        return "\n".join(lines) + "\n", terms


# ---- response parsing ------------------------------------------------------

def _tokens(text: str):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c in "()":
            yield c
            i += 1
        elif c == "|":
            j = text.index("|", i + 1)
            yield text[i:j + 1]
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "()":
                j += 1
            yield text[i:j]
            i = j


def _parse(text: str):
    stack: list[list] = [[]]
    for t in _tokens(text):
        if t == "(":
            stack.append([])
        elif t == ")":
            top = stack.pop()
            stack[-1].append(top)
        else:
            stack[-1].append(t)
    return stack[0]


def _run(cmd: list[str], text: str) -> str:
    p = subprocess.run(cmd, input=text, capture_output=True, text=True, timeout=60)
    return p.stdout


def _unparse(x) -> str:
    return x if isinstance(x, str) else "(" + " ".join(_unparse(y) for y in x) + ")"


def _is_array(x) -> bool:
    """The generator's array terms: a constant array or a store."""
    return isinstance(x, list) and bool(x) and (
        x[0] == "store" or (isinstance(x[0], list) and x[0][:2] == ["as", "const"]))


def _array_parts(x):
    """(base value, index width, [store keys]) of an array term."""
    keys = []
    while x[0] == "store":
        keys.append(x[2])
        x = x[1]
    sort = x[0][2]                      # (Array (_ BitVec I) (_ BitVec E))
    return x[1], int(sort[1][2]), keys


def _expand_array_eq(x):
    """Rewrite every array equality into equalities of elements, for z3: by
    default it gets constant-array equality wrong (see test_eval_matches_z3).
    A small index sort is compared at every index; a 20-bit one at the stored
    indices and through the bases (a few stores cannot cover 2^20 indices, so
    arrays with different bases differ somewhere)."""
    if not isinstance(x, list):
        return x
    x = [_expand_array_eq(y) for y in x]
    if x and x[0] in ("=", "distinct") and len(x) >= 3 and _is_array(x[1]):
        def eq(a, b):
            ba, iw, ka = _array_parts(a)
            bb, _, kb = _array_parts(b)
            if iw <= 4:
                idx = ["(_ bv%d %d)" % (k, iw) for k in range(1 << iw)]
                parts = []
            else:
                idx = [_unparse(k) for k in ka + kb]
                parts = ["(= %s %s)" % (_unparse(ba), _unparse(bb))]
            parts += ["(= (select %s %s) (select %s %s))" % (_unparse(a), k, _unparse(b), k)
                      for k in idx]
            return "(and %s)" % " ".join(parts) if len(parts) > 1 else parts[0]
        args = x[1:]
        if x[0] == "=":
            conj = [eq(args[i], args[i + 1]) for i in range(len(args) - 1)]
            return "(and %s)" % " ".join(conj) if len(conj) > 1 else conj[0]
        conj = ["(not %s)" % eq(args[i], args[j])
                for i in range(len(args)) for j in range(i + 1, len(args))]
        return "(and %s)" % " ".join(conj) if len(conj) > 1 else conj[0]
    return x


def _oracle_term(t: str) -> str:
    return _unparse(_expand_array_eq(_parse(t)[0]))


def _raw_values(text: str, n: int) -> list[str]:
    """The value tokens of a get-value response, as printed."""
    items = _parse(text)
    assert items and items[0] == "sat", text
    assert len(items) == 2 and isinstance(items[1], list) and len(items[1]) == n, text
    vals = [p[1] for p in items[1]]
    assert all(isinstance(v, str) for v in vals), text
    return vals


@pytest.mark.parametrize("seed", range(_N))
def test_eval_matches_z3(seed):
    """z3 proves each value: with the constants pinned, (not (= term value))
    must be unsat. z3's own get-value is not the oracle: it evaluates an
    array equality structurally, and answers false for equal arrays. Nor does
    z3 see array equalities: z3 5.1 finds `sat` for an equality of constant
    arrays over a small index sort that differ at an index no store covers.
    _oracle_term rewrites them into equalities of elements."""
    g = _Gen(seed)
    script, terms = g.script()
    dv = _run([str(_EXE)], script + "(get-value (%s))\n" % " ".join(terms))
    assert "(error" not in dv, "dv-solve refused a term:\n%s\n%s" % (script, dv)
    vals = _raw_values(dv, len(terms))
    checks = "".join("(push 1)(assert (not (= %s %s)))(check-sat)(pop 1)\n"
                     % (_oracle_term(t), v) for t, v in zip(terms, vals))
    z3 = _run([_Z3, "-in"], script + checks).split()
    assert "error" not in " ".join(z3), "ill-sorted term:\n" + script + " ".join(z3)
    assert z3[0] == "sat" and len(z3) == len(terms) + 1, z3
    for t, v, verdict in zip(terms, vals, z3[1:]):
        assert verdict == "unsat", "term %s = %s is wrong (z3: %s)\nscript:\n%s" % (
            t, v, verdict, script)


@pytest.mark.parametrize("seed", range(_N))
def test_translation_matches_eval(seed):
    """The solver's translation against the evaluator: with the constants
    pinned, every term equals its get-value value, so (not (= term value))
    must not be sat. Let names come from a small shared pool, so callee and
    caller lets collide (B78). A sat is checked with z3, to tell a
    translation bug from an evaluator bug."""
    g = _Gen(10_000 + seed, let_pool=3)
    script, terms = g.script()
    dv = _run([str(_EXE)], script + "(get-value (%s))\n" % " ".join(terms))
    assert "(error" not in dv, "dv-solve refused a term:\n%s\n%s" % (script, dv)
    vals = _raw_values(dv, len(terms))
    checks = "".join("(push 1)(assert (not (= %s %s)))(check-sat)(pop 1)\n" % (t, v)
                     for t, v in zip(terms, vals))
    got = _run([str(_EXE)], script + checks).split()
    assert got[0] == "sat" and len(got) == len(terms) + 1, got
    for t, v, verdict in zip(terms, vals, got[1:]):
        if verdict != "sat":
            continue
        z3 = _run([_Z3, "-in"], script + "(assert (not (= %s %s)))(check-sat)\n"
                  % (_oracle_term(t), v)).split()
        raise AssertionError("translation of %s disagrees with its value %s (z3 says the "
                             "negation is %s)\nscript:\n%s" % (t, v, z3[-1], script))


# ---- edge cases, by hand from the SMT-LIB definitions ----------------------

_EDGE = [
    ("(bvudiv #x7 #x0)", "#xf"), ("(bvurem #x7 #x0)", "#x7"),
    ("(bvsdiv #x9 #x0)", "#x1"), ("(bvsdiv #x7 #x0)", "#xf"),
    ("(bvsrem #x9 #x0)", "#x9"), ("(bvsmod #x9 #x0)", "#x9"),
    ("(bvsmod #x9 #x3)", "#x2"), ("(bvsmod #x7 #xd)", "#xe"), ("(bvsrem #x9 #x3)", "#xf"),
    ("(bvsdiv #x8 #xf)", "#x8"),
    ("(bvshl #x1 #x4)", "#x0"), ("(bvlshr #x8 #xf)", "#x0"), ("(bvashr #x8 #xf)", "#xf"),
    ("(bvashr #x8 #x1)", "#xc"),
    ("((_ rotate_left 5) #x1)", "#x2"), ("((_ rotate_right 1) #x1)", "#x8"),
    ("((_ repeat 3) #b10)", "#b101010"), ("((_ sign_extend 4) #x8)", "#xf8"),
    ("((_ extract 7 4) #xa5)", "#xa"), ("(concat #b1 #x0 #b01)", "#b1000001"),
    ("(bvcomp #x3 #x3)", "#b1"), ("(bvxnor #xc #xa)", "#x9"),
    ("(bvmul #xffffffffffffffffffffffffffffffff #xffffffffffffffffffffffffffffffff)",
     "#x00000000000000000000000000000001"),
    ("(bvadd #xffffffffffffffffffffffffffffffff #x00000000000000000000000000000001)",
     "#x00000000000000000000000000000000"),
    ("(bvudiv #x100000000000000000000000000000000 #x000000000000000000000000000000003)",
     "#x055555555555555555555555555555555"),
    ("(_ bv340282366920938463463374607431768211457 128)", "#x00000000000000000000000000000001"),
    ("(bvnego #x8)", "true"), ("(bvsdivo #x8 #xf)", "true"), ("(bvumulo #x8 #x2)", "true"),
    ("(bvsmulo #xc #x2)", "false"), ("(bvsmulo #xc #x3)", "true"),
    ("(=> true false true)", "true"), ("(=> true true false)", "false"),
    ("(xor true true true)", "true"), ("(distinct #x1 #x2 #x1)", "false"),
    ("(= #x1 #x1 #x2)", "false"),
    ("(let ((x #x1)) (let ((x (bvadd x x)) (y x)) (concat x y)))", "#x21"),
    ("(= ((as const (Array (_ BitVec 1) (_ BitVec 4))) #x0)"
     " (store (store ((as const (Array (_ BitVec 1) (_ BitVec 4))) #x5) #b0 #x0) #b1 #x0))", "true"),
]


def _bits(v: str) -> str:
    if v.startswith("#x"):
        return "".join(format(int(c, 16), "04b") for c in v[2:])
    return v[2:] if v.startswith("#b") else v


@pytest.mark.parametrize("term,want", _EDGE, ids=[t for t, _ in _EDGE])
def test_edge_values(term, want):
    out = _run([str(_EXE)], "(set-logic QF_ABV)(check-sat)(get-value (%s))" % term)
    vals = _raw_values(out, 1)
    assert _bits(vals[0]) == _bits(want), out


def test_shared_definitions_cost_dag_size():
    """f_i(x) = f_{i-1}(x) + f_{i-1}(x): a tree of 2^400 nodes, a DAG of 400."""
    lines = ["(set-logic QF_BV)", "(declare-const a (_ BitVec 32))", "(assert (= a #x00000001))",
             "(define-fun f0 ((x (_ BitVec 32))) (_ BitVec 32) x)"]
    lines += ["(define-fun f%d ((x (_ BitVec 32))) (_ BitVec 32) (bvadd (f%d x) (f%d x)))"
              % (i, i - 1, i - 1) for i in range(1, 401)]
    text = "\n".join(lines) + "\n(check-sat)\n(get-value ((f31 a) (f400 a)))\n"
    p = subprocess.run([str(_EXE)], input=text, capture_output=True, text=True, timeout=10)
    vals = _raw_values(p.stdout, 2)
    assert _bits(vals[0]) == format(1 << 31, "032b")
    assert _bits(vals[1]) == "0" * 32


def test_deep_let_and_depth_guard():
    """A 20000-deep let evaluates; nesting past the guard is an error, not a crash."""
    def nest(n):
        return "".join("(let ((x%d (bvadd x%d #x01))) " % (i, i - 1) for i in range(1, n + 1)) \
            + "x%d" % n + ")" * n
    text = ("(set-logic QF_BV)(declare-const x0 (_ BitVec 8))(assert (= x0 #x00))(check-sat)"
            "(get-value (%s))" % nest(20000))
    p = subprocess.run([str(_EXE)], input=text, capture_output=True, text=True, timeout=60)
    assert p.returncode == 0, p.stderr[-500:]
    assert _bits(_raw_values(p.stdout, 1)[0]) == format(20000 % 256, "08b")
    deep = text.replace(nest(20000), nest(300000)) + "(get-value (x0))"
    p = subprocess.run([str(_EXE)], input=deep, capture_output=True, text=True, timeout=60)
    out = p.stdout.splitlines()
    assert out[1].startswith("(error ") and "nested too deeply" in out[1], out[1][:300]
    assert out[2] == "((x0 #b00000000))", "the session did not survive"
