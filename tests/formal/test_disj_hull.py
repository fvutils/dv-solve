"""Disjunction interval-hull propagation must not lose solutions.

Phase 2 of docs/cdcl_verilator_coverage_plan.md. `_fire_disj_clause` used pure
watched-literal propagation: it only acted once all but one disjunct was
falsified. For `x inside {3,4,5}` over a 32-bit x nothing is ever falsified, so
the domain stayed [0, 2^32) and the search enumerated it (10 s -> `unknown`).
`_disj_hull` now tightens each variable to the interval hull of the disjuncts.

WHY THIS FILE IS EXHAUSTIVE, NOT SPOT-CHECKED
---------------------------------------------
The hull computes a *tighter* domain. An off-by-one does not produce a crash or
a bogus model that the validation net would catch -- it silently deletes
solutions, and in the limit turns a satisfiable problem into a wrong `unsat`.
CDCL `unsat` is never re-validated, so this is the one failure mode with no
downstream net. So we do not sample: for every constraint below we recover
dv-solve's ENTIRE solution set -- by pinning each candidate under push/pop and
asking -- and compare it, as a set, against Python brute force over the whole
domain. Set equality catches over-tightening (missing models) and
under-tightening (spurious models) alike.

The `lhs op v` mirror arm and the saturating edges (constants at 0 and at the
width maximum) are the parts most likely to be wrong, so they are covered
exhaustively rather than spot-checked.

These are mutation-tested, not merely green: injecting an off-by-one into the
var-const LTE bound fails 20 of them, into GTE 18, flipping the mirror arm 1,
and turning the free-variable hull-kill into a `continue` fails 19.

Usage:
    direnv exec . pytest tests/formal/test_disj_hull.py -v
"""
from __future__ import annotations

import itertools
import os
import random
import re
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_DV = _REPO / "build" / "dv-solve-smt2"
_PURE = {"DV_NO_BITBLAST": "1", "DV_CDCL_TIME_LIMIT": "20"}
_LIT = re.compile(r"#[bx][0-9a-fA-F]+")

# Small enough to brute force the full cross product, wide enough that the
# hull actually has room to be wrong. Two-variable shapes use a narrower width
# because they probe the full width^2 grid.
_W = 5
_N = 1 << _W
_W2 = 4
_N2 = 1 << _W2

_OPS = {
    "=":     lambda a, b: a == b,
    "bvult": lambda a, b: a < b,
    "bvule": lambda a, b: a <= b,
    "bvugt": lambda a, b: a > b,
    "bvuge": lambda a, b: a >= b,
}


@pytest.fixture(autouse=True)
def _need_binary():
    if not _DV.is_file():
        pytest.skip("dv-solve-smt2 binary not built")


def _hex(v: int, w: int = _W) -> str:
    return f"(_ bv{v} {w})"


def _enumerate_dv(decls: list[str], asserts: list[str],
                  names: list[str], width: int) -> set[tuple]:
    """dv-solve's solution set, probed one candidate at a time.

    We deliberately do NOT enumerate via blocking clauses. Repeated
    `(assert (not (and (= x c))))` builds a heavily hole-punched domain -- the
    B18 shape -- and CDCL starts answering `unknown` partway through. That
    would turn a soundness gate into a silent skip.

    Instead each candidate is pinned under push/pop and asked directly. Every
    query is fully determined, so `unknown` is a real defect rather than a
    resource limit, and we assert on it. This also exercises the propagator
    under an incremental trail, which is where a bad explanation would bite.
    """
    grid = list(itertools.product(range(1 << width), repeat=len(names)))
    lines = ["(set-logic QF_BV)", *decls, *asserts]
    for point in grid:
        lines.append("(push 1)")
        for n, v in zip(names, point):
            lines.append(f"(assert (= {n} {_hex(v, width)}))")
        lines.append("(check-sat)")
        lines.append("(pop 1)")
    lines.append("(exit)")

    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input="\n".join(lines) + "\n", capture_output=True,
                       text=True, timeout=300, env={**os.environ, **_PURE})
    verdicts = [l.strip() for l in p.stdout.splitlines()
                if l.strip() in ("sat", "unsat", "unknown")]
    assert len(verdicts) == len(grid), \
        f"expected {len(grid)} verdicts, got {len(verdicts)}"
    bad = [(pt, v) for pt, v in zip(grid, verdicts) if v == "unknown"]
    assert not bad, f"unknown on fully-pinned points: {bad[:5]}"
    return {pt for pt, v in zip(grid, verdicts) if v == "sat"}


# ---------------------------------------------------------------- one variable

def _disj_1v(terms: list[tuple[str, int]]) -> str:
    parts = " ".join(f"({op} x {_hex(c)})" for op, c in terms)
    return f"(assert (or {parts}))"


def _brute_1v(terms: list[tuple[str, int]]) -> set[tuple]:
    return {(x,) for x in range(_N)
            if any(_OPS[op](x, c) for op, c in terms)}


# Every op paired with every other, at saturating and interior constants.
_EDGE = [0, 1, _N // 2, _N - 2, _N - 1]
_PAIRS = [
    [(o1, c1), (o2, c2)]
    for o1, o2 in itertools.combinations_with_replacement(sorted(_OPS), 2)
    for c1 in (0, _N - 1, 7)
    for c2 in (0, _N - 1, 19)
]


@pytest.mark.parametrize("terms", _PAIRS,
                         ids=lambda t: "_".join(f"{o}{c}" for o, c in t))
def test_two_term_disjunction_exact_solution_set(terms) -> None:
    """Hull over every op pair, including constants at both domain edges."""
    got = _enumerate_dv(["(declare-fun x () (_ BitVec %d))" % _W],
                        [_disj_1v(terms)], ["x"], _W)
    assert got == _brute_1v(terms)


@pytest.mark.parametrize("vals", [
    [3, 4, 5],           # the motivating `inside` set
    [0, 1, 2],           # hull touches the lower edge
    [_N - 3, _N - 2, _N - 1],   # hull touches the upper edge
    [0, _N - 1],         # hull spans the whole domain: must NOT tighten
    [7],                 # degenerate single disjunct
    [2, 9, 17, 30],      # sparse: hull is a strict over-approximation
])
def test_inside_set_exact_solution_set(vals) -> None:
    """`x inside {vals}` must yield exactly `vals` -- no more, no fewer.

    The sparse case matters most: the hull [2,30] is a strict superset of the
    solutions, so the watched-literal rule still has to carry the rest. If the
    hull were mistakenly treated as exact we would gain spurious models.
    """
    got = _enumerate_dv(["(declare-fun x () (_ BitVec %d))" % _W],
                        [_disj_1v([("=", v) for v in vals])], ["x"], _W)
    assert got == {(v,) for v in vals}


def test_hull_does_not_tighten_a_full_span_disjunction() -> None:
    """(x = 0) or (x >= 1) covers everything; every value must survive."""
    got = _enumerate_dv(["(declare-fun x () (_ BitVec %d))" % _W],
                        [_disj_1v([("=", 0), ("bvuge", 1)])], ["x"], _W)
    assert got == {(x,) for x in range(_N)}


# --------------------------------------------------------------- two variables

def test_disjunction_with_a_free_variable_is_not_hulled() -> None:
    """A disjunct that leaves y free must kill the hull for y.

    (x = 3) or (y = 4): when the x-disjunct holds, y is unconstrained. A hull
    that skipped the non-constraining disjunct instead of abandoning the hull
    would pin y to 4 and delete most of the solution set. Mutating that
    `usable = 0` to a `continue` fails 19 tests in this file, so the shape is
    covered -- mostly by the randomized arm below, which generates mixed-variable
    disjunctions in bulk.
    """
    decls = [f"(declare-fun x () (_ BitVec {_W2}))",
             f"(declare-fun y () (_ BitVec {_W2}))"]
    got = _enumerate_dv(
        decls, [f"(assert (or (= x {_hex(3, _W2)}) (= y {_hex(4, _W2)})))"],
        ["x", "y"], _W2)
    want = {(x, y) for x in range(_N2) for y in range(_N2) if x == 3 or y == 4}
    assert got == want


@pytest.mark.parametrize("op", sorted(_OPS))
def test_var_var_disjunct_mirror_arm(op) -> None:
    """`(op x y) or (= x c)` exercises the rhs==v mirror in _disj_var_range.

    The mirror arm reverses the operator onto the right-hand variable; getting
    the strict/non-strict edge backwards there is an off-by-one that only shows
    up as missing solutions.
    """
    decls = [f"(declare-fun x () (_ BitVec {_W2}))",
             f"(declare-fun y () (_ BitVec {_W2}))"]
    got = _enumerate_dv(
        decls, [f"(assert (or ({op} x y) (= x {_hex(6, _W2)})))"],
        ["x", "y"], _W2)
    want = {(x, y) for x in range(_N2) for y in range(_N2)
            if _OPS[op](x, y) or x == 6}
    assert got == want


# ------------------------------------------------- the 2^63 cliff (B22)

_B22 = [
    # (width, assertion, why)
    (64, "(or (bvult (_ bv5 64) v) (bvuge v (_ bv9 64)))",
     "the minimal repro: small constants, wrong UNSAT purely from width"),
    (64, "(or (bvult (_ bv1851740134304133266 64) v)"
         " (bvuge v (_ bv6873229369420466885 64)))",
     "the shape the fuzzer actually found"),
    (64, "(or (= v (_ bv0 64)) (= v (_ bv18446744073709551615 64)))",
     "both representable edges at once"),
    (64, "(or (bvuge v (_ bv18446744073709551615 64)) (bvult v (_ bv3 64)))",
     "disjunct pinned at 2^64-1, whose stored pattern is -1"),
    (63, "(or (bvult (_ bv5 63) v) (bvuge v (_ bv9 63)))",
     "just below the cliff"),
]


@pytest.mark.parametrize("width,body,why", _B22,
                         ids=[f"w{w}-{i}" for i, (w, _, _) in enumerate(_B22)])
def test_wide_unsigned_disjunction_is_not_unsat(width, body, why) -> None:
    """A satisfiable wide-unsigned disjunction must never come back UNSAT.

    For an unsigned var of width >= 64 the stored lo/hi are bit patterns, so
    2^64-1 reads back as -1. `_clause_definitely_false` compared them with bare
    signed operators, so `hi <= c` was true for every constant: both disjuncts
    were falsified simultaneously and the propagator raised a conflict. dv-solve
    answered UNSAT to `(or (bvult 5 v) (bvuge v 9))`.

    This is the failure mode with no downstream net -- a model would have been
    validated, but an UNSAT is trusted. `unknown` is an acceptable answer here;
    `unsat` is not.
    """
    script = (f"(set-logic QF_BV)\n(declare-const v (_ BitVec {width}))\n"
              f"(assert {body})\n(check-sat)\n(exit)\n")
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=script, capture_output=True, text=True,
                       timeout=60, env={**os.environ, **_PURE})
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    assert verdict != "unsat", f"wrong UNSAT ({why})"


@pytest.mark.parametrize("body", [
    "(bvult (_ bv5 65) v)",                              # a single comparison
    "(= v (_ bv5 65))",
    "(or (bvult (_ bv5 65) v) (bvuge v (_ bv9 65)))",    # and inside a disjunction
])
def test_b23_tier2_comparison_not_wrong_unsat(body) -> None:
    """B23: forced pure CDCL must not answer UNSAT to satisfiable >64-bit
    comparisons.

    Found while gating Phase 2, but NOT a disjunction bug and NOT caused by the
    hull -- a lone `(assert (bvult (_ bv5 65) v))` reproduced it. The CDCL
    engine reasoned through var_lo64/var_hi64, which could not represent a
    tier-2 domain, and computed with truncated bounds.

    Fixed by G2 (26e951e): the engine now DECLINES a >64-bit variable, so the
    answer is `unknown`, which is sound. This was pinned xfail(strict=True)
    until then; it is now a regression against the wrong UNSAT coming back.
    Width > 64 still auto-routes to bitblast in production; only
    DV_NO_BITBLAST=1, a test-only override, reaches this path.
    """
    script = ("(set-logic QF_BV)\n(declare-const v (_ BitVec 65))\n"
              f"(assert {body})\n(check-sat)\n(exit)\n")
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=script, capture_output=True, text=True,
                       timeout=60, env={**os.environ, **_PURE})
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    assert verdict in ("sat", "unknown"), f"B23 wrong answer: {verdict}"


# ------------------------------------------------- B29: var-var 2^63 cliff

_B29 = [
    ("(or (bvult a b) (bvugt a b))",  "strict either-way"),
    ("(or (bvule a b) (bvuge a b))",  "non-strict either-way (a tautology)"),
    ("(or (bvult a b) (= a b))",      "less-than or equal-to"),
    ("(or (bvugt a b) (= a b))",      "greater-than or equal-to"),
]


@pytest.mark.parametrize("width", [8, 32, 63, 64])
@pytest.mark.parametrize("body,why", _B29)
def test_b29_var_var_disjunct_wrong_unsat(width, body, why) -> None:
    """B29 (FIXED 2026-08-18): the var-var half of B22.

    B22 made `_clause_definitely_false` -- the var-CONSTANT side -- sign-aware.
    The var-VAR branch of `_fire_disj_clause` kept bare signed operators, so at
    width 64, where an unsigned var's full domain is lo=0 / hi=-1 (the pattern
    2^64-1), every disjunct was "definitely false" at once and the propagator
    raised a level-0 conflict. `(or (bvult a b) (bvugt a b))` -- satisfiable by
    any two distinct values -- answered UNSAT in the DEFAULT engine.

    Every body here is satisfiable at every width; the narrow widths are the
    control, since they took the same code path and always answered `sat`.
    """
    script = (f"(set-logic QF_BV)\n"
              f"(declare-const a (_ BitVec {width}))\n"
              f"(declare-const b (_ BitVec {width}))\n"
              f"(assert {body})\n(check-sat)\n(exit)\n")
    for env in (_PURE, {"DV_CDCL_TIME_LIMIT": "20"}):
        p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                           input=script, capture_output=True, text=True,
                           timeout=60, env={**os.environ, **env})
        verdict = next((l.strip() for l in p.stdout.splitlines()
                        if l.strip() in ("sat", "unsat", "unknown")), "none")
        assert verdict != "unsat", f"B29 wrong UNSAT at w{width} ({why})"


@pytest.mark.parametrize("width", [8, 32, 64])
def test_b29_mirror_var_var_still_unsat(width) -> None:
    """The fix must not buy `sat` by losing the var-var conflict entirely.

    The disjuncts must be separated by CONSTANT bounds, not by a second var-var
    assert: `(bvult a b) AND (bvuge a b)` is also unsat but does not terminate
    at width >= 32 on either engine (pre-existing, see B30 in the backlog), so
    it cannot serve as the mirror.
    """
    script = (f"(set-logic QF_BV)\n"
              f"(declare-const a (_ BitVec {width}))\n"
              f"(declare-const b (_ BitVec {width}))\n"
              f"(assert (bvule a (_ bv5 {width})))\n"
              f"(assert (bvuge b (_ bv100 {width})))\n"
              f"(assert (or (bvugt a b) (bvugt a b)))\n(check-sat)\n(exit)\n")
    p = subprocess.run([str(_DV), "--interactive", "--engine=cdcl"],
                       input=script, capture_output=True, text=True,
                       timeout=60, env={**os.environ, **_PURE})
    verdict = next((l.strip() for l in p.stdout.splitlines()
                    if l.strip() in ("sat", "unsat", "unknown")), "none")
    assert verdict == "unsat", f"genuinely-unsat var-var shape answered {verdict}"


# ------------------------------------------------------------------- randomized

@pytest.mark.parametrize("seed", range(24))
def test_random_disjunction_exact_solution_set(seed) -> None:
    """Randomized shapes over two vars, exhaustively verified per seed."""
    rng = random.Random(seed)
    n = rng.randint(2, 4)
    terms = []
    for _ in range(n):
        v = rng.choice("xy")
        op = rng.choice(sorted(_OPS))
        if rng.random() < 0.4:
            other = "y" if v == "x" else "x"
            terms.append((v, op, other, None))
        else:
            terms.append((v, op, None, rng.randrange(_N2)))

    parts = []
    for v, op, other, c in terms:
        parts.append(f"({op} {v} {other})" if other
                     else f"({op} {v} {_hex(c, _W2)})")
    decls = [f"(declare-fun x () (_ BitVec {_W2}))",
             f"(declare-fun y () (_ BitVec {_W2}))"]
    got = _enumerate_dv(decls, [f"(assert (or {' '.join(parts)}))"],
                        ["x", "y"], _W2)

    def holds(x, y):
        env = {"x": x, "y": y}
        for v, op, other, c in terms:
            rhs = env[other] if other else c
            if _OPS[op](env[v], rhs):
                return True
        return False

    want = {(x, y) for x in range(_N2) for y in range(_N2) if holds(x, y)}
    assert got == want
