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
# dvs_result_t constants (must match dv_solve.h)                      #
# ------------------------------------------------------------------ #
SOLVE_OK      = 0
SOLVE_UNSAT   = 1
SOLVE_TIMEOUT = 2

# Buffer sizes
_CTX_BUF_SIZE = 1 << 20  # 1 MiB — headroom for propagators + decisions


# ------------------------------------------------------------------ #
# SolveOpts ctypes struct. Must match dvs_solve_opts_t in dv_solve.h   #
# field for field (tests/unit/test_wide_watch.py checks it): the C     #
# side reads the whole struct, so a missing field is read from          #
# whatever memory follows.                                             #
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
        ("time_limit_ms",  ctypes.c_uint32),
    ]


# Mirror DVS_COMPILE_UNSUPPORTED_WIDTH and DVS_COMPILE_BAD_VAR in dv_solve.h.
_COMPILE_UNSUPPORTED_WIDTH = -3
_COMPILE_BAD_VAR = -4


class CompileUnsatError(Exception):
    """The constraints were shown to be unsatisfiable while compiling.

    Raised by :class:`SolveCtx` when narrowing the variables' ranges already
    proves there is no solution, before any search. Treat it like a
    ``SOLVE_UNSAT`` result.
    """


class CompileIncompleteError(Exception):
    """One or more constraints use a form this engine cannot compile.

    Raised by :class:`SolveCtx` rather than silently ignoring the constraint.
    The message says how many constraints were affected. The remaining
    cases are mostly 64-bit expressions, such as ``>>`` of a signed 64-bit
    variable.
    """


class CompileUnsupportedError(CompileIncompleteError):
    """The problem is larger than the API supports.

    The Python API supports variables up to 64 bits wide, expressions up to
    255 bits wide, expressions nested up to 20000 deep, and all-different
    constraints over up to 16 variables of up to 32 bits. Wider bit-vectors
    are supported through the SMT-LIB2 front end (``dv-solve-smt2``).
    """


class SolveCtx:
    """A compiled problem, ready to solve.

    Create it from the buffer returned by
    :meth:`SolveProblemBuilder.finalize() <dv_solve.builder.SolveProblemBuilder.finalize>`,
    call :meth:`solve`, then read values with :meth:`get_value`. Call
    :meth:`reset` before solving again. Use it as a context manager (``with``)
    or call :meth:`destroy` to release its native memory promptly.

    Args:
        problem: The finalized problem buffer.
        ctx_buf_size: Initial working-memory size, in bytes.

    Raises:
        CompileUnsatError: The constraints are provably unsatisfiable.
        CompileIncompleteError: A constraint could not be compiled.
        CompileUnsupportedError: The problem goes beyond a supported limit.
    """

    def __init__(self, problem: "SolveProblem", ctx_buf_size: int = _CTX_BUF_SIZE) -> None:  # noqa: F821
        lib = _load_lib()
        if lib is None:
            raise _library_not_found_error()
        self._lib = lib

        # Keep the SolveProblem buffer alive for the lifetime of this context.
        # dvs_solver_compile does not fully copy it, so the compiled context (and
        # any later reset()+solve()) reads from this buffer. A cached/reused ctx
        # outlives the call that built it, so without this reference the buffer
        # would be collected and the ctx would read freed memory.
        self._problem = problem

        # Block allocator owns all dynamic memory used by the context.
        self._ba = lib.dvs_block_alloc_create(None, ctx_buf_size)
        if self._ba is None:
            raise RuntimeError("dvs_block_alloc_create failed")

        # Context lives inside a caller-managed buffer.
        self._ctx_buf = (ctypes.c_uint8 * ctx_buf_size)()
        ctx = lib.dvs_solver_create(self._ctx_buf, ctx_buf_size, self._ba)
        if ctx is None:
            lib.dvs_block_alloc_destroy(self._ba)
            self._ba = None
            raise RuntimeError("dvs_solver_create failed")
        self._ctx = ctx  # c_void_p value

        # Compile constraints from the problem into this context.
        # Accept either SolveProblem (has _sp) or raw ctypes buffer
        sp_ptr = getattr(problem, "_sp", None)
        if sp_ptr is None:
            # Raw ctypes buffer -- cast to void pointer
            sp_ptr = ctypes.cast(problem, ctypes.c_void_p).value
        rc = lib.dvs_solver_compile(self._ctx, sp_ptr)
        # On every error path below, NULL out self._ba after releasing it: the
        # half-constructed SolveCtx still exists (the exception unwinds out of
        # __init__) and will be garbage-collected, at which point __del__ ->
        # destroy() must NOT free the already-freed block allocator again.
        if rc == -2:
            lib.dvs_block_alloc_destroy(self._ba)
            self._ba = None
            raise CompileUnsatError("Domain became empty during compile-time bound tightening")
        if rc == _COMPILE_UNSUPPORTED_WIDTH:
            lib.dvs_block_alloc_destroy(self._ba)
            self._ba = None
            raise CompileUnsupportedError(
                "problem goes beyond a supported limit: a variable wider than "
                "64 bits, an expression wider than 255 bits or nested more "
                "than 20000 deep, or an all-different over more than 16 "
                "variables or one wider than 32 bits"
            )
        if rc == _COMPILE_BAD_VAR:
            lib.dvs_block_alloc_destroy(self._ba)
            self._ba = None
            raise ValueError(
                "variable ids must be 0..n-1, each declared once with add_var, "
                "and every variable an expression names must be declared"
            )
        if rc < 0:
            lib.dvs_block_alloc_destroy(self._ba)
            self._ba = None
            raise RuntimeError(f"dvs_solver_compile failed (rc={rc})")
        if rc > 0:
            lib.dvs_block_alloc_destroy(self._ba)
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
        """Release the native memory. Also called when the context is garbage-collected."""
        if self._ba is not None:
            self._lib.dvs_block_alloc_destroy(self._ba)
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
        time_limit_ms: int = 0,
        use_lcg: bool = False,
    ) -> int:
        """Search for a solution.

        Returns:
            ``SOLVE_OK`` (a solution was found), ``SOLVE_UNSAT`` (none exists)
            or ``SOLVE_TIMEOUT`` (the search gave up; the problem may or may
            not have a solution).

        Args:
            seed: Selects which solution is returned. The same seed gives the
                same solution.
            max_conflicts: The restart unit: the search restarts after
                ``luby(i) * max_conflicts`` conflicts (0: 100). It is NOT a
                total budget -- a large value means the search hardly ever
                restarts, which makes heavy-tailed problems slower, not
                bounded.
            max_restarts: Give up (``SOLVE_TIMEOUT``) after this many
                restarts (0: 10000). This, with ``max_conflicts``, is the
                deterministic bound on one solve.
            time_limit_ms: Wall-clock bound on this solve, in milliseconds
                (0: the ``DV_CDCL_TIME_LIMIT`` environment default, 10 s).
                Unlike the restart bound it depends on the machine.
            use_lcg: Learn clauses from conflicts (lazy clause generation)
                and backjump, instead of backtracking chronologically. Much
                faster on problems whose conflicts come from early decisions.
                A :meth:`restore` to a checkpoint taken before any learning
                forgets everything learnt, so a reused context still solves
                exactly as a fresh one.
            use_phase_save: Search tuning; leave at the default.
            max_shave_iters: Search tuning; leave at the default.
            fair_pick: See below.

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
            time_limit_ms=time_limit_ms,
            use_lcg=1 if use_lcg else 0,
        )
        return self._lib.dvs_solver_solve(self._ctx, ctypes.byref(opts))

    def reset(self) -> None:
        """Clear the previous solution so :meth:`solve` can run again."""
        self._lib.dvs_solver_reset(self._ctx)

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
        n_ok = self._lib.dvs_solver_solve_n(
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

        Returns 0 on success, -1 if capacity exceeded, -2 if UNSAT, -3 for
        an unsupported width, -4 for an undeclared variable, or a positive
        count of constraints that could not be compiled.
        """
        sp_ptr = getattr(aux_problem, "_sp", None)
        if sp_ptr is None:
            sp_ptr = ctypes.cast(aux_problem, ctypes.c_void_p).value
        return self._lib.dvs_solver_add_constraint(self._ctx, sp_ptr)

    def pin(self, var_id: int, value: int) -> bool:
        """Fix *var_id* to *value* for the next solve, and propagate.

        Returns False if the pin conflicts with what is already known (the
        value is outside the variable's current domain, or propagation fails).
        A pin lasts until :meth:`restore` to a checkpoint taken before it, or
        :meth:`reset`; :meth:`solve` does not clear it. The incremental
        pattern: ``cp = checkpoint(); pin(...); solve(); ...; restore(cp)``.
        """
        return self._lib.dvs_solver_pin_var(self._ctx, var_id, value) == 0

    def checkpoint(self) -> int:
        """Save solver state; returns checkpoint index."""
        return self._lib.dvs_solver_checkpoint(self._ctx)

    def restore(self, cp: int) -> None:
        """Restore solver state to checkpoint *cp*."""
        self._lib.dvs_solver_restore(self._ctx, ctypes.c_uint32(cp))

    def propagate_only(self) -> int:
        """Run propagation to fixpoint without search.

        Returns PROP_OK (0) on fixpoint, PROP_CONFLICT (1) if UNSAT.
        Useful for fast feasibility checks without full solve.
        """
        return self._lib.dvs_solver_propagate_only(self._ctx)

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
        result = self._lib.dvs_solver_propagate_only(self._ctx)
        if result == 1:   # PROP_CONFLICT
            return True
        if result == 0:   # PROP_OK / all domains fixed → check if SAT
            return self._lib.dvs_solver_solve(self._ctx, ctypes.byref(_SolveOpts())) == SOLVE_UNSAT
        # Unexpected return code — fall back to solve()
        return self._lib.dvs_solver_solve(self._ctx, ctypes.byref(_SolveOpts())) == SOLVE_UNSAT

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
        test_result = self._lib.dvs_solver_solve(self._ctx, ctypes.byref(_SolveOpts()))
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
                    r = self._lib.dvs_solver_solve(self._ctx, ctypes.byref(_SolveOpts()))
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
                    r = self._lib.dvs_solver_solve(self._ctx, ctypes.byref(_SolveOpts()))
                    if r == SOLVE_OK:
                        best = self.get_value(obj_var)
                        self.restore(cp2)
                    else:
                        self.restore(cp2)
                        hi_cur = mid - 1
            return best

    def get_value(self, var_id: int) -> int:
        """The value of variable ``var_id`` in the last solution.

        Only meaningful after :meth:`solve` returned ``SOLVE_OK``.
        """
        # No ctypes.c_uint32(var_id) here: argtypes already declares c_uint32, so
        # ctypes converts a plain Python int itself. Constructing the wrapper was
        # ~18% of the cost of this call, which runs once per field per solve.
        return self._lib.dvs_solver_get_value(self._ctx, var_id)

    def get_values(self, ids_arr, out_arr, n: int) -> None:
        """Bulk readback: write the solved values of ``ids_arr[0:n]`` into
        ``out_arr[0:n]`` in a single FFI call.

        Both arrays are **caller-owned and caller-allocated** — a ``c_uint32*n``
        and a ``c_int64*n`` — so a caller that reads back the same var-id list
        every solve allocates nothing per solve. That is the entire point: the
        per-element ``get_value`` loop was 28.8% of a warm 128-element
        randomize().

        Values are only meaningful after a solve that returned ``SOLVE_OK``, and
        only for variables that fit in an int64 — a >64-bit variable must use the
        wide reader instead.
        """
        self._lib.dvs_solver_get_values(self._ctx, n, ids_arr, out_arr)

    def validate_model(self) -> int:
        """Re-evaluate every constraint in the problem against the current
        assignment. Returns the number of violations; 0 means the model
        satisfies every constraint the evaluator can check.

        This is the post-solve net for the class of bug where a constraint is
        dropped at compile time and the search then satisfies only what it was
        given. `SolveCtx` raises `CompileIncompleteError` rather than dropping
        anything, so a violation here means a *mis*-compiled constraint rather
        than a missing one -- which is the harder failure to notice, since
        nothing reports it.

        Constructs the evaluator cannot handle (arrays, sums, countones, clog2,
        in_set, in_range) are skipped, so a 0 is not a proof of correctness
        over a problem built from those.

        Call only after a solve that returned SOLVE_OK.
        """
        sp_ptr = getattr(self._problem, "_sp", None)
        if sp_ptr is None:
            sp_ptr = ctypes.cast(self._problem, ctypes.c_void_p).value
        return self._lib.dvs_solver_validate_model(self._ctx, sp_ptr, None)
