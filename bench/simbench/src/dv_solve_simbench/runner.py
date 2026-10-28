"""Subprocess runner: build a bench image with a simulator and run it.

Back ends: verilator, vcs, xcelium, questa (Config.simulator). Images are
cached per (simulator, tool, build args, bench) so configs that share a tool
(and differ only in run-time solver or sampler) share one build. sb_time.c
(the DPI timer) is passed to each compiler, which builds it with the image.
Only the Verilator commands have been run here; the commercial command lines
are checked by unit tests only.
"""
import dataclasses as dc
import glob
import hashlib
import os
import re
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
    info: dict = dc.field(default_factory=dict)   # for the record: replay/session details


class BuildError(Exception):
    pass


def _sv_files(bench: Bench) -> List[str]:
    sv = os.path.join(share_dir(), "sv")
    return [os.path.join(sv, "simbench_pkg.sv"), bench.path,
            os.path.join(sv, "simbench_top.sv"), os.path.join(sv, "sb_time.c")]


def _sibling(tool: str, name: str) -> str:
    """Another executable from the install of `tool` (e.g. vlog next to vsim)."""
    d = os.path.dirname(tool)
    return os.path.join(d, name) if d else name


def build_cmds(cfg: Config, bench: Bench, bdir: str) -> List[List[str]]:
    """Commands (run in `bdir`, in order) that build the image for `bench`."""
    files = _sv_files(bench)
    sim = cfg.simulator
    if sim == "verilator":
        return [[cfg.tool, "--binary", "-Wno-fatal", "--top-module", "simbench_top",
                 "-Mdir", bdir] + cfg.build_args + files]
    if sim == "vcs":
        return [[cfg.tool, "-full64", "-sverilog", "-timescale=1ns/1ps", "-top", "simbench_top",
                 "-Mdir=" + os.path.join(bdir, "csrc"), "-o", os.path.join(bdir, "simv")]
                + cfg.build_args + files]
    if sim == "xcelium":
        return [[cfg.tool, "-64bit", "-sv", "-timescale", "1ns/1ps", "-top", "simbench_top",
                 "-xmlibdirpath", bdir, "-elaborate"] + cfg.build_args + files]
    if sim == "questa":
        lib = os.path.join(bdir, "work")
        return [[_sibling(cfg.tool, "vlib"), lib],
                [_sibling(cfg.tool, "vlog"), "-sv", "-timescale", "1ns/1ps", "-work", lib]
                + cfg.build_args + files]
    raise BuildError("%s: unknown simulator %r" % (cfg.id, sim))


def image_path(cfg: Config, bdir: str) -> str:
    """File whose presence means the build in `bdir` is done; passed to run_cmd()."""
    return os.path.join(bdir, {"verilator": "Vsimbench_top", "vcs": "simv",
                               "xcelium": "xcelium.d", "questa": "work"}[cfg.simulator])


def run_cmd(cfg: Config, image: str, plusargs: List[str]) -> List[str]:
    args = cfg.run_plusargs() + list(plusargs)
    if cfg.simulator in ("verilator", "vcs"):
        return [image] + args
    if cfg.simulator == "xcelium":
        return [cfg.tool, "-64bit", "-R", "-xmlibdirpath", os.path.dirname(image)] + args
    if cfg.simulator == "questa":
        return [cfg.tool, "-c", "-lib", image, "simbench_top",
                "-do", "run -all; quit -f"] + args
    raise ValueError("%s: unknown simulator %r" % (cfg.id, cfg.simulator))


def build(cfg: Config, bench: Bench, cache_dir: str) -> str:
    """Build (or reuse) the image for `bench` with `cfg.tool`; return the image path."""
    if cfg.simulator == "verilator" and not cfg.build_args:
        key_src = "%s|%s" % (cfg.tool, bench.path)     # keeps existing cache keys
    else:
        key_src = "|".join([cfg.simulator, cfg.tool, bench.path] + cfg.build_args)
    key = hashlib.sha1(key_src.encode()).hexdigest()[:12]
    bdir = os.path.join(cache_dir, "%s-%s" % (bench.id, key))
    cmds = build_cmds(cfg, bench, bdir)
    image = image_path(cfg, bdir)
    if os.path.exists(image):
        return image
    os.makedirs(bdir, exist_ok=True)
    log = []
    for cmd in cmds:
        p = subprocess.run(cmd, cwd=bdir, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                           text=True)
        log.append("$ %s\n%s" % (" ".join(cmd), p.stdout))
        if p.returncode != 0:
            break
    with open(os.path.join(bdir, "build.log"), "w") as fp:
        fp.write("".join(log))
    if p.returncode != 0 or not os.path.exists(image):
        raise BuildError("build failed (%d): %s\n%s" % (
            p.returncode, " ".join(cmd), p.stdout[-4000:]))
    return image


def run(cfg: Config, exe: str, rundir: str, plusargs: List[str],
        timeout: float = 600) -> RunOutput:
    os.makedirs(rundir, exist_ok=True)
    env = dict(os.environ)
    env.update(cfg.run_env())
    cmd = run_cmd(cfg, exe, plusargs)
    p = subprocess.run(cmd, cwd=rundir, env=env, stdout=subprocess.PIPE,
                       stderr=subprocess.STDOUT, text=True, timeout=timeout)
    with open(os.path.join(rundir, "run.log"), "w") as fp:
        fp.write(p.stdout)
    return RunOutput(cmd=cmd, returncode=p.returncode, log=p.stdout,
                     result=parse_result(p.stdout))


def probe_sampler(cfg: Config, exe: str, rundir: str, timeout: float = 120) -> Optional[str]:
    """Sampler the image actually used, from a one-call run's solver log.

    The dev Verilator runtime writes '; sampler: <name>' at the head of the
    +verilator+solver+file+ log; a separate run keeps that log out of timing.
    None for other simulators (no sampler control).
    """
    if cfg.simulator != "verilator":
        return None
    os.makedirs(rundir, exist_ok=True)
    path = os.path.join(rundir, "probe.smt2")
    env = dict(os.environ)
    env.update(cfg.run_env())
    cmd = [exe] + cfg.run_plusargs() + ["+sb_n=1", "+verilator+solver+file+" + path]
    subprocess.run(cmd, cwd=rundir, env=env, stdout=subprocess.DEVNULL,
                   stderr=subprocess.DEVNULL, timeout=timeout)
    if not os.path.exists(path):
        return None
    with open(path) as fp:
        m = re.search(r"^\s*; sampler: (\w+)", fp.read(65536), re.M)
    return m.group(1) if m else None
