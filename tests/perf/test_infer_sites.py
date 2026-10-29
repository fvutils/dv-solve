"""Tests for infer_sites.py on small hand-written captures."""
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from infer_sites import infer  # noqa: E402

PRE = "(reset)\n(set-option :produce-models true)\n(set-logic QF_ABV)\n"


def _call(body):
    return PRE + body + "(check-sat)\n(get-value ( a))\n"


def test_state_var_and_dist_slot():
    text = ""
    for k, d in ((1, 7), (2, 9), (3, 9)):
        text += _call(
            "(declare-fun a () (_ BitVec 8))\n"
            "(declare-fun s () (_ BitVec 8))\n"
            "(assert (= s #x%02x))\n"
            "(assert (= #b1 (__Vbv (bvult a s))))\n"
            "(push 1)\n"
            "(assert (ite (bvule #x%02x #x05) (= a #x00) (= a #x01)))\n"
            % (k, d))
    r = infer(text)
    assert r["calls"] == 3
    assert len(r["sites"]) == 1
    s = r["sites"][0]
    assert s["calls"] == 3
    assert s["state"] == ["s"]
    kinds = {x["slot"]: x["kind"] for x in s["slots"]}
    assert sorted(kinds.values()) == ["dist", "state"]


def test_constant_pin_is_not_state():
    text = "".join(_call("(declare-fun a () (_ BitVec 4))\n"
                         "(assert (= a #x3))\n") for _ in range(3))
    s = infer(text)["sites"][0]
    assert s["state"] == [] and s["slots"] == []


def test_randc_history_collapses_to_one_site():
    text, hist = "", []
    for v in (3, 5, 9):
        body = "(declare-fun a () (_ BitVec 4))\n"
        body += "".join("(assert (not (= a (_ bv%d 4))))\n" % h for h in hist)
        text += _call(body)
        hist.append(v)
    r = infer(text)
    assert len(r["sites"]) == 1
    assert r["sites"][0]["randc"] == ["a"]


def test_size_classes_share_family():
    text = ""
    for n in (1, 2, 1):
        body = "(declare-fun d () (Array (_ BitVec 32) (_ BitVec 8)))\n"
        body += "".join("(assert (bvult (select d #x%08x) #x10))\n" % i
                        for i in range(n))
        text += _call(body)
    r = infer(text)
    assert len(r["sites"]) == 2
    assert len({s["family"] for s in r["sites"]}) == 1
    assert sorted(s["calls"] for s in r["sites"]) == [1, 2]
