"""Pick a random 8-bit value greater than 100, then a random pair lo < hi."""
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK
from dv_solve.problem import BIN_GT, BIN_LT

b = SolveProblemBuilder()

# Variables are identified by ids 0, 1, 2, ...
X, LO, HI = 0, 1, 2
b.add_var(X, width=8, is_signed=False, lo=0, hi=255)
b.add_var(LO, width=8, is_signed=False, lo=0, hi=255)
b.add_var(HI, width=8, is_signed=False, lo=0, hi=255)

# Constraints are expression trees: x > 100, lo < hi.
b.add_constraint(b.expr_binary(BIN_GT, b.expr_var(X), b.expr_const(100)))
b.add_constraint(b.expr_binary(BIN_LT, b.expr_var(LO), b.expr_var(HI)))

problem, _ = b.finalize()

# SolveCtx compiles the problem; solve() draws one solution per seed.
with SolveCtx(problem) as ctx:
    for seed in range(1, 4):
        ctx.reset()
        assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
        x, lo, hi = (ctx.get_value(v) for v in (X, LO, HI))
        assert x > 100 and lo < hi
        print(f"seed={seed}: x={x} lo={lo} hi={hi}")
