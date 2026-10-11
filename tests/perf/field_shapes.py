"""Count bit-field shapes in SystemVerilog constraint code.

Decides between field decomposition and a known-bits domain
(docs/bitfield_linearization_research.md, section 4) from real constraints.
For each `rand` packed variable it gathers what the class's constraint blocks
say about the variable's bit fields, and puts the variable in one bucket:

  word     only whole-word constraints
  one      constraints on one field only (extract or mask; option A covers it)
  kbits    two or more fields, or a field and the word, where every field
           constraint pins the field to a constant: known bits cover it (C)
  fields   two or more fields where some field constraint is an ordering,
           an exclusion, a set or a link to another variable: needs B
  dynamic  a select with a non-constant index (foreach bit loops)

Constraints are taken as written: a field constraint under an implication
counts like an unconditional one. Variables of typedef'd types, struct members
and inline `with` blocks are not resolved. This is a source scan, not an
elaborator, so it counts shapes, not solver work.

Usage: python tests/perf/field_shapes.py [--examples N] DIR...
"""
from __future__ import annotations

import argparse
import collections
import os
import re
import sys

SCALAR = {"bit": 1, "logic": 1, "reg": 1, "byte": 8, "shortint": 16,
          "int": 32, "integer": 32, "longint": 64}
CMP = re.compile(r"(==|!=|<=|>=|(?<![<-])<(?![<=])|(?<![>-])>(?![>=])|\binside\b|\bdist\b)")
NUM = re.compile(r"^\s*(\d+\s*'\s*[sS]?[bodhBODH]\s*[0-9a-fA-F_xXzZ?]+|'\s*[sS]?[bodhBODH]\s*"
                 r"[0-9a-fA-F_]+|'[01]|\d+|[A-Z][A-Z0-9_]*)\s*$")


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", " ", s, flags=re.S)
    s = re.sub(r"//[^\n]*", " ", s)
    return re.sub(r'"(\\.|[^"\\])*"', '""', s)


def braces(s, i):
    """Index just past the brace block that opens at s[i]."""
    depth = 0
    for j in range(i, len(s)):
        if s[j] == "{":
            depth += 1
        elif s[j] == "}":
            depth -= 1
            if depth == 0:
                return j + 1
    return len(s)


def ev_int(e):
    try:
        e = re.sub(r"\b\d+'[dD](\d+)", r"\1", e)
        if re.fullmatch(r"[\d\s+\-*()]+", e):
            return int(eval(e))
    except Exception:
        pass
    return None


def packed_width(base, dims, typedefs):
    """Width of a packed type, None when a dimension is not a literal."""
    if base in typedefs:
        width = typedefs[base]
    else:
        width = SCALAR[base]
    packed = re.findall(r"\[([^\]]*)\]", dims)
    if packed:
        width = 1 if base in ("bit", "logic", "reg") else width
        for p in packed:
            hl = p.split(":")
            h, l = (ev_int(hl[0]), ev_int(hl[1])) if len(hl) == 2 else (None, None)
            if h is None or l is None or width is None:
                return None
            width *= abs(h - l) + 1
    return width


def typedefs(src, known):
    """Packed typedef name -> width (None when not a literal)."""
    out = {}
    for m in re.finditer(r"\btypedef\s+(\w+)\s*(?:signed|unsigned)?\s*((?:\[[^\]]*\]\s*)*)"
                         r"(\w+)\s*;", src):
        if m.group(1) in SCALAR or m.group(1) in known:
            out[m.group(3)] = packed_width(m.group(1), m.group(2), known)
    for m in re.finditer(r"\btypedef\s+struct\s+packed\b[^{]*\{", src):
        e = braces(src, m.end() - 1)
        n = re.match(r"\s*(\w+)", src[e:])
        if n:
            out[n.group(1)] = None
    return out


def decls(src, tds, prefix=r"\brandc?\s+"):
    """name -> (width or None, is_array) for declarations after `prefix`."""
    out = {}
    types = "|".join(sorted(set(SCALAR) | set(tds), key=len, reverse=True))
    for m in re.finditer(prefix + r"(" + types + r")\b\s*(?:signed|unsigned)?\s*"
                         r"((?:\[[^\]]*\]\s*)*)([^;]*);", src):
        width = packed_width(m.group(1), m.group(2), tds)
        for nm in re.finditer(r"(\w+)\s*((?:\[[^\]]*\]\s*)*)(?:=[^,]*)?(?:,|$)", m.group(3)):
            if nm.group(1):
                out[nm.group(1)] = (width, bool(nm.group(2).strip()))
    return out


def blocks(src):
    """(kind, std::randomize variables or None, body) of each constraint block."""
    for m in re.finditer(r"\bconstraint\s+\w+\s*\{|"
                         r"(std::)?randomize\s*(?:\(([^)]*)\))?\s*with\s*(?:\([^)]*\)\s*)?\{", src):
        i = m.end() - 1
        body = src[i + 1:braces(src, i) - 1]
        if m.group(0).startswith("constraint"):
            yield "class", None, body
        elif m.group(1):
            yield "std", [v.strip() for v in (m.group(2) or "").split(",")], body
        else:
            yield "with", None, body
    # OpenTitan's `DV_CHECK_*RANDOMIZE_WITH_FATAL(target, constraints[, msg])
    for m in re.finditer(r"`DV_CHECK_(STD_|MEMBER_)?RANDOMIZE_WITH_FATAL\s*\(", src):
        args, depth, cur = [], 0, m.end()
        for j in range(m.end(), len(src)):
            c = src[j]
            if c in "({[":
                depth += 1
            elif c in ")}]":
                if depth == 0:
                    args.append(src[cur:j])
                    break
                depth -= 1
            elif c == "," and depth == 0:
                args.append(src[cur:j])
                cur = j + 1
        if len(args) < 2:
            continue
        body = args[1].strip()
        if m.group(1):
            yield "std", [args[0].strip()], body
        else:
            yield "with", None, body


def atoms(block):
    """Relational atoms of a constraint block, with conditions split off."""
    sets = []

    def keep(m):
        sets.append(m.group(2))
        return f"{m.group(1)} @{len(sets) - 1}@"
    while True:
        m = re.search(r"\b(inside|dist)\s*\{", block)
        if not m:
            break
        end = braces(block, m.end() - 1)
        block = block[:m.start()] + keep(re.match(r"(\w+)\s*\{(.*)\}", block[m.start():end],
                                                  re.S)) + block[end:]
    for stmt in re.split(r";|\{|\}", block):
        for a in re.split(r"->|&&|\|\||\bif\b|\belse\b|\bforeach\b|\bsolve\b.*", stmt):
            a = a.strip()
            neg = a.startswith("!")
            a = a.lstrip("!( ").rstrip(") ")
            if CMP.search(a):
                yield re.sub(r"@(\d+)@", lambda m: "{" + sets[int(m.group(1))] + "}", a), neg


def balanced_tail(s):
    """Drop unbalanced parentheses left over from splitting."""
    while s.count("(") > s.count(")") and "(" in s:
        s = s[s.index("(") + 1:]
    while s.count(")") > s.count("(") and ")" in s:
        s = s[:s.rindex(")")]
    return s.strip()


def refs(side, rv):
    """(var, key) references to rand packed vars in one side of an atom."""
    out = []
    for m in re.finditer(r"(?<![\w.$'])([A-Za-z_]\w*)\b(\s*\[([^\]]*)\])?", side):
        v = m.group(1)
        if v not in rv:
            continue
        w, arr = rv[v]
        if arr:
            continue
        tail = side[m.end():]
        if m.group(2):
            sel = m.group(3).replace(" ", "")
            hl = sel.split(":") if "+:" not in sel and "-:" not in sel else None
            if hl and all(re.fullmatch(r"[A-Z0-9_+\-*()]+|\d+'[dDhH]\w+", x) for x in hl):
                bits = [ev_int(x) for x in hl]
                if w is not None and None not in bits and \
                        (max(bits) - min(bits) + 1) >= w:
                    out.append((v, "word"))
                else:
                    out.append((v, "f:" + sel))
            else:
                out.append((v, "dyn"))
            continue
        op = re.match(r"\s*(&|\||>>|<<|%|/)\s*([\w']+)", tail)
        pre = re.search(r"([\w']+)\s*(&|\|)\s*$", side[:m.start()])
        if op and NUM.match(op.group(2)):
            out.append((v, f"{op.group(1)}:{op.group(2)}"))
        elif pre and NUM.match(pre.group(1)):
            out.append((v, f"{pre.group(2)}:{pre.group(1)}"))
        else:
            out.append((v, "word"))
    return out


def kind(op, other, neg, other_refs):
    if other_refs:
        return "link"
    if op == "dist":
        return "dist"
    if op == "inside":
        vals = other.strip().strip("{}").split(",")
        if neg or len(vals) > 1 or ":" in other:
            return "set"
        return "eq"
    if op == "==":
        return "ne" if neg else "eq"
    if op == "!=":
        return "eq" if neg else "ne"
    return "order"


def scan(paths):
    files = []
    for p in paths:
        for root, _, names in os.walk(p):
            files += [os.path.join(root, n) for n in names if n.endswith((".sv", ".svh"))]
    srcs = {}
    for f in files:
        try:
            srcs[f] = strip_comments(open(f, errors="replace").read())
        except OSError:
            pass
    tds = {}
    for _ in range(3):              # typedefs of typedefs
        for s in srcs.values():
            tds.update(typedefs(s, tds))
    glob = collections.defaultdict(set)
    per_file = {}
    for f, s in srcs.items():
        per_file[f] = decls(s, tds)
        for n, d in per_file[f].items():
            glob[n].add(d)
    gl = {n: next(iter(ds)) for n, ds in glob.items() if len(ds) == 1}
    homes = collections.defaultdict(set)
    for f, d in per_file.items():
        for n in d:
            homes[n].add(f)
    n_kind = collections.Counter()

    facts = collections.defaultdict(list)     # (file, var) -> [(key, kind, atom)]
    pooled = collections.defaultdict(list)    # the same, on the declaring file
    n_blocks = n_atoms = 0
    for f, s in srcs.items():
        rv0 = {**gl, **per_file[f]}
        local = None
        for bk, svars, b in blocks(s):
            n_blocks += 1
            n_kind[bk] += 1
            rv = rv0
            if bk == "std":
                if local is None:
                    local = decls(s, tds, prefix=r"(?<![\w.])")
                rv = {v: local[v] for v in svars if v in local}
            for a, neg in atoms(b):
                n_atoms += 1
                m = CMP.search(a)
                lhs, rhs = balanced_tail(a[:m.start()]), balanced_tail(a[m.end():])
                op = m.group(1)
                lr, rr = refs(lhs, rv), refs(rhs, rv)
                for side, other, mine, theirs in ((lhs, rhs, lr, rr), (rhs, lhs, rr, lr)):
                    for v, key in mine:
                        # `x[a:b] == y[a:b]` links fields; `x == y` links words.
                        k = kind(op, other, neg, [t for t in theirs if t[0] != v])
                        if key != "word" and (len(mine) > 1 or not NUM.match(other)) \
                                and k == "eq" and not re.fullmatch(r"\s*'?[\w']+\s*", other):
                            k = "expr"
                        facts[(f, v)].append((key, k, a))
                        home = f if f in homes[v] or len(homes[v]) != 1 else next(iter(homes[v]))
                        pooled[(home, v)].append((key, k, a))
    return facts, pooled, len(srcs), n_blocks, n_atoms, n_kind


def bucket(fs):
    keys = {k for k, _, _ in fs}
    sub = {k for k in keys if k != "word"}
    if not sub:
        return "word"
    if "dyn" in sub:
        return "dynamic"
    if len(keys) == 1:
        return "one"
    if all(kd == "eq" for k, kd, _ in fs if k != "word"):
        return "kbits"
    return "fields"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dirs", nargs="+")
    ap.add_argument("--examples", type=int, default=3)
    args = ap.parse_args()
    facts, pooled, nf, nb, na, nk = scan(args.dirs)
    print(f"{nf} files, {nb} constraint blocks, {na} relational atoms")
    print("  blocks: " + ", ".join(f"{k} {n}" for k, n in sorted(nk.items())))
    print("\nPer scope (a class with its own blocks; a randomize-with block on its own):")
    report(facts, args.examples)
    print("\nPooled on the declaring class (every scope that constrains the variable):")
    report(pooled, args.examples)


def report(facts, examples):
    by = collections.defaultdict(list)
    for fv, fs in facts.items():
        by[bucket(fs)].append(fv)
    print(f"  {len(facts)} constrained packed variables")
    reasons = collections.Counter()
    for fv in by["fields"]:
        for k, kd, _ in facts[fv]:
            if k != "word" and kd != "eq":
                reasons[kd] += 1
    for b in ("word", "one", "kbits", "fields", "dynamic"):
        print(f"  {b:8s} {len(by[b]):5d}")
        if b == "fields":
            print("           non-constant field constraints by kind: "
                  + ", ".join(f"{k} {n}" for k, n in reasons.most_common()))
        for f, v in sorted(by[b])[:examples if b != "word" else 0]:
            fs = facts[(f, v)]
            print(f"    {os.path.relpath(f)}: {v}")
            seen = set()
            for k, kd, a in fs:
                if (k, kd) in seen or len(seen) >= 6:
                    continue
                seen.add((k, kd))
                print(f"      [{k} {kd}] {' '.join(a.split())[:90]}")


if __name__ == "__main__":
    sys.exit(main())
