"""Python wrapper for the dv-solve BV-SAT completeness engine (``zsp_bbsolver``).

This is dv-solve's *internal completeness fallback*: a bit-blasting BV→SAT
solver (kissat) that is **authoritative for both SAT and UNSAT**. The primary
bounds-propagation engine (``SolveCtx``) is fast and gives good stimulus
distribution; when it returns UNSAT/timeout or cannot compile a construct, the
back-end re-runs the *same* ``SolveProblem`` buffer through ``BVSatCtx`` to get a
definitive verdict — replacing the historical external Boolector fallback.

It is a **completeness fallback, not the default**: a SAT solver finds *a*
model, not a well-distributed one, and (by design) it ignores relaxable
constraints. Soundness rests on the model documented in
``doc/notes/dv_solve_phaseA_bvsat_plan.md`` §2:

* Hard constraints are always respected (a SAT model satisfies them; UNSAT means
  the hard core is infeasible).
* Soft constraints are *soundly* ignored (relaxable — dropping them never flips
  SAT↔UNSAT; only stimulus quality drops).
* ``add_dist`` is ignored, which is sound only under the **Domain-Twin
  Invariant**: any feasible-domain restriction must *also* be a hard constraint
  (the back-end already emits the hard membership twin alongside ``add_dist``).
* ``add_all_different`` / ``add_source`` are not handled by the engine; pyvsc
  never emits them on the dv-solve path (``unique`` lowers to NEQ pairs), but the
  back-end asserts their absence before trusting a verdict.

Single-shot, non-incremental: one ``BVSatCtx`` per check.

Usage::

    b = SolveProblemBuilder()
    # ... add vars / constraints ...
    buf, _sz = b.finalize()
    with BVSatCtx(buf) as bb:
        if bb.check() == BVSAT_SAT:
            x = bb.value(0)
"""
from __future__ import annotations

import ctypes
from typing import Optional

from .lib import _load_lib, _library_not_found_error

# ------------------------------------------------------------------ #
# Result codes (must match zsp_bbsolver.h)                            #
# ------------------------------------------------------------------ #
BVSAT_SAT     = 10
BVSAT_UNSAT   = 20
BVSAT_UNKNOWN = 0
BVSAT_ERROR   = -1


class BVSatCtx:
    """Thin ctypes wrapper around a single ``zsp_bbsolver_t`` check.

    Accepts either a :class:`~dv_solve.problem.SolveProblem` (with a ``_sp``
    pointer) or a finalized raw ctypes buffer from
    ``SolveProblemBuilder.finalize()`` — the same buffer ``SolveCtx`` compiles,
    so the two engines share one encoding with no re-translation.
    """

    def __init__(self, problem) -> None:
        lib = _load_lib()
        if lib is None:
            raise _library_not_found_error()
        self._lib = lib

        # Keep the SolveProblem buffer alive for the lifetime of this context:
        # zsp_bbsolver_new only records the pointer; check() reads the problem
        # during bit-blasting and value() reads it during readback.
        self._problem = problem
        sp_ptr = getattr(problem, "_sp", None)
        if sp_ptr is None:
            # Raw ctypes buffer -> cast to a void pointer (same as SolveCtx).
            sp_ptr = ctypes.cast(problem, ctypes.c_void_p).value

        # alloc may be NULL: the bbsolver owns its transient AIG/CNF/SAT memory
        # and releases it in zsp_bbsolver_free.
        self._bb = lib.zsp_bbsolver_new(None, sp_ptr)
        if not self._bb:
            self._bb = None
            raise RuntimeError("zsp_bbsolver_new failed")

        self._checked = False

    # ------------------------------------------------------------------ #
    # Lifetime                                                            #
    # ------------------------------------------------------------------ #

    def __enter__(self) -> "BVSatCtx":
        return self

    def __exit__(self, *_) -> None:
        self.destroy()

    def __del__(self) -> None:
        try:
            self.destroy()
        except Exception:
            pass

    def destroy(self) -> None:
        """Release the bbsolver and all transient resources."""
        if self._bb is not None:
            self._lib.zsp_bbsolver_free(self._bb)
            self._bb = None

    # ------------------------------------------------------------------ #
    # Check / readback                                                    #
    # ------------------------------------------------------------------ #

    def check(self, seed: int = 0, soft_keep=None) -> int:
        """Bit-blast, encode CNF, and run the SAT solver.

        ``seed`` perturbs the search so repeated checks of the same problem can
        return different satisfying models (the completeness-fallback's only
        source of stimulus diversity). Returns ``BVSAT_SAT`` / ``BVSAT_UNSAT`` /
        ``BVSAT_UNKNOWN`` / ``BVSAT_ERROR``.

        ``soft_keep`` (DSE-2): an optional ``ctypes`` ``c_uint8`` array (in
        ``softs_head`` walk order; 1 = keep) selecting which soft constraints to
        enforce as *hard* for this check. Used by the sampler to enforce the
        MaxSAT-chosen kept set during distribution draws. The caller must keep
        the array alive across the call. ``None`` honors no soft (legacy).
        """
        if self._bb is None:
            raise RuntimeError("BVSatCtx used after destroy()")
        if soft_keep is not None:
            self._lib.zsp_bbsolver_set_soft_keep(
                self._bb, soft_keep, ctypes.c_uint32(len(soft_keep)))
        rc = self._lib.zsp_bbsolver_check(self._bb, ctypes.c_uint64(seed & ((1 << 64) - 1)))
        self._checked = (rc == BVSAT_SAT)
        return rc

    def value(self, var_id: int) -> int:
        """Return the model value for ``var_id`` after a ``BVSAT_SAT`` check.

        Raises ``RuntimeError`` if the engine is not in a SAT state or the
        variable is not in the problem.
        """
        if self._bb is None:
            raise RuntimeError("BVSatCtx used after destroy()")
        if not self._checked:
            raise RuntimeError("BVSatCtx.value() requires a prior SAT check()")
        out = ctypes.c_int64(0)
        rc = self._lib.zsp_bbsolver_value(
            self._bb, ctypes.c_uint32(var_id), ctypes.byref(out))
        if rc != 0:
            raise RuntimeError(
                "zsp_bbsolver_value failed for var %d (rc=%d)" % (var_id, rc))
        return out.value

    def value_wide(self, var_id: int, width: int, is_signed: bool = False) -> int:
        """Return the full (possibly >64-bit) model value for ``var_id``.

        Reassembles the value from little-endian 64-bit limbs, so it is correct
        for ``width > 64`` where :meth:`value` cannot represent the result.
        ``width``/``is_signed`` come from the variable's declaration; a signed
        value is sign-extended from its MSB.
        """
        if self._bb is None:
            raise RuntimeError("BVSatCtx used after destroy()")
        if not self._checked:
            raise RuntimeError("BVSatCtx.value_wide() requires a prior SAT check()")
        n_limbs = (width + 63) // 64 if width > 0 else 1
        buf = (ctypes.c_uint64 * n_limbs)()
        rc = self._lib.zsp_bbsolver_value_wide(
            self._bb, ctypes.c_uint32(var_id), buf, ctypes.c_uint32(n_limbs))
        if rc != 0:
            raise RuntimeError(
                "zsp_bbsolver_value_wide failed for var %d (rc=%d)" % (var_id, rc))
        val = 0
        for i in range(n_limbs):
            val |= int(buf[i]) << (64 * i)
        # Mask to width, then sign-extend from the MSB if signed.
        if width > 0:
            val &= (1 << width) - 1
            if is_signed and (val >> (width - 1)) & 1:
                val -= (1 << width)
        return val

    def try_value(self, var_id: int) -> "Optional[int]":
        """Like :meth:`value` but returns ``None`` instead of raising."""
        try:
            return self.value(var_id)
        except Exception:
            return None

    # ------------------------------------------------------------------ #
    # Diagnostics (post-check)                                            #
    # ------------------------------------------------------------------ #

    def stats(self) -> dict:
        """AIG/CNF size counters — useful for bit-blast blow-up telemetry."""
        if self._bb is None:
            raise RuntimeError("BVSatCtx used after destroy()")
        return {
            "aig_ands":   self._lib.zsp_bbsolver_num_aig_ands(self._bb),
            "sat_clauses": self._lib.zsp_bbsolver_num_sat_clauses(self._bb),
            "sat_vars":   self._lib.zsp_bbsolver_num_sat_vars(self._bb),
        }


def maxsat_keepset(problem, seed: int, n_softs: int):
    """Run the soft-aware MaxSAT serve on ``problem`` and return
    ``(rc, keep_array)``.

    ``rc`` is ``BVSAT_SAT`` / ``BVSAT_UNSAT`` / ``BVSAT_UNKNOWN`` / ``BVSAT_ERROR``.
    On SAT, ``keep_array`` is a ``ctypes`` ``c_uint8`` array of length ``n_softs``
    holding the maximal priority-respecting kept-soft set (``softs_head`` walk
    order). Pass it as ``soft_keep`` to :meth:`BVSatCtx.check` on each sampler
    draw so the distribution enforces exactly the kept softs — the model order is
    fixed by a deterministic rebuild, so one mask is valid across all rebuilds.
    Otherwise ``keep_array`` is ``None``.

    The internally-solved bbsolver is freed here; the caller re-solves through the
    sampler to draw a well-distributed model under the kept set.
    """
    lib = _load_lib()
    if lib is None:
        raise _library_not_found_error()
    sp_ptr = getattr(problem, "_sp", None)
    if sp_ptr is None:
        sp_ptr = ctypes.cast(problem, ctypes.c_void_p).value

    keep = (ctypes.c_uint8 * max(1, n_softs))()
    out_bb = ctypes.c_void_p()
    rc = lib.zsp_bbsolver_check_maxsat(
        None, sp_ptr, ctypes.c_uint64(seed & ((1 << 64) - 1)),
        ctypes.byref(out_bb), keep, ctypes.c_uint32(n_softs))
    if out_bb:
        lib.zsp_bbsolver_free(out_bb)
    if rc != BVSAT_SAT:
        return rc, None
    return rc, keep
