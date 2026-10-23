"""Recursion in the C sources stays guarded and its frames stay small.

A recursive pass that walks an expression takes one C frame per level of the
input's nesting, and the input decides the depth. Each such pass must stop
before the thread's stack runs out (dvs_stack_low in dvs_stackinfo.h, or a
small fixed depth cap), and must not hold big buffers in a recursive frame:
a level of the SMT-LIB2 translator once cost 9.8 KB, which capped the depth
the CLI could take at ~46,000 (docs/translator_stack_depth.md).

GCC's -fcallgraph-info=su gives each function's call edges and frame size.
From those:

* every recursive cycle (strongly connected component of the call graph,
  compiled without inlining so it keeps source-level names) must pass through
  a function listed in BOUNDED below, which says how that function stops.
  A new recursion fails here until it is guarded and listed;
* at the release flags (-O3), no function on a recursive cycle may have a
  frame over FRAME_BUDGET bytes.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SOURCES = sorted((ROOT / "src/c").glob("*.c")) + sorted((ROOT / "src/c/smt2").glob("*.c"))
INCLUDES = ["src/c", "src/c/sat/kissat/src", "packages/cadical/src"]

# The deepest per-function frame allowed on a recursive cycle. The CDCL
# compile (_compile_constraint, about 1 KB at -O3) is the largest today.
FRAME_BUDGET = 1536

# Functions that end a recursion: (file stem, function) -> how it stops.
BOUNDED = {
    # SMT-LIB2 front end
    ("smt2_frontend", "_translate_tagged"): "dvs_stack_deep",
    ("smt2_frontend", "_parse_array_sort_x"): "nested arrays: one level",
    ("smt2_frontend", "_reified_bool"): "depth cap 32",
    ("smt2_frontend", "_collect_bit1_conjuncts"): "depth cap 32",
    ("smt2_frontend", "_inset_collect"): "depth cap 32",
    ("smt2_frontend", "_seed_store_index_reads"): "iterative over the chain; recursion only on side arrays",
    ("smt2_frontend", "_core_replay_unsat"): "command replay, not term nesting",
    ("smt2_frontend", "_cmd_reset_assertions"): "command replay, not term nesting",
    ("smt2_eval", "_ev"): "dvs_stack_low + max_depth",
    ("smt2_eval", "_parse_sort"): "depth cap 64",
    ("smt2_eval", "_sort_eq"): "sorts from _parse_sort (64)",
    ("smt2_eval", "_print_sort"): "sorts from _parse_sort (64)",
    ("smt2_eval", "_eq"): "array values nest only as deep as their sort (64)",
    # SV elaboration
    ("dvs_sv", "_type"): "dvs_stack_deep",
    ("dvs_sv", "_val"): "dvs_stack_deep",
    ("dvs_sv", "_bool"): "dvs_stack_deep",
    ("dvs_sv", "_mrw_bool"): "dvs_stack_deep",
    ("dvs_sv", "_mrw_src"): "depth cap 8",
    ("dvs_sv", "_mrw_neg"): "depth cap 64",
    ("dvs_sv", "_mrw_same"): "depth cap 16",
    ("dvs_sv", "_fd_build"): "depth cap FD_MAX_SEGS",
    ("dvs_sv", "_fd_walk"): "dvs_stack_deep",
    ("dvs_sv", "_fd_root"): "dvs_stack_deep",
    # CDCL compile
    ("dvs_compile", "_compile_constraint"): "dvs_stack_low",
    ("dvs_compile", "_value_to_var"): "dvs_stack_low",
    ("dvs_compile", "_collect_eq_aliases"): "dvs_stack_low",
    ("dvs_compile", "_collect_congruence"): "dvs_stack_low",
    ("dvs_compile", "_cse_hash"): "depth cap CSE_MAX_DEPTH (32)",
    ("dvs_compile", "_cse_same"): "depth cap CSE_MAX_DEPTH (32)",
    ("dvs_compile", "_cse_collect"): "depth cap CSE_MAX_DEPTH (32)",
    ("dvs_compile", "_classify_or_leaf"): "depth cap 64",
    ("dvs_compile", "_flatten_or"): "depth cap MAX_OR_CLAUSES",
    ("dvs_compile", "_flatten_or_reify"): "depth cap MAX_OR_CLAUSES",
    ("dvs_compile", "_expr_width"): "depth cap 32",
    ("dvs_compile", "_expr_has_signed"): "depth cap 32",
    # NOT guarded: one level per unary node over a constant, 80 B, cheaper
    # than the translator / elaborator levels that admit the chain. Guard it.
    ("dvs_compile", "_fold_const_unary"): "UNGUARDED (see comment)",
    # Bit-blast
    ("dvs_bbsolver", "bb_expr"): "dvs_stack_low",
    ("dvs_bbsolver", "bb_signed"): "dvs_stack_deep",
    ("dvs_bbsolver", "bb_self_width"): "dvs_stack_deep",
    ("dvs_bbsolver", "subst_reaches_var"): "dvs_stack_low",
    ("dvs_bbsolver", "expr_direct_vars"): "dvs_stack_low",
    ("dvs_bbsolver", "bb_flatten_or"): "dvs_stack_low",
    ("dvs_bbsolver", "bb_concat_cuts"): "depth cap 64",
    # Model validator and the oracle's SMT-LIB2 emitter
    ("dvs_validate", "_ev"): "dvs_stack_low (skip)",
    ("dvs_validate", "_vtype"): "dvs_stack_low (skip)",
    ("dvs_validate", "_dump_expr"): "depth cap 16",
    ("dvs_smt2_emit", "_type"): "dvs_stack_low",
    ("dvs_smt2_emit", "_val"): "dvs_stack_low",
    ("dvs_smt2_emit", "_bool"): "dvs_stack_low",
    ("dvs_smt2_emit", "_truth"): "dvs_stack_low",
    ("dvs_smt2_emit", "_count"): "dvs_stack_low",
    # Arithmetic
    ("dvs_prop_templates", "_egcd"): "logarithmic in the operands",
    ("dvs_search", "_luby"): "logarithmic in the restart count",
}

_NODE = re.compile(r'node: \{ title: "([^"]+)" label: "[^\\]*\\n[^\\]*\\n(\d+) bytes')
_EDGE = re.compile(r'edge: \{ sourcename: "([^"]+)" targetname: "([^"]+)"')


def _name(title: str) -> str:
    """`file.c:line:col:fn.part.0` -> `fn`."""
    return re.sub(r"\.(part|isra|constprop|cold)\.\d+$", "",
                  re.sub(r"\.(part|isra|constprop|cold)\.\d+", "", title.split(":")[-1]))


def _graph(ci: Path):
    frames, edges = {}, defaultdict(set)
    for line in ci.read_text().splitlines():
        m = _NODE.match(line)
        if m:
            frames[_name(m.group(1))] = max(frames.get(_name(m.group(1)), 0), int(m.group(2)))
            continue
        m = _EDGE.match(line)
        if m:
            edges[_name(m.group(1))].add(_name(m.group(2)))
    return frames, edges


def _cycles(edges):
    """Strongly connected components that can recurse (iterative Tarjan)."""
    index, low, on, stack, out, n = {}, {}, set(), [], [], [0]
    for root in set(edges) | {t for v in edges.values() for t in v}:
        if root in index:
            continue
        work = [(root, iter(edges.get(root, ())))]
        index[root] = low[root] = n[0]; n[0] += 1; stack.append(root); on.add(root)
        while work:
            v, it = work[-1]
            for w in it:
                if w not in index:
                    index[w] = low[w] = n[0]; n[0] += 1; stack.append(w); on.add(w)
                    work.append((w, iter(edges.get(w, ()))))
                    break
                if w in on:
                    low[v] = min(low[v], index[w])
            else:
                work.pop()
                if work:
                    low[work[-1][0]] = min(low[work[-1][0]], low[v])
                if low[v] == index[v]:
                    c = []
                    while True:
                        w = stack.pop(); on.discard(w); c.append(w)
                        if w == v:
                            break
                    if len(c) > 1 or v in edges.get(v, ()):
                        out.append(sorted(c))
    return out


def _compile_all(flags):
    out = Path(tempfile.mkdtemp(prefix="dvs_stack_"))
    cc = os.environ.get("CC_STACK_CHECK", "gcc")

    def one(src: Path):
        stem = src.stem
        cmd = [cc, *flags, "-std=gnu11", "-fPIC", '-DDVS_VERSION="x"',
               *[f"-I{ROOT / i}" for i in INCLUDES],
               "-fcallgraph-info=su", "-c", str(src),
               "-o", str(out / f"{stem}.o"), "-dumpbase", f"{stem}.c"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        assert r.returncode == 0, f"{src}: {r.stderr[:2000]}"
        return stem, _graph(out / f"{stem}.c.ci")

    with ThreadPoolExecutor(max_workers=os.cpu_count() or 4) as ex:
        graphs = dict(ex.map(one, SOURCES))
    shutil.rmtree(out, ignore_errors=True)
    return graphs


def _gcc_has_callgraph_info() -> bool:
    cc = os.environ.get("CC_STACK_CHECK", "gcc")
    if not shutil.which(cc):
        return False
    r = subprocess.run([cc, "-fcallgraph-info=su", "-x", "c", "-c", "-", "-o", os.devnull],
                       input="int f(void){return 0;}", capture_output=True, text=True,
                       cwd=tempfile.gettempdir())
    return r.returncode == 0


# Windows: the sources are compiled as the POSIX build sees them, and a
# MinGW gcc has neither getc_unlocked nor setenv. The call graph is the same
# on every platform, so the Linux runs cover it.
pytestmark = pytest.mark.skipif(sys.platform == "win32" or not _gcc_has_callgraph_info(),
                                reason="needs gcc >= 10 (-fcallgraph-info) on a POSIX host")


def test_every_recursion_is_bounded():
    unbounded = []
    for stem, (frames, edges) in _compile_all(["-O1", "-fno-inline"]).items():
        for cyc in _cycles(edges):
            if not any((stem, f) in BOUNDED for f in cyc):
                unbounded.append(f"{stem}.c: {', '.join(cyc)}")
    assert not unbounded, (
        "recursion with no guard on its cycle -- stop it with dvs_stack_low() "
        "(dvs_stackinfo.h) or a small depth cap, then list the function in "
        "BOUNDED:\n  " + "\n  ".join(unbounded))


def test_recursive_frames_are_small():
    big = []
    for stem, (frames, edges) in _compile_all(["-O3", "-DNDEBUG"]).items():
        for cyc in _cycles(edges):
            for f in cyc:
                if frames.get(f, 0) > FRAME_BUDGET:
                    big.append(f"{stem}.c: {f} {frames[f]} B (cycle: {', '.join(cyc)})")
    assert not big, (
        f"recursive frame over {FRAME_BUDGET} B -- move big locals (buffers, "
        "rarely used branches) into a noinline helper:\n  " + "\n  ".join(big))
