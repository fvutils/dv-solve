"""Affine analysis before simulation and SAT (docs/fw_hdl_implementation_plan.md 4.1, 4.5).

The bit-blast engine first looks for XOR structure in the assertions' AIG and
decides the problem by Gaussian elimination over GF(2) when it can
(src/c/dvs_affine.c). An `unsat` from it involves no SAT call, so a bug in the
XOR recognition or the elimination is a wrong answer.

The generator draws XOR-heavy problems: shifts and rotations by constants,
masks, conditional XOR by a polynomial (the CRC step), constant tables indexed
by a few bits (affine like a CRC table, or arbitrary), extract/concat, with
some nonlinear operators and muxes mixed in so the analysis also meets opaque gates and
its candidate models can fail. Every answer is checked against z3 and against
dv-solve's own SAT path (DV_SIM_CHECK=1 re-solves each verdict reached before
SAT and aborts on a disagreement).

Scale with AFFINE_N (default 1000; the matcher mutant first fails at seed 776);
AFFINE_FIRST moves the window.
"""
from __future__ import annotations

import os
import random
import re
import subprocess
from pathlib import Path

import pytest

from .harness.z3_solver import Z3Solver

_REPO = Path(__file__).resolve().parents[2]
_EXE = Path(os.environ.get("DVS_SMT2_EXE", _REPO / "build" / "dv-solve-smt2"))
_N = int(os.environ.get("AFFINE_N", "1000"))
_FIRST = int(os.environ.get("AFFINE_FIRST", "0"))
_Z3 = Z3Solver()
_stats = {"checked": 0, "unsat": 0, "affine-unsat": 0, "affine-sat": 0, "opaque": 0,
          "tables": 0}

_NONLIN = ["bvand", "bvor", "bvadd", "bvmul"]


class _Gen:
    def __init__(self, seed: int):
        self.r = random.Random(seed)
        # Narrow widths make a misread gate meet the XOR it was taken for.
        self.w = self.r.choice([1, 2, 3, 4, 6, 8, 12, 16, 24, 32])
        self.nonlin = self.r.choice([0.0, 0.05, 0.15, 0.3])
        n = self.r.randint(1, 3)
        self.vars = [f"x{i}" for i in range(n)]

    def const(self, w: int) -> str:
        return f"(_ bv{self.r.randrange(1 << w)} {w})"

    def term(self, d: int) -> str:
        w, r = self.w, self.r
        if d == 0 or r.random() < 0.15:
            return r.choice(self.vars) if r.random() < 0.85 else self.const(w)
        k = r.random()
        a = self.term(d - 1)
        if k < self.nonlin:
            if r.random() < 0.4:
                # A mux with unrelated branches: not affine.
                i = r.randrange(w)
                return (f"(ite (= ((_ extract {i} {i}) {self.term(d - 1)}) #b1) "
                        f"{a} {self.term(d - 1)})")
            return f"({r.choice(_NONLIN)} {a} {self.term(d - 1)})"
        if k < 0.32:
            return self.table(a)
        if k < 0.38:
            return f"(bvxor {a} {self.term(d - 1)})"
        if k < 0.45:
            return f"(bvnot {a})"
        if k < 0.55:
            op = r.choice(["bvshl", "bvlshr"])
            return f"({op} {a} (_ bv{r.randrange(w)} {w}))"
        if k < 0.62:
            op = r.choice(["rotate_left", "rotate_right"])
            return f"((_ {op} {r.randrange(w)}) {a})"
        if k < 0.70:
            return f"(bvand {a} {self.const(w)})"
        if k < 0.82:
            # CRC step: shift right, XOR the polynomial when the low bit is set.
            i = r.randrange(w)
            return (f"(ite (= ((_ extract {i} {i}) {a}) #b1) "
                    f"(bvxor (bvlshr {a} (_ bv1 {w})) {self.const(w)}) "
                    f"(bvlshr {a} (_ bv1 {w})))")
        if k < 0.92 and w >= 2:
            c = r.randint(1, w - 1)
            lo = r.randint(0, w - c)
            return (f"(concat ((_ extract {lo + c - 1} {lo}) {a}) "
                    f"((_ extract {w - c - 1} 0) {self.term(d - 1)}))")
        return f"(bvxor {a} {self.const(w)})"

    def table(self, a: str) -> str:
        """A constant table indexed by a few bits of `a`: affine (a CRC
        table) or arbitrary."""
        w, r = self.w, self.r
        k = min(r.randint(2, 6), w)
        lo = r.randint(0, w - k)
        idx = f"((_ extract {lo + k - 1} {lo}) {a})"
        if r.random() < 0.6:
            base = [r.randrange(1 << w) for _ in range(k + 1)]
            tab = []
            for i in range(1 << k):
                v = base[k]
                for j in range(k):
                    if i >> j & 1:
                        v ^= base[j]
                tab.append(v)
        else:
            tab = [r.randrange(1 << w) for _ in range(1 << k)]
        out = f"(_ bv{tab[-1]} {w})"
        for i in range(len(tab) - 2, -1, -1):
            out = f"(ite (= {idx} (_ bv{i} {k})) (_ bv{tab[i]} {w}) {out})"
        return out

    def pred(self, d: int) -> str:
        r = self.r
        k = r.random()
        if d > 0 and k < 0.15:
            return f"({r.choice(['and', 'or'])} {self.pred(d - 1)} {self.pred(d - 1)})"
        if d > 0 and k < 0.2:
            return f"(not {self.pred(d - 1)})"
        if k < 0.3:
            i = r.randrange(self.w)
            return f"(= ((_ extract {i} {i}) {self.term(3)}) #b{r.randint(0, 1)})"
        return f"({r.choice(['=', '=', 'distinct'])} {self.term(3)} {self.term(3)})"

    def problem(self) -> str:
        lines = ["(set-logic QF_BV)"]
        lines += [f"(declare-const {v} (_ BitVec {self.w}))" for v in self.vars]
        lines += [f"(assert {self.pred(2)})" for _ in range(self.r.randint(1, 4))]
        lines.append("(check-sat)")
        return "\n".join(lines) + "\n"


def _dv(path: Path, extra: dict) -> tuple[str, str, int]:
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env.update({"DV_ENGINE": "bitblast", "DV_LOG": "1", "DV_SIM_CHECK": "1"}, **extra)
    p = subprocess.run([str(_EXE), str(path)], capture_output=True,
                       text=True, timeout=120, env=env)
    out = next((l.strip() for l in p.stdout.splitlines()
                if l.strip() in ("sat", "unsat", "unknown")), "none")
    return out, p.stderr, p.returncode


# Seeds that found bugs, run whatever AFFINE_N is. 5705: a derived table leaf
# read an inner gate of the cut before it was simulated (wrong unsat).
_PINNED = [5705]


@pytest.mark.parametrize("seed", sorted(set(range(_FIRST, _FIRST + _N)) | set(_PINNED)))
def test_affine_agrees_with_z3(seed: int, tmp_path: Path) -> None:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    if not _Z3.is_available():
        pytest.skip("z3 not available")
    smt2 = _Gen(seed).problem()
    f = tmp_path / f"affine_{seed}.smt2"
    f.write_text(smt2)
    want = _Z3.solve(f, timeout_s=60).result
    if want not in ("sat", "unsat"):
        pytest.skip(f"z3 gave {want}")
    got, err, rc = _dv(f, {})
    assert "MISMATCH" not in err and rc in (0, 1), (
        f"affine analysis disagrees with SAT on seed {seed} (exit {rc}):\n{err}\n{smt2}")
    assert got == want, f"seed {seed}: dv-solve {got}, z3 {want}\n{err}\n{smt2}"
    _stats["checked"] += 1
    _stats["unsat"] += want == "unsat"
    m = re.search(r"bb-affine: .*tables=(\d+).*opaque=(\d+).* -> (\w+)", err)
    if m and m.group(3) in ("sat", "unsat"):
        _stats[f"affine-{m.group(3)}"] += 1
        _stats["tables"] += int(m.group(1)) > 0
        _stats["opaque"] += int(m.group(2)) > 0


def test_zz_affine_coverage() -> None:
    """The analysis must decide problems both ways, with opaque gates in some
    and tables in some, or the fuzzer above tests nothing."""
    if _stats["checked"] < 50:
        pytest.skip("too few seeds ran")
    print(f"\naffine fuzz: {_stats}")
    n = _stats["checked"]
    assert _stats["affine-unsat"] >= n // 10, _stats
    assert _stats["affine-sat"] >= n // 10, _stats
    assert _stats["opaque"] >= n // 20, _stats
    assert _stats["tables"] >= n // 40, _stats
