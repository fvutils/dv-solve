"""Standalone dv-solve (config with `replay`): time the bench's constraints without Verilator.

The Verilator run captures every command it sends the solver
(+verilator+solver+file+). bench_vlt_replay then replays that session through
dv-solve in-process (builder API; `api-cache` reuses compiled contexts), so the
time is dv-solve's alone on exactly the constraints, seeds and call count the
simulator used. The replay runs the front end in the capture's mode (from the
config's solver command) and times one named pass per randomize() call,
excluding the warm-up calls, as the simulator loop does.

A config with `replay_cmd` instead runs `<replay_cmd> session.cmds.smt2` (e.g.
z3 -smt2, dv-solve-smt2): one process reads the session text, and the wall time
(process start and text parse included) over all calls is reported.
"""
import dataclasses as dc
import os
import re
import shlex
import subprocess
import time
from typing import Optional

from . import provenance
from .config import Config
from .result import Result
from .runner import RunOutput

_PASS = r"^%s\s+wall\s+[\d.]+ s\s+([\d.]+) us/check-sat.*?(\d+) bitblast failures, (\d+) unsolved"
_PER_CALL = r"^%s\s+per call\s+([\d.]+) us\s+\((\d+) timed calls\)"


def solver_mode(solver: Optional[str]) -> str:
    """The dv-solve-smt2 mode the capture ran in: --mode=verilator or the default."""
    for a in shlex.split(solver or ""):
        if a.startswith("--mode="):
            return "verilator" if a[7:] == "verilator" else "default"
    return "default"


class ReplayError(RuntimeError):
    """A replay that could not be scored; `info` still goes in the run record."""
    def __init__(self, info: dict, msg: str):
        super().__init__(msg)
        self.info = info


def sim_bench(sim: RunOutput) -> str:
    return sim.result.bench if sim.result is not None else "unknown"


def session_plusarg(path: str) -> str:
    return "+verilator+solver+file+" + path


def extract_cmds(log_path: str, cmds_path: str) -> int:
    """Solver-log commands ('  (...)' lines) to one-command-per-line; return check-sat count.
    The log wraps long commands: continuation lines also start with '  ' and are joined
    (with no separator) until the command's parens balance."""
    n = 0
    cmd, depth = "", 0
    with open(log_path) as fi, open(cmds_path, "w") as fo:
        for ln in fi:
            ln = ln.rstrip("\n")
            if cmd and ln.startswith("  "):
                part = ln[2:]
            elif ln.startswith("  ("):
                part = ln[2:]
                n += ln.startswith("  (check-sat")
            else:
                continue
            cmd += part
            depth += part.count("(") - part.count(")")
            if depth <= 0:
                fo.write(cmd + "\n")
                cmd, depth = "", 0
        if cmd:
            fo.write(cmd + "\n")
    return n


def parse_replay(text: str, pass_name: str) -> Optional[dict]:
    """us/check-sat, failures and first-call (new context) time of one pass."""
    m = re.search(_PASS % re.escape(pass_name), text, re.M)
    if m is None:
        return None
    ret = {"us_per_check": float(m.group(1)),
           "failures": int(m.group(2)) + int(m.group(3)), "us_new": None,
           "us_per_call": None, "timed_calls": None}
    c = re.search(_PER_CALL % re.escape(pass_name), text, re.M)
    if c:
        ret["us_per_call"] = float(c.group(1))
        ret["timed_calls"] = int(c.group(2))
    n = re.search(r"on a new one: ([\d.]+) us", text[m.end():])
    if n:
        ret["us_new"] = float(n.group(1))
    return ret


def replay(cfg: Config, sim: RunOutput, session: str, rundir: str,
           reps: int = 3, timeout: float = 600) -> RunOutput:
    """Replay `session` (captured by `sim`) and return a RunOutput whose result is
    dv-solve's time per call. Counts and check errors come from the capture run."""
    if sim.result is None or not os.path.exists(session):
        return sim
    cmds = os.path.join(rundir, "session.cmds.smt2")
    n_check = extract_cmds(session, cmds)
    pass_name = cfg.replay_pass.strip()
    if "," in pass_name:
        raise RuntimeError("replay_pass names one pass, not a list: %r" % cfg.replay_pass)
    mode = solver_mode(cfg.solver)
    cmd = [cfg.replay, cmds, "--reps", str(reps), "--passes", pass_name,
           "--calls", str(sim.result.n + sim.result.warmup),
           "--warmup", str(sim.result.warmup), "--mode", mode]
    p = subprocess.run(cmd, cwd=rundir, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=timeout)
    with open(os.path.join(rundir, "replay.log"), "w") as fp:
        fp.write(p.stdout)
    info = {"session": provenance.session_info(cmds, n_check, cfg.id, sim_bench(sim)),
            "replay": dict(provenance.replay_details(p.stdout, pass_name),
                           args=cmd[2:], returncode=p.returncode)}
    r = parse_replay(p.stdout, pass_name) if p.returncode == 0 else None
    if r is None or r["us_per_call"] is None:
        raise ReplayError(info, "replay failed (rc=%d): %s\n%s"
                           % (p.returncode, " ".join(cmd), p.stdout[-2000:]))
    # The replay times the randomize() calls after the start-up check and the
    # warm-up; when sessions per call vary (randc retries) it cannot separate
    # the warm-up, and averages over all calls.
    if r["timed_calls"] not in (sim.result.n, sim.result.n + sim.result.warmup):
        raise ReplayError(info, "replay timed %d calls, the run made %d (+%d warm-up): %s"
                           % (r["timed_calls"], sim.result.n, sim.result.warmup, " ".join(cmd)))
    res = dc.replace(sim.result,
                     elapsed_ns=int(round(r["us_per_call"] * 1e3 * sim.result.n)),
                     first_call_ns=int(round((r["us_new"] or 0) * 1e3)),
                     failures=sim.result.failures + r["failures"])
    log = "%s\n# replay (%d check-sats, mode %s, pass %s, %.1f us/call): %s\n%s" % (
        sim.log, n_check, mode, pass_name, r["us_per_call"], " ".join(cmd), p.stdout)
    return RunOutput(cmd=cmd, returncode=p.returncode, log=log, result=res, info=info)


def count_answers(text: str) -> dict:
    """Counts of sat/unsat/unknown answer lines and (error ...) lines."""
    ret = {"sat": 0, "unsat": 0, "unknown": 0, "error": 0}
    for ln in text.splitlines():
        ln = ln.strip()
        if ln in ret:
            ret[ln] += 1
        elif ln.startswith("(error"):
            ret["error"] += 1
    return ret


def replay_text(cfg: Config, sim: RunOutput, session: str, rundir: str,
                timeout: float = 600) -> RunOutput:
    """Run `cfg.replay_cmd` on the captured session text `cfg.replay_reps` times;
    the result's time per call is the best wall time / (n + warmup)."""
    if sim.result is None or not os.path.exists(session):
        return sim
    cmds = os.path.join(rundir, "session.cmds.smt2")
    n_check = extract_cmds(session, cmds)
    cmd = shlex.split(cfg.replay_cmd) + [cmds]
    best, p = None, None
    for _ in range(max(1, cfg.replay_reps)):
        t0 = time.perf_counter()
        p = subprocess.run(cmd, cwd=rundir, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, text=True, timeout=timeout)
        wall = time.perf_counter() - t0
        best = wall if best is None else min(best, wall)
    with open(os.path.join(rundir, "replay.log"), "w") as fp:
        fp.write(p.stdout)
    # dv-solve-smt2 may exit non-zero on a (reset)-terminated file with every
    # answer given, so the answers decide, not the exit status.
    a = count_answers(p.stdout)
    info = {"session": provenance.session_info(cmds, n_check, cfg.id, sim_bench(sim)),
            "replay": {"args": cmd, "returncode": p.returncode, "reps": max(1, cfg.replay_reps),
                       "best_wall_s": best, "answers": a}}
    if a["sat"] + a["unsat"] != n_check or a["unknown"] or a["error"]:
        raise ReplayError(info, "replay failed (rc=%d, %d check-sats, answers %s): %s\n%s"
                           % (p.returncode, n_check, a, " ".join(cmd), p.stdout[-2000:]))
    # The session holds the runtime's start-up check, then n + warmup calls;
    # the start-up check and process start are charged to the calls.
    calls = sim.result.n + sim.result.warmup
    ns_call = best * 1e9 / calls if calls else best * 1e9
    res = dc.replace(sim.result, elapsed_ns=int(round(ns_call * sim.result.n)),
                     first_call_ns=0)
    log = "%s\n# replay_text (%d check-sats, best of %d: %.3f s, rc=%d): %s\n%s" % (
        sim.log, n_check, max(1, cfg.replay_reps), best, p.returncode, " ".join(cmd),
        p.stdout[-2000:])
    return RunOutput(cmd=cmd, returncode=p.returncode, log=log, result=res, info=info)
