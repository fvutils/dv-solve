"""Ratios, suite scores, solved counts (design §5).

Ratios are stored as integer milli-nepers, 1000·ln(s_other / s_head): positive
means the head build is faster, +693 is 2× faster, -693 is 2× slower.

s is solving time: CPU time minus the arm's own start-up cost (its time on a
one-variable problem, measured in the same run), floored at MIN_SOLVE_MS so a
near-zero side cannot produce an arbitrary ratio. Start-up is reported on its
own; folding it into the ratio would score process start-up as solver speed.
A fixture is excluded (None) when either side lacks a definite answer, or
when both sides solve in under FLOOR_MARGIN_MS beyond start-up.
"""
from __future__ import annotations

import math

HEAD = "dv-smt2@head"
FLOOR_MARGIN_MS = 1.0
MIN_SOLVE_MS = 0.5
CAT_CAP = 0.4            # no category may carry more than 40% of a score


def _index(rows: list, suite: str) -> dict:
    return {(r["fixture"], f"{r['arm']}@{r['build']}"): r for r in rows if r["suite"] == suite}


def _definite(r) -> bool:
    return r is not None and r["verdict"] in ("sat", "unsat") and r["cpu_ms_min"] is not None


def ratios(record: dict, suite: str, other: str, head: str = HEAD) -> list:
    """Milli-neper ratios in manifest order; None where excluded."""
    man = record["manifests"][suite]
    idx = _index(record["sat"], suite)
    fl = record.get("sat_floor", {}).get(suite, {})
    out = []
    for fx in man["fixtures"]:
        h, o = idx.get((fx["path"], head)), idx.get((fx["path"], other))
        if not (_definite(h) and _definite(o)):
            out.append(None)
            continue
        sh = h["cpu_ms_min"] - (fl.get(head) or 0)
        so = o["cpu_ms_min"] - (fl.get(other) or 0)
        if max(sh, so) < FLOOR_MARGIN_MS:
            out.append(None)
            continue
        out.append(round(1000 * math.log(max(so, MIN_SOLVE_MS) / max(sh, MIN_SOLVE_MS))))
    return out


def solved(record: dict, suite: str) -> dict:
    """Definite answers per arm key."""
    out = {}
    for r in record["sat"]:
        if r["suite"] == suite:
            k = f"{r['arm']}@{r['build']}"
            out[k] = out.get(k, 0) + _definite(r)
    return out


def par2(record: dict, suite: str, budget_s: float) -> dict:
    """Sum of CPU seconds, an unsolved fixture counting 2 × budget."""
    out = {}
    for r in record["sat"]:
        if r["suite"] == suite:
            k = f"{r['arm']}@{r['build']}"
            t = r["cpu_ms_min"] / 1000 if _definite(r) else 2 * budget_s
            out[k] = round(out.get(k, 0) + t, 2)
    return out


def _capped(weights: dict) -> dict:
    """Shares w_c/Σw, with any share above CAT_CAP clipped and the rest rescaled."""
    share = {c: w / sum(weights.values()) for c, w in weights.items() if w > 0}
    for _ in range(len(share)):
        over = {c for c, s in share.items() if s > CAT_CAP + 1e-12}
        if not over:
            break
        free = 1 - CAT_CAP * len(over)
        rest = sum(s for c, s in share.items() if c not in over)
        share = {c: (CAT_CAP if c in over else s * free / rest) for c, s in share.items()}
    return share


def score(rat: list, fixtures: list, weights: dict):
    """Category-weighted geometric mean speed-up factor (>1: head faster); None if empty.

    Each category contributes the mean of its fixtures' log ratios; categories
    with no usable fixture drop out and the remaining shares are rescaled.
    """
    per_cat = {}
    for r, fx in zip(rat, fixtures):
        if r is not None:
            per_cat.setdefault(fx["cat"], []).append(r / 1000)
    w = {c: weights.get(c, 1.0) for c in per_cat}
    if not w:
        return None
    share = _capped(w)
    return math.exp(sum(share[c] * sum(v) / len(v) for c, v in per_cat.items()))
