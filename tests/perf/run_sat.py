"""SAT timing: every fixture × arm, CPU time from wait4 (design §2.3, §5).

Per fixture, the arms run in rotating order once per rep, so a burst of host
load lands on every arm of the comparison rather than on one. Reps: `reps`
(min-of-N), cut to `reps_slow` for an arm whose first run took over
`slow_ms`, and stopped for an arm that timed out. Fixtures are spread over
worker processes, each pinned to its own physical core.

Correctness: an arm of the head build whose definite answer (sat/unsat)
contradicts a definite answer of a reference solver makes the run invalid.
A disagreement among the references, or of the anchor (an old release,
which may predate a soundness fix), is recorded but does not invalidate.
"""
from __future__ import annotations

import os
import resource
import subprocess
import tempfile
import threading
import time
from concurrent.futures import ProcessPoolExecutor
from dataclasses import dataclass, field
from pathlib import Path

_REPO = Path(__file__).resolve().parents[2]
STACK_BYTES = 64 << 20      # v0.1.0's CLI keeps a ~10 MB struct on the stack
VERDICTS = ("sat", "unsat", "unknown")
REFERENCES = ("z3", "bitwuzla", "boolector")
FLOOR_PROBLEM = "(set-logic QF_BV)(declare-const x (_ BitVec 8))(assert (= x #x01))(check-sat)\n"


@dataclass
class Arm:
    name: str
    build: str                       # head | anchor | <tag> | ref
    argv: list                       # fixture path is appended
    env: dict = field(default_factory=dict)
    logics: tuple = ()               # empty: every logic

    def applies(self, logic) -> bool:
        return not self.logics or logic in self.logics


def make_arms(head: Path, anchor: Path, tools: dict) -> list:
    """tools: {"z3": path, "bitwuzla": path, "boolector": path}."""
    arms = [Arm("dv-smt2", "head", [str(head)]),
            Arm("dv-smt2-bb", "head", [str(head)], {"DV_ENGINE": "bitblast"})]
    if anchor:
        arms.append(Arm("dv-smt2", "anchor", [str(anchor)]))
    arms += [Arm("z3", "ref", [str(tools["z3"]), "-smt2"]),
             Arm("bitwuzla", "ref", [str(tools["bitwuzla"])]),
             # boolector cannot parse the yosys-smtbmc declare-sort dialect
             Arm("boolector", "ref", [str(tools["boolector"]), "--smt2"], logics=("QF_BV",))]
    return arms


def _limits():
    resource.setrlimit(resource.RLIMIT_STACK, (STACK_BYTES, STACK_BYTES))


def run_once(argv: list, env: dict, budget_s: float) -> tuple:
    """(verdict, cpu_ms, wall_ms, rss_kb). verdict: sat/unsat/unknown/timeout/error."""
    with tempfile.TemporaryFile() as out:
        t0 = time.perf_counter()
        p = subprocess.Popen(argv, stdout=out, stderr=subprocess.DEVNULL,
                             env={**os.environ, **env}, preexec_fn=_limits)
        timer = threading.Timer(budget_s, p.kill)
        timer.start()
        _, status, ru = os.wait4(p.pid, 0)
        timer.cancel()
        p.returncode = 0                       # reaped here
        wall = (time.perf_counter() - t0) * 1000
        cpu = (ru.ru_utime + ru.ru_stime) * 1000
        if os.WIFSIGNALED(status) and os.WTERMSIG(status) == 9 and wall >= budget_s * 1000 * 0.99:
            return "timeout", cpu, wall, ru.ru_maxrss
        out.seek(0)
        # The first line that IS an answer: z3 reports an option it does not
        # know (tier1's `:seed`) and carries on; bitwuzla prints warnings
        # before its answer. No answer line at all is an error.
        verdict = next((l.strip() for l in out.read().decode(errors="replace").splitlines()
                        if l.strip() in VERDICTS), "error")
        return verdict, cpu, wall, ru.ru_maxrss


def _arm_key(a: Arm) -> str:
    return f"{a.name}@{a.build}"


def run_fixture(fx: dict, arms: list, spec: dict) -> list:
    """All reps of all applicable arms on one fixture → result rows."""
    path = str(_REPO / fx["path"])
    todo = [a for a in arms if a.applies(fx["logic"])]
    res = {_arm_key(a): {"cpu": [], "wall": [], "rss": 0, "verdict": None} for a in todo}
    want = {_arm_key(a): spec["reps"] for a in todo}
    for rep in range(spec["reps"]):
        order = todo[rep % len(todo):] + todo[:rep % len(todo)]
        for a in order:
            k = _arm_key(a)
            r = res[k]
            if len(r["cpu"]) >= want[k] or r["verdict"] in ("timeout", "error"):
                continue
            v, cpu, wall, rss = run_once(a.argv + [path], a.env, spec["budget_s"])
            if r["verdict"] in (None, v) or v in ("timeout", "error"):
                r["verdict"] = v
            else:                              # answers differ between reps: keep the evidence
                r["verdict"] = f"{r['verdict']}|{v}"
            if v in ("timeout", "error"):
                continue
            r["cpu"].append(cpu)
            r["wall"].append(wall)
            r["rss"] = max(r["rss"], rss)
            if len(r["cpu"]) == 1 and cpu > spec["slow_ms"]:
                want[k] = spec["reps_slow"]
    rows = []
    for a in todo:
        r = res[_arm_key(a)]
        cpus = sorted(r["cpu"])
        rows.append({"suite": spec["suite"], "fixture": fx["path"], "sha": fx["sha"],
                     "cat": fx["cat"], "arm": a.name, "build": a.build,
                     "verdict": r["verdict"],
                     "cpu_ms_min": round(cpus[0], 2) if cpus else None,
                     "cpu_ms_med": round(cpus[len(cpus) // 2], 2) if cpus else None,
                     "wall_ms_min": round(min(r["wall"]), 2) if r["wall"] else None,
                     "reps": len(cpus), "rss_kb": r["rss"]})
    return rows


def physical_cores() -> list:
    """One logical CPU per physical core, among those this process may use."""
    seen, cores = set(), []
    for cpu in sorted(os.sched_getaffinity(0)):
        sib = Path(f"/sys/devices/system/cpu/cpu{cpu}/topology/thread_siblings_list")
        key = sib.read_text().strip() if sib.exists() else str(cpu)
        if key not in seen:
            seen.add(key)
            cores.append(cpu)
    return cores


def _pin_worker(queue):
    cpu = queue.get()
    os.sched_setaffinity(0, {cpu})


def floor(arms: list, budget_s: float = 10) -> dict:
    """Start-up cost per arm: CPU ms on a one-variable problem, min of 5."""
    with tempfile.NamedTemporaryFile("w", suffix=".smt2", delete=False) as f:
        f.write(FLOOR_PROBLEM)
    try:
        out = {}
        for a in arms:
            times = [run_once(a.argv + [f.name], a.env, budget_s) for _ in range(5)]
            ok = [t[1] for t in times if t[0] == "sat"]
            out[_arm_key(a)] = round(min(ok), 2) if ok else None
        return out
    finally:
        os.unlink(f.name)


def run_suite(spec: dict, arms: list, workers: int = 0) -> dict:
    """→ {"rows": [...], "floor": {...}, "disagree": [...], "invalid": reason|None}."""
    cores = physical_cores()
    workers = workers or max(1, len(cores) // 4)
    import multiprocessing as mp
    ctx = mp.get_context("fork")
    q = ctx.Queue()
    for c in cores[-workers:]:            # stay off core 0, where the host does most else
        q.put(c)
    rows = []
    with ProcessPoolExecutor(workers, mp_context=ctx, initializer=_pin_worker,
                             initargs=(q,)) as ex:
        for fx_rows in ex.map(run_fixture, spec["fixtures"], [arms] * len(spec["fixtures"]),
                              [spec] * len(spec["fixtures"])):
            rows += fx_rows
    disagree, invalid = check_answers(rows)
    return {"rows": rows, "floor": floor(arms), "disagree": disagree, "invalid": invalid}


def check_answers(rows: list) -> tuple:
    by_fx = {}
    for r in rows:
        by_fx.setdefault(r["fixture"], []).append(r)
    disagree, bad_head = [], []
    for fx, rs in sorted(by_fx.items()):
        ref = {r["arm"]: r["verdict"] for r in rs if r["build"] == "ref" and r["verdict"] in ("sat", "unsat")}
        ref_answers = set(ref.values())
        mine = {f"{r['arm']}@{r['build']}": r["verdict"] for r in rs if r["build"] != "ref"}
        flaky = {k: v for k, v in mine.items() if "|" in (v or "")}
        if len(ref_answers) > 1:
            disagree.append({"fixture": fx, "kind": "references", "answers": {**ref, **mine}})
        for k, v in mine.items():
            if v in ("sat", "unsat") and ref_answers and v not in ref_answers:
                disagree.append({"fixture": fx, "kind": k, "answers": {**ref, **mine}})
                if k.endswith("@head"):
                    bad_head.append(f"{k} answered {v} on {fx}, references {sorted(ref_answers)}")
        for k, v in flaky.items():
            disagree.append({"fixture": fx, "kind": f"{k} (differs between reps)", "answers": {k: v}})
            if k.endswith("@head") and "sat" in v.split("|") and "unsat" in v.split("|"):
                bad_head.append(f"{k} answered both sat and unsat on {fx}")
    return disagree, ("; ".join(bad_head[:5]) + (f" (+{len(bad_head) - 5} more)" if len(bad_head) > 5 else "")
                      if bad_head else None)
