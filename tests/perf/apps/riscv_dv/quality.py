"""Program-level diversity: statistics of the generated instruction stream.

Reads the `main` and `sub_*` bodies of a generated program (directives and
labels stripped). Per program:

  instrs   instruction lines
  dup      % of lines that repeat an earlier line of the same program
  imm_u    % of immediates that are distinct
Pooled over an arm's programs:
  mnem_h   entropy (bits) of the mnemonic histogram
  reg_h    entropy of the register operands
  js_ref   Jensen-Shannon divergence (bits) of the mnemonic histogram from the
           reference arm's

(From solver-bench/scripts/asm_stats.py.)
"""
from __future__ import annotations

import collections
import math
import re

_LABEL = re.compile(r'^\s*[A-Za-z_0-9]+:\s*')
_REG = re.compile(r'\b(zero|ra|sp|gp|tp|t[0-6]|s1[01]|s[0-9]|a[0-7]|x[0-9]+|f[a-z]*[0-9]+)\b')
_IMM = re.compile(r'(?<![\w.])(-?0x[0-9a-fA-F]+|-?\d+)(?![\w])')


def body(path) -> list:
    out, on = [], False
    for line in open(path, errors="replace"):
        if re.match(r'^(main|sub_\d+):', line):
            on = True
        elif re.match(r'^(test_done|write_tohost|instr_end|_exit):', line):
            on = False
        if not on:
            continue
        line = _LABEL.sub("", line.split("#", 1)[0]).strip()
        if line and not line.startswith("."):
            out.append(re.sub(r"\s+", " ", line))
    return out


def entropy(c) -> float:
    n = sum(c.values())
    return -sum(v / n * math.log2(v / n) for v in c.values() if v) if n else 0.0


def js(p, q) -> float:
    np_, nq = sum(p.values()), sum(q.values())
    if not np_ or not nq:
        return float("nan")
    h = 0.0
    for k in set(p) | set(q):
        a, b = p.get(k, 0) / np_, q.get(k, 0) / nq
        m = (a + b) / 2
        h += (a * math.log2(a / m) if a else 0) + (b * math.log2(b / m) if b else 0)
    return h / 2


def program(lines: list) -> dict:
    """One program's statistics, plus the histograms pooling needs."""
    seen, imms = set(), []
    dup = 0
    mn, rg = collections.Counter(), collections.Counter()
    for ln in lines:
        dup += ln in seen
        seen.add(ln)
        op, _, args = ln.partition(" ")
        mn[op] += 1
        rg.update(_REG.findall(args))
        imms += _IMM.findall(_REG.sub("", args))
    return {"instrs": len(lines), "dup": round(100 * dup / max(1, len(lines)), 2),
            "imm_u": round(100 * len(set(imms)) / len(imms), 2) if imms else None,
            "_mn": mn, "_rg": rg, "_dup_n": dup}


def pooled(progs: list, ref_mn=None) -> dict:
    """An arm's statistics over its programs."""
    mn, rg = collections.Counter(), collections.Counter()
    n = dup = 0
    imm = []
    for p in progs:
        mn.update(p["_mn"])
        rg.update(p["_rg"])
        n += p["instrs"]
        dup += p["_dup_n"]
        if p["imm_u"] is not None:
            imm.append(p["imm_u"])
    out = {"programs": len(progs), "instrs": n, "dup": round(100 * dup / max(1, n), 2),
           "mnem_h": round(entropy(mn), 4), "reg_h": round(entropy(rg), 4),
           "imm_u": round(sum(imm) / len(imm), 2) if imm else None, "_mn": mn}
    if ref_mn is not None:
        out["js_ref"] = round(js(mn, ref_mn), 5)
    return out


def public(d: dict) -> dict:
    """Drop the histograms (keys starting with '_') before a dict goes in a record."""
    return {k: v for k, v in d.items() if not k.startswith("_")}
