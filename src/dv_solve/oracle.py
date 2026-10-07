"""The oracle check: dv-solve's answers checked by a reference SMT solver.

Two ways in:

* :class:`Oracle` -- the C API's check (``dvs_oracle_create``). Attach it to
  one :class:`~dv_solve.ctx.SolveCtx` (``SolveCtx(problem, oracle=o)``) or to
  every context created afterwards (:func:`set_default_oracle`). Each
  ``solve()`` is checked: a solution by asking z3 whether the constraints hold
  with every variable fixed to dv-solve's value, an UNSAT by letting z3 solve
  the problem. See docs/api_oracle_plan.md.
* ``DV_ORACLE`` -- the same check for dv-solve-smt2 (docs/oracle_check_plan.md).

Both record into a run directory, which this module also reports on:

    python -m dv_solve.oracle report <root>... [--out DIR] [--allow-truncated]
    python -m dv_solve.oracle extract <run-dir> <q> [-o FILE]

``report`` finds every run directory (a ``run.json`` of kind
``dv-solve-oracle-run``) under the roots, totals the result classes, lists runs
that did not finish, and groups the non-ok queries by signature. It writes
``oracle-report.md`` and ``oracle-report.json`` (into ``--out``, default the
first root) and exits 1 if any query is ``bad-model`` or ``bad-unsat``, or any
run is truncated or aborted (unless ``--allow-truncated``).

``extract`` cuts query <q> out of a run's ``transcript.smt2``
(``DV_ORACLE_KEEP=all``) as a stand-alone script, passing or not. A failing
query's script is already in the run's ``fail/`` directory.

See docs/oracle_check_plan.md.
"""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import sys
import weakref
from collections import Counter, defaultdict
from pathlib import Path

CLASSES = ["ok", "bad-model", "bad-unsat", "gap", "unchecked", "oracle-error", "skipped"]
P0 = ("bad-model", "bad-unsat")


# ----------------------------------------------------------------------- #
# The API oracle                                                           #
# ----------------------------------------------------------------------- #

class OracleMismatch(AssertionError):
    """dv-solve and the reference solver disagree: dv-solve's values violate
    the constraints (bad-model), or it called a satisfiable problem UNSAT
    (bad-unsat). The repro is in the run directory's ``fail/``."""


class _OracleOpts(ctypes.Structure):
    _fields_ = [
        ("solver", ctypes.c_char_p),
        ("bin", ctypes.c_char_p),
        ("command", ctypes.c_char_p),
        ("out_dir", ctypes.c_char_p),
        ("tag", ctypes.c_char_p),
        ("timeout_s", ctypes.c_double),
        ("keep_all", ctypes.c_int),
    ]


class _OracleStats(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint64) for n in
                ("queries", "ok", "bad_model", "bad_unsat", "unchecked", "error", "skipped")] + [
        ("dvs_ms", ctypes.c_double),
        ("oracle_ms", ctypes.c_double),
    ]


def _b(s):
    return None if s is None else os.fsencode(s)


class Oracle:
    """A reference solver process checking dv-solve's answers.

    Args:
        solver: ``"z3"`` (default) or ``"bitwuzla"``.
        bin: The solver's executable (default: found on ``PATH``).
        command: Instead of ``solver``/``bin``, a whole command line of a
            solver that reads SMT-LIB2 on stdin.
        out_dir: The run directory; ``%t`` expands to the UTC time, ``%p`` to
            the pid (default ``dvs-oracle/%t-%p``).
        tag: Free text recorded in ``run.json``.
        timeout_s: Per-query limit (default 10 s); a query the reference
            solver cannot decide in time is recorded ``unchecked``.
        keep_all: Also keep ``transcript.smt2``, every script sent.
        raise_on_fail: Raise :class:`OracleMismatch` from the ``solve()`` (or
            constructor, for a compile-time UNSAT) whose answer was wrong.

    Raises:
        RuntimeError: the reference solver could not be started, or the
            library has no oracle (too old, or Windows).
    """

    RESULTS = {-1: "none", 0: "ok", 1: "bad-model", 2: "bad-unsat", 3: "gap",
               4: "unchecked", 5: "oracle-error", 6: "skipped"}

    def __init__(self, solver="z3", bin=None, command=None, out_dir=None,
                 tag=None, timeout_s=0.0, keep_all=False, raise_on_fail=False):
        from .lib import _load_lib, _library_not_found_error
        lib = _load_lib()
        if lib is None:
            raise _library_not_found_error()
        if not hasattr(lib, "dvs_oracle_create"):
            raise RuntimeError("this dv-solve library has no oracle check "
                               "(dvs_oracle_create); rebuild it")
        self._lib = lib
        opts = _OracleOpts(solver=_b(solver), bin=_b(bin), command=_b(command),
                           out_dir=_b(out_dir), tag=_b(tag),
                           timeout_s=float(timeout_s), keep_all=1 if keep_all else 0)
        self._o = lib.dvs_oracle_create(ctypes.byref(opts), None)
        if not self._o:
            raise RuntimeError("could not start the oracle check (see stderr)")
        self.raise_on_fail = raise_on_fail
        self._ctxs = weakref.WeakSet()
        self._run_dir = os.fsdecode(lib.dvs_oracle_run_dir(self._o))

    # -- attaching -------------------------------------------------------- #

    def _attach(self, ctx, raw_ctx, label=None):
        if self._o is None:
            raise RuntimeError("the oracle is closed")
        if self._lib.dvs_solver_set_oracle(raw_ctx, self._o, _b(label)) != 0:
            raise MemoryError("dvs_solver_set_oracle")
        self._ctxs.add(ctx)

    def _detach(self, ctx, raw_ctx):
        self._lib.dvs_solver_set_oracle(raw_ctx, None, None)
        self._ctxs.discard(ctx)

    def _after(self):
        """Raise for a wrong answer, when asked to."""
        if not self.raise_on_fail:
            return
        res = self.last_result
        if res in P0:
            raise OracleMismatch(
                f"dv-solve oracle: {res} -- see {self._run_dir}/queries.jsonl "
                f"and {self._run_dir}/fail/")

    # -- results ---------------------------------------------------------- #

    @property
    def run_dir(self) -> str:
        return self._run_dir

    @property
    def last_result(self) -> str:
        """The class of the most recent query: ok, bad-model, bad-unsat,
        unchecked, oracle-error, skipped, or none."""
        if self._o is None:
            return "none"
        return self.RESULTS.get(self._lib.dvs_oracle_last_result(self._o), "?")

    def stats(self) -> dict:
        """Totals so far."""
        st = _OracleStats()
        if self._o is not None:
            self._lib.dvs_oracle_get_stats(self._o, ctypes.byref(st))
        return {n: getattr(st, n) for n, _ in _OracleStats._fields_}

    def close(self) -> None:
        """Detach every context, finalise run.json and stop the reference
        solver. Also done at garbage collection."""
        if self._o is None:
            return
        for ctx in list(self._ctxs):
            ctx._detach_oracle()
        self._lib.dvs_oracle_destroy(self._o)
        self._o = None
        global _DEFAULT
        if _DEFAULT is self:
            _DEFAULT = None

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass


def problem_smt2(problem) -> str:
    """The problem as a stand-alone SMT-LIB2 script -- the oracle's own
    translation (``dvs_problem_write_smt2``). POSIX only."""
    import tempfile
    from .lib import _load_lib
    lib = _load_lib()
    libc = ctypes.CDLL(None)
    libc.fopen.restype = ctypes.c_void_p
    libc.fopen.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    libc.fclose.argtypes = [ctypes.c_void_p]
    lib.dvs_problem_write_smt2.restype = ctypes.c_int
    lib.dvs_problem_write_smt2.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    sp = getattr(problem, "_sp", None)
    if sp is None:
        sp = ctypes.cast(problem, ctypes.c_void_p).value
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "p.smt2")
        f = libc.fopen(os.fsencode(path), b"w")
        lib.dvs_problem_write_smt2(sp, f)
        libc.fclose(f)
        with open(path) as fh:
            return fh.read()


_DEFAULT = None


def set_default_oracle(oracle):
    """Attach ``oracle`` to every :class:`~dv_solve.ctx.SolveCtx` created from
    now on (``None`` stops). Returns the previous default."""
    global _DEFAULT
    prev = _DEFAULT
    _DEFAULT = oracle
    return prev


def get_default_oracle():
    return _DEFAULT


def find_runs(roots):
    for root in roots:
        for dirpath, dirnames, filenames in os.walk(root):
            if "run.json" in filenames:
                p = Path(dirpath)
                try:
                    d = json.loads((p / "run.json").read_text())
                except (OSError, ValueError):
                    continue
                if d.get("kind") == "dv-solve-oracle-run":
                    yield p, d
                dirnames[:] = []    # a run directory holds no other runs


def _pid_alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except (OSError, TypeError):
        return False


def report(roots, out_dir, allow_truncated=False):
    totals = Counter()
    runs = []
    groups = defaultdict(list)       # signature -> [(run, record)]
    for run_dir, rj in find_runs(roots):
        status = rj.get("status")
        counts = Counter()
        n = 0
        qfile = run_dir / "queries.jsonl"
        if qfile.exists():
            with qfile.open() as f:
                for line in f:
                    try:
                        r = json.loads(line)
                    except ValueError:
                        continue      # a line cut short by a kill
                    n += 1
                    counts[r["res"]] += 1
                    if r["res"] not in ("ok", "skipped"):
                        sig = (r["res"], r["cmd"], r["sem"], r["dvs"]["verdict"],
                               r["dvs"]["engine"])
                        groups[sig].append((run_dir, r))
        elif "summary" in rj:     # DV_ORACLE_KEEP=summary: counts only
            summ = rj["summary"]
            n = summ.get("queries", 0)
            counts.update({c: summ[c] for c in CLASSES if summ.get(c)})
        if status == "running" and not _pid_alive(rj.get("pid")):
            status = "truncated"
        totals.update(counts)
        runs.append({"dir": str(run_dir), "tag": rj.get("tag"), "status": status,
                     "queries": n, "counts": dict(counts),
                     "oracle": rj.get("oracle", {}), "dv_solve": rj.get("dv_solve", {}),
                     "dvs_ms": rj.get("summary", {}).get("dvs_ms"),
                     "oracle_ms": rj.get("summary", {}).get("oracle_ms")})

    unfinished = [r for r in runs if r["status"] in ("truncated", "aborted", "failed")]
    p0 = sum(totals[c] for c in P0)
    out_dir.mkdir(parents=True, exist_ok=True)

    js = {"runs": runs, "totals": dict(totals),
          "groups": [{"res": k[0], "cmd": k[1], "sem": k[2], "dvs_verdict": k[3],
                      "engine": k[4], "count": len(v),
                      "examples": [{"run": str(rd), "q": r["q"], "file": r.get("file")}
                                   for rd, r in v[:10]]}
                     for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1]))]}
    (out_dir / "oracle-report.json").write_text(json.dumps(js, indent=1) + "\n")

    L = ["# dv-solve oracle report", ""]
    L.append(f"{len(runs)} runs, {sum(r['queries'] for r in runs)} queries. "
             f"**{p0} P0** (bad-model + bad-unsat); {len(unfinished)} runs unfinished.")
    L += ["", "| class | queries |", "|---|---:|"]
    for c in CLASSES:
        L.append(f"| {c} | {totals.get(c, 0)} |")
    L += ["", "## Runs", "",
          "| run | status | queries | ok | bad-model | bad-unsat | gap | unchecked | oracle-error | dvs s | oracle s |",
          "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for r in sorted(runs, key=lambda r: r["tag"] or r["dir"]):
        c = r["counts"]
        L.append(f"| {r['tag'] or r['dir']} | {r['status']} | {r['queries']} | "
                 + " | ".join(str(c.get(k, 0)) for k in CLASSES[:6])
                 + f" | {(r['dvs_ms'] or 0) / 1000:.1f} | {(r['oracle_ms'] or 0) / 1000:.1f} |")
    if groups:
        L += ["", "## Non-ok queries by signature", "",
              "| result | command | semantics | dvs | engine | count | first example |",
              "|---|---|---|---|---|---:|---|"]
        for k, v in sorted(groups.items(), key=lambda kv: -len(kv[1])):
            rd, r = v[0]
            ex = f"{rd}/{r['file']}.smt2" if r.get("file") else f"{rd} q{r['q']}"
            L.append(f"| {k[0]} | {k[1]} | {k[2]} | {k[3]} | {k[4]} | {len(v)} | `{ex}` |")
    (out_dir / "oracle-report.md").write_text("\n".join(L) + "\n")
    print("\n".join(L[:14 + len(CLASSES)]))
    print(f"\nwrote {out_dir / 'oracle-report.md'}")
    if p0 or (unfinished and not allow_truncated):
        return 1
    return 0


def extract(run_dir, q, out):
    t = Path(run_dir) / "transcript.smt2"
    if not t.exists():
        sys.exit(f"{t}: no transcript (the run needs DV_ORACLE_KEEP=all)")
    start, end = f"; @q {q} dvs=", f"; @q {q} orc="
    lines, on = [], False
    with t.open() as f:
        for line in f:
            if line.startswith(start):
                on = True
                lines.append(line)
                continue
            if on and line.startswith(end):
                lines.append(line)
                break
            if on and not (line.strip() == "(reset)" and len(lines) == 1):
                lines.append(line)
    if not lines:
        sys.exit(f"query {q} not in {t}")
    text = "".join(lines)
    if out:
        Path(out).write_text(text)
    else:
        sys.stdout.write(text)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="python -m dv_solve.oracle")
    sub = ap.add_subparsers(dest="cmd", required=True)
    r = sub.add_parser("report")
    r.add_argument("roots", nargs="+")
    r.add_argument("--out", default=None)
    r.add_argument("--allow-truncated", action="store_true")
    e = sub.add_parser("extract")
    e.add_argument("run_dir")
    e.add_argument("q", type=int)
    e.add_argument("-o", "--out", default=None)
    a = ap.parse_args(argv)
    if a.cmd == "report":
        return report(a.roots, Path(a.out or a.roots[0]), a.allow_truncated)
    extract(a.run_dir, a.q, a.out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
