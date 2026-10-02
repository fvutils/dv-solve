"""(get-unsat-core) must always answer on stdout.

Verilator 5.x re-issues an unsatisfiable randomize() problem with named
assertions and then reads ONE line in reply to (get-unsat-core). Until
2026-09-30 dv-solve did not implement the command and wrote its error only to
the diagnostic log, so an unsat randomize() hung the simulation (B33).

Without :produce-unsat-cores the reply is the list of :named assertions in
scope: the whole asserted set is unsatisfiable, so that list is a valid (not
necessarily minimal) core. With it, the core is minimised: dropping any one
reported name leaves a satisfiable set. Verilator sets the option, so its
UNSATCONSTR warnings name only the constraints in conflict.
"""
from __future__ import annotations

import itertools
import random
import subprocess
from pathlib import Path

import pytest

_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"


def _run(script: str, *args: str) -> list[str]:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 binary not built")
    p = subprocess.run([str(_EXE), "--interactive", *args], input=script,
                       capture_output=True, text=True, timeout=20)
    return p.stdout.splitlines()


_HDR = "(set-logic QF_BV)\n(declare-const x (_ BitVec 8))\n"


def test_core_lists_named_assertions_in_scope():
    out = _run(_HDR +
               "(assert (! (bvugt x #x0a) :named c0))\n"
               "(push 1)\n(assert (! (bvult x #x05) :named c1))\n"
               "(check-sat)\n(get-unsat-core)\n"
               "(pop 1)\n(assert (! (bvult x #x0c) :named c2))\n"
               "(assert (bvult x #x02))\n(check-sat)\n(get-unsat-core)\n")
    assert out == ["unsat", "(c0 c1)", "unsat", "(c0 c2)"]


def test_core_without_unsat_is_an_error_reply_not_silence():
    out = _run(_HDR + "(get-unsat-core)\n(assert (= x #x01))\n"
               "(check-sat)\n(get-unsat-core)\n")
    assert len(out) == 3
    assert out[0].startswith("(error") and out[2].startswith("(error")
    assert out[1] == "sat"


def test_reset_clears_names():
    out = _run(_HDR + "(assert (! (= x #x01) :named a))\n(reset)\n" + _HDR +
               "(assert (! (= x #x01) :named b))\n"
               "(assert (! (= x #x02) :named c))\n(check-sat)\n(get-unsat-core)\n")
    assert out == ["unsat", "(b c)"]


def test_verilator_unsat_randomize_shape_does_not_hang():
    # The exact command sequence Verilator 5.046 sends for an unsat randomize().
    script = ("(set-option :produce-models true)\n(set-logic QF_ABV)\n"
              "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
              "(declare-fun x () (_ BitVec 8))\n"
              "(assert (= #b1 (__Vbv (bvugt x #x0a))))\n"
              "(assert (= #b1 (__Vbv (bvult x #x05))))\n(check-sat)\n(reset)\n"
              "(set-option :produce-unsat-cores true)\n(set-logic QF_ABV)\n"
              "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
              "(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))\n"
              "(declare-fun x () (_ BitVec 8))\n"
              "(assert (! (= #b1 (__Vbv (bvugt x #x0a))) :named cons0))\n"
              "(assert (! (= #b1 (__Vbv (bvult x #x05))) :named cons1))\n"
              "(check-sat)\n(get-unsat-core) \n(reset)\n")
    assert _run(script, "--mode=verilator") == ["unsat", "unsat", "(cons0 cons1)"]


_CORES = "(set-option :produce-unsat-cores true)\n"


def test_minimised_core_leaves_out_unneeded_names():
    out = _run(_CORES + _HDR + "(declare-const y (_ BitVec 8))\n"
               "(assert (! (bvugt x #x0a) :named c1))\n"
               "(assert (! (bvult y #x20) :named c2))\n"
               "(assert (! (bvult x #x05) :named c3))\n"
               "(assert (! (= y #x07) :named c4))\n(check-sat)\n(get-unsat-core)\n")
    assert out == ["unsat", "(c1 c3)"]


def test_minimised_core_follows_push_and_pop():
    out = _run(_CORES + _HDR +
               "(assert (! (bvugt x #x0a) :named c0))\n"
               "(assert (! (bvult x #xf0) :named c1))\n"
               "(push 1)\n(assert (! (bvult x #x05) :named c2))\n"
               "(check-sat)\n(get-unsat-core)\n"
               "(pop 1)\n(assert (! (= x #xf5) :named c3))\n"
               "(check-sat)\n(get-unsat-core)\n")
    assert out == ["unsat", "(c0 c2)", "unsat", "(c1 c3)"]


def test_unnamed_assertions_alone_give_an_empty_core():
    out = _run(_CORES + _HDR + "(assert (! (bvugt x #x0a) :named c0))\n"
               "(assert (= x #x01))\n(assert (= x #x02))\n(check-sat)\n(get-unsat-core)\n")
    assert out == ["unsat", "()"]


def test_core_after_check_sat_assuming_is_not_minimised():
    # The core would have to account for the assumptions; report every name.
    out = _run(_CORES + _HDR + "(declare-const p Bool)\n"
               "(assert (! (bvugt x #x0a) :named c0))\n"
               "(assert (! (bvult x #xf0) :named c1))\n"
               "(assert (! (=> p (bvult x #x05)) :named c2))\n"
               "(check-sat-assuming (p))\n(get-unsat-core)\n")
    assert out == ["unsat", "(c0 c1 c2)"]


def test_verilator_unsat_randomize_names_only_the_conflict():
    script = ("(reset)\n(set-option :produce-unsat-cores true)\n(set-logic QF_ABV)\n"
              "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
              "(declare-fun x () (_ BitVec 8))\n(declare-fun y () (_ BitVec 8))\n"
              "(assert (! (= #b1 (__Vbv (bvugt x #x0a))) :named cons0))\n"
              "(assert (! (= #b1 (__Vbv (bvult y x))) :named cons1))\n"
              "(assert (! (= #b1 (__Vbv (bvult x #x05))) :named cons2))\n"
              "(check-sat)\n(get-unsat-core) \n(reset)\n")
    assert _run(script, "--mode=verilator") == ["unsat", "(cons0 cons2)"]


# Random constraint sets over three 4-bit variables, checked by brute force:
# the reported core must be unsatisfiable together with the unnamed
# assertions, and must become satisfiable when any one member is removed.
_OPS = {
    "bvult": lambda a, b: a < b, "bvugt": lambda a, b: a > b,
    "bvule": lambda a, b: a <= b, "=": lambda a, b: a == b,
    "distinct": lambda a, b: a != b,
}
_TERMS = {
    "x": lambda v: v[0], "y": lambda v: v[1], "z": lambda v: v[2],
    "(bvadd x y)": lambda v: (v[0] + v[1]) & 15,
    "(bvand y z)": lambda v: v[1] & v[2],
    "(bvsub z x)": lambda v: (v[2] - v[0]) & 15,
}


def _random_constraint(rng: random.Random):
    op = rng.choice(list(_OPS))
    lhs = rng.choice(list(_TERMS))
    if rng.random() < 0.5:
        k = rng.randrange(16)
        rhs, rf = f"(_ bv{k} 4)", (lambda v, k=k: k)
    else:
        rhs = rng.choice(["x", "y", "z"])
        rf = _TERMS[rhs]
    lf, of = _TERMS[lhs], _OPS[op]
    return f"({op} {lhs} {rhs})", (lambda v: of(lf(v), rf(v)))


def _sat(preds) -> bool:
    return any(all(p(v) for p in preds) for v in itertools.product(range(16), repeat=3))


def test_minimised_cores_are_minimal_against_brute_force():
    rng = random.Random(20261001)
    checked = 0
    for _ in range(400):
        named = [_random_constraint(rng) for _ in range(rng.randrange(3, 8))]
        unnamed = [_random_constraint(rng) for _ in range(rng.randrange(0, 2))]
        if _sat([p for _, p in named + unnamed]):
            continue
        script = (_CORES + "(set-logic QF_BV)\n" +
                  "".join(f"(declare-const {n} (_ BitVec 4))\n" for n in "xyz") +
                  "".join(f"(assert (! {t} :named n{i}))\n" for i, (t, _) in enumerate(named)) +
                  "".join(f"(assert {t})\n" for t, _ in unnamed) +
                  "(check-sat)\n(get-unsat-core)\n")
        out = _run(script)
        assert out[0] == "unsat", (script, out)
        core = [int(n[1:]) for n in out[1].strip("()").split()]
        base = [p for _, p in unnamed]
        assert not _sat(base + [named[i][1] for i in core]), ("core is sat", script, out)
        for drop in core:
            rest = [named[i][1] for i in core if i != drop]
            assert _sat(base + rest), ("core not minimal", script, out, drop)
        checked += 1
    assert checked >= 50, checked
