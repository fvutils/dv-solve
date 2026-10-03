"""A solve under checkpoints keeps to its own decision levels.

Each open checkpoint holds a decision level, so a solve made under them -- a
scenario generator's checkpoint, pin, checkpoint, solve, restore -- starts
above level 0. The search took level 0 as its root: it sealed that level's
mark, and its shave probes and restarts went back to 0, after which its
decisions reused the checkpoints' levels. With clause learning, a backjump
then reached a level whose mark was no longer on the trail, and the solve
crashed (or rewound past the pins made before it).

The search's root is now the level it starts at (``search_base``).

The problem below is the shrunk failure of the soundness campaign's protocol
door: x, y 3 bits, z 6 bits, with `y % 3 == 2` and `y / 4 >= x`. It runs in a
child process, so a crash fails the test rather than the test run.
"""
import subprocess
import sys
import textwrap

_SCRIPT = textwrap.dedent('''
    from dv_solve.builder import SolveProblemBuilder
    from dv_solve.ctx import SolveCtx, SOLVE_OK, SOLVE_UNSAT
    from dv_solve.problem import (BIN_DIV, BIN_EQ, BIN_GT, BIN_GTE, BIN_LSHIFT,
                                  BIN_BOR, BIN_MOD, BIN_OR)

    X, Y, Z = 0, 1, 2
    W = {X: 3, Y: 3, Z: 6}
    b = SolveProblemBuilder()
    for v, w in W.items():
        b.add_var(v, w, False, 0, (1 << w) - 1)
    V, bin_ = b.expr_var, b.expr_binary
    K = lambda c, w: b.expr_const(c, width=w)
    shl = bin_(BIN_LSHIFT, V(Z), V(Z))
    zsz = bin_(BIN_BOR, shl, V(Z))
    b.add_constraint(bin_(BIN_GT, V(Z), shl))
    b.add_constraint(bin_(BIN_GTE, zsz, zsz))
    eq = lambda a, c: bin_(BIN_EQ, a, c)
    b.add_constraint(bin_(BIN_OR, bin_(BIN_OR, bin_(BIN_OR, bin_(BIN_OR,
        eq(V(X), K(5, 3)), bin_(BIN_GT, V(X), V(Y))), eq(V(Y), K(4, 3))),
        bin_(BIN_GT, V(Z), V(Z))), eq(V(X), V(Y))))
    b.add_constraint(eq(bin_(BIN_MOD, V(Y), K(3, 3)), K(2, 3)))
    b.add_constraint(bin_(BIN_GTE, bin_(BIN_DIV, V(Y), K(4, 3)), V(X)))
    prob, _ = b.finalize()

    def ok(m, pin):
        x, y, z = m[X], m[Y], m[Z]
        s = (z << z) & 63
        return (z > s and (x == 5 or x > y or y == 4 or x == y) and y % 3 == 2
                and y // 4 >= x and (pin is None or m[pin[0]] == pin[1]))

    def feasible(pin):
        return any(ok({X: x, Y: y, Z: z}, pin)
                   for x in range(8) for y in range(8) for z in range(64))

    ctx = SolveCtx(prob)
    bad = []
    for k in range(1, 13):
        seed = k * 2654435761 % (1 << 32)
        for pin in [None] + [(v, c) for v in W for c in range(1 << W[v])]:
            cp = ctx.checkpoint()
            if pin is not None and not ctx.pin(pin[0], pin[1]):
                ctx.restore(cp)
                continue
            inner = ctx.checkpoint()
            rc = ctx.solve(seed=seed, max_restarts=5, use_lcg=True)
            if rc != SOLVE_OK:
                ctx.restore(inner)
                rc = ctx.solve(seed=seed, max_restarts=10000)
            if rc == SOLVE_OK:
                m = {v: ctx.get_value(v) for v in W}
                if not ok(m, pin):
                    bad.append(("bad model", k, pin, m))
            elif rc == SOLVE_UNSAT and feasible(pin):
                bad.append(("false unsat", k, pin))
            ctx.restore(cp)
    ctx.destroy()
    print("BAD", bad)
''')


def test_repeated_learning_solves_under_checkpoints():
    r = subprocess.run([sys.executable, "-c", _SCRIPT], capture_output=True,
                       text=True, timeout=300)
    assert r.returncode == 0, "child died (%d): %s" % (r.returncode, r.stderr[-2000:])
    assert "BAD []" in r.stdout, r.stdout[-2000:]
