"""Verilator's SMT-LIB2 transcript, read as the soundness IR.

A capture (`+verilator+solver+file+<path>`) logs every query a randomize()
sends the solver. Only the first query is the class's constraint problem:
later ones add Verilator's own protocol -- the knobs of `solve ... before`
pinned in phases, a value it drew itself for each `dist` (as `x == k`), and
parity "hash" asserts for diversity. The corpus does not measure that
protocol; it uses the first query as an oracle for the native test (the
builder problem must have the same solutions, see equiv.py) and as an
independent check of every drawn model (ir.ev).

`base_query` returns that problem as (widths, constraints) in the IR of
tests/formal/soundness/ir.py, with the `dist` pins dropped.
"""
from __future__ import annotations

import re

_TOK = re.compile(r"\(|\)|#b[01]+|#x[0-9a-fA-F]+|[^\s()]+")


def _sexprs(text: str):
    """The top-level S-expressions of `text`, as nested lists of tokens."""
    stack, out = [[]], []
    for tok in _TOK.findall(text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            done = stack.pop()
            stack[-1].append(done)
            if len(stack) == 1:
                out.append(stack[-1].pop())
        else:
            stack[-1].append(tok)
    return out


def _unsplit(lines, chunk=4096):
    """Undo the log's chunk splits in one write burst (prefix "  " lines).

    Verilator logs each 4096-byte write to the solver as its own text, adding
    a newline when a chunk ends mid-line: a logged line that exactly fills
    the rest of a chunk continues on the next line.
    """
    out, cur, left = [], "", chunk
    for ln in lines:
        c = ln[2:] if ln.startswith("  ") else ln
        if len(c) == left:              # chunk ended mid-line: no real newline
            cur += c
            left = chunk
            continue
        out.append("  " + cur + c)
        cur = ""
        left -= len(c) + 1
        if left <= 0:
            left += chunk
    if cur:
        out.append("  " + cur)
    return out


def _query_text(transcript: str) -> str:
    """The first query: from the first `produce-models` to its check-sat."""
    raw = transcript.splitlines()
    k = next((n for n, ln in enumerate(raw) if "(set-option :produce-models" in ln), None)
    if k is not None:                   # the query starts a write burst
        e = next((n for n in range(k, len(raw)) if not raw[n].startswith("  ")), len(raw))
        raw = raw[:k] + _unsplit(raw[k:e]) + raw[e:]
    lines = [ln for ln in raw
             if not ln.startswith("#") and not ln.startswith("<")]
    text = "\n".join(lines)
    i = text.find("(set-option :produce-models")
    if i < 0:
        raise ValueError("no query in the transcript")
    j = text.find("(check-sat)", i)
    if j < 0:
        raise ValueError("first query has no check-sat")
    return text[i:j]


class _Conv:
    def __init__(self, widths, arrays=None):
        self.w = widths
        self.defs = {}                      # nullary define-fun -> its term
        self.arrays = arrays or {}          # array var -> element width

    def width(self, t):
        if t[0] in ("var", "const"):
            return t[2]
        if t[1] == "bool":
            raise ValueError(f"Boolean where a bit-vector is expected: {t}")
        return t[1]

    def bv1(self, b):                       # __Vbv: Bool -> (_ BitVec 1)
        return ("ite", 1, b, ("const", 1, 1), ("const", 0, 1))

    def term(self, s):
        if isinstance(s, str):
            if s.startswith("#b"):
                return ("const", int(s[2:], 2), len(s) - 2)
            if s.startswith("#x"):
                return ("const", int(s[2:], 16), 4 * (len(s) - 2))
            if s == "true":
                return ("true",)
            if s == "false":
                return ("false",)
            if s in self.w:
                return ("var", s, self.w[s])
            if s in self.defs:
                return self.defs[s]
            raise ValueError(f"unknown symbol {s}")
        head = s[0]
        if head == "select" and isinstance(s[1], str) and s[1] in self.arrays:
            # an array of variables read at a constant index: element `a[k]` as its own variable
            k = self.term(s[2])
            if k[0] != "const":
                raise ValueError(f"select of {s[1]} at a non-constant index")
            name = f"{s[1]}[{k[1]}]"
            self.w[name] = self.arrays[s[1]]
            return ("var", name, self.w[name])
        if isinstance(head, list):          # ((_ extract h l) x) and friends
            ix = head
            assert ix[0] == "_", s
            a = self.term(s[1])
            if ix[1] == "extract":
                h, l = int(ix[2]), int(ix[3])
                return ("extract", h - l + 1, h, l, a)
            if ix[1] in ("zero_extend", "sign_extend"):
                return (ix[1], self.width(a) + int(ix[2]), a)
            if ix[1] == "repeat":           # n copies of a, as a concat chain
                acc = a
                for _ in range(int(ix[2]) - 1):
                    acc = ("concat", self.width(acc) + self.width(a), acc, a)
                return acc
            raise ValueError(f"unsupported indexed op {ix}")
        if head == "_" and s[1].startswith("bv"):
            return ("const", int(s[1][2:]), int(s[2]))
        args = [self.term(a) for a in s[1:]]
        if head == "__Vbv":
            return self.bv1(args[0])
        if head == "__Vbool":
            return ("=", "bool", ("const", 1, 1), args[0])
        if head in ("and", "or"):
            return (head, "bool", *args)
        if head in ("not",):
            return ("not", "bool", args[0])
        if head in ("=>", "xor"):
            return (head, "bool", args[0], args[1])
        if head in ("=", "distinct", "bvult", "bvule", "bvugt", "bvuge",
                    "bvslt", "bvsle", "bvsgt", "bvsge"):
            if head == "=" and len(args) == 2 and args[0][0] in ("true", "false", "and", "or",
                                                                  "not", "=>", "xor") or \
                    (head == "=" and args and len(args[0]) > 1 and args[0][1] == "bool"):
                return ("not", "bool", ("xor", "bool", args[0], args[1]))
            return (head, "bool", args[0], args[1])
        if head == "ite":
            c, a, b = args
            if a[0] in ("true", "false") or (len(a) > 1 and a[1] == "bool"):
                return ("or", "bool", ("and", "bool", c, a), ("and", "bool", ("not", "bool", c), b))
            return ("ite", self.width(a), c, a, b)
        if head == "concat":
            acc = args[0]
            for a in args[1:]:
                acc = ("concat", self.width(acc) + self.width(a), acc, a)
            return acc
        if head in ("bvneg", "bvnot"):
            return (head, self.width(args[0]), args[0])
        if head in ("bvand", "bvor", "bvxor", "bvadd", "bvmul"):   # n-ary in SMT-LIB
            acc = args[0]
            for a in args[1:]:
                acc = (head, self.width(acc), acc, a)
            return acc
        if head in ("bvsub", "bvudiv", "bvurem", "bvshl", "bvlshr", "bvashr",
                    "bvsdiv", "bvsrem", "bvsmod"):
            return (head, self.width(args[0]), args[0], args[1])
        raise ValueError(f"unsupported op {head}")


def _pin_bv(x, names):
    """(__Vbv (= v k)) for v in names (v possibly zero/sign-extended)."""
    if x[0] == "ite":
        inner = x[2]
        if inner[0] == "=" and inner[3][0] == "const":
            v = inner[2]
            if v[0] in ("zero_extend", "sign_extend"):
                v = v[2]
            return v[0] == "var" and v[1] in names
    return False


def _is_pin(t, names):
    """(= #b1 (__Vbv (= v k))) for v in names, after conversion: v == k."""
    return t[0] == "=" and t[2] == ("const", 1, 1) and _pin_bv(t[3], names)


def _strip_pin(t, names):
    """(= #b1 (bvand M pin)) -> (= #b1 M): a `dist` inside an if/else (Verilator
    ANDs the drawn pin onto the branch's membership). Recursive: the pin also
    sits inside an ite branch, e.g. `if (c) {..} else { x dist {..}; }`."""
    if t[0] in ("var", "const", "true", "false"):
        return t
    if t[0] == "bvand" and _pin_bv(t[3], names):
        return _strip_pin(t[2], names)
    head = 4 if t[0] == "extract" else 2
    return tuple(t[:head]) + tuple(_strip_pin(a, names) for a in t[head:])


def _occurs(e, t):
    if t == e:
        return True
    if not isinstance(t, tuple) or t[0] in ("var", "const", "true", "false"):
        return False
    return any(_occurs(e, a) for a in t[1:] if isinstance(a, tuple))


def _is_expr_pin(t, prev):
    """(= #b1 (__Vbv (= E k))) right after a `dist` membership (an OR) over E,
    E not a variable: Verilator's draw for a `dist` on an expression, e.g.
    `$countones(m) dist {..}` (such a dist has no variable to name in DIST)."""
    if prev is None or not (t[0] == "=" and t[2] == ("const", 1, 1) and t[3][0] == "ite"):
        return False
    inner = t[3][2]
    if inner[0] != "=" or inner[3][0] != "const":
        return False
    e = inner[2]
    if e[0] in ("zero_extend", "sign_extend") and e[2][0] == "var" or e[0] == "var":
        return False
    m = prev[3] if prev[0] == "=" and prev[2] == ("const", 1, 1) else prev
    return m[0] in ("bvor", "or") and _occurs(e, m)


def base_query(transcript: str, dist_vars=()):
    """(widths, constraints) of the first query, without `dist` pins."""
    widths, cons, arrays = {}, [], {}
    exprs = _sexprs(_query_text(transcript))
    for s in exprs:
        if s[0] == "declare-fun":
            name, sort = s[1], s[3]
            if isinstance(sort, list) and sort[0] == "Array" and sort[2][1] == "BitVec":
                arrays[name] = int(sort[2][2])     # elements named name[k] where selected
                continue
            if not (isinstance(sort, list) and sort[0] == "_" and sort[1] == "BitVec"):
                raise ValueError(f"unsupported sort for {name}: {sort}")
            widths[name] = int(sort[2])
    conv = _Conv(widths, arrays)
    prev = None
    for s in exprs:
        if s[0] == "assert":
            t = conv.term(s[1])
            if _is_pin(t, set(dist_vars)) or _is_expr_pin(t, prev):
                prev = None
                continue
            t = _strip_pin(t, set(dist_vars))
            cons.append(t)
            prev = t
    return widths, cons


def problem_smt2_query(script: str):
    """(widths, constraints) of a dvs_problem_write_smt2 script (v<id> names)."""
    widths, cons = {}, []
    exprs = _sexprs(script)
    for s in exprs:
        if s[0] in ("declare-const", "declare-fun"):
            sort = s[2] if s[0] == "declare-const" else s[3]
            widths[s[1]] = int(sort[2])
    conv = _Conv(widths)
    for s in exprs:
        if s[0] == "define-fun" and not s[2]:      # shared subterm (define-fun _dN () T e)
            conv.defs[s[1]] = conv.term(s[4])
        if s[0] == "assert":
            cons.append(conv.term(s[1]))
    return widths, cons
