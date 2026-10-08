"""The SMT-LIB conformance corpus (docs/fw_hdl_implementation_plan.md §3.3, piece 0.5).

Each tests/formal/conformance/*.smt2 says in its header what dv-solve must
answer, as a sequence of response classes, and its exit status:

    ; expect: sat values unsat
    ; exit: 1
    ; values: z3        (optional) every get-value value must equal z3's
    ; xfail: <reason>   (optional) a known gap, strict

Classes: sat, unsat, unknown, success, unsupported, true, false (get-option),
error, echo (a string), info (get-info), model (get-model), values
(get-value), assertions (get-assertions). Classes, not text: the corpus checks
the protocol, the value checks check the values.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = _REPO / "build" / "dv-solve-smt2"
_Z3 = shutil.which("z3")
_DIR = Path(__file__).resolve().parent / "conformance"
_CASES = sorted(_DIR.glob("*.smt2"))

pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="build/dv-solve-smt2 not built")

_ATOMS = {"sat", "unsat", "unknown", "success", "unsupported", "true", "false"}


def tokens(text: str):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c in "()":
            yield c
            i += 1
        elif c == '"':
            j = i + 1
            while j < n and not (text[j] == '"' and (j + 1 >= n or text[j + 1] != '"')):
                j += 2 if text[j] == '"' else 1
            yield text[i:j + 1]
            i = j + 1
        elif c == "|":
            j = text.find("|", i + 1)
            j = n if j < 0 else j
            yield text[i:j + 1]
            i = j + 1
        else:
            j = i
            while j < n and not text[j].isspace() and text[j] not in '()"':
                j += 1
            yield text[i:j]
            i = j


def parse(text: str) -> list:
    """Top-level s-expressions; an unbalanced tail is dropped."""
    out, stack = [], [[]]
    for t in tokens(text):
        if t == "(":
            stack.append([])
        elif t == ")":
            if len(stack) > 1:
                x = stack.pop()
                stack[-1].append(x)
        else:
            stack[-1].append(t)
    return stack[0]


def classify(x) -> str:
    if isinstance(x, str):
        if x in _ATOMS:
            return x
        return "echo" if x.startswith('"') else f"?{x}"
    if x and x[0] == "error":
        return "error"
    if x and isinstance(x[0], str) and x[0].startswith(":"):
        return "info"
    if x and all(isinstance(i, list) and i and i[0] == "define-fun" for i in x):
        return "model"
    if x and all(isinstance(i, list) and len(i) == 2 for i in x):
        return "values"
    return "assertions"


def header(path: Path) -> dict:
    h = {}
    for line in path.read_text().splitlines():
        m = re.match(r";\s*(expect|exit|values|xfail):\s*(.*)", line)
        if m:
            h[m.group(1)] = m.group(2).strip()
    return h


def value(v) -> int | bool:
    if v in ("true", "false"):
        return v == "true"
    if isinstance(v, str) and v.startswith("#b"):
        return int(v[2:], 2)
    if isinstance(v, str) and v.startswith("#x"):
        return int(v[2:], 16)
    if isinstance(v, list) and len(v) == 3 and v[0] == "_" and v[1].startswith("bv"):
        return int(v[1][2:])
    raise ValueError(f"not a value: {v!r}")


def run(argv, path):
    env = dict(os.environ)
    env.pop("DV_LOG", None)
    p = subprocess.run(argv + [str(path)], capture_output=True, text=True, timeout=60, env=env)
    return p.returncode, parse(p.stdout), p.stdout


@pytest.mark.parametrize("path", _CASES, ids=[p.stem for p in _CASES])
def test_conformance(path, request):
    h = header(path)
    if "xfail" in h:
        request.applymarker(pytest.mark.xfail(reason=h["xfail"], strict=True))
    rc, resp, raw = run([str(_EXE)], path)
    got = [classify(x) for x in resp]
    assert got == h["expect"].split(), f"responses {got}, expected {h['expect']}:\n{raw}"
    assert rc == int(h["exit"]), f"exit status {rc}, expected {h['exit']}"
    if h.get("values") == "z3":
        if not _Z3:
            pytest.skip("z3 not installed")
        _, zresp, zraw = run([_Z3], path)
        ours = [x for x in resp if classify(x) == "values"]
        theirs = [x for x in zresp if classify(x) == "values"]
        assert len(ours) == len(theirs) == 1, (raw, zraw)
        assert len(ours[0]) == len(theirs[0])
        for (t, v), (_, zv) in zip(ours[0], theirs[0]):
            assert value(v) == value(zv), f"{t}: dv-solve {v}, z3 {zv}"


def test_corpus_is_not_empty():
    assert len(_CASES) >= 40
