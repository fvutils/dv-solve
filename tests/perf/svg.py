"""Minimal deterministic SVG charts for the results pages.

Hand-written rather than matplotlib: the output is byte-stable for the same
data (no embedded dates or ids), and the docs build gains no dependency.
Only what the pages draw: lines/steps on a log-x axis, and a strip plot.
"""
from __future__ import annotations

import math
from html import escape

W, H = 720, 380
ML, MR, MT, MB = 64, 170, 20, 46          # margins; legend sits in the right one
PALETTE = ["#1f77b4", "#d62728", "#2ca02c", "#9467bd", "#ff7f0e", "#8c564b", "#17becf"]


def _fmt(v: float) -> str:
    return f"{v:.1f}".rstrip("0").rstrip(".")


def _log_ticks(lo: float, hi: float) -> list:
    out, e = [], math.floor(math.log10(lo))
    while 10 ** e <= hi * 1.0001:
        if 10 ** e >= lo * 0.9999:
            out.append(10 ** e)
        e += 1
    return out


def _frame(title_x: str, title_y: str, xticks: list, yticks: list) -> list:
    pw, ph = W - ML - MR, H - MT - MB
    s = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" '
         f'font-family="sans-serif" font-size="12" role="img">',
         f'<rect x="0" y="0" width="{W}" height="{H}" fill="#ffffff"/>',
         f'<rect x="{ML}" y="{MT}" width="{pw}" height="{ph}" fill="none" stroke="#999"/>']
    for x, lab in xticks:
        s.append(f'<line x1="{x:.1f}" y1="{MT}" x2="{x:.1f}" y2="{MT + ph}" stroke="#eee"/>')
        s.append(f'<text x="{x:.1f}" y="{MT + ph + 16}" text-anchor="middle">{escape(lab)}</text>')
    for y, lab in yticks:
        s.append(f'<line x1="{ML}" y1="{y:.1f}" x2="{ML + pw}" y2="{y:.1f}" stroke="#eee"/>')
        s.append(f'<text x="{ML - 6}" y="{y + 4:.1f}" text-anchor="end">{escape(lab)}</text>')
    s.append(f'<text x="{ML + pw / 2}" y="{H - 8}" text-anchor="middle">{escape(title_x)}</text>')
    s.append(f'<text transform="translate(14,{MT + ph / 2}) rotate(-90)" text-anchor="middle">'
             f'{escape(title_y)}</text>')
    return s


def _legend(names: list) -> list:
    s, x = [], W - MR + 14
    for i, n in enumerate(names):
        y = MT + 10 + 18 * i
        c = PALETTE[i % len(PALETTE)]
        s.append(f'<line x1="{x}" y1="{y}" x2="{x + 18}" y2="{y}" stroke="{c}" stroke-width="2.5"/>')
        s.append(f'<text x="{x + 24}" y="{y + 4}">{escape(n)}</text>')
    return s


def cactus(series: dict, title_x: str, title_y: str) -> str:
    """series: {name: sorted times (ms)}. Instances solved (y) within time t (x, log)."""
    pts = [t for ts in series.values() for t in ts if t > 0]
    if not pts:
        return ""
    lo, hi = 10 ** math.floor(math.log10(min(pts))), 10 ** math.ceil(math.log10(max(pts)))
    n = max(len(ts) for ts in series.values())
    pw, ph = W - ML - MR, H - MT - MB
    X = lambda t: ML + pw * (math.log10(max(t, lo)) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
    Y = lambda k: MT + ph - ph * k / max(n, 1)
    step = max(1, 10 ** math.floor(math.log10(max(n, 1))) // (2 if n < 50 else 1))
    xt = [(X(t), _fmt(t) if t < 1000 else f"{_fmt(t / 1000)} s") for t in _log_ticks(lo, hi)]
    yt = [(Y(k), str(k)) for k in range(0, n + 1, int(step))]
    s = _frame(title_x, title_y, xt, yt)
    for i, (name, ts) in enumerate(series.items()):
        c = PALETTE[i % len(PALETTE)]
        d = [f"M{X(lo):.1f},{Y(0):.1f}"]
        for k, t in enumerate(ts, 1):
            d.append(f"H{X(t):.1f}V{Y(k):.1f}")
        s.append(f'<path d="{"".join(d)}" fill="none" stroke="{c}" stroke-width="2"/>')
    s += _legend(list(series))
    s.append("</svg>")
    return "\n".join(s) + "\n"


def strip(groups: dict, title_y: str, ref_label: str) -> str:
    """groups: {category: [factor > 0]}. One column per category, log y, line at 1×."""
    vals = [v for vs in groups.values() for v in vs]
    if not vals:
        return ""
    lo = 10 ** math.floor(math.log10(min(vals + [1.0]) * 0.999))
    hi = 10 ** math.ceil(math.log10(max(vals + [1.0]) * 1.001))
    pw, ph = W - ML - MR, H - MT - MB
    Y = lambda v: MT + ph - ph * (math.log10(v) - math.log10(lo)) / (math.log10(hi) - math.log10(lo))
    cats = list(groups)
    X = lambda i: ML + pw * (i + 0.5) / len(cats)
    yt = [(Y(t), f"{_fmt(t)}×") for t in _log_ticks(lo, hi)]
    xt = [(X(i), c) for i, c in enumerate(cats)]
    s = _frame("", title_y, xt, yt)
    s.append(f'<line x1="{ML}" y1="{Y(1):.1f}" x2="{ML + pw}" y2="{Y(1):.1f}" stroke="#333" '
             f'stroke-dasharray="4 3"/>')
    s.append(f'<text x="{ML + pw + 6}" y="{Y(1) + 4:.1f}">{escape(ref_label)}</text>')
    for i, c in enumerate(cats):
        vs = sorted(groups[c])
        for j, v in enumerate(vs):
            jitter = ((j * 7919) % 21 - 10) * pw / len(cats) / 60
            s.append(f'<circle cx="{X(i) + jitter:.1f}" cy="{Y(v):.1f}" r="3.2" '
                     f'fill="{PALETTE[0]}" fill-opacity="0.65"/>')
    s.append("</svg>")
    return "\n".join(s) + "\n"
