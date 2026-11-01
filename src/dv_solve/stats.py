"""Solve statistics: read, merge, report, bundle and audit stats files.

dv-solve counts its solves when ``DV_STATS`` is set (to a path, or to ``1``
for ``dv_solve_stats.<pid>.json``) and writes a stats file at process exit.
The file stays on your machine. It is exact, so files from many runs merge.
What you may choose to send is a *bundle* made from them, which is coarsened
for anonymity: class ids salted, every number rounded down to a power of two,
and classes seen fewer than ``k`` times folded into one. Everything in either
file comes from a closed schema (:data:`SCHEMA`); ``audit`` checks that, and
that none of your own identifiers appear.

    python -m dv_solve.stats report FILE...
    python -m dv_solve.stats merge -o OUT FILE...
    python -m dv_solve.stats explain FILE
    python -m dv_solve.stats bundle -o OUT [--salt-file F | --no-salt] [--k N] FILE...
    python -m dv_solve.stats audit FILE [--words W,W...] [--words-file F]
                                        [--words-from DIR]

See docs/solve_stats_capture_plan.md and the user guide's "Solve statistics"
page.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import math
import os
import re
import secrets
import sys
from typing import Dict, Iterable, List, Optional

# ---------------------------------------------------------------------------
# The closed vocabulary (the C side: src/c/dvs_stats.c, src/c/dvs_class.c;
# tests/unit/test_stats_cli.py keeps the two in step)
# ---------------------------------------------------------------------------

PATHS = ["api", "handle", "dpi", "smt2", "verilator", "bvsat"]
ENGINES = ["cdcl", "bitblast", "cube"]
ANSWERED = ENGINES + ["none"]
RESULTS = ["sat", "unsat", "unknown", "error"]
VIA = ["none", "cdcl-root", "cdcl-search", "cdcl-enum", "cdcl-box",
       "bb-sat", "bb-popcount", "bb-unique", "bb-sorted", "bb-pinfast",
       "bb-divmul", "bb-enum", "bb-affine", "bb-sim-split", "bb-sim-exhaustive",
       "bb-sim-random", "bb-sim-race", "bb-sim-once", "bb-sat-fresh",
       "bb-rediversify", "cube", "hash-skip", "model-reuse", "tainted",
       "cache-unsat", "compile-unsat"]
REASONS = ["other", "wide-var", "wide-arith", "wide-const", "sext64", "sdiv-srem",
           "smod", "flush-failed", "multiplier", "bit-level", "seeded sample",
           "logic", "array", "engine-forced", "probe", "sticky", "enumeration",
           "opt-out", "cdcl-slow", "cdcl-declined", "compile-failed", "pool-overflow",
           "cdcl-unknown", "cdcl-error"]
BAILS = ["deadline", "max-depth", "deadline-conflict", "max-restarts", "propagation",
         "thrash", "work", "other"]
MOTIFS = ["var-cmp-const", "var-eq-const", "var-cmp-var", "var-eq-var", "range", "set",
          "ranges", "mod-eq", "mask-eq", "shift-eq", "linear", "nonlinear", "bitwise",
          "countones", "sum", "array", "guarded", "disjunction", "bool-var", "ite-value",
          "constant", "other"]
WIDTHS = ["1", "8", "16", "32", "64", "wide"]
OPS = ["bit", "arith", "mul", "cmp", "eq", "bool", "other"]
HISTS = ["wall_ns", "work_cdcl", "work_bitblast", "work_cube"]
FEATURES = ["vars", "constraints", "softs", "alldiffs", "dists", "ordered", "components",
            "max_component", "max_depth", "guard_depth", "nodes"]

FILE_SCHEMA = "dv-solve-stats/1"
SNAPSHOT_SCHEMA = "dv-solve-stats-snapshot/1"
BUNDLE_SCHEMA = "dv-solve-stats-bundle/1"
OTHER_CLASS = "other"          # a bundle's class for those under the k-threshold

# What each key means, for `explain` and the user guide. Keys of the name ->
# count maps (via, reasons, ...) are explained by their vocabulary.
SCHEMA: Dict[str, str] = {
    "schema": "the format of this file",
    "dv_solve": "the dv-solve version that wrote it",
    "commit": "the dv-solve source commit it was built from",
    "cpus": "CPUs on the machine",
    "level": "how much a bundle holds: L0 is statistics only",
    "salted": "class ids are salted: they cannot be matched to another bundle's",
    "k": "classes solved fewer times than this are folded into one",
    "snapshot": "the counters",
    "compiles": "problems compiled",
    "solves": "solves",
    "wall_ns": "solver time, nanoseconds",
    "results": "solves by result",
    "paths": "solves by the interface they came through",
    "engine_ran": "solves in which each engine ran",
    "answered_by": "solves each engine answered (none: no engine was needed)",
    "fallbacks": "solves in which more than one engine ran",
    "work": "work done by each engine (CDCL: propagation units; bitblast and cube: "
            "SAT solver ticks)",
    "wasted_work": "work by an engine that did not give the answer",
    "bail": "CDCL searches that gave up, by reason",
    "via": "what produced each answer",
    "reasons": "why solves were routed as they were",
    "hist": "log2 histograms: [bucket, count], bucket b holds values in "
            "[2^(b-1), 2^b)",
    "aux": "solver work done outside check-sat (get-unsat-core)",
    "calls": "commands",
    "motif_version": "the version of the motif list",
    "motifs": "constraints by pattern; at the top level, with the solver time of the "
              "classes holding them",
    "count": "how many",
    "classes_dropped": "solves counted without a class (the class table was full)",
    "classes": "one record per shape class: a problem's structure, with names, ids "
               "and constant values left out",
    "class": "the class id: a one-way hash of the structure",
    "instances": "distinct problems seen in the class (estimate)",
    "hll": "registers of a distinct-count estimate (problems; under values, models)",
    "features": "what the class's problem is made of",
    "widths": "variables by width in bits",
    "ops": "operators by kind",
}
SCHEMA.update({
    "vars": "variables", "constraints": "constraints", "softs": "soft constraints",
    "alldiffs": "unique constraints", "dists": "dist constraints",
    "ordered": "variables in a solve-before order",
    "components": "groups of variables tied by constraints",
    "max_component": "variables in the largest group",
    "max_depth": "the deepest expression, in operators",
    "guard_depth": "the deepest nest of if / implication",
    "nodes": "distinct operators",
    "values": "what the models of the class spanned: a sample of them, per variable "
              "(widest first), never the values themselves",
    "models": "models sampled: every one of an engine's first 64, then one in 16",
    "mismatched": "models not counted: their variables did not line up with the "
                  "class's first model",
    "solutions": "distinct models among those sampled (estimate); far below models "
                 "means the sampling has collapsed",
    "width": "the variable's width in bits (up to 64)",
    "free": "1 if the variable's range has more than one value (the declared range, "
            "narrowed by what compile could deduce)",
    "used": "bits the values used (the highest set bit)",
    "varies": "bits that took both values",
    "max_bias": "the largest bias of a bit that varied, per mille off one half",
    "stuck": "1 if the variable is free but never changed over 16 or more models",
    "time_ns": "time outside the solves, nanoseconds: parse (SMT2 commands stating "
               "the problem), compile, read (reading models back); on the SMT2 and "
               "handle paths compile time is also part of wall_ns",
    "parse": "SMT2 commands that state the problem, from the parsed command (reading "
             "the text is not counted: in a pipe it includes waiting for the simulator)",
    "compile": "compiling the problem for the CDCL engine",
    "read": "SMT2 commands that read a model back (get-value)",
    "previous": "each solve against the previous one on its thread",
    "same": "the same problem as the previous solve",
    "constants": "the same class as the previous solve, other constants",
    "shape": "another class than the previous solve, or the thread's first",
    "root": "how constrained the problem is at the first solve's root (CDCL), summed "
            "over compiles: count is how many",
    "fixed": "rand variables the root fixed to one value",
    "tied": "rand variables compile made equal to another variable",
    "bits": "log2 of the product of the rand variables' declared domain sizes",
    "free_bits": "the same, at the root: after compile, level-0 propagation and "
                 "shaving",
    "quiet": "constraints compiled into no propagator (root bounds, equal variables)",
    "dropped": "constraints compile could not handle",
    "binding": "which constraints did the work, summed over the sampled CDCL solves "
               "(the values sample)",
    "narrowed": "constraints that narrowed a bound in the solve",
    "conflicted": "constraints that took part in a conflict in the solve",
    "busy": "solves in which anything narrowed a bound",
    "top_share": "per mille of the bounds narrowed by the busiest constraint, summed "
                 "over busy solves",
    "other_share": "per mille of the bounds narrowed by propagators of no one "
                   "constraint (unique, dist, soft, later adds), summed over busy solves",
})

_NAME_MAPS = {
    "results": RESULTS, "engine_ran": ENGINES, "answered_by": ANSWERED, "work": ENGINES,
    "wasted_work": ENGINES, "bail": BAILS, "via": VIA, "reasons": REASONS,
    "paths": PATHS, "motifs": MOTIFS, "widths": WIDTHS, "ops": OPS, "hist": HISTS,
}

# ---------------------------------------------------------------------------
# Loading and merging
# ---------------------------------------------------------------------------


def load(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    if d.get("schema") not in (FILE_SCHEMA, BUNDLE_SCHEMA):
        raise ValueError(f"{path}: not a dv-solve stats file (schema {d.get('schema')!r})")
    return d


def _add(a, b):
    """a + b for the counter trees: numbers add, maps add by key, histograms
    add by bucket."""
    if isinstance(a, dict):
        out = dict(a)
        for k, v in b.items():
            out[k] = _add(out[k], v) if k in out else copy.deepcopy(v)
        return out
    if isinstance(a, list):         # [[bucket, count], ...]
        h: Dict[int, int] = {}
        for bucket, n in list(a) + list(b):
            h[bucket] = h.get(bucket, 0) + n
        return [[k, h[k]] for k in sorted(h)]
    return a + b


_PER_CLASS = ("class", "instances", "hll", "features", "motifs", "values")
STUCK_MODELS = 16


def _counters(rec: dict) -> dict:
    return {k: v for k, v in rec.items() if k not in _PER_CLASS}


def hll_estimate(regs: List[int]) -> int:
    m = len(regs)
    e = 0.709 * m * m / sum(2.0 ** -r for r in regs)
    zeros = regs.count(0)
    if e <= 2.5 * m and zeros:
        e = m * math.log(m / zeros)
    return int(e + 0.5)


def _merge_values(a: Optional[dict], b: Optional[dict]) -> Optional[dict]:
    """Two classes' value records as one. Per variable, the bits used and the
    bits that varied are the larger of the two (a lower bound for `varies`),
    as is the bias; layouts that differ keep the one with more models."""
    if not a or not b:
        return copy.deepcopy(a or b)
    la = [(v["width"], v["free"]) for v in a["vars"]]
    lb = [(v["width"], v["free"]) for v in b["vars"]]
    if la != lb:
        big, small = (a, b) if a["models"] >= b["models"] else (b, a)
        out = copy.deepcopy(big)
        out["mismatched"] += small["models"] + small["mismatched"]
        return out
    out = {"models": a["models"] + b["models"],
           "mismatched": a["mismatched"] + b["mismatched"],
           "hll": [max(x, y) for x, y in zip(a["hll"], b["hll"])]}
    out["solutions"] = min(hll_estimate(out["hll"]), out["models"])
    out["vars"] = []
    for va, vb in zip(a["vars"], b["vars"]):
        v = {"width": va["width"], "free": va["free"], "used": max(va["used"], vb["used"]),
             "varies": max(va["varies"], vb["varies"]),
             "max_bias": max(va["max_bias"], vb["max_bias"])}
        v["stuck"] = int(bool(v["free"]) and not v["varies"] and out["models"] >= STUCK_MODELS)
        out["vars"].append(v)
    return {k: out[k] for k in ("models", "mismatched", "solutions", "hll", "vars")}


def merge(files: Iterable[dict]) -> dict:
    """One stats file holding the sum of `files` (unbundled stats files)."""
    files = list(files)
    if not files:
        raise ValueError("nothing to merge")
    for d in files:
        if d.get("schema") != FILE_SCHEMA:
            raise ValueError("only stats files merge; bundles are already final")
    versions = sorted({d["dv_solve"] for d in files})
    motif_versions = {d["snapshot"].get("motif_version") for d in files}
    if len(motif_versions) > 1:
        raise ValueError(f"files use different motif lists: {sorted(motif_versions)}")
    classes: Dict[str, dict] = {}
    run = None
    for d in files:
        s = d["snapshot"]
        top = {k: v for k, v in s.items() if k not in ("classes", "motifs", "schema")}
        run = top if run is None else _add(run, top)
        for c in s["classes"]:
            if c["class"] not in classes:
                classes[c["class"]] = copy.deepcopy(c)
                continue
            m = classes[c["class"]]
            merged = _add(_counters(m), _counters(c))
            merged.update({k: m[k] for k in _PER_CLASS if k in m})
            merged["hll"] = [max(x, y) for x, y in zip(m["hll"], c["hll"])]
            values = _merge_values(m.get("values"), c.get("values"))
            if values:
                merged["values"] = values
            classes[c["class"]] = merged
    for c in classes.values():
        c["instances"] = hll_estimate(c["hll"])
    run["motif_version"] = files[0]["snapshot"].get("motif_version")
    snap = {"schema": SNAPSHOT_SCHEMA, **run}
    snap["motifs"] = _motif_summary(classes.values())
    snap["classes"] = sorted(classes.values(), key=lambda c: (-c["wall_ns"], c["class"]))
    return {"schema": FILE_SCHEMA, "dv_solve": versions[0] if len(versions) == 1
            else "+".join(versions), "commit": files[0].get("commit", "unknown"),
            "cpus": max(d.get("cpus", 0) for d in files), "snapshot": snap}


def _motif_summary(classes) -> dict:
    out: Dict[str, dict] = {}
    for c in classes:
        for m, n in c.get("motifs", {}).items():
            e = out.setdefault(m, {"count": 0, "wall_ns": 0})
            e["count"] += n
            e["wall_ns"] += c["wall_ns"]
    return {m: out[m] for m in MOTIFS if m in out}

# ---------------------------------------------------------------------------
# Bundle: what may leave the site
# ---------------------------------------------------------------------------


def _pow2_floor(n: int) -> int:
    return 0 if n <= 0 else 1 << (int(n).bit_length() - 1)


def _coarsen(x):
    """Every number down to a power of two (0 stays 0)."""
    if isinstance(x, dict):
        return {k: _coarsen(v) for k, v in x.items()}
    if isinstance(x, list):
        if x and isinstance(x[0], dict):
            return [_coarsen(v) for v in x]
        return [[b, _pow2_floor(n)] for b, n in x]  # histograms: buckets are log2
    if isinstance(x, bool) or not isinstance(x, int):
        return x
    return _pow2_floor(x)


def _salted(cls: str, salt: Optional[bytes]) -> str:
    if salt is None:
        return cls
    return hashlib.sha256(salt + bytes.fromhex(cls)).hexdigest()[:32]


def bundle(data: dict, salt: Optional[bytes], k: int = 3) -> dict:
    """The L0 bundle of stats file `data`. `salt` None leaves class ids as
    they are (matchable across bundles and against our corpus); otherwise
    they are replaced by a salted hash."""
    if data.get("schema") != FILE_SCHEMA:
        raise ValueError("bundle takes a stats file")
    s = data["snapshot"]
    kept, folded = [], None
    for c in s["classes"]:
        if c["solves"] >= k:
            r = {key: v for key, v in c.items() if key != "hll"}
            if "values" in r:
                r["values"] = {key: v for key, v in r["values"].items() if key != "hll"}
            r["class"] = _salted(c["class"], salt)
            kept.append(r)
        else:
            counters = _counters(c)
            folded = counters if folded is None else _add(folded, counters)
    if folded is not None:
        kept.append({"class": OTHER_CLASS, **folded})
    top = {key: v for key, v in s.items() if key not in ("classes", "motifs")}
    snap = {**top, "motifs": _motif_summary([c for c in kept if c["class"] != OTHER_CLASS]),
            "classes": kept}
    snap = _coarsen(snap)
    snap["schema"] = SNAPSHOT_SCHEMA
    snap["motif_version"] = s.get("motif_version")
    return {"schema": BUNDLE_SCHEMA, "dv_solve": data["dv_solve"], "level": "L0",
            "salted": salt is not None, "k": k, "snapshot": snap}

# ---------------------------------------------------------------------------
# Audit
# ---------------------------------------------------------------------------

_HEX_CLASS = re.compile(r"^[0-9a-f]{16}$|^[0-9a-f]{32}$")
_SV_KEYWORDS = set("""
    begin end module endmodule class endclass function endfunction task endtask
    constraint rand randc logic bit byte int integer shortint longint unsigned
    signed if else for foreach while return inside dist solve before typedef enum
    struct packed union virtual extends import package endpackage parameter local
    static protected automatic void string this super new null const output input
    inout wire reg assign always initial generate endgenerate case endcase default
    unique priority interface endinterface property sequence assert assume cover
""".split())


def _walk(x, path, out):
    """Every key and value in `x`, as (path, key, value) triples."""
    if isinstance(x, dict):
        for k, v in x.items():
            out.append((path, k, v))
            _walk(v, path + (k,), out)
    elif isinstance(x, list):
        for v in x:
            _walk(v, path + ("[]",), out)


def _allowed_key(path, key) -> bool:
    if key in SCHEMA:
        return True
    parent = next((p for p in reversed(path) if p != "[]"), None)
    names = _NAME_MAPS.get(parent)
    if names is not None and key in names:
        return True
    if parent in PATHS and key in ["solves"] + RESULTS:
        return True
    if parent in MOTIFS and key in ("count", "wall_ns"):
        return True
    return False


def audit(data: dict, words: Iterable[str] = (), text: Optional[str] = None) -> List[str]:
    """Problems found in stats file or bundle `data` (empty: it passes).

    - every key is in the closed schema;
    - every string is a known value or a class id of the right form;
    - in a bundle, every number is 0 or a power of two, and there are no
      distinct-problem registers;
    - none of `words` appears, in clear (case-insensitive) or as a common
      hash of it (md5, sha1, sha256, as hex)."""
    problems: List[str] = []
    is_bundle = data.get("schema") == BUNDLE_SCHEMA
    triples: list = []
    _walk(data, (), triples)
    known_strings = {FILE_SCHEMA, BUNDLE_SCHEMA, SNAPSHOT_SCHEMA, "L0", OTHER_CLASS}
    for path, key, v in triples:
        where = "/".join(path + (key,))
        if not _allowed_key(path, key):
            problems.append(f"{where}: key not in the schema")
        if key == "hll":
            if is_bundle:
                problems.append(f"{where}: a bundle must not hold distinct-problem registers")
            elif not (isinstance(v, list) and all(isinstance(r, int) and 0 <= r <= 64
                                                  for r in v)):
                problems.append(f"{where}: registers must be small integers")
            continue
        if key in HISTS and path and path[-1] == "hist":
            if not (isinstance(v, list) and all(
                    isinstance(e, list) and len(e) == 2 and all(isinstance(n, int)
                                                               for n in e) for e in v)):
                problems.append(f"{where}: not a histogram")
                continue
            for b, n in v:
                if not 0 <= b < 64 or n < 0:
                    problems.append(f"{where}: bucket [{b}, {n}] out of range")
                elif is_bundle and n & (n - 1):
                    problems.append(f"{where}: {n} is not a power of two")
            continue
        if isinstance(v, dict):
            continue
        vals = v if isinstance(v, list) else [v]
        for x in _leaves(vals):
            if isinstance(x, str):
                if key == "class":
                    if x != OTHER_CLASS and not _HEX_CLASS.match(x):
                        problems.append(f"{where}: class id {x!r} is not a hash")
                elif key == "dv_solve":
                    if not re.match(r"^[0-9][0-9a-z.+]*$", x):
                        problems.append(f"{where}: version {x!r} is free text")
                elif key == "commit":
                    if not re.match(r"^([0-9a-f]{4,40}|unknown)$", x):
                        problems.append(f"{where}: commit {x!r} is free text")
                elif x not in known_strings:
                    problems.append(f"{where}: string {x!r} is not a schema value")
            elif isinstance(x, bool):
                if key != "salted":
                    problems.append(f"{where}: unexpected boolean")
            elif isinstance(x, (int, float)):
                if isinstance(x, float) or x < 0:
                    problems.append(f"{where}: {x!r} is not a count")
                elif is_bundle and x and (x & (x - 1)) and key not in ("k", "motif_version"):
                    problems.append(f"{where}: {x} is not a power of two")
    if text is None:
        text = json.dumps(data)
    low = text.lower()
    vocab = _vocabulary()
    for w in words:
        w = w.strip()
        if len(w) < 3 or w.lower() in vocab:
            continue
        if re.search(r"(?<![a-z0-9_])" + re.escape(w.lower()) + r"(?![a-z0-9_])", low):
            problems.append(f"word {w!r} appears")
        for h in (hashlib.md5, hashlib.sha1, hashlib.sha256):
            if h(w.encode()).hexdigest()[:16] in low:
                problems.append(f"word {w!r} appears as a {h().name} hash")
    return problems


def _leaves(vals):
    for x in vals:
        if isinstance(x, list):
            yield from _leaves(x)
        elif not isinstance(x, dict):
            yield x


def _vocabulary() -> set:
    out = set(SCHEMA) | set(FEATURES)
    for names in _NAME_MAPS.values():
        out |= set(names)
    for n in list(out):
        out |= set(re.split(r"[-_ ]", n))
    return {x.lower() for x in out if x}


def words_from_dir(root: str) -> List[str]:
    """Identifiers in the SystemVerilog (and Python) sources under `root`:
    class, field and variable names to look for in a bundle."""
    found = set()
    for dirpath, _, files in os.walk(root):
        for f in files:
            if not f.endswith((".sv", ".svh", ".v", ".vh", ".py")):
                continue
            try:
                with open(os.path.join(dirpath, f), encoding="utf-8", errors="replace") as fh:
                    for tok in re.findall(r"[A-Za-z_][A-Za-z0-9_$]{3,}", fh.read()):
                        if tok.lower() not in _SV_KEYWORDS:
                            found.add(tok)
            except OSError:
                pass
    return sorted(found)

# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------


def _ms(ns: int) -> str:
    return f"{ns / 1e6:.1f} ms" if ns < 1e9 else f"{ns / 1e9:.2f} s"


def _values_note(c: dict) -> str:
    """What stands out in class `c`'s value record: collapsed sampling and
    variables that never change."""
    v = c.get("values")
    if not v or not v.get("models"):
        return ""
    parts = [f"{v['solutions']} distinct of {v['models']} models sampled"]
    if v["models"] >= 32 and v["solutions"] * 2 < v["models"]:
        parts[0] += " (few: a small solution space, or collapsed sampling)"
    stuck = sum(x["stuck"] for x in v.get("vars", []))
    if stuck:
        parts.append(f"{stuck} variable{'s' if stuck > 1 else ''} never changed")
    return "; ".join(parts)


def _shape_note(c: dict) -> str:
    """Class `c`'s constrainedness, binding constraints and time outside the
    solves (signals 3, 5 and 6)."""
    parts = []
    r = c.get("root")
    if r and r.get("count"):
        n = r["count"]
        parts.append(f"root: {r['vars'] / n:.0f} vars, {r['fixed'] / n:.0f} fixed, "
                     f"domain 2^{r['bits'] / n:.0f} -> 2^{r['free_bits'] / n:.0f}")
        if r.get("quiet") or r.get("dropped"):
            parts[-1] += f"; {r['quiet'] / n:.0f} constraints quiet, " \
                         f"{r['dropped'] / n:.0f} dropped"
    b = c.get("binding")
    if b and b.get("solves"):
        n = b["solves"]
        s = f"binding: {b['narrowed'] / n:.1f} of {b['constraints'] / n:.0f} constraints " \
            f"narrow per solve"
        if b.get("busy"):
            s += f", busiest {b['top_share'] / b['busy'] / 10:.0f}% of narrowing"
        parts.append(s)
    tm = c.get("time_ns", {})
    if any(tm.values()):
        parts.append("outside solves: " + ", ".join(
            f"{k} {_ms(v)}" for k, v in tm.items() if v))
    p = c.get("previous", {})
    if sum(p.values()) and p.get("same", 0) * 2 > sum(p.values()):
        parts.append(f"{100 * p['same'] // sum(p.values())}% repeat the previous problem")
    return "; ".join(parts)


def report(data: dict, top: int = 10) -> str:
    s = data["snapshot"]
    total = s["wall_ns"] or 1
    lines = [f"dv-solve {data['dv_solve']}: {s['solves']} solves, {_ms(s['wall_ns'])} "
             f"solver time, {len(s['classes'])} classes, {s.get('compiles', 0)} compiles"]
    res = ", ".join(f"{k} {v}" for k, v in s["results"].items() if v)
    lines.append(f"  results: {res}")
    if s["solves"]:
        eng = ", ".join(f"{k} {v}" for k, v in s["answered_by"].items() if v)
        lines.append(f"  answered by: {eng}; fallbacks {s['fallbacks']}")
        via = sorted(s["via"].items(), key=lambda kv: -kv[1])[:6]
        lines.append("  answers came from: " + ", ".join(f"{k} {v}" for k, v in via))
    if s.get("reasons"):
        lines.append("  routing reasons: " + ", ".join(
            f"{k} {v}" for k, v in sorted(s["reasons"].items(), key=lambda kv: -kv[1])))
    cum = 0
    lines.append(f"  top classes by solver time:")
    for c in s["classes"][:top]:
        cum += c["wall_ns"]
        per = c["wall_ns"] / max(c["solves"], 1) / 1000
        motifs = ", ".join(f"{k} {v}" for k, v in sorted(
            c.get("motifs", {}).items(), key=lambda kv: -kv[1])[:4])
        lines.append(f"    {c['class'][:12]}  {100 * c['wall_ns'] / total:5.1f}% "
                     f"(cum {100 * cum / total:5.1f}%)  {c['solves']} solves, "
                     f"{per:.1f} us/solve, {c.get('instances', '?')} instances  [{motifs}]")
        note = _values_note(c)
        if note:
            lines.append(f"      values: {note}")
        note = _shape_note(c)
        if note:
            lines.append(f"      {note}")
    if s.get("motifs"):
        lines.append("  motifs by the solver time of the classes holding them:")
        for m, e in sorted(s["motifs"].items(), key=lambda kv: -kv[1]["wall_ns"])[:8]:
            lines.append(f"    {m:16s} {e['count']:6d} constraints  "
                         f"{100 * e['wall_ns'] / total:5.1f}% of time")
    return "\n".join(lines)


def explain(data: dict) -> str:
    """Every value in `data` with what it means."""
    out: List[str] = []

    def go(x, indent, key=None):
        pad = "  " * indent
        if isinstance(x, dict):
            for k, v in x.items():
                label = SCHEMA.get(k, "")
                if isinstance(v, (dict, list)):
                    out.append(f"{pad}{k}:" + (f"  -- {label}" if label else ""))
                    go(v, indent + 1, k)
                else:
                    out.append(f"{pad}{k} = {v}" + (f"  -- {label}" if label else ""))
        elif isinstance(x, list):
            if key in HISTS:
                out.append(pad + "  ".join(f"[2^{b - 1}..2^{b}): {n}" if b else f"0: {n}"
                                           for b, n in x))
            elif key == "hll":
                out.append(pad + f"{len(x)} small integers")
            else:
                for i, v in enumerate(x):
                    out.append(f"{pad}[{i}]")
                    go(v, indent + 1, key)
        else:
            out.append(f"{pad}{x}")
    go(data, 0)
    return "\n".join(out)

# ---------------------------------------------------------------------------
# Command line
# ---------------------------------------------------------------------------


def _salt(args) -> Optional[bytes]:
    if args.no_salt:
        return None
    if args.salt_file:
        if os.path.exists(args.salt_file):
            with open(args.salt_file, "rb") as f:
                return f.read()
        s = secrets.token_bytes(32)
        with open(args.salt_file, "wb") as f:
            f.write(s)
        return s
    return secrets.token_bytes(32)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m dv_solve.stats", description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("report", help="summarize stats files")
    p.add_argument("files", nargs="+")
    p.add_argument("--top", type=int, default=10)
    p = sub.add_parser("merge", help="sum stats files into one")
    p.add_argument("files", nargs="+")
    p.add_argument("-o", "--out", required=True)
    p = sub.add_parser("explain", help="print a stats file or bundle, every value labelled")
    p.add_argument("file")
    p = sub.add_parser("bundle", help="make the anonymized file you may send (L0)")
    p.add_argument("files", nargs="+")
    p.add_argument("-o", "--out", required=True)
    p.add_argument("--k", type=int, default=3,
                   help="fold classes solved fewer times than this into one (default 3)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--salt-file", help="keep one salt here, so your bundles match each other")
    g.add_argument("--no-salt", action="store_true",
                   help="leave class ids matchable against other bundles and our corpus")
    p = sub.add_parser("audit", help="check a stats file or bundle")
    p.add_argument("file")
    p.add_argument("--words", default="", help="comma-separated identifiers to look for")
    p.add_argument("--words-file", help="identifiers to look for, one per line")
    p.add_argument("--words-from", help="a source tree whose identifiers to look for")
    args = ap.parse_args(argv)

    if args.cmd == "report":
        data = merge(load(f) for f in args.files)
        print(report(data, args.top))
        return 0
    if args.cmd == "merge":
        data = merge(load(f) for f in args.files)
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1)
        return 0
    if args.cmd == "explain":
        print(explain(load(args.file)))
        return 0
    if args.cmd == "bundle":
        data = merge(load(f) for f in args.files)
        b = bundle(data, _salt(args), args.k)
        problems = audit(b)
        if problems:
            print("bundle not written: it fails the audit:", file=sys.stderr)
            for x in problems:
                print("  " + x, file=sys.stderr)
            return 1
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(b, f, indent=1)
        print(f"wrote {args.out}: {len(b['snapshot']['classes'])} classes; "
              f"read it with `python -m dv_solve.stats explain {args.out}`")
        return 0
    if args.cmd == "audit":
        with open(args.file, encoding="utf-8") as f:
            text = f.read()
        data = json.loads(text)
        words = [w for w in args.words.split(",") if w]
        if args.words_file:
            with open(args.words_file, encoding="utf-8") as f:
                words += [ln.strip() for ln in f if ln.strip()]
        if args.words_from:
            words += words_from_dir(args.words_from)
        problems = audit(data, words, text)
        for x in problems:
            print(x)
        print(f"{args.file}: {'FAIL' if problems else 'ok'} "
              f"({len(problems)} problems, {len(words)} words checked)")
        return 1 if problems else 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
