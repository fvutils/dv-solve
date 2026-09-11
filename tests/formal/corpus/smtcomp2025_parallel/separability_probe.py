#!/usr/bin/env python3
"""Static separability probe for QF_BV .smt2 instances.

Goal: for the hard-but-solvable timeout instances, measure where divide-and-
conquer parallelism is available *before* building a parallel solver.

Two axes:

  1. Independent components. Build a graph whose nodes are declared symbols;
     connect symbols that co-occur in the same top-level constraint (each
     conjunct of a top-level ``(assert (and ...))`` counts as its own
     constraint, so a single monolithic assert still decomposes). Connected
     components = sub-problems solvable fully in parallel (AND of results).
     Many small components => embarrassingly parallel. One giant component =>
     naive decomposition won't help; you need search-space partitioning.

  2. Case-split surface. Count top-level ``(assert (or ...))`` forms and the
     symbols they branch on -- these are the natural cube-and-conquer seeds
     (the parallel-track winner STP-Parti partitions the search this way).

Usage:
    python separability_probe.py smt2/QF_BV/**/68.smt2 [...]
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

_TOKEN = re.compile(r"\(|\)|\|[^|]*\||;[^\n]*|[^\s()]+")


def tokenize(text: str):
    for m in _TOKEN.finditer(text):
        t = m.group(0)
        if t.startswith(";"):
            continue
        yield t


class UF:
    def __init__(self):
        self.p = {}

    def find(self, x):
        self.p.setdefault(x, x)
        r = x
        while self.p[r] != r:
            r = self.p[r]
        while self.p[x] != r:
            self.p[x], x = r, self.p[x]
        return r

    def union(self, a, b):
        ra, rb = self.find(a), self.find(b)
        if ra != rb:
            self.p[ra] = rb


def parse(text: str):
    """Return (declared_symbols:set, top_assert_forms:list[list[token]])."""
    toks = list(tokenize(text))
    declared = set()
    asserts = []
    i, n = 0, len(toks)
    while i < n:
        if toks[i] == "(" and i + 1 < n and toks[i + 1] in ("declare-fun", "declare-const"):
            # (declare-fun NAME (...) SORT)
            declared.add(toks[i + 2])
            depth = 0
            while i < n:
                if toks[i] == "(":
                    depth += 1
                elif toks[i] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            i += 1
            continue
        if toks[i] == "(" and i + 1 < n and toks[i + 1] == "assert":
            depth = 0
            form = []
            while i < n:
                form.append(toks[i])
                if toks[i] == "(":
                    depth += 1
                elif toks[i] == ")":
                    depth -= 1
                    if depth == 0:
                        break
                i += 1
            asserts.append(form)
            i += 1
            continue
        i += 1
    return declared, asserts


def split_conjuncts(form):
    """Yield the immediate top-level conjuncts of (assert (and c1 c2 ...)).

    If the assert body isn't a top-level `and`, yield the whole body once.
    `form` is the token list for one `(assert ...)` including outer parens.
    """
    # form = ( assert <body> )  -> body starts at index 2
    body = form[2:-1]
    if len(body) >= 2 and body[0] == "(" and body[1] == "and":
        inner = body[2:-1]  # tokens between (and ... )
        # split inner into balanced top-level sub-forms
        depth = 0
        cur = []
        for t in inner:
            cur.append(t)
            if t == "(":
                depth += 1
            elif t == ")":
                depth -= 1
                if depth == 0:
                    yield cur
                    cur = []
        if any(x not in ("(", ")") for x in cur):
            yield cur
    else:
        yield body


def analyze(path: Path, declared, syms_of):
    uf = UF()
    for s in declared:
        uf.find(s)
    for edge_syms in syms_of:
        edge_syms = [s for s in edge_syms if s in declared]
        for s in edge_syms[1:]:
            uf.union(edge_syms[0], s)
    comp = {}
    for s in declared:
        comp.setdefault(uf.find(s), []).append(s)
    sizes = sorted((len(v) for v in comp.values()), reverse=True)
    return sizes


def main() -> int:
    paths = [Path(p) for p in sys.argv[1:]]
    if not paths:
        print(__doc__)
        return 2
    print(f"{'benchmark':40s} {'vars':>6s} {'constrs':>8s} {'comps':>6s} "
          f"{'largest%':>8s} {'or-forms':>8s}")
    for p in paths:
        text = p.read_text(errors="replace")
        declared, asserts = parse(text)
        # constraints = union of conjuncts across all top-level asserts
        constr_symbol_sets = []
        or_forms = 0
        for form in asserts:
            body = form[2:-1]
            if len(body) >= 2 and body[0] == "(" and body[1] == "or":
                or_forms += 1
            for conj in split_conjuncts(form):
                syms = {t for t in conj if t in declared}
                if syms:
                    constr_symbol_sets.append(list(syms))
        sizes = analyze(p, declared, constr_symbol_sets)
        nv = len(declared)
        largest = (sizes[0] / nv * 100) if nv else 0.0
        print(f"{p.name[:40]:40s} {nv:6d} {len(constr_symbol_sets):8d} "
              f"{len(sizes):6d} {largest:7.1f}% {or_forms:8d}")
        # component-size histogram (top few)
        if len(sizes) > 1:
            print(f"    component sizes (top 8): {sizes[:8]}"
                  + (f" ... +{len(sizes)-8} more" if len(sizes) > 8 else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
