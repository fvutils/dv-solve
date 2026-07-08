#!/usr/bin/env python3
"""Sanitize a raw Verilator SMT solver transcript into standalone cross-check
fixtures.

Verilator drives the solver interactively: it declares the rand variables and
constraint asserts, issues (check-sat) + (get-value), then runs a bit-level
soft/unsat-assumptions "diversity" dance (declare aN Bool; check-sat-assuming;
get-unsat-assumptions). For the differential corpus we want each *self-contained
(check-sat) query*: the set-logic + define-fun helpers + declare-funs + asserts
in effect at that point, terminated by a single (check-sat). Both dv-solve and
z3 return a definitive sat/unsat on such a fixture, so test_cross_check.py can
diff them.

We emit the FIRST such query (the pure constraint problem, before the
diversity assumptions perturb it). Usage:

    sanitize_transcript.py RAW.smt2 OUT.smt2 [--name NAME]

Exits non-zero if no self-contained (check-sat) query could be extracted.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path


# Interactive/stateful commands that must NOT appear in a standalone fixture.
_DROP_PREFIXES = (
    "(get-value",
    "(get-unsat-assumptions",
    "(get-model",
    "(get-assignment",
    "(echo",
    "(set-option :produce-unsat-assumptions",
    "(set-option :produce-models",
)


def _iter_stmts(text: str):
    """Yield top-level parenthesised SMT statements in order.

    Verilator's transcript interleaves the SMT commands it *sends* with log
    annotations: `# ...` comments (open/timestamp), `< ...` solver replies, and
    `> ...` echoes. Those are not SMT and are dropped here. Statements are one
    per line in practice; tolerate multi-line by balancing parentheses.
    """
    depth = 0
    buf = []
    for line in text.splitlines():
        s = line.rstrip("\n")
        stripped = s.strip()
        if not buf:
            if not stripped:
                continue
            # transcript annotation, not an SMT command
            if stripped[0] in "#<>":
                continue
        buf.append(s)
        depth += s.count("(") - s.count(")")
        if depth <= 0:
            stmt = "\n".join(buf).strip()
            if stmt:
                yield stmt
            buf = []
            depth = 0
    if buf:
        yield "\n".join(buf).strip()


def sanitize(raw: str):
    """Return the text of the first standalone (check-sat) query, or None."""
    stmts = list(_iter_stmts(raw))
    # Skip a leading probe block: some transcripts open with
    # (set-logic ...) (check-sat) (reset) to test solver liveness. Restart
    # accumulation at each (reset) so we capture the real post-reset session.
    out = []
    saw_constraint = False
    for st in stmts:
        if st.startswith("(reset"):
            out = []
            saw_constraint = False
            continue
        if any(st.startswith(p) for p in _DROP_PREFIXES):
            continue
        # The first (push / (check-sat / (check-sat-assuming marks the end of the
        # base constraint problem: everything before it is the pure set of
        # declarations + asserts. Emit exactly that, terminated by one
        # (check-sat), so the fixture is the constraint problem itself — not the
        # push-scoped diversity assumptions Verilator layers on afterward.
        if st.startswith("(push") or st.startswith("(check-sat"):
            if saw_constraint:
                out.append("(check-sat)")
                return "\n".join(out) + "\n"
            # probe check-sat / push with no constraints yet — skip
            continue
        if st.startswith("(pop"):
            continue
        # Keep declarations / definitions / asserts.
        out.append(st)
        if st.startswith("(assert") or st.startswith("(declare"):
            saw_constraint = True
    return None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw")
    ap.add_argument("out")
    ap.add_argument("--name", default=None)
    args = ap.parse_args()

    raw = Path(args.raw).read_text()
    frag = sanitize(raw)
    if frag is None:
        print(f"sanitize: no self-contained (check-sat) query in {args.raw}",
              file=sys.stderr)
        return 1
    header = f"; Sanitized from Verilator transcript: {args.name or Path(args.raw).name}\n"
    Path(args.out).write_text(header + frag)
    n_decl = sum(l.startswith("(declare-fun") for l in frag.splitlines())
    n_assert = sum(l.startswith("(assert") for l in frag.splitlines())
    print(f"wrote {args.out}: {n_decl} declare-fun, {n_assert} assert")
    return 0


if __name__ == "__main__":
    sys.exit(main())
