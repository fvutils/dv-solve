"""Per-bench corpus: the problems simbench benches send, as Verilator sends them.

A corpus file holds the distinct randomize() problems of one bench, taken from
a captured simbench session (`session.cmds.smt2`) by `extract`. One problem is
the text of one call from (set-logic ...) up to its first (check-sat), without
the (reset) and seed lines, followed by the get-value variable list:

    ; problem <bench>#<k> calls=<how many captured calls sent it>
    ; get-value r v
    (set-logic QF_ABV)
    ...
    ; end

A call's problem is its assert stack at the first sat check-sat: Verilator's
soft-constraint retries before it are resolved, and its hash protocol after it
(random parity asserts, which the simulator owns) is left out.

    python3 tests/uniformity/corpus.py extract <session.cmds.smt2> <bench> [--out DIR]
"""
from __future__ import annotations

import argparse
import re
import sys
from collections import Counter
from pathlib import Path

from uniformity import Problem

_GV = re.compile(r"^\(get-value \((.*)\)\)\s*$")



def split_terms(s: str) -> list[str]:
    """Top-level SMT-LIB terms of s: symbols, or parenthesised terms such as
    `(select a #x00000000)` (whitespace inside a term collapsed to one space)."""
    out, depth, cur = [], 0, ""
    for ch in s:
        if ch == "(":
            if depth == 0 and cur.strip():
                out.append(cur.strip())
                cur = ""
            depth += 1
        elif ch == ")":
            depth -= 1
        if depth == 0 and (ch.isspace() or ch == ")"):
            if ch == ")":
                cur += ch
            if cur.strip():
                out.append(" ".join(cur.replace("(", "( ").replace(")", " )").split())
                           .replace("( ", "(").replace(" )", ")"))
            cur = ""
            continue
        cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out

def _is_sat(body: list[str]) -> bool:
    import z3
    sv = z3.Solver()
    sv.set("timeout", 20000)
    try:
        sv.add(z3.parse_smt2_string("\n".join(l for l in body
                                              if not l.startswith("(set-logic"))))
    except z3.Z3Exception:
        return True     # z3 cannot parse it: keep the snapshot, the harness reports it
    return sv.check() != z3.unsat


def calls(text: str) -> list[tuple[list[str], list[str]]]:
    """(problem, get-value names) per randomize() call in a captured session.

    The asserts live on a push/pop stack: Verilator tries soft constraints
    (push, assert, check-sat, pop when unsat) and, after a sat answer, adds its
    parity hashes the same way. The problem is the stack at the first check-sat
    that is sat: the hard constraints plus the soft ones kept, before any hash.
    It is written flat, without push/pop."""
    out = []
    for block in re.split(r"^\(reset\)\s*$", text, flags=re.M):
        decls: list[str] = []
        stack: list[list[str]] = [[]]
        snaps: list[list[str]] = []
        names = None
        for l in block.splitlines():
            t = l.strip()
            if not t or t.startswith("(set-option"):
                continue
            m = _GV.match(t)
            if m:
                names = split_terms(m.group(1))
            elif t.startswith("(push"):
                stack += [[] for _ in range(int(t[5:-1].strip() or 1))]
            elif t.startswith("(pop"):
                del stack[len(stack) - int(t[4:-1].strip() or 1):]
            elif t.startswith("(assert"):
                stack[-1].append(t)
            elif t.startswith("(check-sat"):
                snaps.append(decls + [a for lvl in stack for a in lvl])
            elif t.startswith(("(get-", "(exit", "(echo")):
                continue
            else:
                decls.append(t)        # set-logic, declare-fun, define-fun
        if not names or not snaps:
            continue
        for body in snaps:
            if any(x.startswith("(assert") for x in body) and _is_sat(body):
                out.append((body, names))
                break
    return out


def extract(session: Path, bench: str, out_dir: Path, keep: int = 4) -> Path:
    """Write the `keep` most frequent distinct problems of a session."""
    cs = calls(session.read_text())
    freq = Counter((tuple(b), tuple(n)) for b, n in cs)
    out_dir.mkdir(parents=True, exist_ok=True)
    f = out_dir / f"{bench}.smt2"
    lines = [f"; corpus for {bench} from {len(cs)} captured calls, "
             f"{len(freq)} distinct problems"]
    for k, ((body, names), c) in enumerate(freq.most_common(keep)):
        lines += [f"; problem {bench}#{k} calls={c}", "; get-value " + " ".join(names),
                  *body, "; end"]
    f.write_text("\n".join(lines) + "\n")
    return f


def load(path: Path) -> list[Problem]:
    files = sorted(path.glob("*.smt2")) if path.is_dir() else [path]
    out = []
    for f in files:
        name, names, body = None, None, []
        for l in f.read_text().splitlines():
            if l.startswith("; problem "):
                name, body = l.split()[2], []
                note = l
            elif l.startswith("; get-value "):
                names = split_terms(l[len("; get-value "):])
            elif l.startswith("; end"):
                out.append(Problem(name=name, body=body, names=names, note=note,
                                   source=f"corpus:{f.name}"))
                name = None
            elif name and not l.startswith(";"):
                body.append(l)
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("extract")
    e.add_argument("session")
    e.add_argument("bench", help="corpus name, e.g. arith.countones")
    e.add_argument("--out", default=str(Path(__file__).resolve().parent / "corpus"))
    e.add_argument("--keep", type=int, default=4)
    a = ap.parse_args(argv)
    f = extract(Path(a.session), a.bench, Path(a.out), a.keep)
    print(f)
    return 0


if __name__ == "__main__":
    sys.exit(main())
