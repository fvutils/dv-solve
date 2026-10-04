"""Value-level diversity: the values randomize() returns, from solver transcripts.

A transcript (solver_tap's DVS_TAP_LOG) is reduced, as soon as its cell
finishes, to per (call site, variable) value counts. The call site is the
get-value variable list. For each randomize() the LAST model before the next
(reset) is the one Verilator keeps, so only that one is counted; with
Verilator's own sampler there are several per call.

Over the pairs that have at least MIN_N draws in every compared arm:

  hhmax  entropy of the values over the entropy of a uniform draw from every
         value any compared arm produced (1 = as spread as that union allows;
         the union is a lower bound on the legal set, which riscv-dv's
         constraints do not let us enumerate)
  mode   % of draws equal to the most common value
  js_ref Jensen-Shannon divergence (bits) from the reference arm

The compared arm set is fixed per suite (the trended pair), because Hmax
depends on which arms form the union.
(From solver-bench/scripts/model_dist.py.)
"""
from __future__ import annotations

import collections
import math
import re

from .quality import entropy, js

MIN_N = 50
_PAIR = re.compile(r'\(\s*([^\s()]+|\(select [^()]+\))\s+(#b[01]+|#x[0-9a-fA-F]+|\(_ bv\d+ \d+\))\s*\)')


def _lit(v: str) -> int:
    if v.startswith("#b"):
        return int(v[2:], 2)
    if v.startswith("#x"):
        return int(v[2:], 16)
    return int(v.split()[1][2:])


def reduce(path, into=None) -> dict:
    """{(site, var): Counter(value)} from one transcript, added to `into`."""
    out = into if into is not None else collections.defaultdict(collections.Counter)
    last, site, resp, in_resp = None, None, [], False

    def keep(model):
        if model and model[1]:
            for var, v in model[1].items():
                out[(model[0], var)][v] += 1

    def parse():
        return site, dict((n, _lit(v)) for n, v in _PAIR.findall(" ".join(resp)))

    with open(path, errors="replace") as f:
        for line in f:
            _, _, rest = line.partition(" ")
            if rest.startswith("> "):
                if in_resp:
                    last, in_resp = parse(), False
                cmd = rest[2:].strip()
                if cmd.startswith("(get-value"):
                    site = " ".join(cmd[len("(get-value"):].split()).strip("() ")
                    resp, in_resp = [], True
                elif cmd.startswith("(reset"):
                    keep(last)
                    last = None
            elif rest.startswith("< ") and in_resp:
                resp.append(rest[2:].strip())
    if in_resp:
        last = parse()
    keep(last)
    return out


def summary(counts: dict, arms: list, ref: str) -> dict:
    """{arm: {hhmax, mode, js_ref, pairs}} over the pairs every arm drew MIN_N times."""
    keys = [k for k in set().union(*(counts[a] for a in arms))
            if all(sum(counts[a].get(k, {}).values()) >= MIN_N for a in arms)]
    out = {}
    for a in arms:
        hr, md, jd = [], [], []
        for k in keys:
            union = set().union(*(counts[x][k] for x in arms))
            if len(union) < 2:
                continue
            c = counts[a][k]
            hmax = math.log2(min(len(union), sum(c.values())))
            hr.append(entropy(c) / hmax if hmax else 1.0)
            md.append(100 * max(c.values()) / sum(c.values()))
            jd.append(js(c, counts[ref][k]))
        mean = lambda xs: round(sum(xs) / len(xs), 4) if xs else None
        out[a] = {"hhmax": mean(hr), "mode": mean(md), "js_ref": mean(jd), "pairs": len(hr)}
    return out
