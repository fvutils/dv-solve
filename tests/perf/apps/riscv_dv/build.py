"""Build the riscv-dv generators and the solver wrappers, cached by input hash.

Inputs, each recorded in the run record and part of the cache key:

- the app Verilator: a Verilator source tree with a built bin/verilator
  (`--app-verilator` / $DVS_APP_VERILATOR). Until Verilator releases
  `+verilator+rand+sampler+`, this is the local sampler-control tree; its id
  is its commit plus a hash of any uncommitted changes;
- the runtime patches (patches/V-P*.patch), applied to a copy of its include/;
- UVM (`--app-uvm` / $DVS_APP_UVM: a directory holding src/uvm_pkg.sv);
- riscv-dv at the suite's pinned commit, with the suite's patches;
- the verilator flags and V-P02 (fix_nary.py).

A generator build takes about 80 s per target; only its 70 MB binary is kept.
The solver is chosen at run time, so dv-solve changes never rebuild a
generator.
"""
from __future__ import annotations

import hashlib
import os
import shutil
import subprocess
from pathlib import Path

from . import fix_nary

_HERE = Path(__file__).resolve().parent
PATCHES = _HERE / "patches"

VERILATOR_FLAGS = ["--main", "--exe", "--timing", "-j", "0", "-Wno-fatal", "-Wno-lint",
                   "-Wno-style", "-Wno-ENUMVALUE", "--vpi"]


def _sha(data: bytes, n: int = 16) -> str:
    return hashlib.sha256(data).hexdigest()[:n]


def _tree_sha(root: Path, pattern: str = "**/*") -> str:
    h = hashlib.sha256()
    for p in sorted(root.glob(pattern)):
        if p.is_file():
            h.update(str(p.relative_to(root)).encode())
            h.update(p.read_bytes())
    return h.hexdigest()[:16]


def _run(cmd, log: Path, **kw):
    with open(log, "a") as f:
        f.write("$ " + " ".join(map(str, cmd)) + "\n")
        f.flush()
        r = subprocess.run(list(map(str, cmd)), stdout=f, stderr=subprocess.STDOUT, **kw)
    if r.returncode:
        tail = "".join(log.read_text(errors="replace").splitlines(True)[-30:])
        raise RuntimeError(f"riscv-dv build step failed ({cmd[0]}), log {log}:\n{tail}")


def verilator_id(root: Path) -> dict:
    """The app Verilator's identity: commit, uncommitted-change hash, version."""
    def git(*a):
        return subprocess.run(["git", "-C", str(root), *a], capture_output=True).stdout
    commit = git("rev-parse", "HEAD").decode().strip()
    diff = git("diff", "HEAD", "--", "include", "src", "bin")
    ver = subprocess.run([str(root / "bin" / "verilator"), "--version"], capture_output=True,
                         text=True).stdout.strip()
    return {"commit": commit, "dirty": _sha(diff) if diff else None, "version": ver}


class Inputs:
    """Everything a generator build depends on, and the cache it builds into."""

    def __init__(self, spec: dict, verilator: Path, uvm: Path, cache: Path,
                 riscv_dv_repo: str | None = None):
        self.spec = spec
        self.verilator = verilator.resolve()
        self.uvm = uvm.resolve()
        self.cache = cache.resolve()
        self.repo = riscv_dv_repo or spec["riscv_dv"]["url"]
        if not (self.verilator / "bin" / "verilator").exists():
            raise RuntimeError(f"no bin/verilator under the app Verilator {self.verilator}")
        if not (self.uvm / "src" / "uvm_pkg.sv").exists():
            raise RuntimeError(f"no src/uvm_pkg.sv under UVM {self.uvm}")
        self.vlt = verilator_id(self.verilator)
        self.uvm_sha = _tree_sha(self.uvm / "src")
        self.patch_sha = {p: _sha((PATCHES / p).read_bytes())
                          for p in spec["patches"] + spec["runtime_patches"]}
        self.commit = spec["riscv_dv"]["commit"]

    def record(self) -> dict:
        """What the run record stores about the inputs."""
        return {"verilator": self.vlt, "uvm_sha": self.uvm_sha,
                "riscv_dv": {"url": self.spec["riscv_dv"]["url"], "commit": self.commit},
                "patches": self.patch_sha, "flags": " ".join(VERILATOR_FLAGS),
                "fix_nary": _sha(Path(fix_nary.__file__).read_bytes())}

    def key(self, *extra) -> str:
        return _sha(repr((self.record(), extra)).encode())

    # -- pieces ---------------------------------------------------------

    def runtime(self) -> Path:
        """A copy of the app Verilator's include/ with the runtime patches."""
        rt = self.cache / f"runtime-{self.key('runtime')}"
        if (rt / ".done").exists():
            return rt
        shutil.rmtree(rt, ignore_errors=True)
        rt.mkdir(parents=True)
        shutil.copytree(self.verilator / "include", rt / "include", symlinks=True)
        (rt / "bin").symlink_to(self.verilator / "bin")
        log = rt / "build.log"
        for p in self.spec["runtime_patches"]:
            _run(["patch", "-p1", "-i", PATCHES / p], log, cwd=rt)
        (rt / ".done").touch()
        return rt

    def source(self) -> Path:
        """riscv-dv at the pinned commit, with the suite's patches applied."""
        src = self.cache / f"riscv-dv-{self.commit[:12]}-{self.key('src')}"
        if (src / ".done").exists():
            return src
        shutil.rmtree(src, ignore_errors=True)
        src.mkdir(parents=True)
        log = src.with_suffix(".log")
        log.unlink(missing_ok=True)
        repo = self.repo
        if not Path(repo).is_dir():
            mirror = self.cache / "riscv-dv.git"
            if not mirror.exists():
                _run(["git", "clone", "-q", "--bare", repo, mirror], log)
            repo = str(mirror)
        arch = subprocess.run(["git", "-C", repo, "archive", self.commit], capture_output=True)
        if arch.returncode:
            raise RuntimeError(f"riscv-dv commit {self.commit} not in {repo}: "
                               f"{arch.stderr.decode().strip()}")
        subprocess.run(["tar", "x", "-C", str(src)], input=arch.stdout, check=True)
        for p in self.spec["patches"]:
            _run(["patch", "-p1", "-i", PATCHES / p], log, cwd=src)
        (src / ".done").touch()
        return src

    def generator(self, target: str, jobs: int = 32) -> Path:
        """The target's generator binary; built once per input hash."""
        out = self.cache / f"gen-{self.key('gen', target)}" / target
        exe = out / "Vriscv_instr_gen_tb_top"
        if exe.exists():
            return exe
        src, rt = self.source(), self.runtime()
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True)
        obj = out / "obj_dir"
        log = out / "build.log"
        env = dict(os.environ, RISCV_DV_ROOT=str(src), VERILATOR_ROOT=str(self.verilator))
        uvm = self.uvm / "src"
        _run([self.verilator / "bin" / "verilator", *VERILATOR_FLAGS,
              f"+incdir+{uvm}", uvm / "uvm_pkg.sv", uvm / "dpi" / "uvm_dpi.cc",
              "-f", src / "files.f", f"+incdir+{src}/target/{target}",
              f"+incdir+{src}/user_extension", "--top-module", "riscv_instr_gen_tb_top",
              "-Mdir", obj], log, cwd=src, env=env)
        with open(log, "a") as f:
            f.write(f"fix_nary: patched {fix_nary.patch_dir(obj)} sites\n")
        _run(["make", "-C", obj, "-f", "Vriscv_instr_gen_tb_top.mk", f"-j{jobs}",
              f"VERILATOR_ROOT={rt}"], log, env=env)
        shutil.move(str(obj / "Vriscv_instr_gen_tb_top"), str(exe))
        shutil.rmtree(obj)
        return exe

    def tools(self) -> dict:
        """solver_rusage and solver_tap, compiled from this directory."""
        srcs = {n: _HERE / f"{n}.c" for n in ("solver_rusage", "solver_tap")}
        d = self.cache / f"tools-{_sha(b''.join(p.read_bytes() for p in srcs.values()))}"
        d.mkdir(parents=True, exist_ok=True)
        out = {}
        for n, src in srcs.items():
            exe = d / n
            if not exe.exists():
                _run(["cc", "-O2", "-o", exe, src], d / "build.log")
            out[n] = exe
        return out


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for blk in iter(lambda: f.read(1 << 20), b""):
            h.update(blk)
    return h.hexdigest()
