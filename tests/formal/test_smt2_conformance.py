"""SMT-LIB front-end conformance: errors, exit status, define-sort, Bool values.

Phase 1 of docs/fw_hdl_implementation_plan.md (decisions D1-D3):

- an input error is an `(error "line L column C: ...")` response on stdout;
- after one, the session gives no verdict: check-sat answers `unknown` and
  `(get-info :reason-unknown)` names the error;
- exit status 0 sat, 1 unsat, 2 unknown / no check-sat, 4 input error,
  5 usage error; `--exit-codes=satcomp` gives 10/20/0;
- nothing is written to stderr unless asked for (--stats, -v, DV_LOG).
"""
from __future__ import annotations

import os
import subprocess
from pathlib import Path

import pytest

_REPO = Path(__file__).resolve().parents[2]
_EXE = _REPO / "build" / "dv-solve-smt2"

pytestmark = pytest.mark.skipif(not _EXE.is_file(), reason="build/dv-solve-smt2 not built")

_HDR = "(set-logic QF_BV)\n"


def _run(text: str, *args: str, mode: str = "batch", env: dict | None = None):
    """Run `text` in batch (a file) or interactive (stdin) mode."""
    e = dict(os.environ)
    e.pop("DV_LOG", None)
    e.update(env or {})
    flag = "--batch" if mode == "batch" else "--interactive"
    p = subprocess.run([str(_EXE), flag, *args], input=text, capture_output=True,
                       text=True, timeout=60, env=e)
    return p.returncode, [ln for ln in p.stdout.splitlines() if ln.strip()], p.stderr


def _errors(lines):
    return [ln for ln in lines if ln.startswith("(error ")]


# ---- parse errors (R2) -----------------------------------------------------

@pytest.mark.parametrize("mode", ["batch", "interactive"])
@pytest.mark.parametrize("text,where,what", [
    (_HDR + "(declare-const a (_ BitVec 4)))\n(assert (= a #x1))\n(check-sat)\n",
     "line 2 column 31", "unexpected ')'"),
    (_HDR + "(declare-const a (_ BitVec 4))\n(assert (= a #x1)))\n(check-sat)\n",
     "line 3 column 19", "unexpected ')'"),
    (_HDR + "(declare-const a (_ BitVec 4))\n(assert (= a #xFG))\n(check-sat)\n",
     "line 3 column 14", "invalid bit-vector literal '#xFG'"),
    (_HDR + "(declare-const a (_ BitVec 4))\n(assert (= a #b012))\n(check-sat)\n",
     "line 3 column 14", "invalid bit-vector literal"),
    (_HDR + "(declare-const a (_ BitVec 4))\n(assert (= a ((_ extract 3ab 0) a)))\n(check-sat)\n",
     "line 3 column 26", "invalid numeral"),
])
def test_parse_error_reported_and_no_verdict(mode, text, where, what):
    rc, out, _ = _run(text, mode=mode)
    errs = _errors(out)
    assert errs and where in errs[0] and what in errs[0], out
    assert "sat" not in out and "unsat" not in out, out
    assert "unknown" in out, out
    assert rc == 4


def test_unterminated_quoted_symbol():
    """It runs to the end of input, swallowing the check-sat: no verdict."""
    rc, out, _ = _run(_HDR + "(declare-const |a (_ BitVec 4))\n(check-sat)\n")
    assert len(out) == 1 and "line 2 column 16: unterminated quoted symbol" in out[0], out
    assert rc == 4


@pytest.mark.parametrize("mode", ["batch", "interactive"])
def test_quoted_symbol_with_parens(mode):
    text = _HDR + "(declare-const |a(1)| (_ BitVec 4))\n(assert (= |a(1)| #x1))\n(check-sat)\n"
    rc, out, _ = _run(text, mode=mode)
    assert out == ["sat"] and rc == 0


@pytest.mark.parametrize("mode", ["batch", "interactive"])
def test_unclosed_paren_at_end_of_input(mode):
    rc, out, _ = _run(_HDR + "(declare-const a (_ BitVec 4))\n(check-sat)\n(assert (= a #x1)\n",
                      mode=mode)
    assert out[0] == "sat"
    assert "line 4 column 1" in out[1] and "never closed" in out[1], out
    assert rc == 4


def test_parser_resyncs_after_error():
    """Every error is reported, and the commands after them still run."""
    text = (_HDR + "(declare-const a (_ BitVec 4)))\n(assert (= a #xZZ))\n"
            "(echo \"still here\")\n(check-sat)\n")
    rc, out, _ = _run(text)
    assert len(_errors(out)) == 2 and "'#xZZ'" in out[1]
    assert '"still here"' in out and out[-1] == "unknown"
    assert rc == 4


def test_atom_command_is_an_error():
    rc, out, _ = _run(_HDR + "foo\n(check-sat)\n")
    assert "invalid command, '(' expected" in out[0] and out[-1] == "unknown"
    assert rc == 4


def test_error_cap():
    rc, out, _ = _run(_HDR + ")\n" * 30 + "(check-sat)\n")
    errs = _errors(out)
    assert len(errs) == 21 and "too many errors" in errs[-1]
    assert out[-1] == "unknown" and rc == 4


# ---- unsupported and malformed commands -----------------------------------

@pytest.mark.parametrize("cmd,what", [
    ("(frobnicate)", "unsupported command 'frobnicate'"),
    ("(declare-datatype D ((c)))", "unsupported command 'declare-datatype'"),
    ("(declare-sort S 1)", "unsupported: declare-sort with arity > 0"),
    ("(declare-const)", "declare-const requires name and sort"),
    ("(define-fun f)", "define-fun requires name"),
    ("(assert)", "assert requires exactly one expression"),
    ("(pop 1)", "pop: stack underflow"),
])
def test_bad_command_poisons(cmd, what):
    rc, out, _ = _run(_HDR + cmd + "\n(check-sat)\n")
    assert what in out[0], out
    assert out[-1] == "unknown" and rc == 4


def test_unsupported_logic_is_an_error():
    rc, out, _ = _run("(set-logic QF_LIA)\n(check-sat)\n")
    assert "unsupported logic 'QF_LIA'" in out[0] and out[-1] == "unknown" and rc == 4


def test_unknown_symbol_in_assert_is_an_error():
    rc, out, _ = _run(_HDR + "(declare-const a (_ BitVec 4))\n(assert (= b #x1))\n(check-sat)\n")
    assert "unknown variable 'b'" in out[0] and out[-1] == "unknown" and rc == 4


def test_unsupported_sort_stays_an_honest_unknown():
    """An unsupported construct (not malformed input) is `unknown`, exit 2,
    and its later uses are not reported as unknown symbols."""
    rc, out, _ = _run(_HDR + "(declare-const x Int)\n(assert (= x x))\n(check-sat)\n"
                      "(get-info :reason-unknown)\n")
    assert out[0] == "unknown" and not _errors(out), out
    assert out[1].startswith('(:reason-unknown "unsupported: '), out
    assert rc == 2


def test_reason_unknown_names_the_input_error():
    rc, out, _ = _run(_HDR + "(declare-const a (_ BitVec 4)))\n(check-sat)\n"
                      "(get-info :reason-unknown)\n")
    assert out[-1] == '(:reason-unknown "input error: line 2 column 31: unexpected \')\'")'


def test_reset_clears_poison_but_not_exit_status():
    text = (_HDR + ")\n(reset)\n" + _HDR +
            "(declare-const a (_ BitVec 4))\n(assert (= a #x1))\n(check-sat)\n")
    rc, out, _ = _run(text)
    assert out[-1] == "sat"
    assert rc == 4


# ---- exit status (D3) ------------------------------------------------------

_SAT = _HDR + "(declare-const a (_ BitVec 4))\n(assert (= a #x1))\n(check-sat)\n"
_UNSAT = _HDR + "(declare-const a (_ BitVec 4))\n(assert (distinct a a))\n(check-sat)\n"
_UNKNOWN = _HDR + "(declare-const x Int)\n(check-sat)\n"


@pytest.mark.parametrize("text,rc,satcomp", [
    (_SAT, 0, 10), (_UNSAT, 1, 20), (_UNKNOWN, 2, 0),
    (_HDR + "(declare-const a (_ BitVec 4))\n", 2, 0),     # no check-sat
    (_HDR + ")\n(check-sat)\n", 4, 4),
])
def test_exit_status(text, rc, satcomp):
    assert _run(text)[0] == rc
    assert _run(text, "--exit-codes=satcomp")[0] == satcomp


@pytest.mark.parametrize("args", [["--no-such-flag"], ["--engine=fast"],
                                  ["--exit-codes=posix"], ["/no/such/file.smt2"]])
def test_usage_error_exit_status(args):
    p = subprocess.run([str(_EXE), *args], capture_output=True, text=True, timeout=30)
    assert p.returncode == 5


def test_help_documents_exit_status():
    p = subprocess.run([str(_EXE), "--help"], capture_output=True, text=True, timeout=30)
    for line in ("0  sat", "1  unsat", "2  unknown", "4  the input had an error",
                 "5  usage error"):
        assert line in p.stdout


# ---- diagnostics channel (D1) ----------------------------------------------

def test_stderr_silent_by_default():
    _, _, err = _run(_HDR + "(declare-const x Int)\n(check-sat)\n")
    assert err == ""


def test_verbose_prints_diagnostics():
    _, _, err = _run(_HDR + "(declare-const x Int)\n(check-sat)\n", "-v")
    assert "unsupported sort" in err


def test_dv_log_path(tmp_path):
    log = tmp_path / "dv.log"
    _, _, err = _run(_HDR + "(declare-const x Int)\n(check-sat)\n", env={"DV_LOG": str(log)})
    assert err == "" and "unsupported sort" in log.read_text()


# ---- define-sort (R4) --------------------------------------------------------

@pytest.mark.parametrize("engine", ["auto", "bitblast", "cdcl"])
def test_define_sort_aliases(engine):
    text = _HDR + """
(define-sort W () (_ BitVec 8))
(define-sort W2 () W)
(define-sort B () Bool)
(define-fun inc ((x W2)) W (bvadd x #x01))
(define-fun big ((x W)) B (bvugt x #x80))
(declare-const a W2)
(declare-fun b () B)
(assert (= (inc a) #x91))
(assert (= b (big a)))
(check-sat)
(get-value (a b))
"""
    rc, out, _ = _run(text, f"--engine={engine}")
    assert out == ["sat", "((a #b10010000)", " (b true))"], out
    assert rc == 0


def test_define_sort_array_alias():
    text = """(set-logic QF_ABV)
(define-sort A () (_ BitVec 2))
(define-sort D () (_ BitVec 4))
(define-sort M () (Array A D))
(declare-const m M)
(assert (= (select m #b01) #x7))
(check-sat)
(get-value ((select m #b01)))
"""
    rc, out, _ = _run(text)
    assert out[0] == "sat" and out[1].endswith("#b0111))"), out


@pytest.mark.parametrize("cmd,what", [
    ("(define-sort (_ BitVec 4) () (_ BitVec 4))", "invalid sort definition, symbol expected"),
    ("(define-sort S (X) (_ BitVec 4))", "unsupported: define-sort with parameters"),
    ("(define-sort S () Real)", "unknown or unsupported sort"),
    ("(define-sort Bool () (_ BitVec 1))", "already defined"),
    ("(define-sort S () (_ BitVec 4))\n(define-sort S () (_ BitVec 4))", "already defined"),
    ("(define-sort S ())", "malformed define-sort"),
])
def test_define_sort_errors(cmd, what):
    rc, out, _ = _run(_HDR + cmd + "\n(check-sat)\n")
    assert what in _errors(out)[0], out
    assert out[-1] == "unknown" and rc == 4


# ---- get-value / get-model (R1 echo, R5) ---------------------------------------

def test_bool_values_and_terms():
    text = _HDR + """(declare-const b Bool)
(declare-const a (_ BitVec 4))
(assert b)
(assert (= a #x5))
(check-sat)
(get-value (b))
(get-value ((not b)))
(get-value ((bvult a #x3)))
(get-model)
"""
    rc, out, _ = _run(text)
    assert out[1] == "((b true))"
    assert out[2] == "(((not b) false))"
    assert out[3] == "(((bvult a #x3) false))"
    assert "  (define-fun b () Bool true)" in out


def test_term_echo_is_z3_style():
    text = _HDR + """(declare-const a (_ BitVec 4))
(declare-const c (_ BitVec 3))
(assert (= a #x5))
(assert (= c #b101))
(check-sat)
(get-value ((bvadd a #b0001) (bvadd c #b001)))
"""
    _, out, _ = _run(text)
    assert out[1] == "(((bvadd a #x1) #b0110)"
    assert out[2] == " ((bvadd c #b001) #b110))"


@pytest.mark.parametrize("pre,query,what", [
    ("", "(get-value (a))", "no model available"),
    ("(check-sat)", "(get-value ())", "non-empty list of terms"),
    ("(check-sat)", "(get-value (nosuch))", "cannot evaluate 'nosuch'"),
    ("", "(get-model)", "no model available"),
])
def test_query_errors_do_not_poison(pre, query, what):
    """A query that cannot be answered is an error, but the assertions are
    intact: a later check-sat still answers."""
    text = _HDR + "(declare-const a (_ BitVec 4))\n" + pre + "\n" + query + "\n(check-sat)\n"
    rc, out, _ = _run(text)
    assert what in _errors(out)[0], out
    assert out[-1] == "sat" and rc == 4


def test_unevaluable_term_never_gets_a_partial_response():
    text = _HDR + """(declare-const a (_ BitVec 4))
(define-fun f ((x (_ BitVec 4))) (_ BitVec 4) (bvadd x #x1))
(check-sat)
(get-value (a (f a)))
"""
    _, out, _ = _run(text)
    assert out[1].startswith("(error ") and "(f a)" in out[1], out
    assert len(out) == 2
