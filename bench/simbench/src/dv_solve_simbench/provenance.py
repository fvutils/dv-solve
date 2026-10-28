"""What a record needs for troubleshooting a mismatch between two runs.

The 10-27 replay numbers could not be reproduced and nothing in their records
could say why: not which dv-solve library the replay loaded, how it was built,
how the replay was scored, or how fast the host was at the time. This module
gathers that:

- build_info(): the dv-solve build behind a binary (`--build-info`): commit,
  dirty, build type, optimisation, sanitizers, compiler, and the library file
  actually loaded (LD_LIBRARY_PATH beats a binary's RUNPATH) with its sha256.
- calib(): fixed work timed per record: a Python loop (host alone) and, with a
  bench_vlt_replay binary, its --calib (host x library build).
- host_state(): CPU governor and clock, CPU affinity, pytest-xdist workers.
- env_info(): environment variables that change the simulator or solver.
- session_info(): size, hash and check-sat count of a captured solver session;
  SIMBENCH_SESSION_DIR=<dir> also keeps it as <dir>/<config>/<bench>.smt2.gz.
- replay_details(): everything the replay printed about its scoring.
"""
import functools
import gzip
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import time
from typing import Dict, Optional

# Variables that change how the simulator, the solver or the bench behaves.
_ENV_PREFIXES = ("DV_", "DVS_", "VERILATOR", "SIMBENCH_", "ZSP_")
_ENV_NAMES = ("LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH", "PATH",
              "OMP_NUM_THREADS", "MALLOC_ARENA_MAX", "GLIBC_TUNABLES")


def env_info(extra: Optional[Dict[str, str]] = None) -> Dict[str, str]:
    env = dict(os.environ)
    env.update(extra or {})
    return {k: v for k, v in sorted(env.items())
            if k.startswith(_ENV_PREFIXES) or k in _ENV_NAMES}


def _read(path: str) -> Optional[str]:
    try:
        with open(path) as fp:
            return fp.read().strip()
    except OSError:
        return None


def host_state() -> dict:
    """Things about the host that change between runs (not the machine class)."""
    st = {"governor": _read("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")}
    mhz = []
    try:
        with open("/proc/cpuinfo") as fp:
            for ln in fp:
                if ln.startswith("cpu MHz"):
                    mhz.append(float(ln.split(":", 1)[1]))
    except (OSError, ValueError):
        pass
    if mhz:
        st["cpu_mhz_mean"] = round(sum(mhz) / len(mhz))
        st["cpu_mhz_max"] = round(max(mhz))
    try:
        st["affinity_cpus"] = len(os.sched_getaffinity(0))
    except (AttributeError, OSError):
        st["affinity_cpus"] = None
    st["xdist_workers"] = int(os.environ.get("PYTEST_XDIST_WORKER_COUNT", "1") or 1)
    return st


@functools.lru_cache(maxsize=None)
def file_sha256(path: str) -> Optional[str]:
    try:
        h = hashlib.sha256()
        with open(path, "rb") as fp:
            for blk in iter(lambda: fp.read(1 << 20), b""):
                h.update(blk)
        return h.hexdigest()
    except OSError:
        return None


def parse_build_info(text: str) -> dict:
    """`build-info k=v ...` and `library <path>` lines (dvs_build_info())."""
    out = {}
    m = re.search(r"^build-info (.*)$", text, re.M)
    if m:
        out["build_info"] = m.group(1)
        for k, v in re.findall(r'(\w+)=("[^"]*"|\S*)', m.group(1)):
            v = v.strip('"')
            out[k] = int(v) if k in ("dirty", "optimized", "asan", "ndebug") and v.isdigit() else v
    m = re.search(r"^library (.*)$", text, re.M)
    if m:
        out["library"] = m.group(1).strip()
    return out


def _dvs_binary(cmd: Optional[str]) -> Optional[str]:
    """The binary of a dv-solve command line (dv-solve-smt2, bench_vlt_replay), else None."""
    words = shlex.split(cmd or "")
    if not words:
        return None
    exe = os.path.expanduser(words[0])
    name = os.path.basename(exe)
    if not (name.startswith("dv-solve") or name.startswith("bench_vlt_replay")):
        return None
    return shutil.which(exe) or exe


def _ld_key(env: Dict[str, str]) -> tuple:
    return tuple(env.get(k, "") for k in ("LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH"))


@functools.lru_cache(maxsize=None)
def _build_info(binary: str, ld: tuple) -> dict:
    env = dict(os.environ)
    for k, v in zip(("LD_LIBRARY_PATH", "LD_PRELOAD", "DYLD_LIBRARY_PATH"), ld):
        if v:
            env[k] = v
        else:
            env.pop(k, None)
    info = {"binary": binary, "binary_sha256": file_sha256(binary)}
    try:
        p = subprocess.run([binary, "--build-info"], capture_output=True, text=True,
                           timeout=30, env=env)
        info.update(parse_build_info(p.stdout))
        if "build_info" not in info:
            info["error"] = "no --build-info (rc=%d): %s" % (p.returncode, (p.stdout + p.stderr)[-300:])
    except (OSError, subprocess.TimeoutExpired) as e:
        info["error"] = str(e)
    if info.get("library"):
        lib = info["library"]
        if not os.path.isabs(lib):
            lib = shutil.which(lib) or os.path.abspath(lib)
        info["library"] = lib
        info["library_sha256"] = file_sha256(lib)
    return info


def build_info(cmd: Optional[str], extra_env: Optional[Dict[str, str]] = None) -> Optional[dict]:
    """dv-solve build behind the command `cmd`, under the environment it runs in; None
    when `cmd` is not a dv-solve binary."""
    binary = _dvs_binary(cmd)
    if binary is None:
        return None
    env = dict(os.environ)
    env.update(extra_env or {})
    return dict(_build_info(binary, _ld_key(env)))


def _py_loop_ms() -> float:
    """Fixed pure-Python work, best of 3: a host-speed reading for any config."""
    best = 1e30
    for _ in range(3):
        t = time.perf_counter()
        x = 0
        for i in range(300000):
            x = (x * 31 + i) & 0xFFFFFFFF
        best = min(best, time.perf_counter() - t)
    return round(best * 1e3, 3)


def parse_calib(text: str) -> dict:
    m = re.search(r"^calib cpu_ns_per_iter ([\d.]+) dvs_solve_us ([\d.]+) dvs_compile_us ([\d.]+)(.*)$",
                  text, re.M)
    if not m:
        return {}
    out = {"cpu_ns_per_iter": float(m.group(1)), "dvs_solve_us": float(m.group(2)),
           "dvs_compile_us": float(m.group(3))}
    if m.group(4).strip():
        out["calib_note"] = m.group(4).strip()
    return out


def calib_binary() -> Optional[str]:
    """A bench_vlt_replay to calibrate with: SIMBENCH_DVS_REPLAY, else the configs' default."""
    for p in (os.environ.get("SIMBENCH_DVS_REPLAY"),
              "~/projects/verilator/verilator-sampler-control/dv-solve/build/bench_vlt_replay"):
        if p and os.path.exists(os.path.expanduser(p)):
            return os.path.expanduser(p)
    return None


def calib(replay: Optional[str] = None) -> dict:
    """Fixed work timed now. With a bench_vlt_replay binary (the config's, else
    calib_binary()), also its --calib: a fixed dv-solve solve and compile."""
    out = {"py_loop_ms": _py_loop_ms()}
    binary = _dvs_binary(replay) if replay else calib_binary()
    if binary:
        try:
            p = subprocess.run([binary, "--calib"], capture_output=True, text=True, timeout=120)
            out.update(parse_calib(p.stdout))
            out["calib_library"] = parse_build_info(p.stdout).get("library")
        except (OSError, subprocess.TimeoutExpired) as e:
            out["calib_error"] = str(e)
    return out


def session_info(path: str, n_check: Optional[int], config_id: str, bench_id: str) -> dict:
    """Size and hash of a captured session; kept compressed under SIMBENCH_SESSION_DIR."""
    info = {"bytes": os.path.getsize(path) if os.path.exists(path) else None,
            "sha256": file_sha256.__wrapped__(path), "n_check": n_check}
    keep = os.environ.get("SIMBENCH_SESSION_DIR")
    if keep and os.path.exists(path):
        d = os.path.join(keep, config_id)
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, bench_id.replace("/", "_") + ".smt2.gz")
        with open(path, "rb") as fi, gzip.open(dst, "wb") as fo:
            shutil.copyfileobj(fi, fo)
        info["kept"] = dst
    return info


def replay_details(text: str, pass_name: str) -> dict:
    """What bench_vlt_replay printed about the session and the timed pass."""
    d = parse_build_info(text)
    m = re.search(r"^\s+(\d+) commands, (\d+) check-sats, ([\d.]+) MB", text, re.M)
    if m:
        d.update(commands=int(m.group(1)), check_sats=int(m.group(2)), mb=float(m.group(3)))
    m = re.search(r"^\s+mode (\w+); (\d+) solver sessions, (\d+) randomize\(\) calls, "
                  r"(\d+) warm-up, (\d+) timed(.*)$", text, re.M)
    if m:
        d.update(mode=m.group(1), sessions=int(m.group(2)), calls=int(m.group(3)),
                 warmup=int(m.group(4)), timed_calls=int(m.group(5)),
                 warmup_in_average=bool(m.group(6).strip()))
    m = re.search(r"^\s+captured (\d+) problems \((\d+) answered on bitblast, (\d+) with "
                  r"constraints added under push, (\d+) unsat\), (\d+) check-sats not captured",
                  text, re.M)
    if m:
        d.update(captured=int(m.group(1)), captured_bitblast=int(m.group(2)),
                 captured_with_addons=int(m.group(3)), captured_unsat=int(m.group(4)),
                 not_captured=int(m.group(5)))
    m = re.search(r"^%s\s+wall\s+([\d.]+) s\s+([\d.]+) us/check-sat\s+\(CPU ([\d.]+) s; "
                  r"(\d+) distinct, (\d+) reused; (\d+) CDCL answers escalated to bitblast, "
                  r"(\d+) bitblast failures, (\d+) unsolved\)" % re.escape(pass_name), text, re.M)
    if m:
        d.update(pass_name=pass_name, wall_s=float(m.group(1)), us_per_check=float(m.group(2)),
                 cpu_s=float(m.group(3)), distinct=int(m.group(4)), reused=int(m.group(5)),
                 escalated=int(m.group(6)), bitblast_failures=int(m.group(7)),
                 unsolved=int(m.group(8)))
        tail = text[m.end():]
        # The pass's block ends at the next pass's first line.
        nxt = re.search(r"^(?!%s\s+per call)\S" % re.escape(pass_name), tail, re.M)
        block = tail[:nxt.start()] if nxt else tail
        for key, lab in (("build", "build (builder)"), ("compile", "compile/bitblast"),
                         ("solve", "solve+read")):
            s = re.search(r"^\s+%s\s+([\d.]+) s\s+([\d.]+) us" % re.escape(lab), block, re.M)
            if s:
                d[key + "_us_per_check"] = float(s.group(2))
        s = re.search(r"on a reused context: ([\d.]+) us per call; on a new one: ([\d.]+) us", block)
        if s:
            d.update(us_reused=float(s.group(1)), us_new=float(s.group(2)))
    m = re.search(r"^%s\s+per call\s+([\d.]+) us" % re.escape(pass_name), text, re.M)
    if m:
        d["us_per_call"] = float(m.group(1))
    q = replay_quality(text, pass_name)
    if q:
        d["quality"] = q
    c = replay_census(text)
    if c:
        d["census"] = c
    return d


_CENSUS = (r"^census\s+(\d+) calls, (\d+) distinct problems \(call repeat ([\d.]+)\); components per "
           r"call mean ([\d.]+) max (\d+); (\d+) component instances, (\d+) distinct "
           r"\(component repeat ([\d.]+)\)$")
_SITE = re.compile(r"^\s+site (\S+) vars (\d+) aux (\d+) box2 ([\d.]+) class (\w+) instances (\d+) "
                   r"keys (\d+) constructs (\S+)$", re.M)


def replay_census(text: str) -> Optional[dict]:
    """bench_vlt_replay --census (P0.5): the captured calls split into
    independent components, their sampler-layer classes and constructs
    (shares of component instances), and the busiest component sites."""
    m = re.search(_CENSUS, text, re.M)
    if not m:
        return None
    g = m.groups()
    c = {"calls": int(g[0]), "distinct_problems": int(g[1]), "call_repeat": float(g[2]),
         "components_mean": float(g[3]), "components_max": int(g[4]),
         "instances": int(g[5]), "distinct_instances": int(g[6]),
         "component_repeat": float(g[7])}
    m2 = re.search(r"^census\s+calls all closed-form \(free/interval\) ([\d.]+), all <= small "
                   r"([\d.]+)$", text, re.M)
    if m2:
        c["calls_closed_form"] = float(m2.group(1))
        c["calls_small"] = float(m2.group(2))
    for key, lab in (("classes", "class"), ("constructs", "construct")):
        m3 = re.search(r"^census\s+%s((?: \w+ [\d.]+)*)$" % lab, text, re.M)
        if m3:
            t = m3.group(1).split()
            c[key] = {t[i]: float(t[i + 1]) for i in range(0, len(t), 2)}
    c["sites"] = [{"name": s.group(1), "vars": int(s.group(2)), "aux": int(s.group(3)),
                   "box_log2": float(s.group(4)), "class": s.group(5),
                   "instances": int(s.group(6)), "keys": int(s.group(7)),
                   "constructs": [] if s.group(8) == "-" else s.group(8).split(",")}
                  for s in _SITE.finditer(text)]
    return c

_QUALITY = (r"^%s\s+quality (\d+) models: (\d+) checked, (\d+) invalid, (\d+) unchecked; "
            r"(\d+) fields: (\d+) forced, (\d+) stuck, (\d+) undecided; "
            r"min distinct ([\d.]+|-), min entropy ([\d.]+|-)$")
_FIELD = re.compile(r"^\s+field (\S+) n (\d+) distinct (\d+) ratio ([\d.]+) entropy ([\d.]+)"
                    r"(?: (forced|stuck|undecided))?$")


def replay_quality(text: str, pass_name: str) -> Optional[dict]:
    """bench_vlt_replay --quality for one pass: the replayed models checked
    against the constraints, and per field (solver variable) its distinct ratio
    and normalized entropy, as metric.field_metrics computes them. min_* are
    over the fields that are not forced (one value is all the constraints allow)."""
    m = re.search(_QUALITY % re.escape(pass_name), text, re.M)
    if not m:
        return None
    g = m.groups()
    q = dict(zip(("models", "checked", "invalid", "unchecked", "n_fields", "forced", "stuck",
                  "undecided"), map(int, g[:8])))
    q["min_distinct"] = None if g[8] == "-" else float(g[8])
    q["min_entropy"] = None if g[9] == "-" else float(g[9])
    fields = []
    for ln in text[m.end():].splitlines()[1:]:
        f = _FIELD.match(ln)
        if not f:
            break
        fields.append({"name": f.group(1), "n": int(f.group(2)), "distinct": int(f.group(3)),
                       "ratio": float(f.group(4)), "entropy": float(f.group(5)),
                       "status": f.group(6) or "varies"})
    q["fields"] = fields
    return q
