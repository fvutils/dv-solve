#!/usr/bin/env python3
"""Infer randomize-site annotations from a captured Verilator SMT session.

Verilator sends each randomize() call as (reset) .. declares .. asserts ..
(check-sat) .., with the non-rand members and the dist draws inlined as
literals. The problem-buffer design (docs/verilator_problem_buffer.md) needs
per-site facts that the text does not carry: which calls belong to one site,
and which inputs change from call to call. This script recovers them:

- A call is the commands from one (reset) to the next.
- The site of a call is its declared-var set plus the asserts sent before the
  first check-sat, with every literal replaced by its sort (the skeleton).
  Asserts under (push) there are Verilator's per-call dist lowering.
- A declared var is STATE if, on every call of its site, a base assert pins it
  to a literal, and the literal takes more than one value.
- A base assert (not (= v literal)) is a randc history exclusion: v is
  reported under 'randc' and the assert is left out of the skeleton.
- Sites with the same declared-var set share a 'family'; in an array bench
  they are the size classes of one SV site (the selects differ per size).
- A literal slot (position in the skeleton) that takes more than one value is
  'dist' if it is under (push) or compared against another literal (a dist
  draw), else 'state' (an inlined non-rand member).

A var pinned on every call can also be a solve-before phase pin (the second
solve fixes the first one's result); the text cannot tell the two apart.

Output is a JSON sidecar:
{calls, sites: [{id, family, calls, vars, state, randc, slots}]}.

Usage: infer_sites.py <session.cmds.smt2> [-o out.json]
"""
import argparse
import hashlib
import json
import sys


def tokenize(text):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in "()":
            yield c
            i += 1
        elif c.isspace():
            i += 1
        elif c == ";":
            while i < n and text[i] != "\n":
                i += 1
        elif c == "|":
            j = text.index("|", i + 1)
            yield text[i:j + 1]
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in "()":
                j += 1
            yield text[i:j]
            i = j


def parse(text):
    """Return the list of top-level commands as nested lists."""
    stack, out = [[]], None
    for t in tokenize(text):
        if t == "(":
            stack.append([])
        elif t == ")":
            e = stack.pop()
            stack[-1].append(e)
        else:
            stack[-1].append(t)
    out = stack[0]
    return out


def literal_sort(e):
    """Sort tag of a literal, or None if e is not a literal."""
    if isinstance(e, str):
        if e.startswith("#x"):
            return "bv%d" % (4 * (len(e) - 2))
        if e.startswith("#b"):
            return "bv%d" % (len(e) - 2)
        if e in ("true", "false"):
            return None
        if e[0].isdigit():
            return "int"
        return None
    if (len(e) == 3 and e[0] == "_" and isinstance(e[1], str)
            and e[1].startswith("bv")):
        return "bv%s" % e[2]
    return None


CMP_OPS = {"=", "distinct", "bvule", "bvult", "bvuge", "bvugt",
           "bvsle", "bvslt", "bvsge", "bvsgt"}


def skeleton(e, consts, kinds):
    """Skeleton string of e; appends each literal to consts and its slot
    kind ('dist' if compared against another literal, else 'state')."""
    s = literal_sort(e)
    if s is not None:
        consts.append(e if isinstance(e, str) else "(_ %s %s)" % (e[1], e[2]))
        kinds.append("state")
        return "#" + s
    if isinstance(e, str):
        return e
    parts = []
    lit_args = 0
    if e and isinstance(e[0], str) and e[0] in CMP_OPS and len(e) == 3:
        lit_args = sum(literal_sort(a) is not None for a in e[1:])
    for a in e:
        k0 = len(kinds)
        parts.append(skeleton(a, consts, kinds))
        if lit_args == 2 and len(kinds) == k0 + 1 and literal_sort(a):
            kinds[k0] = "dist"
    return "(" + " ".join(parts) + ")"


def unwrap(e):
    """Strip Verilator's (= #b1 (__Vbv X)) / (__Vbool X) wrappers."""
    while True:
        if (isinstance(e, list) and len(e) == 3 and e[0] == "="
                and e[1] == "#b1"):
            e = e[2]
        elif isinstance(e, list) and len(e) == 2 and e[0] in ("__Vbv",
                                                              "__Vbool"):
            e = e[1]
        else:
            return e


def pinned(e, names):
    """(var, literal) if assert e pins a declared var to a literal."""
    e = unwrap(e)
    if isinstance(e, list) and len(e) == 3 and e[0] == "=":
        a, b = e[1], e[2]
        if isinstance(a, str) and a in names and literal_sort(b):
            return a, str(b)
        if isinstance(b, str) and b in names and literal_sort(a):
            return b, str(a)
    return None


def excluded(e, names):
    """var if assert e is a randc history exclusion (not (= var literal))."""
    if isinstance(e, list) and len(e) == 2 and e[0] == "not":
        p = pinned(e[1], names)
        if p:
            return p[0]
    return None


def split_calls(cmds):
    calls, cur = [], None
    for c in cmds:
        if c and c[0] == "reset":
            cur = {"decls": [], "base": [], "pushed": [], "depth": 0,
                   "nsat": 0}
            calls.append(cur)
            continue
        if cur is None:
            continue
        head = c[0] if c else None
        if head == "declare-fun" and cur["nsat"] == 0:
            sort = c[3]
            cur["decls"].append((c[1], sort if isinstance(sort, str)
                                 else " ".join(map(str, sort))))
        elif head == "push" and cur["nsat"] == 0:
            cur["depth"] += 1
        elif head == "assert" and cur["nsat"] == 0:
            cur["pushed" if cur["depth"] else "base"].append(c[1])
        elif head == "check-sat":
            cur["nsat"] += 1
    return [c for c in calls if c["decls"]]


def infer(text):
    calls = split_calls(parse(text))
    sites = {}
    for call in calls:
        names = {n for n, _ in call["decls"]}
        consts, kinds, skels, pins, randc = [], [], [], {}, set()
        for a in call["base"]:
            v = excluded(a, names)
            if v:
                randc.add(v)
                continue
            skels.append(skeleton(a, consts, kinds))
            p = pinned(a, names)
            if p:
                pins[p[0]] = p[1]
        skels.append("push")
        k0 = len(kinds)
        for a in call["pushed"]:
            skels.append(skeleton(a, consts, kinds))
        kinds[k0:] = ["dist"] * (len(kinds) - k0)
        decls = repr(sorted(call["decls"]))
        fam = hashlib.sha1(decls.encode()).hexdigest()[:12]
        sid = hashlib.sha1((decls + repr(skels)).encode()).hexdigest()[:12]
        s = sites.setdefault(sid, {"id": sid, "family": fam, "calls": 0,
                                   "vars": sorted(names), "randc": set(),
                                   "kinds": kinds, "consts": [],
                                   "pins": []})
        s["calls"] += 1
        s["randc"] |= randc
        s["consts"].append(consts)
        s["pins"].append(pins)
    out = []
    for s in sites.values():
        state = []
        for v in s["vars"]:
            vals = [p.get(v) for p in s["pins"]]
            if all(x is not None for x in vals) and len(set(vals)) > 1:
                state.append(v)
        slots = []
        for i, k in enumerate(s["kinds"]):
            vals = {c[i] for c in s["consts"]}
            if len(vals) > 1:
                slots.append({"slot": i, "kind": k, "distinct": len(vals)})
        out.append({"id": s["id"], "family": s["family"],
                    "calls": s["calls"], "vars": s["vars"], "state": state,
                    "randc": sorted(s["randc"]), "slots": slots})
    out.sort(key=lambda s: -s["calls"])
    return {"calls": len(calls), "sites": out}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("capture")
    ap.add_argument("-o", "--out", help="write JSON here (default stdout)")
    args = ap.parse_args(argv)
    with open(args.capture) as f:
        res = infer(f.read())
    js = json.dumps(res, indent=1)
    if args.out:
        with open(args.out, "w") as f:
            f.write(js + "\n")
    else:
        print(js)
    return 0


if __name__ == "__main__":
    sys.exit(main())
