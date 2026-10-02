"""Delta-debugging shrinker for failing problems.

`shrink(p, fails)` returns a smaller Problem for which `fails(problem)` is
still true: it drops constraints, replaces sub-terms by one of their
children, a variable, or 0/1, and lowers constants, keeping each change only
while the failure reproduces. Widths are kept.
"""
from __future__ import annotations

from .ir import Problem, children, width


def _positions(t, path=()):
    yield path, t
    for i, c in children(t):
        yield from _positions(c, path + (i,))


def _replace(t, path, new):
    if not path:
        return new
    i = path[0]
    lst = list(t)
    lst[i] = _replace(t[i], path[1:], new)
    return tuple(lst)


def _candidates(p: Problem, t):
    """Simpler terms of the same sort as `t`."""
    if t[0] in ("true", "false", "var"):
        return []
    if t[0] == "const":
        v, w = t[1], t[2]
        return [("const", c, w) for c in (0, 1, v // 2) if c < v]
    out = []
    if t[1] == "bool":
        out += [c for _, c in children(t) if len(c) > 1 and c[1] == "bool"]
        out += [("true",), ("false",)]
    else:
        w = width(t)
        out += [c for _, c in children(t) if c[0] not in ("true", "false") and
                (c[1] != "bool") and width(c) == w]
        out += [("var", n, vw) for n, vw in p.widths.items() if vw == w]
        out += [("const", 0, w), ("const", 1, w)]
    return out


def shrink(p: Problem, fails, budget: int = 400) -> Problem:
    calls = 0

    def still(q: Problem) -> bool:
        nonlocal calls
        calls += 1
        try:
            return fails(q)
        except Exception:
            return False

    cur = Problem(dict(p.widths), list(p.cons), set(p.bins))
    changed = True
    while changed and calls < budget:
        changed = False
        # 1. drop whole constraints
        i = 0
        while i < len(cur.cons) and calls < budget:
            q = Problem(cur.widths, cur.cons[:i] + cur.cons[i + 1:])
            if q.cons and still(q):
                cur, changed = q, True
            else:
                i += 1
        # 2. simplify inside each constraint
        for ci in range(len(cur.cons)):
            progress = True
            while progress and calls < budget:
                progress = False
                for path, sub in list(_positions(cur.cons[ci])):
                    for cand in _candidates(cur, sub):
                        if cand == sub:
                            continue
                        new_c = _replace(cur.cons[ci], path, cand)
                        q = Problem(cur.widths, cur.cons[:ci] + [new_c] + cur.cons[ci + 1:])
                        if still(q):
                            cur, changed, progress = q, True, True
                            break
                    if progress or calls >= budget:
                        break
    # 3. drop unused variables
    used = set()
    for c in cur.cons:
        for _, sub in _positions(c):
            if sub[0] == "var":
                used.add(sub[1])
    if used and used != set(cur.widths):
        q = Problem({n: w for n, w in cur.widths.items() if n in used}, cur.cons)
        if still(q):
            cur = q
    return cur
