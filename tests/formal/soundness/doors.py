"""Front doors: run one Problem through a dv-solve entry point.

Each returns a list of Outcome, one per question asked (an incremental
script asks several). `expect` is the brute-force answer, so a door's
results can be judged without trusting dv-solve.
"""
from __future__ import annotations

import ctypes
import re
import subprocess
from dataclasses import dataclass

from .ir import OracleUndecided, Problem, ev, satisfiable, smt, width


@dataclass
class Outcome:
    door: str
    expect: str            # "sat" / "unsat"
    got: str               # "sat" / "unsat" / "unknown" / "timeout" / "error"
    model_ok: bool = True  # a returned model satisfies the constraints
    detail: str = ""

    @property
    def failed(self) -> bool:
        if self.got in ("unknown",):
            return False
        return self.got != self.expect or not self.model_ok


def _model(out: str) -> dict:
    m = {}
    for name, val in re.findall(r"\((\w+) #b([01]+)\)", out):
        m[name] = int(val, 2)
    for name, val in re.findall(r"\((\w+) #x([0-9a-fA-F]+)\)", out):
        m[name] = int(val, 16)
    return m


def _detail(err: str) -> str:
    """Keep stderr's tail, but never lose a step-checker report."""
    i = err.find("[step-check] INVALID")
    return err[i:i + 2000] if i >= 0 else err[-2000:]


def _check_model(p: Problem, cons: list, m: dict) -> bool:
    return set(m) == set(p.widths) and all(ev(c, m) for c in cons)


def smt2_batch(p: Problem, exe: str, timeout: float = 60.0) -> list:
    try:
        exp = "sat" if satisfiable(p.widths, p.cons) else "unsat"
    except OracleUndecided:
        return []
    try:
        r = subprocess.run([exe], input=p.smt2(), capture_output=True, text=True,
                           timeout=timeout)
        out, err = r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return [Outcome("smt2", exp, "timeout")]
    words = out.split()
    got = words[0] if words and words[0] in ("sat", "unsat", "unknown") else "error"
    ok = got != "sat" or _check_model(p, p.cons, _model(out))
    return [Outcome("smt2", exp, got, ok, _detail(err))]


def smt2_incremental(p: Problem, exe: str, timeout: float = 60.0) -> list:
    """Assert a prefix, check; push, assert the rest, check; pop, check."""
    k = max(1, len(p.cons) // 2)
    first, rest = p.cons[:k], p.cons[k:]
    decl = "(set-logic QF_BV)\n" + "".join(
        f"(declare-const {n} (_ BitVec {w}))\n" for n, w in p.widths.items())
    script = (decl + "".join(f"(assert {smt(c)})\n" for c in first) + "(check-sat)\n"
              "(push 1)\n" + "".join(f"(assert {smt(c)})\n" for c in rest) + "(check-sat)\n"
              "(get-value (" + " ".join(p.widths) + "))\n(pop 1)\n(check-sat)\n")
    try:
        exps = ["sat" if satisfiable(p.widths, first) else "unsat",
                "sat" if satisfiable(p.widths, p.cons) else "unsat"]
    except OracleUndecided:
        return []
    exps.append(exps[0])
    try:
        r = subprocess.run([exe, "--interactive"], input=script, capture_output=True,
                           text=True, timeout=timeout)
        out, err = r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return [Outcome("smt2-incr", exps[1], "timeout")]
    answers = [w for w in out.split() if w in ("sat", "unsat", "unknown")]
    res = []
    for i, exp in enumerate(exps):
        got = answers[i] if i < len(answers) else "error"
        ok = True
        if i == 1 and got == "sat":
            ok = _check_model(p, p.cons, _model(out))
        res.append(Outcome(f"smt2-incr[{i}]", exp, got, ok, _detail(err)))
    return res


# ---- Python builder (SystemVerilog semantics; builder-safe problems only) ----

_UNSAFE = {"bvudiv", "bvurem", "bvsdiv", "bvsrem", "bvsmod", "bvashr",
           "bvslt", "bvsle", "bvsgt", "bvsge"}


def builder_safe(p: Problem) -> bool:
    """True when every operator means the same in SystemVerilog and SMT-LIB."""
    def ok(t):
        if t[0] in _UNSAFE:
            return False
        if t[0] in ("var", "const", "true", "false"):
            return True
        start = 4 if t[0] == "extract" else 2
        return all(ok(c) for c in t[start:])
    return all(ok(c) for c in p.cons)


def _lower(b, t, ids):
    from dv_solve.problem import (BIN_ADD, BIN_SUB, BIN_MUL, BIN_BAND, BIN_BOR, BIN_BXOR,
                                  BIN_LSHIFT, BIN_RSHIFT, BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE,
                                  BIN_GT, BIN_GTE, BIN_AND, BIN_OR, UN_NEG, UN_NOT, UN_INVERT)
    binops = {"bvadd": BIN_ADD, "bvsub": BIN_SUB, "bvmul": BIN_MUL, "bvand": BIN_BAND,
              "bvor": BIN_BOR, "bvxor": BIN_BXOR, "bvshl": BIN_LSHIFT, "bvlshr": BIN_RSHIFT,
              "=": BIN_EQ, "distinct": BIN_NEQ, "bvult": BIN_LT, "bvule": BIN_LTE,
              "bvugt": BIN_GT, "bvuge": BIN_GTE, "xor": BIN_NEQ}
    k = t[0]
    L = lambda x: _lower(b, x, ids)  # noqa: E731
    if k == "var": return b.expr_var(ids[t[1]])
    if k == "const": return b.expr_const(t[1], width=t[2])
    if k in ("true", "false"): return b.expr_const(1 if k == "true" else 0, width=1)
    if k in binops: return b.expr_binary(binops[k], L(t[2]), L(t[3]))
    if k == "bvneg": return b.expr_unary(UN_NEG, L(t[2]))
    if k == "bvnot": return b.expr_unary(UN_INVERT, L(t[2]))
    if k == "not": return b.expr_unary(UN_NOT, L(t[2]))
    if k == "ite": return b.expr_ite(L(t[2]), L(t[3]), L(t[4]))
    if k == "extract": return b.expr_extract(L(t[4]), t[2], t[3])
    if k == "concat": return b.expr_concat(L(t[2]), L(t[3]), width(t[3]))
    if k in ("zero_extend", "sign_extend"):
        return b.expr_extend(L(t[2]), width(t[2]), t[1], k == "sign_extend")
    if k in ("and", "or"):
        acc = L(t[2])
        for a in t[3:]:
            acc = b.expr_binary(BIN_AND if k == "and" else BIN_OR, acc, L(a))
        return acc
    if k == "=>": return b.expr_binary(BIN_OR, b.expr_unary(UN_NOT, L(t[2])), L(t[3]))
    raise ValueError(f"no builder lowering for {k}")


def builder(p: Problem, time_limit_ms: int = 10000) -> list:
    """Solve through the Python builder, with clause learning off and on."""
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.ctx import (SolveCtx, CompileUnsatError, CompileIncompleteError,
                              SOLVE_OK, SOLVE_UNSAT, _SolveOpts)
    if max(p.widths.values()) > 64:
        return []                    # the builder's variables are at most 64 bits
    try:
        exp = "sat" if satisfiable(p.widths, p.cons) else "unsat"
    except OracleUndecided:
        return []
    ids = {n: i for i, n in enumerate(p.widths)}
    b = SolveProblemBuilder()
    for n, w in p.widths.items():
        b.add_var(ids[n], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in p.cons:
        b.add_constraint(_lower(b, c, ids))
    prob, _ = b.finalize()
    res = []
    try:
        ctx = SolveCtx(prob)
    except CompileUnsatError:
        return [Outcome("builder", exp, "unsat")]
    except CompileIncompleteError:
        return [Outcome("builder", exp, "unknown")]
    with ctx:
        for lcg in (0, 1):
            ctx.reset()
            rc = ctx._lib.dvs_solver_solve(ctx._ctx, ctypes.byref(_SolveOpts(
                seed=1, use_lcg=lcg, time_limit_ms=time_limit_ms)))
            got = {SOLVE_OK: "sat", SOLVE_UNSAT: "unsat"}.get(rc, "timeout")
            ok = True
            if got == "sat":
                m = {n: ctx.get_value(ids[n]) & ((1 << w) - 1) for n, w in p.widths.items()}
                ok = all(ev(c, m) for c in p.cons)
            res.append(Outcome(f"builder[lcg={lcg}]", exp, got, ok))
    return res
