"""Randomization quality metrics from a +sb_dump file.

A dump has one line per randomize call, one whitespace-separated token per
field. Values are compared as tokens, so the bench's print format does not
matter.
"""
import math
from collections import Counter
from typing import List, Optional


def load_dump(path: str) -> List[List[str]]:
    """Rows of the dump file; empty lines are skipped."""
    with open(path) as fp:
        return [ln.split() for ln in fp if ln.strip()]


def _columns(rows: List[List[str]]) -> List[List[str]]:
    width = max((len(r) for r in rows), default=0)
    return [[r[i] for r in rows if i < len(r)] for i in range(width)]


def entropy(values: List[str]) -> float:
    """Shannon entropy normalized to [0,1] by log N (1 = all values distinct)."""
    n = len(values)
    if n < 2:
        return 0.0
    h = -sum(c / n * math.log(c / n) for c in Counter(values).values())
    return h / math.log(n)


def chi2(values: List[str], ref: List[str]) -> Optional[float]:
    """Two-sample chi-square statistic per degree of freedom, over the values
    seen in either sample; None when there are too few shared bins to mean
    anything (each sample mostly distinct)."""
    a, b = Counter(values), Counter(ref)
    na, nb = len(values), len(ref)
    if not na or not nb:
        return None
    keys = set(a) | set(b)
    if len(keys) < 2 or len(keys) > (na + nb) / 5:
        return None
    ka, kb = math.sqrt(nb / na), math.sqrt(na / nb)
    stat = sum((ka * a[k] - kb * b[k]) ** 2 / (a[k] + b[k]) for k in keys)
    return stat / (len(keys) - 1)


def field_metrics(rows: List[List[str]], ref: Optional[List[List[str]]] = None) -> dict:
    """Per-field distinct ratio, normalized entropy and (with `ref`) chi2/dof,
    plus their minima over fields (the worst field) as the summary."""
    cols = _columns(rows)
    rcols = _columns(ref) if ref else []
    fields = []
    for i, col in enumerate(cols):
        f = {"distinct": len(set(col)) / len(col) if col else 0.0,
             "entropy": round(entropy(col), 4)}
        f["distinct"] = round(f["distinct"], 4)
        if i < len(rcols):
            c = chi2(col, rcols[i])
            if c is not None:
                f["chi2"] = round(c, 3)
        fields.append(f)
    out = {"n": len(rows), "fields": fields,
           "min_distinct": min((f["distinct"] for f in fields), default=None),
           "min_entropy": min((f["entropy"] for f in fields), default=None)}
    c2 = [f["chi2"] for f in fields if "chi2" in f]
    if c2:
        out["max_chi2"] = max(c2)
    return out


def first_value_diversity(first_rows: List[List[str]]) -> float:
    """Distinct ratio of the first-call rows over a set of seeds (1.0 = every
    seed gave a different first solution)."""
    if not first_rows:
        return 0.0
    return round(len(set(tuple(r) for r in first_rows)) / len(first_rows), 4)
