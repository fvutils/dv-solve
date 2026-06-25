"""Python wrapper for the C SolveCtx (solver session) API.

Usage::

    sp = SolveProblem()
    # ... add vars / constraints ...
    with SolveCtx(sp) as ctx:
        result = ctx.solve(seed=42)
        if result == SOLVE_OK:
            x = ctx.get_value(0)
"""
from __future__ import annotations

import ctypes
from typing import Optional

from .lib import _load_lib, _library_not_found_error

# ------------------------------------------------------------------ #
# SolveResult constants (must match zsp_search.h)                     #
# ------------------------------------------------------------------ #
SOLVE_OK      = 0
SOLVE_UNSAT   = 1
SOLVE_TIMEOUT = 2

# Buffer sizes
_CTX_BUF_SIZE = 1 << 20  # 1 MiB — headroom for propagators + decisions


# ------------------------------------------------------------------ #
# SolveOpts ctypes struct                                              #
# Layout (must match zsp_search.h exactly):                            #
#   seed(uint64=8) + max_conflicts(uint32=4) + max_restarts(uint32=4)  #
#   + use_phase_save(uint8=1) + _pad[3] + max_shave_iters(uint32=4)   #
#   → total 24 bytes                                                   #
# ------------------------------------------------------------------ #
class _SolveOpts(ctypes.Structure):
    _fields_ = [
        ("seed",           ctypes.c_uint64),
        ("max_conflicts",  ctypes.c_uint32),
        ("max_restarts",   ctypes.c_uint32),
        ("use_phase_save", ctypes.c_uint8),
        ("use_lcg",        ctypes.c_uint8),
        ("fair_pick",      ctypes.c_uint8),
        ("_pad",           ctypes.c_uint8 * 1),
        ("max_shave_iters", ctypes.c_uint32),
    ]


class CompileUnsatError(Exception):
    """Raised when solver_compile detects UNSAT via bound tightening.

    Means the constraint system is provably unsatisfiable without needing
    to run the search loop.
    """


class CompileIncompleteError(Exception):
    """Raised when solver_compile could not handle one or more constraints.

    The native back-end catches this and transparently falls back to the
    Python solver so that all constraints are respected.
    """


class SolveCtx:
    """Thin ctypes wrapper around the C ``SolveCtx`` + block-allocator.

    The context is compiled against a ``SolveProblem`` and then solved.
    """

    def __init__(self, problem: "SolveProblem", ctx_buf_size: int = _CTX_BUF_SIZE) -> None:  # noqa: F821
        lib = _load_lib()
        if lib is None:
            raise _library_not_found_error()
        self._lib = lib

        # Keep the SolveProblem buffer alive for the lifetime of this context.
        # solver_compile does not fully copy it, so the compiled context (and
        # any later reset()+solve()) reads from this buffer. A cached/reused ctx
        # outlives the call that built it, so without this reference the buffer
        # would be collected and the ctx would read freed memory.
        self._problem = problem

        # Block allocator owns all dynamic memory used by the context.
        self._ba = lib.zsp_block_alloc_create(None, ctx_buf_size)
        if self._ba is None:
            raise RuntimeError("zsp_block_alloc_create failed")

        # Context lives inside a caller-managed buffer.
        self._ctx_buf = (ctypes.c_uint8 * ctx_buf_size)()
        ctx = lib.solver_create(self._ctx_buf, ctx_buf_size, self._ba)
        if ctx is None:
            lib.zsp_block_alloc_destroy(self._ba)
            self._ba = None
            raise RuntimeError("solver_create failed")
        self._ctx = ctx  # c_void_p value

        # Compile constraints from the problem into this context.
        # Accept either SolveProblem (has _sp) or raw ctypes buffer
        sp_ptr = getattr(problem, "_sp", None)
        if sp_ptr is None:
            # Raw ctypes buffer -- cast to void pointer
            sp_ptr = ctypes.cast(problem, ctypes.c_void_p).value
        rc = lib.solver_compile(self._ctx, sp_ptr)
        # On every error path below, NULL out self._ba after releasing it: the
        # half-constructed SolveCtx still exists (the exception unwinds out of
        # __init__) and will be garbage-collected, at which point __del__ ->
        # destroy() must NOT free the already-freed block allocator again.
        if rc == -2:
            lib.zsp_block_alloc_destroy(self._ba)
            self._ba = None
            raise CompileUnsatError("Domain became empty during compile-time bound tightening")
        if rc < 0:
            lib.zsp_block_alloc_destroy(self._ba)
            self._ba = None
            raise RuntimeError(f"solver_compile failed (rc={rc})")
        if rc > 0:
            lib.zsp_block_alloc_destroy(self._ba)
            self._ba = None
            raise CompileIncompleteError(
                f"{rc} constraint(s) could not be compiled natively"
            )

    # ------------------------------------------------------------------ #
    # Context manager support                                              #
    # ------------------------------------------------------------------ #

    def __enter__(self) -> "SolveCtx":
        return self

    def __exit__(self, *_) -> None:
        self.destroy()

    def __del__(self) -> None:
        # Free native resources if the ctx is dropped without an explicit
        # destroy()/context-manager exit (e.g. a long-lived cached ctx whose
        # owner is garbage-collected).
        try:
            self.destroy()
        except Exception:
            pass

    def destroy(self) -> None:
        """Release all native resources."""
        if self._ba is not None:
            self._lib.zsp_block_alloc_destroy(self._ba)
            self._ba = None

    # ------------------------------------------------------------------ #
    # Solve API                                                            #
    # ------------------------------------------------------------------ #

    def solve(
        self,
        seed: int = 0,
        max_conflicts: int = 0,
        max_restarts: int = 0,
        use_phase_save: bool = False,
        max_shave_iters: int = 0,
        fair_pick: bool = False,
    ) -> int:
        """Run the search loop; returns SOLVE_OK, SOLVE_UNSAT, or SOLVE_TIMEOUT.

        ``fair_pick`` selects the decision-variable tie-break: ``False`` (fast)
        uses deterministic MRV and finds *a* solution quickly; ``True``
        (uniform) breaks ties randomly so the solution distribution has uniform
        marginals / full coverage — the right mode for constrained-random
        stimulus generation.
        """
        opts = _SolveOpts(
            seed=seed,
            max_conflicts=max_conflicts,
            max_restarts=max_restarts,
            use_phase_save=1 if use_phase_save else 0,
            fair_pick=1 if fair_pick else 0,
            max_shave_iters=max_shave_iters,
        )
        return self._lib.solver_solve(self._ctx, ctypes.byref(opts))

    def reset(self) -> None:
        """Reset solver to post-compile state for a fresh solve."""
        self._lib.solver_reset(self._ctx)

    def solve_n(
        self,
        n: int,
        var_ids: "ctypes.Array[ctypes.c_uint32]",
        n_vars: int,
        base_seed: int = 1,
        max_shave_iters: int = 0,
    ) -> "tuple[int, list[list[int]]]":
        """Run *n* independent solves, resetting between each.

        Returns ``(n_ok, solutions)`` where *solutions* is a list of
        ``n_ok`` value-lists (one per successful solve, each containing
        the values of *var_ids* in order).

        This keeps the entire loop in Python/ctypes but avoids rebuilding
        the SolveProblem and SolveCtx on each iteration.
        """
        out = (ctypes.c_int64 * (n * n_vars))()
        n_ok = self._lib.solver_solve_n(
            self._ctx, n, n_vars, var_ids, out,
            base_seed, max_shave_iters,
        )
        solutions: list[list[int]] = []
        for i in range(n_ok):
            row = out[i * n_vars : (i + 1) * n_vars]
            solutions.append(list(row))
        return n_ok, solutions

    def add_constraint(self, aux_problem) -> int:
        """Add constraints from an auxiliary SolveProblem to this context.

        Returns 0 on success, -1 if capacity exceeded, -2 if UNSAT.
        """
        sp_ptr = getattr(aux_problem, "_sp", None)
        if sp_ptr is None:
            sp_ptr = ctypes.cast(aux_problem, ctypes.c_void_p).value
        return self._lib.solver_add_constraint(self._ctx, sp_ptr)

    def checkpoint(self) -> int:
        """Save solver state; returns checkpoint index."""
        return self._lib.solver_checkpoint(self._ctx)

    def restore(self, cp: int) -> None:
        """Restore solver state to checkpoint *cp*."""
        self._lib.solver_restore(self._ctx, ctypes.c_uint32(cp))

    def propagate_only(self) -> int:
        """Run propagation to fixpoint without search.

        Returns PROP_OK (0) on fixpoint, PROP_CONFLICT (1) if UNSAT.
        Useful for fast feasibility checks without full solve.
        """
        return self._lib.solver_propagate_only(self._ctx)

    def check_unsat(self) -> bool:
        """Return True iff the current constraint set has no satisfying assignment.

        Calls ``propagate_only()`` first; if propagation detects a conflict the
        function returns ``True`` immediately without entering CDCL search.
        Falls through to ``solve()`` only when propagation is inconclusive
        (domains are non-empty but not fully fixed).

        This is the key primitive for the ODC analysis engine: repeated
        checkpoint → add_cube_constraint → check_unsat → restore queries can
        determine whether a candidate cube is disjoint from an observability
        region without any CDCL overhead for the common (propagation-decisive)
        case.
        """
        result = self._lib.solver_propagate_only(self._ctx)
        if result == 1:   # PROP_CONFLICT
            return True
        if result == 0:   # PROP_OK / all domains fixed → check if SAT
            return self._lib.solver_solve(self._ctx, ctypes.byref(_SolveOpts())) == SOLVE_UNSAT
        # Unexpected return code — fall back to solve()
        return self._lib.solver_solve(self._ctx, ctypes.byref(_SolveOpts())) == SOLVE_UNSAT

    def optimize(
        self,
        obj_var: int,
        minimize: bool = True,
        lo: int = 0,
        hi: int = 0x7FFF_FFFF,
    ) -> "Optional[int]":
        """Find the optimal value of *obj_var* subject to current constraints.

        Uses binary search over ``solve()`` calls (no native optimize needed).
        Returns the optimal integer value, or ``None`` if the problem is UNSAT.

        Args:
            obj_var:  Variable ID to optimise.
            minimize: If True, minimise; if False, maximise.
            lo:       Lower bound for binary search (inclusive).
            hi:       Upper bound for binary search (inclusive).
        """
        # Quick feasibility check first.
        cp = self.checkpoint()
        test_result = self._lib.solver_solve(self._ctx, ctypes.byref(_SolveOpts()))
        self.restore(cp)
        if test_result == SOLVE_UNSAT:
            return None

        if minimize:
            best = self.get_value(obj_var)  # any feasible solution is an upper bound
            lo_cur = lo
            while lo_cur < best:
                mid = (lo_cur + best) // 2
                cp2 = self.checkpoint()
                # Add constraint obj_var <= mid
                from .problem import SolveProblem, BIN_LTE
                aux = SolveProblem()
                v = aux.add_var(obj_var, width=32, is_signed=False, lo=lo, hi=mid)
                e_v = aux.expr_var(obj_var)
                e_mid = aux.expr_const(mid)
                aux.add_constraint(aux.expr_binary(BIN_LTE, e_v, e_mid))
                rc = self.add_constraint(aux)
                if rc == -2:  # UNSAT
                    self.restore(cp2)
                    lo_cur = mid + 1
                else:
                    r = self._lib.solver_solve(self._ctx, ctypes.byref(_SolveOpts()))
                    if r == SOLVE_OK:
                        best = self.get_value(obj_var)
                        self.restore(cp2)
                    else:
                        self.restore(cp2)
                        lo_cur = mid + 1
            return best
        else:
            # Maximise: symmetric, search from hi down.
            best = self.get_value(obj_var)
            hi_cur = hi
            while best < hi_cur:
                mid = (best + 1 + hi_cur) // 2
                cp2 = self.checkpoint()
                from .problem import SolveProblem, BIN_GTE
                aux = SolveProblem()
                v = aux.add_var(obj_var, width=32, is_signed=False, lo=mid, hi=hi)
                e_v = aux.expr_var(obj_var)
                e_mid = aux.expr_const(mid)
                aux.add_constraint(aux.expr_binary(BIN_GTE, e_v, e_mid))
                rc = self.add_constraint(aux)
                if rc == -2:
                    self.restore(cp2)
                    hi_cur = mid - 1
                else:
                    r = self._lib.solver_solve(self._ctx, ctypes.byref(_SolveOpts()))
                    if r == SOLVE_OK:
                        best = self.get_value(obj_var)
                        self.restore(cp2)
                    else:
                        self.restore(cp2)
                        hi_cur = mid - 1
            return best

    def get_value(self, var_id: int) -> int:
        """Return assigned value for var_id after a successful solve."""
        return self._lib.solver_get_value(self._ctx, ctypes.c_uint32(var_id))
