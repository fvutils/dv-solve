"""Parser fuzzer (docs/fw_hdl_implementation_plan.md §3.3, piece 0.5).

Valid scripts, mutated the way generators break them: a paren deleted or
added, the text cut short, two scripts spliced without a newline (fw-hdl's
printer bug), a literal corrupted, a line repeated. For each, against z3:

- dv-solve does not crash or hang, and exits with a documented status;
- after a parse error, no check-sat answers sat or unsat (unless the script
  then resets, which clears the error state);
- when z3 reports an error, dv-solve reports one too, or gives no sat/unsat
  at all (an unsupported sort such as Int is an honest unknown here, an
  error in z3);
- when neither reports an error, every definite answer agrees with z3's.

Seeds are fixed; scale with PARSER_FUZZ_N (default 300).
"""
from __future__ import annotations

import os
import random
import re
import shutil
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = _REPO / "build" / "dv-solve-smt2"
_Z3 = shutil.which("z3")
_N = int(os.environ.get("PARSER_FUZZ_N", "300"))
_HERE = Path(__file__).resolve().parent
_BASES = sorted(p for p in (_HERE / "conformance").glob("*.smt2")
                if p.stem.startswith(("ok_", "value_", "exit_")) and "xfail" not in p.read_text()) + \
         sorted((_HERE / "smt2/fw_hdl/gen/quickcheck-trivial").glob("*.smt2"))[:40]

pytestmark = [pytest.mark.skipif(not _EXE.is_file(), reason="build/dv-solve-smt2 not built"),
              pytest.mark.skipif(not _Z3, reason="z3 not installed")]

# dv-solve's parse-level errors: after one, the session gives no verdict
_PARSE_ERR = re.compile(r"unexpected '\)'|never closed|invalid (numeral|bit-vector literal|token)"
                        r"|unterminated|expected|empty command")


def _strip_comments(text: str) -> str:
    return "\n".join(l for l in text.splitlines() if not l.lstrip().startswith(";")) + "\n"


def mutate(rng: random.Random, text: str, other: str) -> tuple[str, str]:
    k = rng.randrange(6)
    if k == 0:                                   # delete a paren
        idx = [i for i, c in enumerate(text) if c in "()"]
        i = rng.choice(idx)
        return "delete-paren", text[:i] + text[i + 1:]
    if k == 1:                                   # add a paren
        i = rng.randrange(len(text) + 1)
        return "add-paren", text[:i] + rng.choice("()") + text[i:]
    if k == 2:                                   # cut short
        return "truncate", text[:rng.randrange(1, len(text))]
    if k == 3:                                   # splice without a newline
        return "splice", text.rstrip("\n") + other
    if k == 4:                                   # corrupt a literal
        lits = [m for m in re.finditer(r"#[xb][0-9a-fA-F]+", text)]
        if lits:
            m = rng.choice(lits)
            j = rng.randrange(m.start() + 2, m.end())
            return "corrupt-literal", text[:j] + rng.choice("gz2") + text[j + 1:]
    lines = text.splitlines(keepends=True)       # repeat a line
    i = rng.randrange(len(lines))
    return "repeat-line", "".join(lines[:i + 1] + [lines[i]] + lines[i + 1:])


def _responses(out: str) -> list:
    """Top-level responses, as text."""
    res, depth, cur, i, n = [], 0, [], 0, len(out)
    while i < n:
        c = out[i]
        if c == '"':
            j = out.find('"', i + 1)
            j = n - 1 if j < 0 else j
            cur.append(out[i:j + 1])
            i = j + 1
            continue
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        cur.append(c)
        if depth <= 0 and (c == ")" or c == "\n"):
            t = "".join(cur).strip()
            if t:
                res.append(t)
            cur, depth = [], 0
        i += 1
    if "".join(cur).strip():
        res.append("".join(cur).strip())
    return res


def _run(argv, text):
    try:
        p = subprocess.run(argv, input=text, capture_output=True, text=True, timeout=30)
    except subprocess.TimeoutExpired:
        return None, []
    return p.returncode, _responses(p.stdout)


@pytest.mark.parametrize("seed", range(_N))
def test_mutated_input(seed):
    rng = random.Random(seed)
    base, other = rng.choice(_BASES), rng.choice(_BASES)
    kind, text = mutate(rng, _strip_comments(base.read_text()), _strip_comments(other.read_text()))
    rc, ours = _run([str(_EXE)], text)
    assert rc is not None, f"dv-solve hung ({kind} of {base.name}):\n{text}"
    assert rc in (0, 1, 2, 4), f"exit status {rc} ({kind} of {base.name}):\n{text}"
    _, theirs = _run([_Z3, "-in"], text)
    ctx = f"{kind} of {base.name} (seed {seed}):\n{text}\ndv-solve: {ours}\nz3: {theirs}"

    errs = [r for r in ours if r.startswith("(error")]
    first_parse = next((i for i, r in enumerate(ours)
                        if r.startswith("(error") and _PARSE_ERR.search(r)), None)
    if first_parse is not None and "(reset" not in text:
        late = [r for r in ours[first_parse:] if r in ("sat", "unsat")]
        assert not late, "a verdict after a parse error: " + ctx

    z3_errs = [r for r in theirs if r.startswith("(error")]
    if z3_errs:
        assert errs or not [r for r in ours if r in ("sat", "unsat")], \
            "z3 reports an error, dv-solve none and answers: " + ctx
    elif not errs:
        mine = [r for r in ours if r in ("sat", "unsat", "unknown")]
        z3v = [r for r in theirs if r in ("sat", "unsat", "unknown")]
        assert len(mine) == len(z3v), "a different number of answers: " + ctx
        for a, b in zip(mine, z3v):
            assert a == "unknown" or b == "unknown" or a == b, "answers disagree: " + ctx
