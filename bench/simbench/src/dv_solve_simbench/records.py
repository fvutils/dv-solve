"""JSONL run records, one per (config, bench) run.

Each record is a well-formed dv-solve `tests/perf/schema.py` run record
(schema 1, kind "manual") so its validator and loaders accept it; the
simbench-specific part sits under the "simbench" key. Like tests/perf, only
the standard library is used, and nothing in the machine block names the host.
"""
import functools
import hashlib
import json
import os
import platform
import subprocess
import time
from typing import Optional

from .config import Config
from .result import Result
from .runner import Bench

SCHEMA = 1
KIND = "manual"
SIMBENCH = 1        # version of the "simbench" block


def _loadavg() -> float:
    try:
        with open("/proc/loadavg") as fp:
            return float(fp.read().split()[0])
    except OSError:
        return -1.0


@functools.lru_cache(maxsize=None)
def _machine_class() -> dict:
    """Same fields and id as tests/perf/calib.py:machine()."""
    cpu = "unknown"
    try:
        with open("/proc/cpuinfo") as fp:
            for line in fp:
                if line.startswith("model name"):
                    cpu = line.split(":", 1)[1].strip()
                    break
    except OSError:
        pass
    mem_gb = 0
    try:
        with open("/proc/meminfo") as fp:
            for line in fp:
                if line.startswith("MemTotal:"):
                    mem_gb = round(int(line.split()[1]) / (1024 * 1024))
                    break
    except OSError:
        pass
    kernel = ".".join(platform.release().split(".")[:2])
    cores = os.cpu_count() or 0
    mid = hashlib.sha256(f"{cpu}|{cores}|{mem_gb}|{kernel}".encode()).hexdigest()[:5]
    return {"id": "m" + mid, "cpu": cpu, "cores": cores, "mem_gb": mem_gb, "kernel": kernel}


def machine(loadavg_start: float, loadavg_end: float) -> dict:
    """Machine block; env SIMBENCH_HOST adds a free-form label (e.g. "alice-ws") for merged tables."""
    m = dict(_machine_class())
    if os.environ.get("SIMBENCH_HOST"):
        m["label"] = os.environ["SIMBENCH_HOST"]
    m.update(loadavg_start=loadavg_start, loadavg_end=loadavg_end,
             noisy=max(loadavg_start, loadavg_end) > m["cores"] / 2)
    return m


def _git(*args) -> str:
    here = os.path.dirname(os.path.abspath(__file__))
    try:
        p = subprocess.run(["git", *args], cwd=here, capture_output=True, text=True)
        return p.stdout.strip() if p.returncode == 0 else ""
    except OSError:
        return ""


@functools.lru_cache(maxsize=None)
def run_info() -> dict:
    """The dv-solve checkout simbench runs from."""
    return {"commit": _git("rev-parse", "HEAD") or "unknown",
            "ref": _git("rev-parse", "--abbrev-ref", "HEAD") or "unknown",
            "dirty": bool(_git("status", "--porcelain", "--untracked-files=no"))}


@functools.lru_cache(maxsize=None)
def file_sha(path: str) -> Optional[str]:
    """sha1 of a bench source, so results from different checkouts can be matched."""
    try:
        with open(path, "rb") as fp:
            return hashlib.sha1(fp.read()).hexdigest()
    except OSError:
        return None


@functools.lru_cache(maxsize=None)
def tool_version(tool: str, flag: str = "--version") -> Optional[str]:
    try:
        p = subprocess.run([tool, flag], capture_output=True, text=True, timeout=30)
        return p.stdout.strip().splitlines()[0] if p.stdout.strip() else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def utc_now() -> str:
    return time.strftime("%Y%m%dT%H%M%SZ", time.gmtime())


def make_record(cfg: Config, bench: Bench, result: Optional[Result], params: dict,
                loadavg_start: float, error: str = "", utc: Optional[str] = None,
                sampler_active: Optional[str] = None,
                randomization: Optional[dict] = None) -> dict:
    """Run record for one run; `result` is None when no SIMBENCH_RESULT was seen."""
    valid = result is not None and result.ok and not error
    rec = {
        "schema": SCHEMA, "valid": valid, "kind": KIND,
        "run": dict(run_info(), utc=utc or utc_now()),
        "machine": machine(loadavg_start, _loadavg()),
        "tools": {"lock_sha": None, "simulator": cfg.simulator, "simulator_path": cfg.tool,
                  "simulator_version": tool_version(cfg.tool, cfg.version_flag),
                  "solver": cfg.solver, "sampler": cfg.sampler,
                  "sampler_active": sampler_active},
        "calib": {},
        "simbench": {
            "v": SIMBENCH, "config": cfg.id, "bench": bench.id, "tier": bench.tier,
            "bench_sha": file_sha(bench.path),
            "tags": list(bench.tags), "params": dict(params),
            "result": None if result is None else {
                **{k: getattr(result, k) for k in result.__dataclass_fields__},
                "ns_per_call": result.ns_per_call, "calls_per_s": result.calls_per_s},
        },
    }
    if randomization is not None and rec["simbench"]["result"] is not None:
        rec["simbench"]["result"]["randomization"] = randomization
    if not valid:
        rec["reason"] = error or ("no SIMBENCH_RESULT" if result is None else
                                  "bench failed (failures=%d check_errors=%d timed_out=%s)"
                                  % (result.failures, result.check_errors, result.timed_out))
    return rec


def append(path: str, rec: dict) -> None:
    with open(path, "a") as fp:
        fp.write(json.dumps(rec, sort_keys=True) + "\n")


def load(path: str) -> list:
    with open(path) as fp:
        return [json.loads(ln) for ln in fp if ln.strip()]
