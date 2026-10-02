"""Find which constraints conflict: drop each one in turn (no core API needed)."""
from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, CompileUnsatError, SOLVE_OK
from dv_solve.problem import BIN_GTE, BIN_LTE, BIN_LT

LEN, HDR = 0, 1

# Each constraint is a function that adds itself to a builder.
CONSTRAINTS = {
    "len_min": lambda b: b.expr_binary(BIN_GTE, b.expr_var(LEN), b.expr_const(64)),
    "len_max": lambda b: b.expr_binary(BIN_LTE, b.expr_var(LEN), b.expr_const(1500)),
    "hdr_fits": lambda b: b.expr_binary(BIN_LT, b.expr_var(HDR), b.expr_var(LEN)),
    "short": lambda b: b.expr_binary(BIN_LT, b.expr_var(LEN), b.expr_const(40)),
}


def satisfiable(names) -> bool:
    b = SolveProblemBuilder()
    b.add_var(LEN, width=16, is_signed=False, lo=0, hi=0xFFFF)
    b.add_var(HDR, width=16, is_signed=False, lo=0, hi=0xFFFF)
    for n in names:
        b.add_constraint(CONSTRAINTS[n](b))
    problem, _ = b.finalize()
    try:
        with SolveCtx(problem) as ctx:
            return ctx.solve() == SOLVE_OK
    except CompileUnsatError:
        return False


core = list(CONSTRAINTS)
assert not satisfiable(core)
# Drop each constraint in turn; keep it out if the rest is still unsatisfiable.
for name in list(core):
    rest = [n for n in core if n != name]
    if not satisfiable(rest):
        core = rest
print("conflict:", " ".join(core))
