"""Load simbench configurations (configs.yaml) and detect their tools."""
import dataclasses as dc
import functools
import os
import re
import shlex
import shutil
import subprocess
from typing import Dict, List, Optional

import yaml

from . import share_dir

_VAR_RE = re.compile(r"\$\{([A-Za-z_][A-Za-z0-9_]*)(?::-([^}]*))?\}")


def expand(value: str, environ: Optional[Dict[str, str]] = None) -> str:
    """Expand ${VAR} and ${VAR:-default}, then ~ at the start of each word."""
    env = os.environ if environ is None else environ

    def sub(m):
        v = env.get(m.group(1))
        if v is None or v == "":
            v = m.group(2) if m.group(2) is not None else ""
        return v

    value = _VAR_RE.sub(sub, value)
    return " ".join(os.path.expanduser(w) if w.startswith("~") else w
                    for w in value.split(" "))


# Simulators the runner can drive, and the flag that prints each tool's version.
# Only Verilator has the solver/sampler controls (VERILATOR_SOLVER, sampler plusarg).
SIMULATORS = {"verilator": "--version", "vcs": "-ID", "xcelium": "-version", "questa": "-version"}


SAMPLER_OPTION = "+verilator+rand+sampler+"


@functools.lru_cache(maxsize=None)
def sampler_control(tool: str) -> bool:
    """Whether Verilator `tool` has sampler control: its --help lists the
    +verilator+rand+sampler+<name> runtime option (upstream since #8899). A
    Verilator without it ignores the sampler plusarg and runs its own sampler,
    so a config asking for one would silently measure something else."""
    try:
        p = subprocess.run([tool, "--help"], capture_output=True, text=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return False
    return SAMPLER_OPTION in (p.stdout + p.stderr)


def _which(prog: str) -> Optional[str]:
    if os.sep in prog:
        return prog if os.path.isfile(prog) and os.access(prog, os.X_OK) else None
    return shutil.which(prog)


@dc.dataclass
class Config:
    id: str
    simulator: str
    tool: str
    solver: Optional[str] = None
    sampler: Optional[str] = None
    version: Optional[str] = None     # substring required in `tool --version`
    plusargs: List[str] = dc.field(default_factory=list)
    build_args: List[str] = dc.field(default_factory=list)   # extra compile/elaborate args
    env: Dict[str, str] = dc.field(default_factory=dict)
    replay: Optional[str] = None      # bench_vlt_replay: time the captured session without Verilator
    replay_pass: str = "api-cache"   # the one bench_vlt_replay pass timed
    replay_cmd: Optional[str] = None  # e.g. "z3 -smt2": time the captured session text as one process
    replay_reps: int = 1
    available: bool = False
    reason: str = ""

    @property
    def version_flag(self) -> str:
        return SIMULATORS.get(self.simulator, "--version")

    def run_env(self) -> Dict[str, str]:
        """Environment additions for a simulation run."""
        env = dict(self.env)
        if self.solver and self.simulator == "verilator":
            env["VERILATOR_SOLVER"] = self.solver
        return env

    def run_plusargs(self) -> List[str]:
        args = list(self.plusargs)
        if self.sampler and self.simulator == "verilator":
            args.append("+verilator+rand+sampler+%s" % self.sampler)
        return args

    def check_sampler_control(self) -> None:
        """Raise unless this config's Verilator has the sampler control it needs."""
        if self.simulator == "verilator" and self.sampler and not sampler_control(self.tool):
            raise RuntimeError(
                "%s: %s has no sampler control (%s<name> is not in its --help); "
                "config wants sampler %r. Use a Verilator that has it (upstream master "
                "since #8899) via SIMBENCH_VERILATOR_SC." % (self.id, self.tool, SAMPLER_OPTION,
                                                           self.sampler))

    def detect(self) -> "Config":
        """Set available/reason from the presence of the tools."""
        if self.simulator not in SIMULATORS:
            self.available, self.reason = False, "unsupported simulator %r (known: %s)" % (
                self.simulator, ", ".join(SIMULATORS))
            return self
        if self.simulator != "verilator" and (self.solver or self.sampler or self.replay
                                              or self.replay_cmd):
            self.available, self.reason = False, \
                "solver/sampler/replay are Verilator-only (simulator %s)" % self.simulator
            return self
        tool = _which(self.tool)
        if tool is None:
            self.available, self.reason = False, "simulator not found: %s" % self.tool
            return self
        self.tool = tool
        if self.solver:
            words = shlex.split(self.solver)
            if not words or _which(words[0]) is None:
                self.available, self.reason = False, "solver not found: %s" % self.solver
                return self
        if self.replay:
            replay = _which(self.replay)
            if replay is None:
                self.available, self.reason = False, "replay tool not found: %s" % self.replay
                return self
            self.replay = replay
        if self.replay_cmd:
            words = shlex.split(self.replay_cmd)
            exe = _which(words[0]) if words else None
            if exe is None:
                self.available, self.reason = False, "replay command not found: %s" % self.replay_cmd
                return self
            self.replay_cmd = " ".join([shlex.quote(exe)] + [shlex.quote(w) for w in words[1:]])
        if self.sampler not in (None, "default", "solver"):
            self.available, self.reason = False, "bad sampler %r" % self.sampler
            return self
        self.available, self.reason = True, ""
        return self


def dvs_defaults() -> Dict[str, str]:
    """When simbench runs from a dv-solve checkout (bench/simbench), default the
    dv-solve tools to that checkout's build: SIMBENCH_DVS_SMT2 and
    SIMBENCH_DVS_REPLAY. Benchmarking any other dv-solve needs them set."""
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))
    build = os.environ.get("SIMBENCH_DVS_BUILD") or os.path.join(root, "build")
    if not os.path.exists(os.path.join(root, "src", "c", "dv_solve.h")):
        return {}
    return {"SIMBENCH_DVS_SMT2": os.path.join(build, "dv-solve-smt2"),
            "SIMBENCH_DVS_REPLAY": os.path.join(build, "bench_vlt_replay")}


def default_path() -> str:
    return os.path.join(share_dir(), "configs.yaml")


def _parse(path: str, environ) -> List[Config]:
    with open(path) as f:
        data = yaml.safe_load(f) or {}
    ret = []
    for c in data.get("configs", []):
        ret.append(Config(
            id=c["id"],
            simulator=c.get("simulator", "verilator"),
            tool=expand(str(c["tool"]), environ),
            solver=expand(str(c["solver"]), environ) if c.get("solver") else None,
            sampler=c.get("sampler"),
            version=str(c["version"]) if c.get("version") else None,
            plusargs=[expand(str(p), environ) for p in c.get("plusargs", [])],
            build_args=[expand(str(a), environ) for a in c.get("build_args", [])]
            + (shlex.split(environ.get("SIMBENCH_VERILATOR_ARGS", ""))
               if c.get("simulator", "verilator") == "verilator" else []),
            env={k: expand(str(v), environ) for k, v in (c.get("env") or {}).items()},
            replay=expand(str(c["replay"]), environ) if c.get("replay") else None,
            replay_pass=str(c.get("replay_pass") or "api-cache"),
            replay_cmd=expand(str(c["replay_cmd"]), environ) if c.get("replay_cmd") else None,
            replay_reps=int(c.get("replay_reps") or 1)))
    return ret


def load_configs(paths: Optional[List[str]] = None,
                 environ: Optional[Dict[str, str]] = None,
                 detect: bool = True) -> Dict[str, Config]:
    """Load configs from the default file plus SIMBENCH_CONFIGS (os.pathsep list).
    Later files override earlier ones by id. Order of first appearance is kept."""
    env = dict(os.environ if environ is None else environ)
    env.update({k: v for k, v in dvs_defaults().items() if not env.get(k)})
    if paths is None:
        paths = [default_path()]
        extra = env.get("SIMBENCH_CONFIGS", "")
        paths += [p for p in extra.split(os.pathsep) if p]
    ret: Dict[str, Config] = {}
    for p in paths:
        for c in _parse(p, env):
            ret[c.id] = c
    if detect:
        for c in ret.values():
            c.detect()
    return ret
