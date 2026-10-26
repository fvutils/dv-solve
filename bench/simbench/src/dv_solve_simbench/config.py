"""Load simbench configurations (configs.yaml) and detect their tools."""
import dataclasses as dc
import os
import re
import shlex
import shutil
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
    env: Dict[str, str] = dc.field(default_factory=dict)
    replay: Optional[str] = None      # bench_vlt_replay: time the captured session without Verilator
    replay_pass: str = "api,api-cache"
    available: bool = False
    reason: str = ""

    def run_env(self) -> Dict[str, str]:
        """Environment additions for a simulation run."""
        env = dict(self.env)
        if self.solver:
            env["VERILATOR_SOLVER"] = self.solver
        return env

    def run_plusargs(self) -> List[str]:
        args = list(self.plusargs)
        if self.sampler:
            args.append("+verilator+rand+sampler+%s" % self.sampler)
        return args

    def detect(self) -> "Config":
        """Set available/reason from the presence of the tools."""
        if self.simulator != "verilator":
            self.available, self.reason = False, "unsupported simulator %s" % self.simulator
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
        if self.sampler not in (None, "default", "solver"):
            self.available, self.reason = False, "bad sampler %r" % self.sampler
            return self
        self.available, self.reason = True, ""
        return self


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
            env={k: expand(str(v), environ) for k, v in (c.get("env") or {}).items()},
            replay=expand(str(c["replay"]), environ) if c.get("replay") else None,
            replay_pass=str(c.get("replay_pass") or "api,api-cache")))
    return ret


def load_configs(paths: Optional[List[str]] = None,
                 environ: Optional[Dict[str, str]] = None,
                 detect: bool = True) -> Dict[str, Config]:
    """Load configs from the default file plus SIMBENCH_CONFIGS (os.pathsep list).
    Later files override earlier ones by id. Order of first appearance is kept."""
    env = os.environ if environ is None else environ
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
