"""--mode=verilator: constraint shapes from riscv-dv that used to leave the
fast CDCL path, and the answers they must still give.

- **Constant-true guards.** A constraint guarded by a static condition arrives
  as `(__Vbv (=> (__Vbool #b1) (__Vbool (bvand A B ..))))`. It was neither
  folded nor split into its conjuncts, and the search took ~600 conflicts on it
  as one reified conjunction (the probe timed out and the query fell to
  bitblast, ~50 ms). Constant guards now fold and the conjunction splits.
- **Clearing a dynamic array.** Before sizing, Verilator asserts
  `(= arr ((as const (Array ..)) v))`. That one equality routed the whole
  problem to the word-level array engine (~5 ms) though nothing read the array.
  The value is now the array's default: every element read is `v`.
- **`bvsmod`.** riscv_loop_instr's `(limit - init) % step == 0` (signed) was
  lowered with a divider per sign combination, four 32-bit dividers on
  bitblast. It now divides the magnitudes once. Compared with zero, as here,
  it is `bvurem |x| |t| == 0`, which stays on CDCL (~2.5 ms against ~6 ms).
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest


_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
_needs_exe = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")

_PRELUDE = (
    "(set-option :produce-models true)\n"
    "(set-logic QF_ABV)\n"
    "(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))\n"
    "(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))\n"
)
_VAL = re.compile(r"\(([\w.]+|\(select \w+ #x[0-9a-f]+\)) #b([01]+)\)")


def _run(body: str, get: str, tmp_path: Path, seeds=(1,)):
    script = ""
    for seed in seeds:
        script += f"(set-option :random-seed {seed})\n" + _PRELUDE + body
        script += f"(check-sat)\n(get-value ({get}))\n(reset)\n"
    log = tmp_path / "dv.log"
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"], input=script,
                       capture_output=True, text=True, timeout=60,
                       env={"DV_LOG": str(log), "PATH": "/usr/bin:/bin"})
    verdicts = [l for l in r.stdout.split() if l in ("sat", "unsat", "unknown")]
    vals = [(n, int(v, 2)) for n, v in _VAL.findall(r.stdout)]
    times = [l for l in (log.read_text(encoding="utf-8") if log.exists() else "").splitlines()
             if l.startswith("vlt-time")]
    return verdicts, vals, times, r.stdout


def _conflicts(line: str) -> int:
    return int(re.search(r"conflicts=(\d+)", line).group(1))


@_needs_exe
def test_constant_true_guard_conjunction_stays_on_cdcl(tmp_path):
    # imm[5:0] != 0 and imm[31:5] == 0, under a static guard: imm in [1, 31].
    a = "(__Vbv (not (= ((_ zero_extend 26) ((_ extract 5 0) imm)) #x00000000)))"
    b = "(__Vbv (= ((_ zero_extend 5) ((_ extract 31 5) imm)) #x00000000))"
    body = ("(declare-fun imm () (_ BitVec 32))\n"
            f"(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (bvand {a} {b}))))))\n")
    seeds = list(range(1, 21))
    verdicts, vals, times, _ = _run(body, "imm", tmp_path, seeds)
    assert verdicts == ["sat"] * len(seeds)
    assert all(1 <= v <= 31 for _n, v in vals), vals
    assert len({v for _n, v in vals}) >= 5
    assert all("bitblast" not in t for t in times), times
    assert max(_conflicts(t) for t in times) < 20, times


@_needs_exe
def test_constant_guards_fold_both_ways(tmp_path):
    body = ("(declare-fun x () (_ BitVec 8))\n"
            "(assert (= #b1 (__Vbv (=> (__Vbool #b0) (__Vbool (__Vbv (= x #x05)))))))\n"
            "(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvugt x #xf0)))))))\n"
            "(assert (= #b1 (bvor (__Vbv (= #x01 #x02)) (__Vbv (= x #xf3)) (__Vbv (= x #xf4)))))\n")
    verdicts, vals, _, _ = _run(body, "x", tmp_path, range(1, 11))
    assert verdicts == ["sat"] * 10
    assert {v for _n, v in vals} <= {0xf3, 0xf4}
    verdicts, _, _, _ = _run(
        "(declare-fun x () (_ BitVec 8))\n"
        "(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (= #x01 #x02)))))))\n",
        "x", tmp_path)
    assert verdicts == ["unsat"]


_ARR = "(Array (_ BitVec 32) (_ BitVec 8))"


@_needs_exe
def test_cleared_array_stays_off_array_engine(tmp_path):
    body = (f"(declare-fun a () {_ARR})\n(declare-fun n () (_ BitVec 32))\n"
            f"(assert (= a ((as const {_ARR}) #x07)))\n"
            "(assert (bvult n #x00000010))\n")
    verdicts, vals, times, out = _run(body, "n (select a #x00000003)", tmp_path)
    assert verdicts == ["sat"]
    assert ("(select a #x00000003)", 7) in vals, out
    assert "((as const" in _run(body, "a", tmp_path)[3]
    assert all("array" not in t for t in times), times


@_needs_exe
def test_cleared_array_default_constrains_reads(tmp_path):
    # Reads made before and after the default, and a symbolic read (which
    # promotes the array to the word-level engine), all see the default.
    pre = f"(declare-fun a () {_ARR})\n(declare-fun i () (_ BitVec 32))\n"
    clear = f"(assert (= a ((as const {_ARR}) #x07)))\n"
    for body, want in [
        (pre + clear + "(assert (= (select a #x00000002) #x08))\n", "unsat"),
        (pre + "(assert (bvuge (select a #x00000002) #x05))\n" + clear, "sat"),
        (pre + "(assert (= (select a #x00000002) #x08))\n" + clear, "unsat"),
        (pre + clear + "(assert (= (select a i) #x08))\n", "unsat"),
        (pre + clear + "(assert (= (select a i) #x07))\n", "sat"),
    ]:
        verdicts, _, _, out = _run(body, "i", tmp_path)
        assert verdicts == [want], (body, out)


@_needs_exe
def test_divisibility_by_signed_step(tmp_path):
    # riscv_loop_instr: (limit - init) mod step == 0 with signed operands in
    # [-10, 10], step nonzero and pointing from init toward limit.
    body = ("(declare-fun init () (_ BitVec 32))\n(declare-fun step () (_ BitVec 32))\n"
            "(declare-fun limit () (_ BitVec 32))\n"
            "(assert (bvsge init #xfffffff6))(assert (bvsle init #x0000000a))\n"
            "(assert (bvsge step #xfffffff6))(assert (bvsle step #x0000000a))\n"
            "(assert (bvsge limit #xffffffe0))(assert (bvsle limit #x00000020))\n"
            "(assert (not (= limit init)))\n"
            "(assert (ite (bvslt init limit) (bvsgt step #x00000000) (bvslt step #x00000000)))\n"
            "(assert (= (bvsmod (bvsub limit init) step) #x00000000))\n")
    seeds = list(range(1, 31))
    verdicts, vals, times, out = _run(body, "init step limit", tmp_path, seeds)
    assert verdicts == ["sat"] * len(seeds), out

    def s32(v):
        return v - (1 << 32) if v >> 31 else v
    models = [dict(vals[k:k + 3]) for k in range(0, len(vals), 3)]
    for m in models:
        i, st, li = s32(m["init"]), s32(m["step"]), s32(m["limit"])
        assert st != 0 and (li - i) % st == 0 and (li - i) * st > 0, m
    assert len({tuple(m.values()) for m in models}) >= 10
    assert all("bitblast" not in t for t in times), times
    us = sorted(int(t.split()[1]) for t in times)
    assert us[len(us) // 2] < 20000, us


def _smod(x: int, t: int, w: int) -> int:
    m = 1 << w
    if t == 0:
        return x
    xs, ts = (x - m if x >> (w - 1) else x), (t - m if t >> (w - 1) else t)
    r = abs(xs) % abs(ts) * (-1 if xs < 0 else 1)
    if r and (r < 0) != (ts < 0):
        r += ts
    return r % m


@_needs_exe
def test_smod_compared_with_zero_exhaustive(tmp_path):
    # Every 4-bit (x, t), in the three forms the lowering must recognise.
    forms = ["(= (bvsmod x t) (_ bv0 4))", "(not (= #x0 (bvsmod x t)))",
             "(= #b1 (__Vbv (= (bvsmod x t) #x0)))"]
    body, want = "", []
    for x in range(16):
        for t in range(16):
            for k, f in enumerate(forms):
                body += (_PRELUDE + "(declare-fun x () (_ BitVec 4))\n"
                         "(declare-fun t () (_ BitVec 4))\n"
                         f"(assert (= x #x{x:x}))\n(assert (= t #x{t:x}))\n"
                         f"(assert {f})\n(check-sat)\n(reset)\n")
                want.append("sat" if (_smod(x, t, 4) == 0) != (k == 1) else "unsat")
    log = tmp_path / "dv.log"
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"], input=body,
                       capture_output=True, text=True, timeout=120,
                       env={"DV_LOG": str(log), "PATH": "/usr/bin:/bin"})
    got = [l for l in r.stdout.split() if l in ("sat", "unsat", "unknown")]
    assert got == want
    assert "bitblast" not in log.read_text(encoding="utf-8")
