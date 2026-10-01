"""Problem IR for the soundness campaign: terms, bit-exact evaluation, SMT-LIB2.

A problem is a dict of variable widths and a list of Boolean constraints.
Terms are tuples:

    ('var', name, w)  ('const', value, w)  (op, w, *args)   (op, 'bool', *args)

`w` is the result width of a bit-vector term. Evaluation follows SMT-LIB
QF_BV exactly (division by zero included); it shares no code with dv-solve,
which is what makes it an oracle. See docs/soundness_coverage_plan.md.
"""
from __future__ import annotations

import itertools
from dataclasses import dataclass, field

BV_BIN = ["bvadd", "bvsub", "bvmul", "bvand", "bvor", "bvxor", "bvshl", "bvlshr",
          "bvashr", "bvudiv", "bvurem", "bvsdiv", "bvsrem", "bvsmod"]
BV_UN = ["bvneg", "bvnot"]
CMP = ["=", "distinct", "bvult", "bvule", "bvugt", "bvuge",
       "bvslt", "bvsle", "bvsgt", "bvsge"]
BOOL = ["and", "or", "not", "=>", "xor", "ite"]


@dataclass
class Problem:
    widths: dict
    cons: list
    bins: set = field(default_factory=set)

    def smt2(self, get_model: bool = True) -> str:
        s = "(set-logic QF_BV)\n"
        s += "".join(f"(declare-const {n} (_ BitVec {w}))\n" for n, w in self.widths.items())
        s += "".join(f"(assert {smt(c)})\n" for c in self.cons)
        s += "(check-sat)\n"
        if get_model:
            s += "(get-value (" + " ".join(self.widths) + "))\n"
        return s


def _s(v: int, w: int) -> int:
    return v - (1 << w) if v >> (w - 1) & 1 else v


def width(t) -> int:
    return t[2] if t[0] in ("var", "const") else t[1]


def _binop(op, a, b, w):
    m = (1 << w) - 1
    if op == "bvadd": return (a + b) & m
    if op == "bvsub": return (a - b) & m
    if op == "bvmul": return (a * b) & m
    if op == "bvand": return a & b
    if op == "bvor": return a | b
    if op == "bvxor": return a ^ b
    if op == "bvshl": return (a << b) & m if b < w else 0
    if op == "bvlshr": return a >> b if b < w else 0
    if op == "bvashr": return (_s(a, w) >> min(b, w)) & m
    if op == "bvudiv": return m if b == 0 else a // b
    if op == "bvurem": return a if b == 0 else a % b
    sa, sb = _s(a, w), _s(b, w)
    if op == "bvsdiv":
        if sb == 0: return 1 if sa < 0 else m
        q = abs(sa) // abs(sb)
        return (-q if (sa < 0) != (sb < 0) else q) & m
    if op == "bvsrem":
        if sb == 0: return a
        r = abs(sa) % abs(sb)
        return (-r if sa < 0 else r) & m
    if op == "bvsmod":
        if sb == 0: return a
        r = abs(sa) % abs(sb)
        if r == 0: return 0
        if sa < 0 and sb > 0: return (-r + sb) & m
        if sa >= 0 and sb < 0: return (r + sb) & m
        if sa < 0 and sb < 0: return (-r) & m
        return r
    raise ValueError(op)


def _cmp(op, a, b, w):
    if op == "=": return a == b
    if op == "distinct": return a != b
    x, y = (a, b) if op in ("bvult", "bvule", "bvugt", "bvuge") else (_s(a, w), _s(b, w))
    return {"lt": x < y, "le": x <= y, "gt": x > y, "ge": x >= y}[op[3:]]


def ev(t, env):
    k = t[0]
    if k == "var": return env[t[1]]
    if k == "const": return t[1]
    if k == "true": return True
    if k == "false": return False
    w = t[1]
    if k in BV_BIN: return _binop(k, ev(t[2], env), ev(t[3], env), w)
    if k == "bvneg": return (-ev(t[2], env)) & ((1 << w) - 1)
    if k == "bvnot": return (~ev(t[2], env)) & ((1 << w) - 1)
    if k == "ite": return ev(t[3], env) if ev(t[2], env) else ev(t[4], env)
    if k == "extract": return (ev(t[4], env) >> t[3]) & ((1 << (t[2] - t[3] + 1)) - 1)
    if k == "concat": return (ev(t[2], env) << width(t[3])) | ev(t[3], env)
    if k == "zero_extend": return ev(t[2], env)
    if k == "sign_extend": return _s(ev(t[2], env), width(t[2])) & ((1 << w) - 1)
    if k in CMP: return _cmp(k, ev(t[2], env), ev(t[3], env), width(t[2]))
    if k == "and": return all(ev(a, env) for a in t[2:])
    if k == "or": return any(ev(a, env) for a in t[2:])
    if k == "not": return not ev(t[2], env)
    if k == "=>": return (not ev(t[2], env)) or ev(t[3], env)
    if k == "xor": return ev(t[2], env) != ev(t[3], env)
    raise ValueError(k)


def smt(t) -> str:
    k = t[0]
    if k == "var": return t[1]
    if k == "const": return f"(_ bv{t[1]} {t[2]})"
    if k in ("true", "false"): return k
    if k == "extract": return f"((_ extract {t[2]} {t[3]}) {smt(t[4])})"
    if k in ("zero_extend", "sign_extend"):
        return f"((_ {k} {t[1] - width(t[2])}) {smt(t[2])})"
    return "(" + k + " " + " ".join(smt(a) for a in t[2:]) + ")"


def children(t):
    """(index, child) pairs of the sub-terms of `t`."""
    if t[0] in ("var", "const", "true", "false"):
        return []
    start = 4 if t[0] == "extract" else 2
    return [(i, t[i]) for i in range(start, len(t))]


def solutions(widths: dict, cons: list):
    names = list(widths)
    for vals in itertools.product(*[range(1 << widths[n]) for n in names]):
        env = dict(zip(names, vals))
        if all(ev(c, env) for c in cons):
            yield env


def satisfiable(widths: dict, cons: list) -> bool:
    return next(solutions(widths, cons), None) is not None
