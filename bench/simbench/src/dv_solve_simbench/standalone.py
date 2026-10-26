"""Standalone dv-solve (config with `replay`): time the bench's constraints without Verilator.

The Verilator run captures every command it sends the solver
(+verilator+solver+file+). bench_vlt_replay then replays that session through
dv-solve in-process (builder API; `api-cache` reuses compiled contexts), so the
time is dv-solve's alone on exactly the constraints, seeds and call count the
simulator used.
"""
import dataclasses as dc
import os
import re
import subprocess
from typing import Optional

from .config import Config
from .result import Result
from .runner import RunOutput

_PASS = r"^%s\s+wall\s+[\d.]+ s\s+([\d.]+) us/check-sat.*?(\d+) bitblast failures, (\d+) unsolved"


def session_plusarg(path: str) -> str:
    return "+verilator+solver+file+" + path


def extract_cmds(log_path: str, cmds_path: str) -> int:
    """Solver-log commands ('  (...)' lines) to one-command-per-line; return check-sat count."""
    n = 0
    with open(log_path) as fi, open(cmds_path, "w") as fo:
        for ln in fi:
            if ln.startswith("  ("):
                fo.write(ln[2:])
                n += ln.startswith("  (check-sat")
    return n


def parse_replay(text: str, pass_name: str) -> Optional[dict]:
    """us/check-sat, failures and first-call (new context) time of one pass."""
    m = re.search(_PASS % re.escape(pass_name), text, re.M)
    if m is None:
        return None
    ret = {"us_per_check": float(m.group(1)),
           "failures": int(m.group(2)) + int(m.group(3)), "us_new": None}
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
    cmd = [cfg.replay, cmds, "--reps", str(reps), "--passes", cfg.replay_pass]
    p = subprocess.run(cmd, cwd=rundir, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=timeout)
    with open(os.path.join(rundir, "replay.log"), "w") as fp:
        fp.write(p.stdout)
    # Several passes may be timed (comma list); report the fastest.
    r = None
    if p.returncode == 0:
        rs = [parse_replay(p.stdout, x.strip()) for x in cfg.replay_pass.split(",")]
        if rs and all(rs):
            r = min(rs, key=lambda x: x["us_per_check"])
            r["pass"] = cfg.replay_pass.split(",")[rs.index(r)].strip()
    if r is None:
        raise RuntimeError("replay failed (rc=%d): %s\n%s"
                           % (p.returncode, " ".join(cmd), p.stdout[-2000:]))
    # The session holds the runtime's start-up check, then n + warmup calls of
    # one or more check-sats each (dist: 2, under push); time per call is the
    # per-check rate times check-sats per call.
    calls = sim.result.n + sim.result.warmup
    per_call = (n_check - 1) / calls if calls and n_check > 1 else 1.0
    res = dc.replace(sim.result,
                     elapsed_ns=int(round(r["us_per_check"] * 1e3 * per_call * sim.result.n)),
                     first_call_ns=int(round((r["us_new"] or 0) * 1e3)),
                     failures=sim.result.failures + r["failures"])
    log = "%s\n# replay (%d check-sats, %.2f per call, fastest pass %s): %s\n%s" % (
        sim.log, n_check, per_call, r["pass"], " ".join(cmd), p.stdout)
    return RunOutput(cmd=cmd, returncode=p.returncode, log=log, result=res)
