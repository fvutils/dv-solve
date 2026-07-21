"""Reusable sampling / distribution-quality harness for dv-solve.

The native CDCL engine randomizes every solve from a seed, so SystemVerilog
``dist`` weighting and free-variable randomization are *distribution-quality*
properties (how often each value shows up over many solves) rather than
sat/unsat facts. A sat oracle (z3) cannot check them; this harness can.

It builds a small problem through the ctypes C-API (the same surface the
``test_dist.py`` unit tests use), runs ``n`` seeded solves, and collects a
per-value histogram. Statistical goodness-of-fit is delegated to
``scipy.stats`` (chi-square), so the checks are standard and well-tested.

Determinism: seeds are derived from a fixed base via a multiplicative
scramble (``_seed_at``) so consecutive iterations are decorrelated but the
whole sample is reproducible — the resulting tests do not flake.

Typical use::

    from .dist_harness import DistProblem, assert_uniform, assert_gof

    dp = DistProblem(libzsp)
    dp.add_var(0, width=4, lo=0, hi=15)
    hist = dp.sample(var_id=0, n=8000)
    assert_uniform(hist, range(16))          # free var ~ uniform
"""
from __future__ import annotations

import ctypes
from collections import Counter
from typing import Iterable, Mapping, Sequence

from scipy import stats

# ------------------------------------------------------------------ #
# ctypes mirrors of the C ABI (kept in sync with test_dist.py)        #
# ------------------------------------------------------------------ #

EXPR_NULL = 0xFFFF_FFFF
SOLVE_OK = 0

_SP_BUF_SIZE = 65536
_CTX_BUF_SIZE = 524288

# BinOp codes — mirror the C enum in src/c/zsp_problem.h.
BIN_ADD, BIN_SUB, BIN_MUL, BIN_DIV, BIN_MOD = range(5)
BIN_BAND, BIN_BOR, BIN_BXOR, BIN_LSHIFT, BIN_RSHIFT = range(5, 10)
BIN_EQ, BIN_NEQ, BIN_LT, BIN_LTE, BIN_GT, BIN_GTE = range(10, 16)
BIN_AND, BIN_OR = 16, 17


class DistEntry(ctypes.Structure):
    """ctypes mirror of the C ``DistEntry`` struct."""
    _fields_ = [
        ("lo", ctypes.c_int64),
        ("hi", ctypes.c_int64),
        ("weight", ctypes.c_uint32),
        ("is_per_value", ctypes.c_uint8),
        ("_dpad", ctypes.c_uint8 * 3),
    ]


class SolveOpts(ctypes.Structure):
    """ctypes mirror of the C ``SolveOpts`` struct."""
    _fields_ = [
        ("seed", ctypes.c_uint64),
        ("max_conflicts", ctypes.c_uint32),
        ("max_restarts", ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("_pad", ctypes.c_uint8 * 3),
        ("max_shave_iters", ctypes.c_uint32),
    ]


# Modules that were already ``bind``-ed (keyed by id) — binding restypes /
# argtypes twice is harmless but we avoid the churn.
_bound: set = set()


def bind(lib: ctypes.CDLL) -> None:
    """Attach restype / argtypes to the C entry points this harness uses."""
    if id(lib) in _bound:
        return
    lib.zsp_block_alloc_create.restype = ctypes.c_void_p
    lib.zsp_block_alloc_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.zsp_block_alloc_destroy.restype = None
    lib.zsp_block_alloc_destroy.argtypes = [ctypes.c_void_p]

    lib.solve_problem_init.restype = ctypes.c_void_p
    lib.solve_problem_init.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
    lib.problem_add_var.restype = ctypes.c_uint32
    lib.problem_add_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                    ctypes.c_uint8, ctypes.c_uint8,
                                    ctypes.c_int64, ctypes.c_int64]
    lib.problem_add_dist.restype = ctypes.c_uint32
    lib.problem_add_dist.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                     ctypes.c_uint32, ctypes.c_void_p]
    lib.problem_add_constraint.restype = ctypes.c_uint32
    lib.problem_add_constraint.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

    lib.expr_var.restype = ctypes.c_uint32
    lib.expr_var.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.expr_const.restype = ctypes.c_uint32
    lib.expr_const.argtypes = [ctypes.c_void_p, ctypes.c_int64, ctypes.c_uint8]
    lib.expr_binary.restype = ctypes.c_uint32
    lib.expr_binary.argtypes = [ctypes.c_void_p, ctypes.c_uint32,
                                ctypes.c_uint32, ctypes.c_uint32]

    lib.solver_create.restype = ctypes.c_void_p
    lib.solver_create.argtypes = [ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.c_void_p]
    lib.solver_compile.restype = ctypes.c_int
    lib.solver_compile.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.solver_solve.restype = ctypes.c_int
    lib.solver_solve.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
    lib.solver_get_value.restype = ctypes.c_int64
    lib.solver_get_value.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    lib.solver_reset.restype = None
    lib.solver_reset.argtypes = [ctypes.c_void_p]
    lib.solver_set_seed.restype = None
    lib.solver_set_seed.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
    _bound.add(id(lib))


# 2**64 golden-ratio odd constant — decorrelates consecutive seeds while
# keeping the whole run reproducible.
_SEED_MULT = 0x9E3779B97F4A7C15
_SEED_MASK = (1 << 64) - 1


def _seed_at(base: int, i: int) -> int:
    """Deterministic, decorrelated seed for iteration ``i`` of a run."""
    s = (base + (i + 1) * _SEED_MULT) & _SEED_MASK
    return s or 1  # 0 is remapped to a nonzero default by the solver


# ------------------------------------------------------------------ #
# Problem builder                                                     #
# ------------------------------------------------------------------ #

class DistProblem:
    """A small single-/multi-var problem you can sample many times.

    Build it with :meth:`add_var` / :meth:`add_dist`, then call
    :meth:`sample` to get a value histogram over ``n`` seeded solves. The
    underlying ctypes buffers are held for the lifetime of the object.
    """

    def __init__(self, lib: ctypes.CDLL):
        bind(lib)
        self.lib = lib
        self._sp_buf = (ctypes.c_uint8 * _SP_BUF_SIZE)()
        self.sp = lib.solve_problem_init(self._sp_buf, _SP_BUF_SIZE)
        assert self.sp, "solve_problem_init failed"
        self._dist_arrays: list = []   # keep DistEntry arrays alive
        self._ctx = None
        self._ctx_buf = None
        self._ba = None

    # -- construction ------------------------------------------------ #
    def add_var(self, var_id: int, width: int, lo: int, hi: int,
                is_signed: int = 0) -> None:
        ref = self.lib.problem_add_var(self.sp, var_id, width, is_signed, lo, hi)
        assert ref != EXPR_NULL, f"problem_add_var({var_id}) overflow"

    def add_dist(self, var_id: int,
                 entries: Sequence[tuple]) -> None:
        """Attach a dist to ``var_id``.

        ``entries`` is a sequence of ``(lo, hi, weight, is_per_value)``
        tuples — ``is_per_value`` True == SystemVerilog ``:=`` (each value
        gets ``weight``), False == ``:/`` (the whole range shares ``weight``).
        """
        arr = (DistEntry * len(entries))()
        for i, (lo, hi, w, ipv) in enumerate(entries):
            arr[i].lo = lo
            arr[i].hi = hi
            arr[i].weight = w
            arr[i].is_per_value = 1 if ipv else 0
        self._dist_arrays.append(arr)  # anchor against GC
        ref = self.lib.problem_add_dist(self.sp, var_id, len(entries), arr)
        assert ref != EXPR_NULL, f"problem_add_dist({var_id}) overflow"

    # -- expression / hard-constraint builders ----------------------- #
    def var(self, var_id: int) -> int:
        return self.lib.expr_var(self.sp, var_id)

    def const(self, value: int, width: int = 32) -> int:
        return self.lib.expr_const(self.sp, value, width)

    def binop(self, op: int, lhs: int, rhs: int) -> int:
        ref = self.lib.expr_binary(self.sp, op, lhs, rhs)
        assert ref != EXPR_NULL, "expr_binary overflow"
        return ref

    def add_constraint(self, expr_ref: int) -> None:
        ref = self.lib.problem_add_constraint(self.sp, expr_ref)
        assert ref != EXPR_NULL, "problem_add_constraint overflow"

    def constrain(self, var_id: int, op: int, value: int, width: int = 32) -> None:
        """Add ``var <op> const`` as a hard constraint (op is a ``BIN_*`` code)."""
        self.add_constraint(self.binop(op, self.var(var_id), self.const(value, width)))

    def _ensure_ctx(self):
        if self._ctx is not None:
            return
        self._ctx_buf = (ctypes.c_uint8 * _CTX_BUF_SIZE)()
        self._ba = self.lib.zsp_block_alloc_create(None, 0)
        self._ctx = self.lib.solver_create(self._ctx_buf, _CTX_BUF_SIZE, self._ba)
        rc = self.lib.solver_compile(self._ctx, self.sp)
        assert rc >= 0, f"solver_compile failed: {rc}"

    # -- sampling ---------------------------------------------------- #
    def sample(self, var_id: int, n: int, seed_base: int = 0x1234_5678,
               allow_fail: bool = False) -> Counter:
        """Run ``n`` seeded solves; return a ``Counter`` of ``var_id`` values.

        When ``allow_fail`` is False (default) every solve must return
        ``SOLVE_OK``. Set it True only to probe expected-infeasible shapes.
        """
        self._ensure_ctx()
        lib, ctx = self.lib, self._ctx
        hist: Counter = Counter()
        for i in range(n):
            seed = _seed_at(seed_base, i)
            lib.solver_reset(ctx)
            lib.solver_set_seed(ctx, seed)
            opts = SolveOpts(seed=seed)
            rc = lib.solver_solve(ctx, ctypes.byref(opts))
            if rc != SOLVE_OK:
                if allow_fail:
                    hist["<fail>"] += 1
                    continue
                raise AssertionError(f"solve failed at i={i} seed={seed}: rc={rc}")
            hist[lib.solver_get_value(ctx, var_id)] += 1
        return hist

    def sample_joint(self, var_ids: Sequence[int], n: int,
                     seed_base: int = 0x1234_5678) -> Counter:
        """Like :meth:`sample` but keys the histogram on a tuple of values."""
        self._ensure_ctx()
        lib, ctx = self.lib, self._ctx
        hist: Counter = Counter()
        for i in range(n):
            seed = _seed_at(seed_base, i)
            lib.solver_reset(ctx)
            lib.solver_set_seed(ctx, seed)
            opts = SolveOpts(seed=seed)
            rc = lib.solver_solve(ctx, ctypes.byref(opts))
            assert rc == SOLVE_OK, f"solve failed at i={i}: rc={rc}"
            hist[tuple(lib.solver_get_value(ctx, v) for v in var_ids)] += 1
        return hist

    def close(self):
        if self._ba is not None:
            self.lib.zsp_block_alloc_destroy(self._ba)
            self._ba = None
            self._ctx = None


# ------------------------------------------------------------------ #
# Statistical assertions (scipy-backed)                               #
# ------------------------------------------------------------------ #

def chi2_gof(hist: Mapping, expected_prob: Mapping[object, float]):
    """Chi-square goodness-of-fit of ``hist`` against ``expected_prob``.

    Returns scipy's ``Power_divergenceResult`` (``.statistic``, ``.pvalue``).
    ``expected_prob`` must cover every key with nonzero expectation and sum
    to 1. Observed keys outside ``expected_prob`` are the caller's job to
    reject first (see :func:`assert_support`).
    """
    keys = list(expected_prob.keys())
    n = sum(hist.get(k, 0) for k in keys)
    observed = [hist.get(k, 0) for k in keys]
    expected = [expected_prob[k] * n for k in keys]
    return stats.chisquare(f_obs=observed, f_exp=expected)


def assert_support(hist: Mapping, allowed: Iterable) -> None:
    """Fail if any sampled value lies outside ``allowed`` (a soundness check).

    A dist that restricts the domain must never emit an out-of-set value;
    that would be a real bug, not a distribution-quality nit.
    """
    allowed_set = set(allowed)
    stray = {k: c for k, c in hist.items() if k not in allowed_set}
    assert not stray, f"values outside allowed support {sorted(allowed_set)}: {stray}"


def assert_gof(hist: Mapping, expected_prob: Mapping[object, float],
               alpha: float = 1e-4, label: str = "") -> None:
    """Assert ``hist`` fits ``expected_prob`` (chi-square p-value > alpha).

    ``alpha`` is deliberately tiny: seeds are fixed so the test is
    deterministic, and the effect sizes we probe are large, so a small alpha
    keeps a comfortable margin while still catching a grossly wrong shape.
    Also asserts the sample never strayed outside the expected support.
    """
    assert_support(hist, expected_prob.keys())
    res = chi2_gof(hist, expected_prob)
    assert res.pvalue > alpha, (
        f"{label or 'distribution'} rejects expected shape: "
        f"chi2={res.statistic:.2f} p={res.pvalue:.2e} (alpha={alpha:.0e})\n"
        f"  observed={dict(sorted((k, hist.get(k, 0)) for k in expected_prob))}\n"
        f"  expected_prob={dict(expected_prob)}"
    )


def assert_uniform(hist: Mapping, support: Iterable, alpha: float = 1e-4,
                   label: str = "") -> None:
    """Assert ``hist`` is uniform over ``support``."""
    support = list(support)
    p = 1.0 / len(support)
    assert_gof(hist, {k: p for k in support}, alpha=alpha,
               label=label or f"uniform over {len(support)} values")


def restrict(hist: Mapping, keys: Iterable) -> Counter:
    """Sub-histogram containing only ``keys`` (for intra-range checks)."""
    keyset = set(keys)
    return Counter({k: c for k, c in hist.items() if k in keyset})


def marginal(joint: Mapping, axis: int) -> Counter:
    """Collapse a tuple-keyed joint histogram onto one axis."""
    out: Counter = Counter()
    for key, c in joint.items():
        out[key[axis]] += c
    return out


def assert_independent(joint: Mapping, xs: Sequence, ys: Sequence,
                       alpha: float = 1e-4, label: str = "") -> None:
    """Assert the two axes of a tuple-keyed joint histogram are independent.

    Chi-square test of independence on the ``xs`` × ``ys`` contingency table
    (``scipy.stats.chi2_contingency``). A low p-value means the value picked
    for one variable is correlated with the other — a randomization-quality
    defect for two constraint-independent rand vars.
    """
    table = [[joint.get((x, y), 0) for y in ys] for x in xs]
    res = stats.chi2_contingency(table)
    pvalue = getattr(res, "pvalue", None)
    if pvalue is None:  # older scipy returns a plain tuple
        pvalue = res[1]
    assert pvalue > alpha, (
        f"{label or 'joint'} axes are not independent: "
        f"chi2_contingency p={pvalue:.2e} (alpha={alpha:.0e})"
    )
