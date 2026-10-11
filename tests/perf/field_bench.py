"""Field constraints on packed words: CDCL cost and spread, standalone API.

The shapes come from real SystemVerilog constraint code -- OpenTitan,
riscv-dv, core-v-verif / CVA6 and open protocol testbenches; see
docs/field_constraint_examples.md (E-numbers below) and
docs/bitfield_linearization_research.md sections 9-11.

Each shape is drawn N times (seeded, fair_pick) through SolveCtx; the report
gives the mean and p99 time per randomize and the Jensen-Shannon divergence
of one projection of the solutions from its exact (uniform over solutions)
distribution, next to the JSD an exact sampler gets with the same N.

    env -u LD_LIBRARY_PATH DVS_SOLVER_PATH=<build>/lib \\
        python tests/perf/field_bench.py [N] [name-substring ...]
"""
from __future__ import annotations

import math
import random
import sys
import time
from collections import Counter
from pathlib import Path

sys.path[:0] = [str(Path(__file__).resolve().parents[2]),
                str(Path(__file__).resolve().parents[2] / "src")]

from tests.perf.rand_benches import V, C, B, P, AND, OR, NOT, ZX  # noqa: E402
from tests.formal.soundness.doors import _lower                   # noqa: E402
from tests.formal.soundness.ir import ev                          # noqa: E402
from dv_solve.builder import SolveProblemBuilder                  # noqa: E402
from dv_solve.ctx import SolveCtx, SOLVE_OK, CompileIncompleteError  # noqa: E402


def EX(t, hi, lo): return ("extract", hi - lo + 1, hi, lo, t)            # noqa: E704
def IN(t, vals, w): return OR(*[P("=", t, C(v, w)) for v in vals])       # noqa: E704
def IMP(a, b): return ("=>", "bool", a, b)                               # noqa: E704
def CAT(*ts):                                                            # noqa: E302
    """{ts[0], ts[1], ...}, most significant first."""
    t = ts[-1]
    for h in reversed(ts[:-1]):
        t = ("concat", h[1] + t[1], h, t)
    return t


N_DRAWS = 2000
SHAPES = []


def shape(fn):
    SHAPES.append(fn)
    return fn


def _jsd(p, q):
    return sum(0.5 * (p[k] * math.log2(2 * p[k] / (p[k] + q[k])) if p[k] else 0)
               + 0.5 * (q[k] * math.log2(2 * q[k] / (p[k] + q[k])) if q[k] else 0)
               for k in q)


def run(name, widths, cons, key, truth, n=None):
    n = n or N_DRAWS
    names = sorted(widths)
    ids = {k: i for i, k in enumerate(names)}
    bld = SolveProblemBuilder()
    for k in names:
        w = widths[k]
        bld.add_var(ids[k], width=w, is_signed=False, lo=0, hi=(1 << w) - 1)
    for c in cons:
        bld.add_constraint(_lower(bld, c, ids))
    prob, _ = bld.finalize()
    rng = random.Random(7)
    ts, cnt, bad = [], Counter(), 0
    try:
        ctx = SolveCtx(prob)
    except CompileIncompleteError:
        print(f"{name:34s} DECLINED (CompileIncomplete)", flush=True)
        return
    with ctx:
        for _ in range(n):
            t0 = time.perf_counter()
            ctx.reset()
            rc = ctx.solve(seed=rng.getrandbits(63) + 1, fair_pick=True, time_limit_ms=2000)
            ts.append(time.perf_counter() - t0)
            if rc != SOLVE_OK:
                print(f"{name:34s} rc={rc}", flush=True)
                return
            m = {k: ctx.get_value(ids[k]) & ((1 << widths[k]) - 1) for k in names}
            if not all(ev(c, m) for c in cons):
                bad += 1
            cnt[key(m)] += 1
    tot = sum(truth.values())
    q = {k: v / tot for k, v in truth.items()}
    p = {k: cnt.get(k, 0) / n for k in q}
    stray = sum(v for k, v in cnt.items() if k not in q)
    sim = Counter(rng.choices(list(q), weights=list(q.values()), k=n))
    js0 = _jsd({k: sim[k] / n for k in q}, q)
    ts.sort()
    print(f"{name:34s} mean={sum(ts) / n * 1e6:9.1f}us p99={ts[int(.99 * n)] * 1e6:9.1f}us "
          f"jsd={_jsd(p, q):.3f} (exact {js0:.3f}) bins={len(q)} bad={bad} stray={stray}",
          flush=True)


# ---------------------------------------------------------------------------
# Corpus shapes (research doc section 9)

@shape
def mbx():
    """OpenTitan mbx_seq_item: aligned base/limit window."""
    b, l = V("b", 32), V("l", 32)
    N, S = 1 << 28, 1 << 24
    tr = {k: sum(max(0, N - 8 - i) for i in (k * S, (k + 1) * S - 1)) * S // 2 for k in range(16)}
    run("ot mbx base/limit", {"b": 32, "l": 32},
        [P("=", EX(b, 1, 0), C(0, 2)), P("=", EX(l, 1, 0), C(0, 2)),
         P("bvult", l, C(0x40000000, 32)), P("bvuge", l, b),
         P("bvuge", B("bvudiv", 32, B("bvsub", 32, l, b), C(4, 32)), C(8, 32))],
        lambda m: m["b"] >> 26, tr)


@shape
def dma():
    """OpenTitan dma_seq_item: 64-bit address, low/high fields, size link."""
    a, t = V("a", 64), V("t", 32)
    tr = Counter()
    for tv in range(4, 1025, 4):
        for k in range(16):
            lo, hi = max(k << 28, 0x1000), min(((k + 1) << 28) - 1, 0xFFFFFFFF - tv)
            tr[k] += max(0, (hi // 4) - ((lo + 3) // 4) + 1)
    run("ot dma src_addr (64b)", {"a": 64, "t": 32},
        [P("=", EX(a, 1, 0), C(0, 2)), P("=", EX(a, 63, 32), C(0, 32)),
         P("bvuge", a, C(0x1000, 64)), P("bvule", EX(a, 31, 0), ("bvnot", 32, t)),
         P("bvuge", t, C(1, 32)), P("bvule", t, C(1024, 32)),
         P("=", B("bvurem", 32, t, C(4, 32)), C(0, 32))],
        lambda m: m["a"] >> 28, dict(tr))


@shape
def tpm():
    """OpenTitan spi_device TPM: window, aligned, locality, register set."""
    x = V("x", 24)
    OFFS = [0x000, 0x004, 0x008, 0x014, 0x018, 0x024, 0xF00, 0xF04]
    run("ot tpm_addr", {"x": 24},
        [P("bvuge", x, C(0xD40000, 24)), P("bvule", x, C(0xD4FFFF, 24)),
         P("=", EX(x, 1, 0), C(0, 2)), P("bvult", EX(x, 15, 12), C(5, 4)),
         IN(B("bvand", 12, EX(x, 11, 0), C(0xFFC, 12)), OFFS, 12)],
        lambda m: ((m["x"] >> 12) & 0xF, m["x"] & 0xFFC),
        {(loc, o): 1 for loc in range(5) for o in OFFS})


def _riscv_illegal():
    x, co, cm = V("x", 32), V("co", 2), V("cm", 3)
    EXC = [0, 1, 2, 0x102, 0x302, 0x7b2, 0x105]
    return [P("=", EX(x, 1, 0), co), P("distinct", co, C(3, 2)), P("=", EX(x, 15, 13), cm),
            P("distinct", EX(x, 19, 15), C(0, 5)), P("distinct", EX(x, 11, 7), C(0, 5)),
            NOT(IN(EX(x, 31, 20), EXC, 12))], EXC


@shape
def riscv_illegal():
    """riscv-dv illegal compressed instruction: crossing slices, not split."""
    cons, EXC = _riscv_illegal()
    run("riscv-dv illegal instr_bin", {"x": 32, "co": 2, "cm": 3}, cons,
        lambda m: m["x"] >> 24, {k: 16 - sum(1 for e in EXC if e >> 4 == k) for k in range(256)})


@shape
def flash():
    """OpenTitan flash ctrl_data: three zero fields."""
    x = V("x", 32)
    run("ot flash ctrl_data", {"x": 32},
        [P("=", EX(x, 3, 1), C(0, 3)), P("=", EX(x, 15, 11), C(0, 5)), P("=", EX(x, 31, 28), C(0, 4))],
        lambda m: (m["x"] >> 16) & 0xFF, {k: 1 for k in range(256)})


@shape
def csr12():
    """riscv-dv csr: field exclusion + excluded registers."""
    c = V("c", 12)
    EXR = [0x300, 0x301, 0x302, 0x7b0, 0x7b1, 0x7b2, 0x180, 0xf14]
    tr = Counter(v >> 6 for v in range(4096) if (v >> 10) != 3 and v not in EXR)
    run("riscv-dv csr", {"c": 12},
        [P("distinct", EX(c, 11, 10), C(3, 2)), NOT(IN(c, EXR, 12))], lambda m: m["c"] >> 6, dict(tr))


@shape
def addr_offset():
    """riscv-dv / CVA6 load-store page (E9): 4K aligned, one bit above 20."""
    x = V("x", 32)
    run("riscv-dv addr_offset (E9)", {"x": 32},
        [P("=", EX(x, 11, 0), C(0, 12)), P("=", EX(x, 31, 31), C(0, 1)),
         P("distinct", EX(x, 30, 20), C(0, 11))],
        lambda m: m["x"] >> 27, {k: (1 << 15) - (1 << 8 if k == 0 else 0) for k in range(16)})


# ---------------------------------------------------------------------------
# Shapes from docs/field_constraint_examples.md

LUI, AUIPC, JAL, JALR, BRANCH, LOAD, STORE, OPIMM, OP, FENCE, SYSTEM = \
    0x37, 0x17, 0x6F, 0x67, 0x63, 0x03, 0x23, 0x13, 0x33, 0x0F, 0x73
OPCODES = [LUI, AUIPC, JAL, JALR, BRANCH, LOAD, STORE, OPIMM, OP, FENCE, SYSTEM]


def _rv32(imm):
    i = V("i", 32)
    op, f3 = EX(i, 6, 0), EX(i, 14, 12)
    is_ = lambda o: P("=", op, C(o, 7))                                 # noqa: E731
    cons = [IN(op, OPCODES, 7),
            IMP(is_(BRANCH), IN(f3, [0, 1, 4, 5, 6, 7], 3)),
            IMP(is_(LOAD), IN(f3, [0, 1, 2, 4, 5], 3)),
            IMP(AND(is_(OPIMM), P("=", EX(i, 13, 12), C(1, 2))), IN(EX(i, 31, 25), [0, 0x20], 7)),
            IMP(is_(OP), IN(EX(i, 31, 25), [0, 0x20], 7)),
            IMP(AND(is_(SYSTEM), P("=", f3, C(0, 3))), IN(EX(i, 31, 7), [0, 0x2000], 25)),
            IMP(AND(is_(BRANCH), P("=", f3, C(0, 3))),
                OR(P("distinct", EX(i, 19, 15), C(0, 5)), P("distinct", EX(i, 24, 20), C(0, 5)))),
            IMP(is_(JAL), P("distinct", EX(i, 11, 7), C(0, 5)))]
    if imm:
        cons.append(IMP(is_(BRANCH), P("distinct", CAT(EX(i, 31, 31), EX(i, 7, 7), EX(i, 30, 25),
                                                       EX(i, 11, 8)), C(0, 12))))
    tr = {}
    for o in OPCODES:
        for f in range(8):
            if o == JAL:
                n = 31 << 17
            elif o == BRANCH:
                n = 0 if f in (2, 3) else ((1 << 12) - (1 if imm else 0)) * (1023 if f == 0 else 1024)
            elif o == LOAD:
                n = 0 if f in (3, 6, 7) else 1 << 22
            elif o == OPIMM:
                n = 2 << 15 if f & 3 == 1 else 1 << 22
            elif o == OP:
                n = 2 << 15
            elif o == SYSTEM:
                n = 2 if f == 0 else 1 << 22
            else:
                n = 1 << 22
            if n:
                tr[(o, f)] = n
    return cons, tr


@shape
def rv32():
    """RV32I instruction word (E7): opcode set, dependent funct sets, nested
    slices ([13:12] in funct3, [31:7] around every field)."""
    cons, tr = _rv32(False)
    run("rv32 instr (E7)", {"i": 32}, cons, lambda m: (m["i"] & 0x7F, (m["i"] >> 12) & 7), tr)


@shape
def rv32_imm():
    """... plus the scattered B-immediate {i[31], i[7], i[30:25], i[11:8]} != 0 (E8)."""
    cons, tr = _rv32(True)
    run("rv32 instr + B-imm concat (E8)", {"i": 32}, cons,
        lambda m: (m["i"] & 0x7F, (m["i"] >> 12) & 7), tr)


def _csr_excl(n):
    x = V("x", 32)
    exc = sorted(random.Random(n).sample(range(4096), n))
    tr = {k: 256 - sum(1 for e in exc if e >> 8 == k) for k in range(16)}
    run(f"csr exclusion N={n} (E2/E3)", {"x": 32},
        [P("=", EX(x, 6, 0), C(0x73, 7)), IN(EX(x, 14, 12), [1, 2, 3, 5, 6, 7], 3),
         NOT(IN(EX(x, 31, 20), exc, 12))],
        lambda m: m["x"] >> 28, tr)


@shape
def csr_excl():
    """SYSTEM opcode, CSR funct3, a list of N CSR addresses excluded."""
    for n in (4, 16, 64):
        _csr_excl(n)


def _axi(form):
    a, s, ln = V("a", 32), V("s", 3), V("ln", 8)
    if form == "mask":
        align = [P("=", B("bvand", 32, a, B("bvsub", 32, B("bvshl", 32, C(1, 32), ZX(s, 32)),
                                             C(1, 32))), C(0, 32))]
    elif form == "slice":
        align = [IMP(P("=", s, C(k, 3)), P("=", EX(a, k - 1, 0), C(0, k))) for k in range(1, 8)]
    else:
        align = [P("=", B("bvurem", 32, a, B("bvshl", 32, C(1, 32), ZX(s, 32))), C(0, 32))]
    bnd = P("bvule", B("bvadd", 20, ZX(EX(a, 11, 0), 20),
                       B("bvshl", 20, B("bvadd", 20, ZX(ln, 20), C(1, 20)), ZX(s, 20))),
            C(4096, 20))
    region = [P("bvuge", a, C(0x10000000, 32)), P("bvule", a, C(0x1FFFFFFF, 32))]
    tr = {k: sum(max(0, (4096 >> k) - l) for l in range(256)) for k in range(8)}
    run(f"axi burst, {form} alignment (E16-E20)", {"a": 32, "s": 3, "ln": 8},
        align + [bnd] + region, lambda m: m["s"], tr)


@shape
def axi():
    """AXI address linked to rand size/len: 4K boundary, three alignment spellings."""
    for form in ("slice", "mask", "modulo"):
        _axi(form)


@shape
def cache():
    """Cache stress address (E28): hot tag/index, word aligned."""
    a, h = V("a", 32), V("h", 1)
    tr = {k: (1 << 20) * 16 + (32 if k < 4 else 0) for k in range(64)}
    run("cache hot tag/index (E28)", {"a": 32, "h": 1},
        [IMP(P("=", h, C(1, 1)), P("bvule", EX(a, 31, 12), C(1, 20))),
         IMP(P("=", h, C(1, 1)), P("bvule", EX(a, 11, 6), C(3, 6))),
         P("=", EX(a, 1, 0), C(0, 2))],
        lambda m: (m["a"] >> 6) & 63, tr)


@shape
def cache_set():
    """Six addresses sharing one set index, unique tags (E29)."""
    w = {f"a{i}": 30 for i in range(6)}
    w["x"] = 14
    xs = [V(f"a{i}", 30) for i in range(6)]
    cons = [P("=", EX(t, 13, 0), V("x", 14)) for t in xs]
    cons += [P("distinct", EX(xs[i], 29, 14), EX(xs[j], 29, 14))
             for i in range(6) for j in range(i + 1, 6)]
    run("cache same-set x6 (E29)", w, cons, lambda m: m["a0"] >> 26, {k: 1 for k in range(16)})


def _pcie(width):
    a, j, k, f = V("a", width), V("j", 1), V("k", 1), V("f", 1)
    one = lambda t: P("=", t, C(1, 1))                                  # noqa: E731
    cons = [P("=", EX(a, 31, 12), C(0x12345, 20)),
            IMP(NOT(one(j)), P("=", EX(a, 1, 0), C(0, 2))),
            IMP(one(j), P("bvuge", EX(a, 1, 0), C(1, 2))),
            IMP(NOT(one(k)), P("bvule", EX(a, 11, 0), C(0xF, 12))),
            IMP(one(k), P("bvuge", EX(a, 11, 0), C(0xFF0, 12)))]
    if width > 32:
        cons.append(IMP(NOT(one(f)), P("=", EX(a, width - 1, 32), C(0, width - 32))))
    run(f"pcie page edge, {width}b addr (E24)", {"a": width, "j": 1, "k": 1, "f": 1}, cons,
        lambda m: (m["j"], m["k"], m["a"] & 15),
        {(jv, kv, nb): 1 for jv in (0, 1) for kv in (0, 1) for nb in range(16)
         if (nb & 3 == 0) == (jv == 0)})


@shape
def pcie():
    """PCIe page edge and misalignment sweep: nested x[11:0] / x[1:0]."""
    _pcie(32)
    _pcie(48)
    _pcie(64)


@shape
def cva6():
    """CVA6 unsupported instruction (E1): compressed vs 32-bit slices, which
    cross ([15:13] / [14:12] / [12:10]): not split."""
    i = V("i", 32)
    cm, cop, cms = V("cm", 1), V("cop", 2), V("cms", 3)
    op, f3, f7 = V("op", 7), V("f3", 3), V("f7", 7)
    one = P("=", cm, C(1, 1))
    cons = [IMP(one, AND(P("=", EX(i, 1, 0), cop), P("=", EX(i, 15, 13), cms))),
            IMP(NOT(one), AND(P("=", EX(i, 6, 0), op), P("=", EX(i, 31, 25), f7),
                              P("=", EX(i, 14, 12), f3))),
            P("distinct", cop, C(3, 2)),
            IMP(AND(one, P("=", cop, C(1, 2))),
                AND(P("=", EX(i, 12, 10), C(7, 3)), NOT(IN(EX(i, 4, 2), range(6), 3)),
                    P("=", EX(i, 6, 5), C(3, 2))))]
    # comp=0: i free, cop (3) and cms (8) free; comp=1: op/f3/f7 free (2^17).
    tr = {(0, lo): (1 << 30) * 24 for lo in range(4)}
    tr.update({(1, 0): (1 << 47), (1, 2): (1 << 47), (1, 1): (1 << 40)})
    run("cva6 unsupported instr (E1)",
        {"i": 32, "cm": 1, "cop": 2, "cms": 3, "op": 7, "f3": 3, "f7": 7}, cons,
        lambda m: (m["cm"], m["i"] & 3), tr)


@shape
def small():
    """Small header words (E26, E30) and a 64-bit descriptor base (E15)."""
    t = V("t", 16)
    run("vlan tci (E26)", {"t": 16},
        [P("=", EX(t, 12, 12), C(0, 1)), P("bvult", EX(t, 11, 0), C(4095, 12))],
        lambda m: m["t"] >> 13, {k: 1 for k in range(8)})
    s, d = V("s", 8), V("d", 8)
    run("noc mesh src/dst (E30)", {"s": 8, "d": 8},
        [P("bvule", EX(s, 7, 4), C(3, 4)), P("bvule", EX(s, 3, 0), C(5, 4)),
         P("bvule", EX(d, 7, 4), C(3, 4)), P("bvule", EX(d, 3, 0), C(5, 4)),
         P("distinct", s, d)],
        lambda m: m["s"], {(r << 4) | c: 1 for r in range(4) for c in range(6)})
    b = V("b", 64)
    lo, hi = 1 << 48, (1 << 64) - 1 - 33

    def n_aligned(a, z):
        return max(0, z // 16 - (a + 15) // 16 + 1)
    tr = {k: n_aligned(max(lo, k << 60), min(hi, ((k + 1) << 60) - 1)) for k in range(16)}
    run("idma descriptor base 64b (E15)", {"b": 64},
        [P("=", B("bvand", 64, b, C(0xF, 64)), C(0, 64)), P("bvugt", b, C(0x0000FFFFFFFFFFFF, 64)),
         P("bvugt", B("bvsub", 64, C((1 << 64) - 1, 64), b), C(32, 64))],
        lambda m: m["b"] >> 60, tr)


if __name__ == "__main__":
    args = sys.argv[1:]
    if args and args[0].isdigit():
        N_DRAWS = int(args.pop(0))
    for fn in SHAPES:
        if not args or any(a in fn.__name__ for a in args):
            fn()
