"""Machine fingerprint, host load, and the calibration kernel (design §5.1).

The kernel is a fixed workload on the pinned z3: one real fixture and one
generated pigeonhole problem, about 2 s of CPU each. Its times say whether the
machine is the one earlier runs were measured on; the trend quantities are
ratios measured within one job, so the kernel detects drift rather than
correcting for it.
"""
from __future__ import annotations

import hashlib
import os
import platform
import subprocess
import tempfile
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]


def _php(n: int) -> str:
    """Pigeonhole: n pigeons, n-1 holes. Unsatisfiable, pure Boolean."""
    m = n - 1
    lines = ["(set-logic QF_UF)"]
    lines += [f"(declare-const p{i}_{j} Bool)" for i in range(n) for j in range(m)]
    lines += ["(assert (or " + " ".join(f"p{i}_{j}" for j in range(m)) + "))" for i in range(n)]
    for j in range(m):
        for i in range(n):
            for k in range(i + 1, n):
                lines.append(f"(assert (not (and p{i}_{j} p{k}_{j})))")
    lines.append("(check-sat)")
    return "\n".join(lines) + "\n"


# name -> (smt2 text, expected answer)
def kernels() -> dict:
    ops = (_REPO / "tests/formal/smt2/verilator/t_constraint_operators.smt2").read_text()
    return {"z3-ops": (ops, "sat"), "z3-php10": (_php(10), "unsat")}


def cpu_run(argv: list, stdin_text: str) -> tuple:
    """Run argv with stdin_text on stdin; return (child CPU s, first stdout line).

    CPU time is user + sys from wait4, so it is this child's alone and
    excludes the time it spent waiting for a core (design §2.3). Input goes
    through a temporary file rather than a pipe, so nothing can deadlock and
    the reap stays ours.
    """
    with tempfile.TemporaryFile() as fin, tempfile.TemporaryFile() as fout:
        fin.write(stdin_text.encode())
        fin.seek(0)
        p = subprocess.Popen(argv, stdin=fin, stdout=fout, stderr=subprocess.DEVNULL)
        _, _, ru = os.wait4(p.pid, 0)
        p.returncode = 0               # reaped here; stop Popen from waiting again
        fout.seek(0)
        lines = fout.read().decode(errors="replace").strip().splitlines()
    return ru.ru_utime + ru.ru_stime, (lines[0] if lines else "")


def calibrate(reps: int = 5) -> dict:
    """Minimum CPU ms per kernel over reps. Raises if an answer is wrong."""
    out = {}
    for name, (text, expect) in kernels().items():
        best = None
        for _ in range(reps):
            cpu, ans = cpu_run(["z3", "-in"], text)
            if ans != expect:
                raise RuntimeError(f"calibration kernel {name}: expected {expect}, got {ans!r}")
            best = cpu if best is None else min(best, cpu)
        out[name] = round(best * 1000, 1)
    return {"kernel_cpu_ms": out, "reps": reps}


def loadavg() -> float:
    """1-minute load average of the whole host (/proc/loadavg is not namespaced)."""
    try:
        return float(Path("/proc/loadavg").read_text().split()[0])
    except OSError:
        return -1.0


def machine() -> dict:
    """Machine class. Nothing here names the host (design §4.4)."""
    cpu = "unknown"
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    mem_gb = 0
    try:
        for line in Path("/proc/meminfo").read_text().splitlines():
            if line.startswith("MemTotal:"):
                mem_gb = round(int(line.split()[1]) / (1024 * 1024))
                break
    except OSError:
        pass
    kernel = ".".join(platform.release().split(".")[:2])
    cores = os.cpu_count() or 0
    mid = hashlib.sha256(f"{cpu}|{cores}|{mem_gb}|{kernel}".encode()).hexdigest()[:5]
    return {"id": "m" + mid, "cpu": cpu, "cores": cores, "mem_gb": mem_gb, "kernel": kernel}


def tool_versions() -> dict:
    """Versions of the pinned tools actually on PATH, plus the lock hash."""
    from .tools import lock_sha
    tools = {"lock_sha": lock_sha()}
    try:
        v = subprocess.run(["z3", "--version"], capture_output=True, text=True).stdout.split()
        tools["z3"] = v[2] if len(v) > 2 else None
    except OSError:
        tools["z3"] = None
    return tools
