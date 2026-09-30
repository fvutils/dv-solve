"""Regression: `bvneg` must wrap modulo 2^w on every engine.

Until 2026-09-30 the SMT-LIB2 front end lowered `bvneg` to UN_NEG, which the
CDCL engine (and the model validator) treat as integer negation with no wrap.
For an unsigned x in [0, 2^w) the result then lies in (-2^w, 0], so
`(= (bvneg x) K)` came back `unsat` for EVERY constant K -- including
`(= (bvneg x) #xff)`, satisfied by x = 1. The fuzzer never generated `bvneg`,
so nothing caught it. The front end now lowers `bvneg x` as `bvsub 0 x`.

Each case is cross-checked against z3 on both dv-solve engines.
"""
from __future__ import annotations

import pytest

from .harness.dv_solve_smt2_solver import DvSolveSMT2Solver, DvSolveSMT2BBSolver
from .harness.z3_solver import Z3Solver

_DV_CDCL = DvSolveSMT2Solver()
_DV_BB = DvSolveSMT2BBSolver()
_Z3 = Z3Solver()


def _lit(v: int, w: int) -> str:
    return f"(_ bv{v} {w})"


def _cases() -> list[tuple[str, str]]:
    cases = []
    # Every constant at width 4; boundary constants at the wider widths.
    for w, ks in ((4, range(16)),
                  (8, (0, 1, 0x7F, 0x80, 0x81, 0xFF)),
                  (16, (0, 1, 0x8000, 0xFFFF)),
                  (32, (0, 1, 0x80000000, 0xFFFFFFFF)),
                  (64, (0, 1, 0xFF, (1 << 63) - 1))):
        for k in ks:
            cases.append((f"eq_w{w}_k{k}", f"(declare-const x (_ BitVec {w}))\n"
                          f"(assert (= (bvneg x) {_lit(k, w)}))"))
    # Composed uses: bvneg under arithmetic and comparisons.
    cases += [
        ("pinned_sat", "(declare-const x (_ BitVec 8))\n"
                       "(assert (= x #x01))\n(assert (= (bvneg x) #xff))"),
        ("pinned_unsat", "(declare-const x (_ BitVec 8))\n"
                         "(assert (= x #x01))\n(assert (= (bvneg x) #x01))"),
        ("add", "(declare-const x (_ BitVec 8))\n(declare-const y (_ BitVec 8))\n"
                "(assert (bvugt x #x10))\n(assert (= (bvadd (bvneg x) y) #x05))"),
        ("ult_const", "(declare-const x (_ BitVec 8))\n"
                      "(assert (bvugt x #x80))\n(assert (bvult (bvneg x) #x10))"),
        # 8 bits, not wider: CDCL cannot see that double negation is the
        # identity, so it enumerates x until its time budget runs out and then
        # escalates to bitblast. Correct, but ~10 s at 16 bits.
        ("double", "(declare-const x (_ BitVec 8))\n"
                   "(assert (distinct (bvneg (bvneg x)) x))"),
    ]
    return cases


_CASES = _cases()


@pytest.fixture(scope="module")
def z3_oracle():
    if not _Z3.is_available():
        pytest.skip("z3 not on PATH; cannot cross-check")
    return _Z3


@pytest.mark.parametrize("name,body", _CASES, ids=[c[0] for c in _CASES])
@pytest.mark.parametrize("engine", [_DV_CDCL, _DV_BB], ids=["cdcl", "bitblast"])
def test_bvneg_matches_z3(name, body, engine, z3_oracle, tmp_path):
    if not engine.is_available():
        pytest.skip("dv-solve-smt2 binary not built")
    f = tmp_path / f"{name}.smt2"
    f.write_text(f"(set-logic QF_BV)\n{body}\n(check-sat)\n")
    want = z3_oracle.solve(f, timeout_s=10.0).result
    got = engine.solve(f, timeout_s=10.0).result
    assert got == want, f"{name}: dv-solve says {got}, z3 says {want}\n{f.read_text()}"
