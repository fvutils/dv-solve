"""A tour of the constraint builder: one small problem per feature."""
from collections import Counter

from dv_solve.builder import SolveProblemBuilder
from dv_solve.ctx import SolveCtx, SOLVE_OK
from dv_solve.problem import BIN_ADD, BIN_ASHR, BIN_EQ, BIN_GT, BIN_LT, BIN_RSHIFT


def solve(b, var_ids, seed=1):
    """Compile and solve a builder; return the values of `var_ids`."""
    problem, _ = b.finalize()
    with SolveCtx(problem) as ctx:
        assert ctx.solve(seed=seed, fair_pick=True) == SOLVE_OK
        return [ctx.get_value(v) for v in var_ids]


def u8(b, var_id):
    """Declare an unsigned 8-bit variable and return an expression for it."""
    b.add_var(var_id, width=8, is_signed=False, lo=0, hi=255)
    return b.expr_var(var_id)


# Membership: x is one of a set of values, y lies in a range.
b = SolveProblemBuilder()
x, y = u8(b, 0), u8(b, 1)
b.add_constraint(b.expr_in_set(x, [b.expr_const(v) for v in (3, 5, 9)]))
b.add_constraint(b.expr_in_range(y, b.expr_const(10), b.expr_const(20)))
xv, yv = solve(b, [0, 1])
assert xv in (3, 5, 9) and 10 <= yv <= 20

# Conditional: if mode == 1 then len > 8, else len < 4 (ite over two predicates).
b = SolveProblemBuilder()
mode, ln = u8(b, 0), u8(b, 1)
b.add_constraint(b.expr_in_range(mode, b.expr_const(0), b.expr_const(1)))
b.add_constraint(b.expr_ite(b.expr_binary(BIN_EQ, mode, b.expr_const(1)),
                            b.expr_binary(BIN_GT, ln, b.expr_const(8)),
                            b.expr_binary(BIN_LT, ln, b.expr_const(4))))
mv, lv = solve(b, [0, 1])
assert (lv > 8) if mv == 1 else (lv < 4)

# Bit slices: the low nibble of word is 0xA, and hi:lo concatenates to word.
b = SolveProblemBuilder()
word, hi, lo = u8(b, 0), u8(b, 1), u8(b, 2)
b.add_constraint(b.expr_binary(BIN_EQ, b.expr_extract(word, 3, 0), b.expr_const(0xA)))
b.add_constraint(b.expr_in_range(hi, b.expr_const(0), b.expr_const(15)))
b.add_constraint(b.expr_in_range(lo, b.expr_const(0), b.expr_const(15)))
b.add_constraint(b.expr_binary(BIN_EQ, b.expr_concat(hi, lo, 4), word))
wv, hv, lov = solve(b, [0, 1, 2])
assert wv & 0xF == 0xA and (hv << 4 | lov) == wv

# Widths follow SystemVerilog: the constant is 32 bits wide, so the 8-bit sum
# is evaluated at 32 bits and doesn't wrap. a + b == 300 is satisfiable, and
# x + 3 < 10 only holds for x up to 6.
b = SolveProblemBuilder()
a, c = u8(b, 0), u8(b, 1)
b.add_constraint(b.expr_binary(BIN_EQ, b.expr_binary(BIN_ADD, a, c), b.expr_const(300)))
av, cv = solve(b, [0, 1])
assert av + cv == 300

b = SolveProblemBuilder()
x = u8(b, 0)
b.add_constraint(b.expr_binary(BIN_LT, b.expr_binary(BIN_ADD, x, b.expr_const(3)), b.expr_const(10)))
assert all(solve(b, [0], seed=s)[0] <= 6 for s in range(1, 9))

# Shifts: for a signed x of -8, x >>> 1 is -4. x >> 1 is a logical shift of
# the 32-bit pattern, so it is a large positive number, not -4.
for op, expect in ((BIN_ASHR, -4), (BIN_RSHIFT, 0x7FFFFFFC)):
    b = SolveProblemBuilder()
    b.add_var(0, width=8, is_signed=True, lo=-8, hi=-8)
    b.add_var(1, width=32, is_signed=True, lo=-(1 << 31), hi=(1 << 31) - 1)
    b.add_var(2, width=32, is_signed=False, lo=0, hi=(1 << 32) - 1)
    r = 1 if op == BIN_ASHR else 2
    shifted = b.expr_binary(op, b.expr_var(0), b.expr_const(1))
    b.add_constraint(b.expr_binary(BIN_EQ, b.expr_var(r), shifted))
    assert solve(b, [r]) == [expect]

# All different, plus a sum: three distinct values that add up to 12.
b = SolveProblemBuilder()
vals = [u8(b, i) for i in range(3)]
for v in vals:
    b.add_constraint(b.expr_in_range(v, b.expr_const(0), b.expr_const(9)))
b.add_all_different([0, 1, 2])
b.add_var(3, width=8, is_signed=False, lo=12, hi=12)          # the total
b.add_constraint(b.expr_sum(b.expr_var(3), vals))
got = solve(b, [0, 1, 2])
assert len(set(got)) == 3 and sum(got) == 12

# Bit count: exactly three bits of flags are set.
b = SolveProblemBuilder()
flags = u8(b, 0)
b.add_var(1, width=8, is_signed=False, lo=3, hi=3)
b.add_constraint(b.expr_countones(b.expr_var(1), flags))
(fv,) = solve(b, [0])
assert bin(fv).count("1") == 3

# Soft constraints: kept when possible, dropped when they conflict.
# Priority 0 is the most important; higher numbers are dropped first.
b = SolveProblemBuilder()
p, q = u8(b, 0), u8(b, 1)
b.add_constraint(b.expr_binary(BIN_GT, p, b.expr_const(10)))                # hard
b.add_soft_constraint(b.expr_binary(BIN_EQ, p, b.expr_const(5)), priority=1)  # conflicts: dropped
b.add_soft_constraint(b.expr_binary(BIN_EQ, q, b.expr_const(7)), priority=0)  # kept
pv, qv = solve(b, [0, 1])
assert pv > 10 and qv == 7

# Weighted distribution: kind is 0 about three times as often as 1.
counts = Counter()
for seed in range(1, 401):
    b = SolveProblemBuilder()
    u8(b, 0)
    b.add_dist(0, [{"lo": 0, "hi": 0, "weight": 3},
                   {"lo": 1, "hi": 1, "weight": 1}])
    counts[solve(b, [0], seed=seed)[0]] += 1
assert set(counts) == {0, 1} and 2.0 < counts[0] / counts[1] < 4.5

print("all features OK")
