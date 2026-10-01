"""The snippets shown on the concepts pages, in a program that runs them.

The pages include the lines between each "# [name]" and "# [/name]" pair.
"""
from collections import Counter

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK
from dv_solve.problem import BIN_GT

# Variables and expressions (concepts/problem-model).
b = SolveProblemBuilder()
# [var]
b.add_var(0, width=8, is_signed=False, lo=0, hi=255)
# [/var]
# [expr]
x   = b.expr_var(0)
ten = b.expr_const(10)
b.add_constraint(b.expr_binary(BIN_GT, x, ten))    # x > 10
# [/expr]
problem, _ = b.finalize()
with SolveCtx(problem) as ctx:
    for seed in range(1, 9):
        ctx.reset()
        assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
        assert 10 < ctx.get_value(0) <= 255

# Weighted distribution (concepts/randomization-seeds).
KIND = 0
counts = Counter()
for seed in range(1, 401):
    b = SolveProblemBuilder()
    b.add_var(KIND, width=4, is_signed=False, lo=0, hi=15)
    # [dist]
    # kind is 0 about three times as often as 1
    b.add_dist(KIND, [{"lo": 0, "hi": 0, "weight": 3},
                      {"lo": 1, "hi": 1, "weight": 1}])
    # [/dist]
    problem, _ = b.finalize()
    with SolveCtx(problem) as ctx:
        assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
        counts[ctx.get_value(KIND)] += 1
assert set(counts) == {0, 1} and 2.0 < counts[0] / counts[1] < 4.5

print("concepts OK")
