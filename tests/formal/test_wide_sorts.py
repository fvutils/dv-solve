"""Bit-vector sorts wider than 192 bits (docs/fw_hdl_implementation_plan.md, piece 6.6).

Yosys `write_smt2 -stbv` keeps a module's whole state in one bit-vector sort
(`define-sort |m_s| () (_ BitVec N)`) and reads it through `extract`; N is the
sum of the register and input widths, often several hundred bits. The front
end used to reject sorts wider than 192 bits; it now takes them up to 65535.

The generator draws problems of that shape: a few state vectors of 193 to 1500
bits, a `define-fun` per field reading them, the fields combined by bitwise,
arithmetic, shift and compare operators, and some whole-state operators and
wide constants too. Every verdict is checked against z3, and every `sat` model
is read back with get-value and checked by z3.

Scale with WIDE_N (default 300); WIDE_FIRST moves the window.
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
_FW = Path(__file__).resolve().parent / "smt2" / "fw_hdl" / "gen"
_N = int(os.environ.get("WIDE_N", "300"))
_FIRST = int(os.environ.get("WIDE_FIRST", "0"))
_Z3 = Z3Solver()
_stats = {"checked": 0, "sat": 0, "unsat": 0, "over255": 0}

_BINOPS = ["bvxor", "bvand", "bvor", "bvadd", "bvsub"]


class _Gen:
    def __init__(self, seed: int):
        self.r = random.Random(seed)
        r = self.r
        self.sw = r.choice([193, 225, 256, 257, 300, 386, 511, 770, 1024, 1500])
        self.states = [f"s{i}" for i in range(r.randint(1, 3))]
        self.funs: list[tuple[str, int, int]] = []   # (name, hi, lo)
        for i in range(r.randint(2, 6)):
            w = r.choice([1, 1, 4, 8, 16, 19, 32, 33, 64, 65, 100, 200])
            w = min(w, self.sw)
            lo = r.randint(0, self.sw - w)
            self.funs.append((f"|m#{i}|", lo + w - 1, lo))

    def const(self, w: int) -> str:
        v = self.r.getrandbits(w)
        if w % 4 == 0 and self.r.random() < 0.4:
            return f"#x{v:0{w // 4}x}"
        return f"(_ bv{v} {w})"

    def field(self) -> tuple[str, int]:
        name, hi, lo = self.r.choice(self.funs)
        return f"({name} {self.r.choice(self.states)})", hi - lo + 1

    def fit(self, t: str, w: int, to: int) -> str:
        """`t` (w bits) truncated or extended to `to` bits."""
        if w == to:
            return t
        if w > to:
            lo = self.r.randint(0, w - to)
            return f"((_ extract {lo + to - 1} {lo}) {t})"
        op = self.r.choice(["zero_extend", "sign_extend"])
        return f"((_ {op} {to - w}) {t})"

    def term(self, w: int, d: int) -> str:
        r = self.r
        if d == 0 or r.random() < 0.2:
            if r.random() < 0.15:
                return self.const(w)
            t, tw = self.field()
            return self.fit(t, tw, w)
        k = r.random()
        if k < 0.45:
            return f"({r.choice(_BINOPS)} {self.term(w, d - 1)} {self.term(w, d - 1)})"
        if k < 0.52 and w <= 32:
            return f"(bvmul {self.term(w, d - 1)} {self.term(w, d - 1)})"
        if k < 0.60:
            return f"(bvnot {self.term(w, d - 1)})"
        if k < 0.68:
            op = r.choice(["bvshl", "bvlshr", "bvashr"])
            return f"({op} {self.term(w, d - 1)} (_ bv{r.randrange(w + 2)} {w}))"
        if k < 0.74 and w >= 2:
            c = r.randint(1, w - 1)
            return f"(concat {self.term(c, d - 1)} {self.term(w - c, d - 1)})"
        if k < 0.80:
            op = r.choice(["rotate_left", "rotate_right"])
            return f"((_ {op} {r.randrange(w)}) {self.term(w, d - 1)})"
        if k < 0.90:
            return f"(ite {self.pred(d - 1)} {self.term(w, d - 1)} {self.term(w, d - 1)})"
        # A whole state vector, cut down or widened to w.
        return self.fit(r.choice(self.states), self.sw, w)

    def pred(self, d: int) -> str:
        r = self.r
        k = r.random()
        if d > 0 and k < 0.15:
            return f"({r.choice(['and', 'or'])} {self.pred(d - 1)} {self.pred(d - 1)})"
        if d > 0 and k < 0.2:
            return f"(not {self.pred(d - 1)})"
        if k < 0.3:
            # Whole-state equality or ordering, as in a transition relation.
            a, b = r.sample(self.states * 2, 2)
            op = r.choice(["=", "distinct", "bvult", "bvsle"])
            rhs = self.const(self.sw) if r.random() < 0.3 else self.term(self.sw, 1)
            return f"({op} {a} {rhs})"
        w = r.choice([1, 8, 19, 32, 64, 65, 128, 300, self.sw])
        op = r.choice(["=", "=", "distinct", "bvult", "bvule", "bvslt", "bvsge"])
        return f"({op} {self.term(w, 2)} {self.term(w, 2)})"

    def problem(self) -> str:
        lines = ["(set-logic QF_BV)",
                 f"(define-sort |m_s| () (_ BitVec {self.sw}))"]
        for name, hi, lo in self.funs:
            lines.append(f"(define-fun {name} ((state |m_s|)) (_ BitVec {hi - lo + 1}) "
                         f"((_ extract {hi} {lo}) state))")
        lines += [f"(declare-fun {s} () |m_s|)" for s in self.states]
        lines += [f"(assert {self.pred(2)})" for _ in range(self.r.randint(1, 4))]
        lines.append("(check-sat)")
        return "\n".join(lines) + "\n"


def _dv(text: str, path: Path) -> tuple[str, list[str], str]:
    path.write_text(text)
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    env["DV_SIM_CHECK"] = "1"
    p = subprocess.run([str(_EXE), str(path)], capture_output=True, text=True,
                       timeout=120, env=env)
    out = [l.strip() for l in p.stdout.splitlines() if l.strip()]
    return (out[0] if out else "none"), out[1:], p.stderr


@pytest.mark.parametrize("seed", range(_FIRST, _FIRST + _N))
def test_wide_sorts_agree_with_z3(seed: int, tmp_path: Path) -> None:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    if not _Z3.is_available():
        pytest.skip("z3 not available")
    g = _Gen(seed)
    smt2 = g.problem()
    f = tmp_path / f"wide_{seed}.smt2"
    f.write_text(smt2)
    want = _Z3.solve(f, timeout_s=60).result
    if want not in ("sat", "unsat"):
        pytest.skip(f"z3 gave {want}")
    query = smt2 + f"(get-value ({' '.join(g.states)}))\n"
    got, rest, err = _dv(query, tmp_path / f"dv_{seed}.smt2")
    assert "MISMATCH" not in err, f"seed {seed}: {err}\n{smt2}"
    assert got == want, f"seed {seed}: dv-solve {got}, z3 {want}\n{err}\n{smt2}"
    _stats["checked"] += 1
    _stats[want] += 1
    _stats["over255"] += g.sw > 255
    if want != "sat":
        return
    # The model, read back at full width, must satisfy the problem.
    vals = re.findall(r"\((s\d+) (#[bx][0-9a-fA-F]+|\(_ bv\d+ \d+\))\)", " ".join(rest))
    assert sorted(v for v, _ in vals) == sorted(g.states), f"seed {seed}: {rest}"
    pins = "".join(f"(assert (= {v} {x}))\n" for v, x in vals)
    check = tmp_path / f"model_{seed}.smt2"
    check.write_text(smt2.replace("(check-sat)\n", pins + "(check-sat)\n"))
    assert _Z3.solve(check, timeout_s=60).result == "sat", \
        f"seed {seed}: model fails\n{pins}\n{smt2}"


def test_zz_wide_sorts_coverage() -> None:
    """Both verdicts, and sorts past the old uint8_t width, must be exercised."""
    if _stats["checked"] < 50:
        pytest.skip("too few seeds ran")
    print(f"\nwide sorts: {_stats}")
    n = _stats["checked"]
    assert _stats["sat"] >= n // 5 and _stats["unsat"] >= n // 10, _stats
    assert _stats["over255"] >= n // 3, _stats


# ---- fixtures from the fwhdl-core suite ------------------------------------

@pytest.mark.parametrize("name,want", [
    ("affine-proof/parity_w256", "unsat"),
    ("codegen-equiv/addtree8x16_l2_ok", "unsat"),
    ("codegen-equiv/addtree8x16_l3_late", "sat"),
    ("codegen-equiv/rotl32_l5_ok", "unsat"),
    ("codegen-equiv/rotl32_l5_early", "sat"),
])
def test_wide_fixture(name: str, want: str) -> None:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    p = subprocess.run([str(_EXE), str(_FW / f"{name}.smt2")], capture_output=True,
                       text=True, timeout=120, env=env)
    assert p.stdout.split()[:1] == [want], p.stdout + p.stderr


@pytest.mark.parametrize("text,msg", [
    ("(declare-const x (_ BitVec 300))(assert (= ((_ extract 300 0) x) x))", "extract"),
    ("(declare-const x (_ BitVec 300))(assert (= ((_ extract 3 4) x) #b0))", "extract"),
])
def test_bad_extract_is_an_error(text: str, msg: str) -> None:
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    p = subprocess.run([str(_EXE)], input="(set-logic QF_BV)" + text + "(check-sat)",
                       capture_output=True, text=True, timeout=30, env=env)
    assert "error" in p.stdout + p.stderr and msg in p.stdout + p.stderr, p.stdout + p.stderr
    assert p.stdout.split()[:1] != ["sat"] and p.stdout.split()[:1] != ["unsat"], p.stdout


def test_width_limit() -> None:
    """65535 bits is the widest sort; one more is an honest unknown."""
    if not _EXE.is_file():
        pytest.skip("dv-solve-smt2 not built")
    env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
    ok = ("(set-logic QF_BV)(declare-const x (_ BitVec 65535))"
          "(assert (= ((_ extract 65534 65534) x) #b1))(check-sat)")
    p = subprocess.run([str(_EXE)], input=ok, capture_output=True, text=True,
                       timeout=60, env=env)
    assert p.stdout.split()[:1] == ["sat"], p.stdout + p.stderr
    bad = ok.replace("65535))", "65536))").replace("65534 65534", "0 0")
    p = subprocess.run([str(_EXE)], input=bad, capture_output=True, text=True,
                       timeout=60, env=env)
    assert p.stdout.split()[:1] == ["unknown"], p.stdout + p.stderr
