"""Run one riscv-dv generation (a cell) under one arm, and classify it.

Two modes:

- **timed**: the solver runs under solver_rusage, which is not in the data
  path. Solver CPU is its rusage.
- **recorded**: the solver runs under solver_tap, which also writes the
  transcript that dist.py reduces. The tap costs dv-solve about 13% of its
  wall time but a reference solver well under 1%, so a reference arm's
  recorded run is also its timed run.

CPU accounting. This process makes itself a child subreaper, so every
descendant ends up reaped either inside the simulator's tree (and counted in
the simulator's wait4 rusage) or here. Then:
  total  = simulator wait4 rusage + every orphan reaped here
  solver = the solver's own rusage (from the wrapper or tap, or, for a solver
           orphaned by a SIGKILLed wrapper, its reaped rusage)
  sim    = total - solver   (includes the wrapper's or tap's own small cost)
"""
from __future__ import annotations

import ctypes
import json
import os
import re
import signal
import subprocess
import time
from pathlib import Path

from . import quality

_PR_SET_CHILD_SUBREAPER = 36
_subreaper = False


def _become_subreaper():
    global _subreaper
    if not _subreaper:
        libc = ctypes.CDLL(None, use_errno=True)
        if libc.prctl(_PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0:
            raise RuntimeError("prctl(PR_SET_CHILD_SUBREAPER) failed")
        _subreaper = True


def _cpu(ru) -> float:
    return ru.ru_utime + ru.ru_stime


def _reap_orphans(deadline_s: float = 10.0) -> dict:
    """{pid: cpu_s} for every descendant reparented here, waiting up to deadline."""
    out, end = {}, time.monotonic() + deadline_s
    while True:
        try:
            pid, _, ru = os.wait4(-1, os.WNOHANG)
        except ChildProcessError:
            return out
        if pid:
            out[pid] = _cpu(ru)
        elif time.monotonic() > end:
            return out
        else:
            time.sleep(0.02)


def _jsonl(path: Path) -> list:
    if not path.exists():
        return []
    rows = []
    for line in path.read_text().splitlines():
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass
    return rows


_FAIL_PATS = [
    ("unknown", re.compile(r"Solver returned unknown")),
    ("solver_died", re.compile(r"Solver died or replied unreadably|Solver failed repeatedly")),
    ("no_solver", re.compile(r"Unable to communicate with SAT solver")),
]


def classify(log: str, rc, timed_out: bool, solver_rows: list, programs: list) -> tuple:
    """(outcome, signature) for one run. See the design doc §6.1."""
    flags = {name for name, p in _FAIL_PATS if p.search(log)}
    fatals = re.findall(r"UVM_FATAL (\S+) .*?\[(\S+)\] (.*)", log)
    errors = re.findall(r"UVM_ERROR (\S+) .*?\[(\S+)\] (.*)", log)
    sim_err = re.findall(r"^%Error: (.*)", log, re.M)
    rand_fail = [f for f in fatals + errors if "andomiz" in f[2]]
    crashed = any(r.get("status", 0) not in (0,) and r.get("event") == "done"
                  for r in solver_rows) and "solver_died" in flags
    first = (rand_fail or fatals + errors)[:1]
    sig = (f"{first[0][0]} [{first[0][1]}] {first[0][2][:120]}" if first
           else sim_err[0][:160] if sim_err else f"rc={rc}")
    if timed_out:
        return "timeout", f"over the cell budget"
    if crashed or "solver_died" in flags or "no_solver" in flags:
        return "crash", sig
    if "unknown" in flags:
        return "unknown", sig
    if rand_fail:
        return "rand_fail", sig
    if rc == 0 and programs and not fatals and not errors and not sim_err:
        return "pass", None
    return "sim_error", sig


def options_ok(cell: dict, instrs: int) -> tuple:
    """Did the testlist options take effect? (design §5.4)

    With UVM's DPI layer missing, riscv-dv silently ignores its options and
    generates ~400-600 instructions whatever +instr_cnt says. Healthy programs
    have at least +instr_cnt instructions (1.0-4.4x on T2), or about 17k when
    a test sets none."""
    want = [int(p.split("=", 1)[1]) for p in cell["plusargs"] if p.startswith("+instr_cnt=")]
    floor = max(1000, int(0.9 * want[0])) if want else 1000
    return instrs >= floor, floor


def run_cell(gen: Path, cell: dict, solver_cmd: list, sampler: str, mode: str,
             tools: dict, workdir: Path, timeout_s: int) -> dict:
    """Run one generation; return the row (without suite/arm bookkeeping)."""
    _become_subreaper()
    workdir.mkdir(parents=True, exist_ok=True)
    for f in workdir.glob("*"):
        if f.is_file():
            f.unlink()
    asm = workdir / "asm"
    asm.mkdir(exist_ok=True)
    for f in asm.glob("*"):
        f.unlink()
    env = dict(os.environ)
    for k in ("DVS_RUSAGE_OUT", "DVS_TAP_JSON", "DVS_TAP_LOG"):
        env.pop(k, None)
    if mode == "timed":
        acct = workdir / "rusage.jsonl"
        env["DVS_RUSAGE_OUT"] = str(acct)
        wrapper = [str(tools["solver_rusage"])]
    else:
        acct = workdir / "tap.json"
        env["DVS_TAP_JSON"] = str(acct)
        env["DVS_TAP_LOG"] = str(workdir / "tap.tr")
        env["DVS_TAP_QTIMEOUT"] = "120"
        wrapper = [str(tools["solver_tap"])]
    env["VERILATOR_SOLVER"] = " ".join(wrapper + list(map(str, solver_cmd)))
    argv = [str(gen), f"+verilator+seed+{cell['seed']}", f"+UVM_TESTNAME={cell['gen_test']}",
            "+num_of_tests=1", "+start_idx=0", f"+asm_file_name={asm}/test"]
    if sampler == "solver":
        argv.append("+verilator+rand+sampler+solver")
    argv += cell["plusargs"]

    log_path = workdir / "sim.log"
    from ... import calib
    load = calib.loadavg()
    t0 = time.monotonic()
    with open(log_path, "wb") as logf:
        p = subprocess.Popen(argv, stdout=logf, stderr=subprocess.STDOUT, env=env,
                             start_new_session=True)
        timed_out = False
        while True:
            pid, status, ru = os.wait4(p.pid, os.WNOHANG)
            if pid:
                break
            if time.monotonic() - t0 > timeout_s:
                timed_out = True
                os.killpg(p.pid, signal.SIGKILL)
                _, status, ru = os.wait4(p.pid, 0)
                break
            time.sleep(0.01)
    wall = time.monotonic() - t0
    p.returncode = os.waitstatus_to_exitcode(status)
    orphans = _reap_orphans()

    rows = _jsonl(acct)
    if mode == "timed":
        done = {r["pid"]: r["user_s"] + r["sys_s"] for r in rows if r.get("event") == "done"}
        spawned = [r["pid"] for r in rows if r.get("event") == "spawn"]
        solver = sum(done.get(pid, orphans.get(pid, 0.0)) for pid in spawned)
        lost = [pid for pid in spawned if pid not in done and pid not in orphans]
        spawns = len(spawned)
        counts = None
    else:
        solver = sum(r.get("user_s", 0) + r.get("sys_s", 0) for r in rows)
        lost = []
        spawns = len(rows)
        counts = {k: sum(r.get(k, 0) for r in rows)
                  for k in ("check_sat", "check_sat_assuming", "get_value", "sat", "unsat",
                            "unknown", "error")}
    total = _cpu(ru) + sum(orphans.values())

    log = log_path.read_text(errors="replace")
    progs = sorted(asm.glob("*.S"))
    outcome, sig = classify(log, p.returncode, timed_out, rows, progs)
    if counts and counts["error"] and outcome == "pass":
        outcome, sig = "sim_error", "solver reported (error ...)"
    row = {"outcome": outcome, "signature": sig, "rc": p.returncode,
           "wall_s": round(wall, 3), "solver_cpu_s": round(solver, 3),
           "sim_cpu_s": round(max(0.0, total - solver), 3), "spawns": spawns,
           "load": load}
    if lost:
        row["solver_cpu_lost"] = len(lost)
    if counts:
        row["calls"] = counts
    if progs:
        prog = quality.program(quality.body(progs[0]))
        from .build import sha256_file
        row["prog_sha"] = sha256_file(progs[0])[:16]
        row["_prog"] = prog
        row["options_ok"], row["options_floor"] = options_ok(cell, prog["instrs"])
    else:
        row["options_ok"] = None
    row["_transcript"] = workdir / "tap.tr" if mode == "recorded" else None
    return row
