"""Testcases behind docs/fw_hdl_requests.md (from fw-hdl, 2026-10-03).

R1 (get-value on terms), R2 (malformed input), R4 (define-sort) and R5 (Bool
values) are met.

R3 (engine routing) is a timing question. The tests here only pin the
verdicts, on both engines. The timings come from
``python tests/formal/test_fw_hdl_requests.py --bench``.
"""
from __future__ import annotations

import re
import subprocess
import sys
import time
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = _REPO / "build" / "dv-solve-smt2"
_DIR = Path(__file__).resolve().parent / "smt2" / "fw_hdl"

pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="build/dv-solve-smt2 not built")


def _run(name: str, *args: str, timeout: float = 120.0) -> list[str]:
    p = subprocess.run([str(_EXE), *args, str(_DIR / name)], capture_output=True,
                       text=True, timeout=timeout)
    return [ln for ln in p.stdout.splitlines() if ln.strip()]


def _value(tok: str) -> int:
    if tok.startswith("#b"):
        return int(tok[2:], 2)
    if tok.startswith("#x"):
        return int(tok[2:], 16)
    return {"true": 1, "false": 0}[tok]


def _pair(line: str) -> tuple[str, str]:
    """`((TERM VALUE))` -> (TERM, VALUE), with TERM verbatim."""
    m = re.fullmatch(r"\(\((.*) (#[bx][0-9a-fA-F]+|true|false)\)\)", line.strip())
    assert m, f"malformed get-value response: {line!r}"
    return m.group(1), m.group(2)


# ---- R1: get-value on terms ---------------------------------------------

# (query index in get_value_terms.smt2, term, value)
_GV = [
    (0, "a", 0x5),
    (1, "k", 0x6),
    (2, "(bvadd a #x1)", 0x6),
    (3, "(f a)", 0x6),
    (4, "(f #x2)", 0x3),
    (5, "(f (f a))", 0x7),
    (6, "(bvneg a)", 0xB),
    (7, "(bvlshr a #x1)", 0x2),
    (8, "(bvxor a a a)", 0x5),
    (9, "(= a #x5)", 1),
]


@pytest.mark.parametrize("engine", ["auto", "bitblast", "cdcl"])
@pytest.mark.parametrize("idx,term,value", [
    pytest.param(i, t, v, id=f"q{i}") for i, t, v in _GV])
def test_get_value_term(engine, idx, term, value):
    args = [] if engine == "auto" else [f"--engine={engine}"]
    out = _run("get_value_terms.smt2", *args)
    assert out[0] == "sat"
    got_term, got_val = _pair(out[1 + idx])
    assert got_term == term, f"term not echoed: {out[1 + idx]!r}"
    assert _value(got_val) == value, f"wrong value: {out[1 + idx]!r}"
    if term.startswith("(="):
        assert got_val in ("true", "false"), f"Bool term printed as a bit-vector: {got_val}"


def test_crc32_counterexample_value():
    """The counterexample's output, via the design's own function."""
    out = _run("crc32_counterexample.smt2")
    assert out[0] == "sat"
    assert _value(_pair(out[1])[1]) == 0x31
    term, val = _pair(out[2])
    assert term == "(|main| a)" and _value(val) == 0x83DCEFB7, out[2]


# ---- R2: malformed input must be reported --------------------------------

@pytest.mark.parametrize("name", ["unbalanced_parens.smt2", "unbalanced_parens_assert.smt2",
                                  "define_sort_malformed.smt2"])
def test_unbalanced_parens_reported(name):
    p = subprocess.run([str(_EXE), str(_DIR / name)], capture_output=True, text=True,
                       timeout=30)
    assert "error" in (p.stdout + p.stderr), f"no diagnostic; stdout={p.stdout!r}"
    assert p.returncode != 0 and p.returncode != 1, \
        f"exit {p.returncode} reads as sat (0) or unsat (1)"


def test_unevaluable_term_is_an_error():
    """R1.1: a term we cannot evaluate gets an (error ...), never a value."""
    p = subprocess.run([str(_EXE)], capture_output=True, text=True, timeout=30, input=(
        "(set-logic QF_BV)(declare-const a (_ BitVec 4))(assert (= a #x5))(check-sat)"
        "(get-value (a (bv2nat a)))"))
    out = p.stdout.splitlines()
    assert out[0] == "sat"
    assert out[1].startswith("(error ") and "bv2nat" in out[1], out[1:]
    assert len(out) == 2, "partial response before the error"


# ---- R5: Bool values -----------------------------------------------------

def test_bool_value_printed_as_bool():
    p = subprocess.run([str(_EXE), str(_DIR.parent / "usecase" / "dma" / "dma2_sstruct.smt2")],
                       capture_output=True, text=True, timeout=60)
    assert "(stall true)" in p.stdout or "(stall false)" in p.stdout, p.stdout


# ---- R4: define-sort ------------------------------------------------------

@pytest.mark.parametrize("engine", ["auto", "bitblast", "cdcl"])
def test_define_sort(engine):
    args = [] if engine == "auto" else [f"--engine={engine}"]
    assert _run("define_sort.smt2", *args) == ["sat"]


# ---- Verdicts that must hold whatever the engine (R3 guards) -------------

@pytest.mark.parametrize("engine", ["auto", "bitblast", "cdcl"])
@pytest.mark.parametrize("name", ["crc32_affine2.smt2", "crc32_inverse.smt2"])
def test_crc32_verdicts(engine, name):
    args = [] if engine == "auto" else [f"--engine={engine}"]
    out = _run(name, *args)
    if name == "crc32_affine2.smt2":
        assert out == ["unsat"]
    else:
        assert out[0] == "sat" and _value(_pair(out[1])[1]) == 0x31


# ---- R3 benchmark (not a test) ------------------------------------------

def _bench() -> None:
    import shutil
    others = [(n, shutil.which(n)) for n in ("z3", "boolector", "bitwuzla")]
    for name in ("crc32_affine2.smt2", "crc32_affine3.smt2"):
        runs = [("dv-solve auto", [str(_EXE)]),
                ("dv-solve cdcl", [str(_EXE), "--engine=cdcl"]),
                ("dv-solve bitblast", [str(_EXE), "--engine=bitblast"])]
        runs += [(n, [p]) for n, p in others if p]
        for label, cmd in runs:
            t0 = time.monotonic()
            try:
                p = subprocess.run(cmd + [str(_DIR / name)], capture_output=True,
                                   text=True, timeout=120)
                res = p.stdout.strip().splitlines()[0] if p.stdout.strip() else "?"
            except subprocess.TimeoutExpired:
                res = "timeout"
            print(f"{name:22} {label:18} {res:8} {time.monotonic() - t0:7.2f} s")


if __name__ == "__main__" and "--bench" in sys.argv:
    _bench()
