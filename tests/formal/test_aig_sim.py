"""Bit-parallel simulation before SAT (docs/fw_hdl_implementation_plan.md 3.3, 3.4).

The bit-blast engine first simulates the assertions' input cone: an exhaustive
sweep when it is small (which decides `unsat` with no SAT call, so a simulator
bug is a wrong answer), a random pass otherwise, and a sweep raced against SAT
in between (src/c/dvs_aigsim.c, _solve_with_sim in src/c/dvs_bbsolver.c).

The fuzzer draws small problems over the whole operator set, so the sweep
decides most of them, and checks every answer two ways: against z3, and
against dv-solve's own SAT path (DV_SIM_CHECK=1 makes the solver re-solve each
simulation verdict with SAT and abort on a disagreement). Half the seeds force
the race (DV_SIM_MAX_WORK=0). Every third sets `:random-seed`, which makes the
sweep sample a model uniformly instead of stopping at the first (piece 6.4);
the front end's model check turns a model that breaks an assertion into
`unknown`, which fails here.

Scale with AIG_SIM_N (default 300).
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
# DVS_SMT2_EXE: test another build of dv-solve-smt2.
_EXE = Path(os.environ.get("DVS_SMT2_EXE", _REPO / "build" / "dv-solve-smt2"))
_N = int(os.environ.get("AIG_SIM_N", "300"))
_Z3 = Z3Solver()
_stats = {"exhaustive": 0, "raced": 0, "random": 0, "checked": 0, "unsat": 0}

_SAME = ["bvand", "bvor", "bvxor", "bvadd", "bvsub", "bvmul", "bvudiv", "bvurem",
         "bvshl", "bvlshr", "bvashr", "bvnand", "bvxnor"]
_CMP = ["=", "distinct", "bvult", "bvule", "bvugt", "bvslt", "bvsle", "bvsge"]


class _Gen:
    def __init__(self, seed: int):
        self.r = random.Random(seed)
        n = self.r.randint(1, 3)
        budget = self.r.randint(4, 20)
        self.vars = []
        for i in range(n):
            w = max(1, min(8, budget // (n - i) + self.r.randint(-1, 1)))
            budget -= w
            self.vars.append((f"x{i}", w))

    def const(self, w: int) -> str:
        return f"(_ bv{self.r.randrange(1 << w)} {w})"

    def leaf(self, w: int) -> str:
        same = [v for v, vw in self.vars if vw == w]
        if same and self.r.random() < 0.7:
            return self.r.choice(same)
        wider = [(v, vw) for v, vw in self.vars if vw > w]
        if wider and self.r.random() < 0.5:
            v, vw = self.r.choice(wider)
            lo = self.r.randint(0, vw - w)
            return f"((_ extract {lo + w - 1} {lo}) {v})"
        narrower = [(v, vw) for v, vw in self.vars if vw < w]
        if narrower and self.r.random() < 0.5:
            v, vw = self.r.choice(narrower)
            ext = self.r.choice(["zero_extend", "sign_extend"])
            return f"((_ {ext} {w - vw}) {v})"
        return self.const(w)

    def term(self, w: int, d: int) -> str:
        if d == 0 or self.r.random() < 0.25:
            return self.leaf(w)
        k = self.r.random()
        if k < 0.5:
            return f"({self.r.choice(_SAME)} {self.term(w, d - 1)} {self.term(w, d - 1)})"
        if k < 0.6:
            return f"({self.r.choice(['bvnot', 'bvneg'])} {self.term(w, d - 1)})"
        if k < 0.7:
            return f"(ite {self.pred(d - 1)} {self.term(w, d - 1)} {self.term(w, d - 1)})"
        if k < 0.8 and w >= 2:
            a = self.r.randint(1, w - 1)
            return f"(concat {self.term(a, d - 1)} {self.term(w - a, d - 1)})"
        if k < 0.9:
            ww = self.r.randint(w, min(w + 6, 12))
            lo = self.r.randint(0, ww - w)
            return f"((_ extract {lo + w - 1} {lo}) {self.term(ww, d - 1)})"
        return f"((_ rotate_left {self.r.randint(0, w)}) {self.term(w, d - 1)})"

    def pred(self, d: int) -> str:
        k = self.r.random()
        if d > 0 and k < 0.2:
            op = self.r.choice(["and", "or"])
            return f"({op} {self.pred(d - 1)} {self.pred(d - 1)})"
        if d > 0 and k < 0.3:
            return f"(not {self.pred(d - 1)})"
        w = self.r.choice([vw for _, vw in self.vars] + [self.r.randint(1, 8)])
        return f"({self.r.choice(_CMP)} {self.term(w, d)} {self.term(w, d)})"

    def problem(self, seeded: int = 0) -> str:
        lines = ["(set-logic QF_BV)"]
        if seeded:
            lines[:0] = ["(set-option :produce-models true)", f"(set-option :random-seed {seeded})"]
        lines += [f"(declare-const {v} (_ BitVec {w}))" for v, w in self.vars]
        lines += [f"(assert {self.pred(3)})" for _ in range(self.r.randint(1, 3))]
        lines.append("(check-sat)")
        return "\n".join(lines) + "\n"


def _dv(path: Path, extra: dict) -> tuple[str, str, int]:
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    # DV_AFFINE=0: affine analysis (test_affine.py) would decide some first.
    env.update({"DV_ENGINE": "bitblast", "DV_LOG": "1", "DV_SIM_CHECK": "1",
                "DV_AFFINE": "0"}, **extra)
    p = subprocess.run([str(_EXE), str(path)], capture_output=True,
                       text=True, timeout=60, env=env)
    out = next((l.strip() for l in p.stdout.splitlines()
                if l.strip() in ("sat", "unsat", "unknown")), "none")
    return out, p.stderr, p.returncode


@pytest.mark.parametrize("seed", range(_N))
def test_sim_agrees_with_z3(seed: int, tmp_path: Path) -> None:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    if not _Z3.is_available():
        pytest.skip("z3 not available")
    smt2 = _Gen(seed).problem(seeded=seed + 1 if seed % 3 == 0 else 0)
    f = tmp_path / f"sim_{seed}.smt2"
    f.write_text(smt2)
    want = _Z3.solve(f, timeout_s=30).result
    if want not in ("sat", "unsat"):
        pytest.skip(f"z3 gave {want}")
    extra = {"DV_SIM_MAX_WORK": "0"} if seed % 2 else {}
    got, err, rc = _dv(f, extra)
    assert "MISMATCH" not in err and rc in (0, 1), (
        f"simulation disagrees with SAT on seed {seed} (exit {rc}):\n{err}\n{smt2}")
    assert got == want, f"seed {seed}: dv-solve {got}, z3 {want}\n{err}\n{smt2}"
    m = re.search(r"bb-sim: (\w+).*?( raced)?", err)
    _stats["checked"] += 1
    _stats["unsat"] += want == "unsat"
    if m:
        _stats["raced" if " raced" in err else m.group(1)] += 1


def test_zz_sim_coverage() -> None:
    """The sweep must actually decide problems, unsat ones included, or the
    fuzzer above tests nothing."""
    if _stats["checked"] < 50:
        pytest.skip("too few seeds ran")
    print(f"\naig-sim fuzz: {_stats}")
    assert _stats["exhaustive"] >= _stats["checked"] // 3, _stats
    assert _stats["unsat"] >= _stats["checked"] // 10, _stats
