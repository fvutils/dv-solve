"""Check every code block on the documentation site.

Each fenced block in docs/site is one of:

- an included file (``literalinclude``) from docs/examples/, which
  test_doc_examples.py runs;
- a C declaration, checked against dv_solve.h by compiling it
  (test_c_api_matches_header);
- a SystemVerilog declaration, checked against src/sv
  (test_sv_api_matches_packages);
- a Sphinx directive that the docs build (sphinx -W) checks;
- a command or its output, run or checked by a test named in _COVERED;
- or listed in _NOT_RUN, with the reason it cannot run here.

test_every_block_is_covered fails for a block that is none of these, so a new
block on the site needs a check before it can be published.
"""
from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_SITE = _REPO / "docs" / "site"
_EXAMPLES = _REPO / "docs" / "examples"
_VERILATOR = _REPO / "packages" / "verilator-bin" / "bin" / "verilator"


@dataclass
class Block:
    page: str                       # path relative to docs/site
    line: int                       # line of the opening fence
    kind: str                       # language, or {directive}
    arg: str                        # text after the language / directive
    options: dict = field(default_factory=dict)
    body: list = field(default_factory=list)

    @property
    def key(self) -> tuple:
        return (self.page, self.body[0] if self.body else self.arg)

    @property
    def text(self) -> str:
        return "\n".join(self.body) + "\n"


def _parse(page: Path) -> list:
    rel = page.relative_to(_SITE).as_posix()
    lines = page.read_text().splitlines()
    blocks, i = [], 0
    while i < len(lines):
        m = re.match(r"^(`{3,})\s*(\{[\w:-]+\}|[\w-]*)\s*(.*)$", lines[i])
        if not m:
            i += 1
            continue
        fence, kind, arg = m.groups()
        j = i + 1
        while j < len(lines) and lines[j].rstrip() != fence:
            j += 1
        inner = lines[i + 1:j]
        opts = {}
        while inner and kind.startswith("{") and re.match(r"^:[\w-]+:", inner[0]):
            k, _, v = inner.pop(0)[1:].partition(":")
            opts[k] = v.strip().strip('"')
        blocks.append(Block(rel, i + 1, kind, arg.strip(), opts, inner))
        i = j + 1
    return blocks


def _all_blocks() -> list:
    out = []
    for page in sorted(_SITE.rglob("*.md")):
        if "_build" not in page.parts:
            out.extend(_parse(page))
    return out


def _block(page: str, first: str) -> Block:
    for b in _parse(_SITE / page):
        if b.body and b.body[0] == first:
            return b
    raise AssertionError(f"{page}: no block starting {first!r}; update the test with the page")


# Blocks checked by a test in this module: (page, first line) -> test name.
_COVERED = {
    ("getting-started/install.md", "import dv_solve"): "test_install_python_block",
    ("getting-started/install.md", "build/dv-solve-smt2 --version"): "test_install_source_commands",
    ("getting-started/install.md",
     'PYTHONPATH=src python3 -c "import dv_solve; print(dv_solve.get_libdirs())"'):
        "test_install_source_commands",
    ("getting-started/quickstart-python.md", "seed=1: x=188 lo=30 hi=221"):
        "test_quickstart_python_output",
    ("getting-started/quickstart-smt2.md", "dv-solve-smt2 quickstart.smt2"):
        "test_quickstart_smt2_command",
    ("getting-started/quickstart-smt2.md", "$ dv-solve-smt2"): "test_quickstart_smt2_session",
    ("getting-started/quickstart-verilator.md", "verilator --binary packet.sv"):
        "test_quickstart_verilator",
    ("getting-started/quickstart-verilator.md",
     'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 --interactive --mode=verilator"'):
        "test_quickstart_verilator",
    ("getting-started/quickstart-verilator.md", "addr=00000a1c len=5 kind=14"):
        "test_quickstart_verilator",
    ("getting-started/quickstart-verilator.md", "./obj_dir/Vpacket +verilator+seed+1"):
        "test_quickstart_verilator",
    ("guides/verilator.md",
     'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 --interactive --mode=verilator"'):
        "test_verilator_guide_setup",
    ("guides/verilator.md",
     'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 --interactive --mode=verilator '
     '--verilator-hash=ignore"'):
        "test_verilator_guide_hash_ignore",
    ("guides/verilator.md",
     "%Warning-UNSATCONSTR: bad.sv:3: Unsatisfied constraint: 'constraint c1 { x > 10; }'"):
        "test_verilator_guide_unsat",
    ("guides/verilator.md",
     "%Warning: .../verilated_random.cpp:362: Solver returned unknown (timed out or incomplete), "
     "so randomize() may return 0; warned once"):
        "test_verilator_guide_unknown",
    ("guides/systemverilog-dpi.md", "python3 packet_problem.py        # writes packet_problem_pkg.sv"):
        "test_dpi_guide",
    ("guides/systemverilog-dpi.md",
     "SV=$(python3 -c 'import dv_solve; print(dv_solve.get_svdirs()[0])')"): "test_dpi_guide",
    ("guides/systemverilog-dpi.md", "addr=0000091c len=4 kind=8"): "test_dpi_guide",
    ("guides/packaging.md", "cc app.c $(python3 -c 'import dv_solve as d;"): "test_packaging_link_line",
    ("reference/c-api.md", '#include "dv_solve.h"'): "test_c_api_matches_header",
    ("reference/c-api.md", "addr=000001f4 len=9 kind=13"): "test_c_api_output",
    ("reference/cli.md", "dv-solve-smt2 [options] [file.smt2]"): "test_cli_usage",
}

# Blocks that cannot run in the test suite.
_NOT_RUN = {
    ("getting-started/install.md", "pip install dv-solve"): "installs from PyPI",
    ("getting-started/install.md", "git clone https://github.com/fvutils/dv-solve.git"):
        "clones and builds the whole project; CI's build does the same",
    ("getting-started/install.md", "ivpm update -d use        # fetches CaDiCaL into ./packages"):
        "fetches packages from the network",
    ("internals/architecture.md", " Python / C builder ─┐"): "a diagram, not code",
    ("results/methodology.md", "python3 -m tests.perf.tools fetch --dest perf-tools"):
        "downloads the pinned solvers and times the whole suite; perf.yml runs these "
        "steps every night, and test_perf_render.py checks the rendering",
}

# Sphinx directives the docs build checks (sphinx -W fails on a bad one).
_SPHINX_CHECKED = {"{toctree}", "{autoclass}", "{autoexception}", "{automodule}", "{autofunction}",
                   "{list-table}"}


def test_every_block_is_covered() -> None:
    blocks = _all_blocks()
    assert blocks, "no pages found"
    missing = []
    for b in blocks:
        if (b.kind in _SPHINX_CHECKED or b.kind == "{literalinclude}"
                or b.kind.startswith("{c:")
                or (b.page == "reference/sv-api.md" and b.kind == "systemverilog")
                or b.key in _COVERED or b.key in _NOT_RUN):
            continue
        missing.append(f"{b.page}:{b.line}: {b.kind} {b.body[:1]}")
    assert not missing, "blocks no test checks:\n" + "\n".join(missing)
    keys = {b.key for b in blocks}
    stale = [k for k in list(_COVERED) + list(_NOT_RUN) if k not in keys]
    assert not stale, f"registered blocks no longer on the site: {stale}"
    for name in set(_COVERED.values()):
        assert callable(globals().get(name)), f"_COVERED names a missing test {name}"


def test_literalincludes_resolve() -> None:
    for b in _all_blocks():
        if b.kind != "{literalinclude}":
            continue
        path = (_SITE / b.page).parent / b.arg
        assert path.is_file(), f"{b.page}:{b.line}: {b.arg} does not exist"
        assert _EXAMPLES in path.resolve().parents, f"{b.page}:{b.line}: include outside docs/examples"
        text = path.read_text()
        for opt in ("start-after", "end-before"):
            if opt in b.options:
                assert b.options[opt] in text, f"{b.page}:{b.line}: {opt} marker missing"


# ------------------------------------------------------------------ #
# Environment the commands run in: what a user who built dv-solve and  #
# installed Verilator would have on PATH.                              #
# ------------------------------------------------------------------ #

def _smt2_exe() -> str | None:
    exe = _REPO / "build" / "dv-solve-smt2"
    return str(exe) if exe.is_file() else shutil.which("dv-solve-smt2")


def _verilator() -> str | None:
    return str(_VERILATOR) if _VERILATOR.is_file() else shutil.which("verilator")


@pytest.fixture(scope="module")
def shell_env() -> dict:
    env = dict(os.environ)
    env["PYTHONPATH"] = os.pathsep.join(
        [str(_REPO / "src")] + ([env["PYTHONPATH"]] if env.get("PYTHONPATH") else []))
    probe = subprocess.run(
        [sys.executable, "-c",
         "import sys; from dv_solve.lib import _load_lib; "
         "sys.exit(0 if _load_lib() is not None else 3)"],
        env=env, capture_output=True, timeout=60)
    if probe.returncode != 0:
        pytest.skip("libdv_solve not built")
    path = [os.path.dirname(sys.executable)]
    for tool in (_smt2_exe(), _verilator()):
        if tool:
            path.append(os.path.dirname(tool))
    env["PATH"] = os.pathsep.join(path + [env.get("PATH", "")])
    return env


def _sh(script: str, cwd: Path, env: dict, timeout: int = 900) -> subprocess.CompletedProcess:
    return subprocess.run(["bash", "-e", "-c", script], cwd=cwd, env=env,
                          capture_output=True, text=True, timeout=timeout)


def _need(tool: str | None, what: str) -> str:
    if tool is None:
        pytest.skip(f"{what} not available")
    return tool


_PKT = re.compile(r"addr=([0-9a-f]{8}) len=(\d+) kind=(\d+)")


def _packets(text: str) -> list:
    """The packets printed in `text`, each checked against the constraints."""
    pkts = []
    for a, n, k in _PKT.findall(text):
        addr, ln, kind = int(a, 16), int(n), int(k)
        assert addr % 4 == 0 and 1 <= ln <= 16 and addr + ln <= 0x1000, (a, n, k)
        assert kind != 0 and (kind != 7 or ln > 8), (a, n, k)
        pkts.append((addr, ln, kind))
    return pkts


def _same_shape(shown: str, real: str) -> None:
    """A sample output on the site has the lines a real run prints."""
    def shape(t):
        return [re.sub(r"[0-9a-f]+", "N", ln) for ln in t.splitlines() if ln.strip()]
    assert shape(shown) == shape(real), f"site shows:\n{shown}\nrun prints:\n{real}"


# ------------------------------------------------------------------ #
# Getting started                                                      #
# ------------------------------------------------------------------ #

def test_install_python_block(shell_env: dict) -> None:
    b = _block("getting-started/install.md", "import dv_solve")
    r = subprocess.run([sys.executable, "-c", b.text], env=shell_env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


def test_install_source_commands(shell_env: dict) -> None:
    if not (_REPO / "build" / "dv-solve-smt2").is_file():
        pytest.skip("no source build in build/")
    for first in ("build/dv-solve-smt2 --version",
                  'PYTHONPATH=src python3 -c "import dv_solve; print(dv_solve.get_libdirs())"'):
        b = _block("getting-started/install.md", first)
        r = _sh(b.text, _REPO, shell_env, timeout=60)
        assert r.returncode == 0 and r.stdout.strip(), r.stdout + r.stderr
    assert "build" in r.stdout, r.stdout     # get_libdirs() finds the source build


def test_quickstart_python_output(shell_env: dict) -> None:
    shown = _block("getting-started/quickstart-python.md", "seed=1: x=188 lo=30 hi=221").text
    r = subprocess.run([sys.executable, str(_EXAMPLES / "quickstart.py")], env=shell_env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    _same_shape(shown, r.stdout)
    for line in shown.splitlines():
        x, lo, hi = map(int, re.match(r"seed=\d+: x=(\d+) lo=(\d+) hi=(\d+)", line).groups())
        assert 100 < x <= 255 and lo < hi <= 255, line


def test_quickstart_smt2_command(shell_env: dict) -> None:
    _need(_smt2_exe(), "dv-solve-smt2")
    b = _block("getting-started/quickstart-smt2.md", "dv-solve-smt2 quickstart.smt2")
    r = _sh(b.text, _EXAMPLES / "smt2", shell_env, timeout=60)
    assert r.stdout == (_EXAMPLES / "smt2" / "quickstart.expected").read_text(), r.stdout + r.stderr


def test_quickstart_smt2_session(shell_env: dict) -> None:
    """Replay the interactive session: send its commands, compare the answers."""
    exe = _need(_smt2_exe(), "dv-solve-smt2")
    lines = _block("getting-started/quickstart-smt2.md", "$ dv-solve-smt2").body[1:]
    is_cmd = [ln.startswith("(") and not ln.startswith("((") for ln in lines]
    cmds = [ln for ln, c in zip(lines, is_cmd) if c]
    answers = [ln for ln, c in zip(lines, is_cmd) if not c]
    r = subprocess.run([exe], input="\n".join(cmds) + "\n", capture_output=True,
                       text=True, timeout=60)
    assert r.stdout.splitlines() == answers, r.stdout + r.stderr


def _verilator_run(src: Path, tmp: Path, env: dict, build: str, run: str) -> str:
    shutil.copy(src, tmp / src.name)
    b = _sh(build, tmp, env)
    assert b.returncode == 0, b.stdout[-2000:] + b.stderr[-2000:]
    r = _sh(run.replace("/path/to/dv-solve-smt2", _smt2_exe()), tmp, env, timeout=120)
    assert r.returncode == 0, r.stdout + r.stderr
    return r.stdout + r.stderr


def test_quickstart_verilator(shell_env: dict, tmp_path: Path) -> None:
    _need(_verilator(), "verilator")
    _need(_smt2_exe(), "dv-solve-smt2")
    page = "getting-started/quickstart-verilator.md"
    build = _block(page, "verilator --binary packet.sv").text
    run = _block(page, 'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 '
                       '--interactive --mode=verilator"').text
    shown = _block(page, "addr=00000a1c len=5 kind=14").text
    out = _verilator_run(_EXAMPLES / "verilator" / "packet.sv", tmp_path, shell_env, build, run)
    assert "Warning" not in out, out            # no solver errors, no unknowns
    assert len(_packets(shown)) == 5 and len(_packets(out)) == 5, out
    assert len(set(_packets(out))) > 1, out

    # The same seed repeats a run; a different seed gives different values.
    seeded = _block(page, "./obj_dir/Vpacket +verilator+seed+1").text
    export = run.splitlines()[0].replace("/path/to/dv-solve-smt2", _smt2_exe())
    runs = [_sh(export + "\n" + cmd, tmp_path, shell_env, timeout=120).stdout
            for cmd in (seeded, seeded, seeded.replace("seed+1", "seed+2"))]
    pkts = [_packets(o) for o in runs]
    assert len(pkts[0]) == 5 and pkts[0] == pkts[1] and pkts[0] != pkts[2], runs


# ------------------------------------------------------------------ #
# Guides                                                               #
# ------------------------------------------------------------------ #

def test_verilator_guide_setup() -> None:
    guide = _block("guides/verilator.md", 'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 '
                                          '--interactive --mode=verilator"').text
    quick = _block("getting-started/quickstart-verilator.md",
                   'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 '
                   '--interactive --mode=verilator"').body[0]
    assert guide.strip() == quick, "the guide and the quick start set VERILATOR_SOLVER differently"


def test_verilator_guide_hash_ignore() -> None:
    """The documented VERILATOR_SOLVER line, given a parity query that
    contradicts the one before it, keeps answering sat (the parity constraint
    is skipped), where the default answers unsat."""
    exe = _need(_smt2_exe(), "dv-solve-smt2")
    line = _block("guides/verilator.md", 'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 '
                                         '--interactive --mode=verilator '
                                         '--verilator-hash=ignore"').body[0]
    args = line.split('"')[1].split()[1:]
    script = ("(set-logic QF_ABV)(declare-fun x () (_ BitVec 8))"
              "(assert (= #b1 (ite (bvult x #x10) #b1 #b0)))(check-sat)"
              "(assert (= #b1 ((_ extract 0 0) x)))(check-sat)"
              "(assert (= #b0 ((_ extract 0 0) x)))(check-sat)")
    for a, want in ((args, "sat"), ([x for x in args if "hash" not in x], "unsat")):
        r = subprocess.run([exe, *a], input=script, capture_output=True, text=True, timeout=30)
        assert r.stdout.split()[-1] == want, (a, r.stdout)


_VLT_RUN = 'export VERILATOR_SOLVER="/path/to/dv-solve-smt2 --interactive --mode=verilator"\n'


def test_verilator_guide_unsat(shell_env: dict, tmp_path: Path) -> None:
    _need(_verilator(), "verilator")
    _need(_smt2_exe(), "dv-solve-smt2")
    shown = _block("guides/verilator.md", "%Warning-UNSATCONSTR: bad.sv:3: Unsatisfied constraint: "
                                          "'constraint c1 { x > 10; }'").body
    out = _verilator_run(_EXAMPLES / "verilator" / "bad.sv", tmp_path, shell_env,
                         "verilator --binary bad.sv", _VLT_RUN + "./obj_dir/Vbad")
    assert out.splitlines()[:len(shown)] == shown, out


# A variable wider than 192 bits is beyond what dv-solve supports (see the
# SMT-LIB2 guide), so Verilator gets `unknown`. If this starts solving, pick
# another unsupported construct.
_UNKNOWN_SV = """\
class Item;
  rand bit [255:0] w, v;
  constraint c { w * v == 256'd12345678901234567890123; }
endclass
module top;
  initial begin
    automatic Item it = new;
    if (it.randomize() == 0) $display("randomize() failed");
    $finish;
  end
endmodule
"""


def test_verilator_guide_unknown(shell_env: dict, tmp_path: Path) -> None:
    _need(_verilator(), "verilator")
    _need(_smt2_exe(), "dv-solve-smt2")
    shown = _block("guides/verilator.md", "%Warning: .../verilated_random.cpp:362: Solver returned "
                                          "unknown (timed out or incomplete), so randomize() "
                                          "may return 0; warned once").body[0]
    src = tmp_path / "src" / "unk.sv"
    src.parent.mkdir()
    src.write_text(_UNKNOWN_SV)
    out = _verilator_run(src, tmp_path, shell_env, "verilator --binary unk.sv",
                         _VLT_RUN + "./obj_dir/Vunk")
    warns = [re.sub(r"^%Warning: \S*/verilated_random", "%Warning: .../verilated_random", ln)
             for ln in out.splitlines() if ln.startswith("%Warning")]
    assert warns and warns[0] == shown, out
    assert "randomize() failed" in out, out


def test_dpi_guide(shell_env: dict, tmp_path: Path) -> None:
    _need(_verilator(), "verilator")
    page = "guides/systemverilog-dpi.md"
    gen = _block(page, "python3 packet_problem.py        # writes packet_problem_pkg.sv").text
    build = _block(page, "SV=$(python3 -c 'import dv_solve; print(dv_solve.get_svdirs()[0])')").text
    shown = _block(page, "addr=0000091c len=4 kind=8").text
    for f in ("packet_problem.py", "packet_dpi.sv"):
        shutil.copy(_EXAMPLES / "sv-dpi" / f, tmp_path / f)
    g = _sh(gen, tmp_path, shell_env, timeout=60)
    assert g.returncode == 0, g.stdout + g.stderr
    r = _sh(build, tmp_path, shell_env)
    out = r.stdout + r.stderr
    assert r.returncode == 0, out[-4000:]
    _same_shape(shown, "\n".join(ln for ln in out.splitlines()
                                 if ln.startswith(("addr=", "pinned:"))))
    assert len(_packets(shown)) == 5 and len(_packets(out)) == 5, out
    assert len(set(_packets(out))) > 1, out
    for text in (shown, out):
        pinned = [int(n) for n in re.findall(r"pinned: len=(\d+)", text)]
        assert len(pinned) == 3 and all(8 < n <= 16 for n in pinned), text


def test_packaging_link_line(shell_env: dict, tmp_path: Path) -> None:
    if shutil.which("cc") is None:
        pytest.skip("no C compiler")
    b = _block("guides/packaging.md", "cc app.c $(python3 -c 'import dv_solve as d;")
    shutil.copy(_EXAMPLES / "c" / "packet.c", tmp_path / "app.c")
    c = _sh(b.text, tmp_path, shell_env, timeout=120)
    assert c.returncode == 0, c.stdout + c.stderr
    libdirs = subprocess.run(
        [sys.executable, "-c", "import dv_solve, os; print(os.pathsep.join(dv_solve.get_libdirs()))"],
        env=shell_env, capture_output=True, text=True, timeout=60).stdout.strip()
    r = subprocess.run([str(tmp_path / "a.out")], env=dict(shell_env, LD_LIBRARY_PATH=libdirs),
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and len(_packets(r.stdout)) == 5, r.stdout + r.stderr


# ------------------------------------------------------------------ #
# Reference                                                            #
# ------------------------------------------------------------------ #

def test_c_api_output(shell_env: dict, tmp_path: Path) -> None:
    """Build packet.c as strict C99 and run it; the page shows its output."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    shown = _block("reference/c-api.md", "addr=000001f4 len=9 kind=13").text
    q = subprocess.run(
        [sys.executable, "-c",
         "import dv_solve as d; print(' '.join(['-I'+p for p in d.get_incdirs()]"
         " + ['-L'+p for p in d.get_libdirs()] + ['-Wl,-rpath,'+p for p in d.get_libdirs()]"
         " + ['-l'+l for l in d.get_libs()]))"],
        capture_output=True, text=True, env=shell_env, timeout=60)
    assert q.returncode == 0, q.stderr
    exe = tmp_path / "packet"
    b = subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-pedantic",
                        str(_EXAMPLES / "c" / "packet.c"), "-o", str(exe)] + q.stdout.split(),
                       capture_output=True, text=True, timeout=120)
    assert b.returncode == 0, b.stdout + b.stderr
    r = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stdout + r.stderr
    _same_shape(shown, r.stdout)
    assert len(_packets(shown)) == 5 and len(_packets(r.stdout)) == 5, r.stdout
    assert len(set(_packets(r.stdout))) > 1, r.stdout


def test_cli_usage() -> None:
    exe = _need(_smt2_exe(), "dv-solve-smt2")
    shown = _block("reference/cli.md", "dv-solve-smt2 [options] [file.smt2]").body[0]
    r = subprocess.run([exe, "--help"], capture_output=True, text=True, timeout=30)
    usage = (r.stdout + r.stderr).splitlines()[0]
    assert usage == "Usage: " + shown, usage


def _c_decls() -> dict:
    """The C declarations on the C API page, by directive."""
    text = (_SITE / "reference" / "c-api.md").read_text()
    out = {}
    for kind, sig in re.findall(r"^`{3,}\{c:(\w+)\}\s*(.+)$", text, re.M):
        out.setdefault(kind, []).append(sig.strip())
    return out


def _header_text() -> str:
    text = (_REPO / "src" / "c" / "dv_solve.h").read_text()
    return re.sub(r"/\*.*?\*/", " ", text, flags=re.S)


def test_c_api_matches_header(tmp_path: Path) -> None:
    """Compile every documented declaration against dv_solve.h.

    Each function is redeclared with its documented signature, each struct
    member and enumerator is used, and each macro is tested with #ifndef. A
    signature that differs from the header is a conflicting declaration and
    fails to compile.
    """
    cc = shutil.which("cc") or shutil.which("gcc")
    if cc is None:
        pytest.skip("no C compiler")
    decls = _c_decls()
    text = (_SITE / "reference" / "c-api.md").read_text()
    src = [_block("reference/c-api.md", '#include "dv_solve.h"').text,
           "#include <stddef.h>"]
    for sig in decls["function"]:
        src.append(sig + ";")
    for sig in decls["type"]:
        parts = sig.split()
        if len(parts) == 1:
            src.append(f"typedef {parts[0]} *_chk_{parts[0]};")       # declared
        else:
            src.append(f"typedef {sig};")                              # same type again
    for name in decls["macro"]:
        src.append(f"#ifndef {name}\n#error {name} is not defined\n#endif")
    for name in decls["enumerator"]:
        src.append(f"int _chk_{name} = (int){name};")
    # Members: each is checked against the type of the struct's field.
    for m in re.finditer(r"^(`{4,})\{c:struct\}\s*(\w+)\n(.*?)^\1$", text, re.M | re.S):
        struct, body = m.group(2), m.group(3)
        for mtype, mname in re.findall(r"\{c:member\}\s*(.+?)\s+(\w+)\s*$", body, re.M):
            src.append(f"_Static_assert(_Generic(((({struct} *)0)->{mname}), {mtype}: 1, "
                       f"default: 0), \"{struct}.{mname} is not {mtype}\");")
    f = tmp_path / "chk.c"
    f.write_text("\n".join(src) + "\n")
    r = subprocess.run([cc, "-std=c11", "-Wall", "-Werror", "-pedantic", "-fsyntax-only",
                        "-I", str(_REPO / "src" / "c"), str(f)],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr


def test_c_api_documents_whole_header() -> None:
    """Every function, type, macro and enumerator in dv_solve.h is on the page."""
    hdr = _header_text()
    page = (_SITE / "reference" / "c-api.md").read_text()
    documented = {re.search(r"(\w+)\s*\(", s).group(1) for s in _c_decls()["function"]}
    declared = set(re.findall(r"^[\w ]+?\**\s*\b(dvs_\w+)\s*\(", hdr, re.M))
    assert declared == documented, (sorted(declared - documented), sorted(documented - declared))
    names = set(re.findall(r"\b(DVS_[A-Z0-9_]+|dvs_\w+_t)\b", hdr))
    names.discard("DV_SOLVE_H")
    absent = sorted(n for n in names if not re.search(rf"\b{n}\b", page))
    assert not absent, f"in dv_solve.h but not on the C API page: {absent}"
    for struct in ("dvs_dist_entry_t", "dvs_solve_opts_t"):
        body = re.search(r"typedef struct \{([^}]*)\}\s*" + struct, hdr).group(1)
        members = [m for m in re.findall(r"(\w+)(?:\[\d+\])?;", body) if not m.startswith("_")]
        absent = [m for m in members if f"`{m}`" not in page and not re.search(
            rf"\{{c:member\}}.*\b{m}\s*$", page, re.M)]
        assert not absent, f"{struct} members not on the page: {absent}"


def _sv_decls(path: Path) -> set:
    text = re.sub(r"//.*", "", path.read_text())
    decls = re.findall(r"(?:import \"DPI-C\"\s+)?(?<!\w)((?:pure\s+)?(?:virtual\s+)?(?:local\s+)?"
                       r"(?:function|class)\b[^;]*;)", text)
    return {re.sub(r"\(\s+", "(", re.sub(r"\s+\)", ")", " ".join(d.split()))) for d in decls}


def test_sv_api_matches_packages() -> None:
    page_text = (_SITE / "reference" / "sv-api.md").read_text()
    shown = {" ".join(b.text.split()) for b in _parse(_SITE / "reference" / "sv-api.md")
             if b.kind == "systemverilog"}
    shown |= {" ".join(s.split()) for s in re.findall(r"^`((?:pure |virtual )*function [^`]+;)`",
                                                     page_text, re.M)}
    dpi = _sv_decls(_REPO / "src" / "sv" / "dvs_dpi_pkg.sv")
    rnd = _sv_decls(_REPO / "src" / "sv" / "dvs_randomizer_pkg.sv")
    wrong = sorted(s for s in shown if s not in dpi | rnd)
    assert not wrong, f"declarations on the page that the packages do not have: {wrong}"
    public = dpi | {d for d in rnd if not d.startswith(("local ", "function new"))}
    absent = sorted(d for d in public if d not in shown)
    assert not absent, f"package declarations not on the page: {absent}"


# ------------------------------------------------------------------ #
# README                                                               #
# ------------------------------------------------------------------ #

_README = _REPO / "README.md"
_SITE_URL = "https://dvkit.org/fvutils/dv-solve/"


def test_readme_quickstart(shell_env: dict) -> None:
    code = re.search(r"^```python\n(.*?)^```", _README.read_text(), re.M | re.S).group(1)
    r = subprocess.run([sys.executable, "-c", code], env=shell_env,
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0 and "x = " in r.stdout, r.stdout + r.stderr


def test_readme_links_reach_pages() -> None:
    links = re.findall(re.escape(_SITE_URL) + r"([\w/.-]*)", _README.read_text())
    assert links
    for path in links:
        if path:
            assert (_SITE / (path + ".md")).is_file(), f"README links to a missing page: {path}"
