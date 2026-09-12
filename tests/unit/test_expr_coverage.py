"""Every row of the two coverage probes, as CI.

The probes in `docs/` are diagnostic scripts: they print a table and say nothing
about pass or fail, which is the right shape for measuring a gap and the wrong
shape for keeping it closed. This module imports their case tables and asserts
that every row solves correctly on BOTH engines.

Importing rather than restating the shapes is deliberate. A probe row and a test
row that drift apart are worse than either alone -- the document would describe
coverage the suite does not actually hold.

What a row checks is what makes this worth running: each probe evaluates the
constraint in Python against the returned assignment, so a shape that compiles
but is enforced WRONGLY fails here rather than passing. That distinction is the
whole point -- the failure mode this suite exists to catch is a constraint that
reached no propagator and was therefore never enforced.
"""
import importlib.util
import os
import sys

import pytest

_DOCS = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                     "..", "..", "docs")


def _load(name):
    path = os.path.join(_DOCS, name + ".py")
    if not os.path.exists(path):
        pytest.skip("probe %s not present" % name)
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


_expr = _load("expr_coverage_probe")
_feat = _load("feature_coverage_probe")


@pytest.mark.parametrize(
    "cid,desc,fn,check",
    _expr.CASES,
    ids=[c[0] for c in _expr.CASES],
)
def test_expr_shape(cid, desc, fn, check):
    """An expression shape must compile AND be enforced, on both engines."""
    assert _expr.try_cdcl(fn, check) == "ok", "cdcl: %s" % desc
    assert _expr.try_bb(fn, check) == "ok", "bb: %s" % desc


# W8/W9 declare a variable wider than 64 bits. The propagator engine cannot
# search those: tier-2 storage exists, but trail_record_lb/ub refuse to tighten
# it, so a constraint on such a variable would compile and go unenforced. It
# must therefore DECLINE, which is what "UNSUPPORTED" asserts below.
#
# That distinction is the whole of G2. These rows used to report NO-SOLUTION:
# the int64 bound accessors read the WideBoundsN limb-count header as the lower
# bound, so a 65-bit variable with NO constraints at all had the empty domain
# [2, 0] and the solve reported UNSAT. A refusal is a worse answer than a
# correct one and a far better answer than a wrong one -- and the bit-blaster,
# asserted "ok" for these rows like every other, gives the correct one.
_CDCL_MUST_DECLINE = {"W8", "W9"}


@pytest.mark.parametrize(
    "cid,desc,fn,check",
    _expr.CASES,
    ids=[c[0] for c in _expr.CASES],
)
def test_expr_shape_validates(cid, desc, fn, check):
    """The independent post-solve check: re-evaluate every constraint in the
    problem against the assignment via solver_validate_model.

    This is a different question from the one above. There, the probe's own
    Python evaluation says the answer is right. Here the C evaluator walks the
    original constraint DAG -- so a propagator that agrees with the probe's
    arithmetic but disagrees with the problem as submitted is caught too. It is
    also the check the randomization path had no way to run at all until
    validate_model was exposed in the wrapper."""
    from dv_solve.ctx import SolveCtx, CompileIncompleteError, CompileUnsatError

    sp = _expr.build(fn)
    try:
        ctx = SolveCtx(sp)
    except (CompileIncompleteError, CompileUnsatError) as e:
        pytest.fail("%s: %s" % (desc, type(e).__name__))
    try:
        assert ctx.solve(seed=3) == 0, desc
        assert ctx.validate_model() == 0, (
            "%s: model violates the problem it was solved from" % desc)
    finally:
        ctx.destroy()


@pytest.mark.parametrize(
    "case", _feat.CASES, ids=[c["cid"] for c in _feat.CASES]
)
def test_feature_axis(case):
    """Signedness, width tier, membership, bitwise, AllDifferent, soft, dist,
    and both solve entry points."""
    bb = _feat.run_bb(case)
    assert bb == "ok", "bb: %s" % case["desc"]

    cdcl = _feat.run_cdcl(case)
    if case["cid"] in _CDCL_MUST_DECLINE:
        assert cdcl == "UNSUPPORTED", (
            "%s: cdcl must decline a >64-bit variable, not answer %r -- an "
            "UNSAT here is the G2 regression, and an 'ok' means tier-2 search "
            "landed and this row should move to the normal assertion"
            % (case["cid"], cdcl))
        return
    assert cdcl == "ok", "cdcl: %s" % case["desc"]
