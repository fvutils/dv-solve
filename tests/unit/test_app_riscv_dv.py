"""The riscv-dv application suite's pure parts (docs/riscv_dv_results_design.md).

The suite itself needs a built riscv-dv generator and a Verilator with
+verilator+rand+sampler+; these tests cover what decides its numbers:
outcome classification, the options-took-effect check, program and value
diversity, and the solver CPU accounting of solver_rusage under a parent that
is a child subreaper.
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import textwrap

import pytest

from tests.perf.apps.riscv_dv import dist, quality, run


def test_classify_pass_and_failures(tmp_path):
    prog = [tmp_path / "test_0.S"]
    assert run.classify("UVM_INFO ok\n", 0, False, [], prog) == ("pass", None)
    out, sig = run.classify("UVM_FATAL src/x.sv(97) @ 0: reporter [instr_stream] Cannot inject "
                            "the instruction\n", 1, False, [], prog)
    assert out == "sim_error" and "Cannot inject" in sig
    out, _ = run.classify("UVM_ERROR t.sv(3) @ 0: r [cfg] Cannot randomize cfg\n", 1, False, [], prog)
    assert out == "rand_fail"
    assert run.classify("Solver returned unknown\n", 0, False, [], prog)[0] == "unknown"
    assert run.classify("Solver died or replied unreadably\n", 0, False, [], prog)[0] == "crash"
    assert run.classify("", None, True, [], prog)[0] == "timeout"
    assert run.classify("UVM_INFO ok\n", 0, False, [], [])[0] == "sim_error"   # no program


def test_options_check_catches_ignored_plusargs():
    cell = {"plusargs": ["+instr_cnt=10000", "+num_of_sub_program=5"]}
    assert run.options_ok(cell, 18081) == (True, 9000)       # T2: options in effect
    assert run.options_ok(cell, 548) == (False, 9000)        # T1: UVM_NO_DPI dropped them
    assert run.options_ok({"plusargs": []}, 17933)[0]        # no +instr_cnt: riscv-dv default
    assert not run.options_ok({"plusargs": []}, 404)[0]


def _asm(path, lines):
    path.write_text("main:\n" + "".join(f"  {l}\n" for l in lines) + "test_done:\n  ecall\n")
    return quality.program(quality.body(path))


def test_program_statistics(tmp_path):
    p = _asm(tmp_path / "a.S", ["addi a0, a1, 5", "addi a0, a1, 5", "lw t0, 4(sp)", ".align 2",
                                "x1: add s1, s2, s3  # comment"])
    assert p["instrs"] == 4 and p["dup"] == 25.0
    assert p["imm_u"] == round(100 * 2 / 3, 2)               # 5, 5, 4
    flat = _asm(tmp_path / "b.S", ["addi a0, a0, 1"] * 8)
    pooled = quality.pooled([flat], ref_mn=quality.pooled([p])["_mn"])
    assert pooled["dup"] == 87.5 and pooled["mnem_h"] == 0.0 and pooled["js_ref"] > 0


TRANSCRIPT = """0.0 > (set-logic QF_ABV)
0.0 > (reset)
0.1 > (check-sat)
0.1 < sat
0.1 > (get-value (a b))
0.1 < ((a #b0001)
0.1 <  (b #x2))
0.2 > (check-sat-assuming (p))
0.2 < sat
0.2 > (get-value (a b))
0.2 < ((a #b0011) (b #x2))
0.3 > (reset)
0.4 > (check-sat)
0.4 < sat
0.4 > (get-value (a b))
0.4 < ((a (_ bv7 4)) (b #x2))
"""


def test_reduce_keeps_the_last_model_of_each_randomize(tmp_path):
    t = tmp_path / "tap.tr"
    t.write_text(TRANSCRIPT)
    c = dist.reduce(t)
    assert dict(c[("a b", "a")]) == {3: 1, 7: 1}             # not the first model, 1
    assert dict(c[("a b", "b")]) == {2: 2}


def test_value_summary_over_the_shared_pairs():
    from collections import Counter
    spread = {("s", "x"): Counter({v: 10 for v in range(8)})}
    narrow = {("s", "x"): Counter({0: 70, 1: 10})}
    out = dist.summary({"dv": spread, "ref": narrow}, ["dv", "ref"], "ref")
    assert out["dv"]["hhmax"] == 1.0 and out["dv"]["pairs"] == 1
    assert out["ref"]["mode"] == 87.5 and out["ref"]["js_ref"] == 0.0
    assert out["dv"]["js_ref"] > 0
    few = {("s", "x"): Counter({0: 10})}                    # under MIN_N: not compared
    assert dist.summary({"dv": spread, "ref": few}, ["dv", "ref"], "ref")["dv"]["pairs"] == 0


@pytest.fixture(scope="module")
def rusage_bin(tmp_path_factory):
    src = os.path.join(os.path.dirname(run.__file__), "solver_rusage.c")
    exe = tmp_path_factory.mktemp("rusage") / "solver_rusage"
    if subprocess.run(["cc", "-O2", "-o", str(exe), src]).returncode:
        pytest.skip("no C compiler")
    return exe


BURN = "import sys,time\nt=time.process_time()\nwhile time.process_time()-t<{s}: pass\nsys.stdin.read()\n"


def _parent(rusage_bin, out, kill_wrapper):
    """A stand-in for Verilator: spawn the solver through the wrapper, feed it
    EOF, then either reap the wrapper or SIGKILL it as Verilator may."""
    return textwrap.dedent(f"""
        import os, signal, subprocess, sys, time
        p = subprocess.Popen([{str(rusage_bin)!r}, sys.executable, "-c", {BURN.format(s=0.4)!r}],
                             stdin=subprocess.PIPE, env=dict(os.environ, DVS_RUSAGE_OUT={str(out)!r}))
        time.sleep(0.8)
        if {kill_wrapper}:
            os.kill(p.pid, signal.SIGKILL)
            p.wait()
            p.stdin.close()
        else:
            p.stdin.close()
            p.wait()
        """)


@pytest.mark.parametrize("kill_wrapper", [False, True])
def test_solver_cpu_survives_a_killed_wrapper(tmp_path, rusage_bin, kill_wrapper):
    run._become_subreaper()
    out = tmp_path / "rusage.jsonl"
    p = subprocess.Popen([sys.executable, "-c", _parent(rusage_bin, out, kill_wrapper)])
    os.wait4(p.pid, 0)
    p.returncode = 0
    orphans = run._reap_orphans(deadline_s=5)
    rows = [json.loads(l) for l in out.read_text(encoding="utf-8").splitlines()]
    spawned = [r["pid"] for r in rows if r["event"] == "spawn"]
    done = {r["pid"]: r["user_s"] + r["sys_s"] for r in rows if r["event"] == "done"}
    assert len(spawned) == 1
    cpu = done.get(spawned[0], orphans.get(spawned[0]))
    assert cpu is not None and cpu >= 0.35                   # the solver's 0.4 s, found either way
    assert (spawned[0] in orphans) == kill_wrapper
