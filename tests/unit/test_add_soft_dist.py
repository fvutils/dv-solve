"""Soft constraints and distributions of a problem added to a compiled context.

``dvs_solver_add_constraint`` used to compile only the added problem's hard and
all-different constraints: its softs and dists were dropped and the add still
returned 0. A caller that compiles the variables once and adds the constraints
in force per solve (the zuspec C engine) then ignored every soft and dist.
They now behave as if they had been in the compiled problem; what an add
cannot take is refused with DVS_COMPILE_ADD_UNSUPPORTED.
"""
from dv_solve.ctx import SolveCtx
from dv_solve.problem import BIN_EQ, BIN_NEQ, SolveProblem

ADD_UNSUPPORTED = -7
SEEDS = range(1, 9)


def _base():
    sp = SolveProblem()
    sp.add_var(0, 4, False, 0, 15)
    return sp


def _eq(sp, k, op=BIN_EQ):
    return sp.expr_binary(op, sp.expr_var(0), sp.expr_const(k))


def _soft(k, prio):
    ap = _base()
    ap.add_soft_constraint(_eq(ap, k), prio)
    return ap


def _dist(k):
    ap = _base()
    ap.add_dist(0, [{"lo": k, "hi": k, "weight": 1, "is_per_value": 1}])
    return ap


def _vals(ctx):
    out = []
    for s in SEEDS:
        assert ctx.solve(seed=s) == 0
        out.append(ctx.get_value(0))
    return out


def test_added_soft_is_kept():
    with SolveCtx(_base()) as ctx:
        assert ctx.add_constraint(_soft(7, 0)) == 0
        assert _vals(ctx) == [7] * len(SEEDS)


def test_without_soft_values_vary():
    """The baseline the other tests rely on: x is free, so not always 7."""
    with SolveCtx(_base()) as ctx:
        assert len(set(_vals(ctx))) > 1


def test_softs_of_two_adds_are_ranked():
    for order in (((3, 1), (7, 0)), ((7, 0), (3, 1))):
        with SolveCtx(_base()) as ctx:
            for k, prio in order:
                assert ctx.add_constraint(_soft(k, prio)) == 0
            assert _vals(ctx) == [7] * len(SEEDS)


def test_added_soft_ranks_with_compiled_soft():
    for compiled_prio, added_prio, want in ((1, 0, 7), (0, 1, 3)):
        sp = _base()
        sp.add_soft_constraint(_eq(sp, 3), compiled_prio)
        with SolveCtx(sp) as ctx:
            assert ctx.add_constraint(_soft(7, added_prio)) == 0
            assert _vals(ctx) == [want] * len(SEEDS)


def test_added_hard_beats_added_soft():
    with SolveCtx(_base()) as ctx:
        assert ctx.add_constraint(_soft(7, 0)) == 0
        hard = _base()
        hard.add_constraint(_eq(hard, 7, BIN_NEQ))
        assert ctx.add_constraint(hard) == 0
        assert 7 not in _vals(ctx)


def test_added_soft_survives_reset():
    with SolveCtx(_base()) as ctx:
        assert ctx.add_constraint(_soft(7, 0)) == 0
        ctx.reset()
        assert _vals(ctx) == [7] * len(SEEDS)


def test_added_dist_is_drawn():
    with SolveCtx(_base()) as ctx:
        assert ctx.add_constraint(_dist(9)) == 0
        assert _vals(ctx) == [9] * len(SEEDS)


def test_first_added_dist_wins():
    sp = _base()
    sp.add_dist(0, [{"lo": 4, "hi": 4, "weight": 1, "is_per_value": 1}])
    with SolveCtx(sp) as ctx:
        assert ctx.add_constraint(_dist(9)) == 0
        assert _vals(ctx) == [4] * len(SEEDS)
    with SolveCtx(_base()) as ctx:
        assert ctx.add_constraint(_dist(9)) == 0
        assert ctx.add_constraint(_dist(4)) == 0
        assert _vals(ctx) == [9] * len(SEEDS)


def test_restore_removes_added_soft_and_dist():
    with SolveCtx(_base()) as ctx:
        free = _vals(ctx)
        for add, want in ((_soft(7, 0), 7), (_dist(9), 9)):
            for _ in range(3):
                cp = ctx.checkpoint()
                assert ctx.add_constraint(add) == 0
                assert _vals(ctx) == [want] * len(SEEDS)
                ctx.restore(cp)
                assert _vals(ctx) == free


def test_too_many_softs_refused():
    with SolveCtx(_base()) as ctx:
        ap = _base()
        for k in range(65):
            ap.add_soft_constraint(_eq(ap, k % 16), k)
        assert ctx.add_constraint(ap) == ADD_UNSUPPORTED
        assert len(set(_vals(ctx))) > 1
