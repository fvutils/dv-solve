"""--mode=verilator: asserts made after the CDCL compile, and per-shape routing.

riscv-dv's riscv_illegal_instr (`+illegal_instr_ratio`) randomizes under a
`(push 1)`: the class constraints are compiled, then Verilator asserts the
weighted choice of `exception` and solves. The fixture is that problem,
minimized from a recorded session.

- **Pool headroom.** The CDCL context pool was sized for the compiled problem
  alone, and the pushed assert overflowed it. The add then left constraints
  uncompiled (CDCL models failed validation and every solve fell back to
  bitblast), and could read the failed allocation as a conflict: a wrong
  `unsat` on a satisfiable query. The pool now keeps headroom, and an add that
  overflows anyway is answered on bitblast.
- **Routing key.** After the compile plus a pushed assert the instance could
  not be fingerprinted, so it reused the CDCL route decided for Verilator's
  start-up handshake: no bounded probe, up to 10 s per solve.
- **Engine per shape.** Each riscv_illegal_instr call inlines different object
  state, so it is a new instance with a new probe, and CDCL needs 10-50 ms
  where bitblast needs ~1 ms. Once CDCL averages over 3 ms on the same
  declared variables, the next two instances try bitblast, and the shape stays
  there if that was at least twice as fast.
"""
from __future__ import annotations

import re
import subprocess
from pathlib import Path

import pytest


_EXE = Path(__file__).resolve().parents[2] / "build" / "dv-solve-smt2"
_needs_exe = pytest.mark.skipif(not _EXE.is_file(), reason="dv-solve-smt2 not built")
_FIXTURE = Path(__file__).parent / "data" / "vlt_illegal_instr_push.smt2"
_HANDSHAKE = "(set-logic QF_ABV)\n(check-sat)\n(reset)\n"
_VAL = re.compile(r"\((\w+) #b([01]+)\)")


def _run(script: str, tmp_path: Path, env_extra=None):
    log = tmp_path / "dv.log"
    log.unlink(missing_ok=True)
    env = {"DV_LOG": str(log), "PATH": "/usr/bin:/bin", **(env_extra or {})}
    r = subprocess.run([str(_EXE), "--interactive", "--mode=verilator"], input=script,
                       capture_output=True, text=True, timeout=120, env=env)
    verdicts = [l for l in r.stdout.split() if l in ("sat", "unsat", "unknown")]
    vals = [(n, int(v, 2)) for n, v in _VAL.findall(r.stdout)]
    text = log.read_text() if log.exists() else ""
    routes = [l.split()[2] for l in text.splitlines() if l.startswith("vlt-time")]
    return verdicts, vals, routes, text


def _illegal_instr(seed: int, push: bool = True, variant: int = 0) -> str:
    body = _FIXTURE.read_text()
    if not push:
        body = body.replace("(push 1)\n", "")
    if variant:
        # A distinct instance of the same shape, as each riscv-dv call is (it
        # inlines different object state); the excluded func7 is irrelevant.
        body = body.replace("(push 1)\n", "") + f"(assert (not (= func7 (_ bv{variant} 7))))\n"
    return (f"(set-option :random-seed {seed})\n" + body
            + "(check-sat)\n(get-value (exception))\n(reset)\n")


@_needs_exe
@pytest.mark.parametrize("seed", [1, 2, 3, 4, 5, 6])
def test_pushed_assert_is_enforced(tmp_path, seed):
    # The pushed ite chain picks exception = 4 (the first `7 <= k` that holds).
    verdicts, vals, routes, log = _run(_HANDSHAKE + _illegal_instr(seed), tmp_path)
    assert verdicts == ["sat", "sat"], log[-2000:]
    assert vals == [("exception", 4)]
    assert "model-validation" not in log, log[-2000:]
    assert "pool-overflow" not in log


@_needs_exe
def test_pushed_instance_is_probed_not_sticky(tmp_path):
    body = ("(set-option :produce-models true)\n(set-option :random-seed 5)\n"
            "(set-logic ALL)\n(declare-fun e () (_ BitVec 3))\n"
            "(assert (not (= e #b101)))\n(push 1)\n(assert (bvuge e #b110))\n"
            "(check-sat)\n(get-value (e))\n")
    verdicts, vals, routes, _ = _run(_HANDSHAKE + body, tmp_path)
    assert verdicts == ["sat", "sat"]
    assert vals in ([("e", 6)], [("e", 7)])
    assert routes[1] == "cdcl/probe", routes


@_needs_exe
def test_expensive_shape_moves_to_bitblast(tmp_path):
    # CDCL takes 15-50 ms on each instance, bitblast 1-2 ms: two CDCL solves,
    # two bitblast trials, then the shape stays on bitblast.
    seeds = [2, 4, 2, 4, 2]
    script = _HANDSHAKE + "".join(_illegal_instr(s, push=False, variant=k + 1)
                                  for k, s in enumerate(seeds))
    verdicts, vals, routes, log = _run(script, tmp_path)
    assert verdicts == ["sat"] * (1 + len(seeds))
    assert vals == [("exception", 4)] * len(seeds)
    assert routes[1].startswith("cdcl/") and routes[2].startswith("cdcl/"), routes
    assert routes[3:] == ["bitblast/cdcl-slow"] * 3, routes


@_needs_exe
def test_cheap_shape_stays_on_cdcl(tmp_path):
    body = ("(set-option :produce-models true)\n(set-logic QF_BV)\n"
            "(declare-fun x () (_ BitVec 16))\n(declare-fun y () (_ BitVec 16))\n"
            "(assert (bvult x #x1000))\n(assert (bvugt y x))\n")
    script = _HANDSHAKE + "".join(f"(set-option :random-seed {s})\n" + body
                                  + "(check-sat)\n(reset)\n" for s in range(1, 9))
    verdicts, _, routes, _ = _run(script, tmp_path)
    assert verdicts == ["sat"] * 9
    assert all(r.startswith("cdcl/") for r in routes[1:]), routes
