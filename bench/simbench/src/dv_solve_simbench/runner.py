"""Subprocess runner: build a bench image with a simulator and run it.

Only Verilator is supported for now. Images are cached per (tool, bench)
so configs that share a tool (and differ only in run-time solver or
sampler) share one build.
"""
import dataclasses as dc
import glob
import hashlib
import os
import subprocess
from typing import Dict, List, Optional

import yaml

from . import share_dir
from .config import Config
from .result import Result, parse_result


@dc.dataclass
class Bench:
    id: str
    tier: str
    path: str                       # bench.sv
    tags: List[str] = dc.field(default_factory=list)
    fields: List[str] = dc.field(default_factory=list)


def find_benches(root: str) -> List[Bench]:
    benches = []
    for yml in sorted(glob.glob(os.path.join(root, "**", "bench.yaml"), recursive=True)):
        with open(yml) as fp:
            d = yaml.safe_load(fp) or {}
        benches.append(Bench(id=d["id"], tier=d.get("tier", "standard"),
                             path=os.path.join(os.path.dirname(yml), "bench.sv"),
                             tags=d.get("tags", []), fields=d.get("fields", [])))
    return benches


@dc.dataclass
class RunOutput:
    cmd: List[str]
    returncode: int
    log: str
    result: Optional[Result]


class BuildError(Exception):
    pass


def _sv_files(bench: Bench) -> List[str]:
    sv = os.path.join(share_dir(), "sv")
    return [os.path.join(sv, "simbench_pkg.sv"), bench.path,
            os.path.join(sv, "simbench_top.sv"), os.path.join(sv, "sb_time.c")]


def build(cfg: Config, bench: Bench, cache_dir: str) -> str:
    """Build (or reuse) the image for `bench` with `cfg.tool`; return the executable."""
    key = hashlib.sha1(("%s|%s" % (cfg.tool, bench.path)).encode()).hexdigest()[:12]
    mdir = os.path.join(cache_dir, "%s-%s" % (bench.id, key))
    exe = os.path.join(mdir, "Vsimbench_top")
    if os.path.isfile(exe):
        return exe
    os.makedirs(mdir, exist_ok=True)
    cmd = [cfg.tool, "--binary", "-Wno-fatal", "--top-module", "simbench_top",
           "-Mdir", mdir] + _sv_files(bench)
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    with open(os.path.join(mdir, "build.log"), "w") as fp:
        fp.write(p.stdout)
    if p.returncode != 0 or not os.path.isfile(exe):
        raise BuildError("build failed (%d): %s\n%s" % (
            p.returncode, " ".join(cmd), p.stdout[-4000:]))
    return exe


def run(cfg: Config, exe: str, rundir: str, plusargs: List[str],
        timeout: float = 600) -> RunOutput:
    os.makedirs(rundir, exist_ok=True)
    env = dict(os.environ)
    env.update(cfg.run_env())
    cmd = [exe] + cfg.run_plusargs() + list(plusargs)
    p = subprocess.run(cmd, cwd=rundir, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=timeout)
    with open(os.path.join(rundir, "run.log"), "w") as fp:
        fp.write(p.stdout)
    return RunOutput(cmd=cmd, returncode=p.returncode, log=p.stdout,
                     result=parse_result(p.stdout))
