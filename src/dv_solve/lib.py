"""ctypes handle for libdv_solve.so.

Call ``_load_lib()`` to obtain the cached CDLL handle (or None when the
library is not available).  The first successful call also wires all
argtypes/restypes so callers never have to do it themselves.
"""
from __future__ import annotations

import ctypes
from pathlib import Path
from typing import Optional

from . import _resolve

# ------------------------------------------------------------------ #
# Module-level cache                                                   #
# ------------------------------------------------------------------ #

_LIB_CACHE: Optional[ctypes.CDLL] = None
_LOAD_ATTEMPTED = False


def _lib_patterns() -> list[str]:
    """Glob patterns for the dv_solve shared library on the current platform."""
    return _resolve.lib_patterns("dv_solve")


def _candidate_paths() -> list[Path]:
    """Ordered directories searched for the solver library.

    Delegates to :mod:`dv_solve._resolve` so that what the ctypes loader opens
    and what ``get_libdirs()`` reports for linking can never diverge -- see
    that module's docstring for the contract and for why ``LD_LIBRARY_PATH``
    is now a fallback rather than an override.

    Two behaviours changed with that move. ``/tmp/pytest-*/dvs_build*`` is no
    longer searched: it let an unrelated, possibly half-built test tree supply
    the solver to production code on any developer box that had ever run the
    suite. The C unit-test fixtures never relied on it -- they build into a
    tmp_path and hand the path to ``ctypes.CDLL`` directly -- and any test that
    wants a specific build must now say so via ``DVS_SOLVER_PATH``.
    """
    return [Path(d) for d in _resolve.lib_search_dirs()]


def _find_library() -> Optional[Path]:
    """Search candidate directories and return the first matching library."""
    found = _resolve.find_library("dv_solve")
    return Path(found) if found else None


def _library_not_found_error() -> RuntimeError:
    """Build an actionable error for when the native library is unavailable.

    When an installation WAS selected (``DVS_SOLVER_PATH`` set, typically) the
    error names it, rather than reporting a generic search failure.
    """
    if _resolve.select_installation() is not None:
        try:
            _resolve.require_library("dv_solve")
        except RuntimeError as e:
            return e
    return _resolve.missing_artifact_error(
        "native library (%s)" % " / ".join(_lib_patterns()),
        "The solver is unavailable.")


def _wire_argtypes(lib: ctypes.CDLL) -> None:
    """Set argtypes and restypes on every exported function."""
    _wire_builder_argtypes(lib)
    c = ctypes

    # dvs_block_alloc
    lib.dvs_block_alloc_create.restype  = c.c_void_p
    lib.dvs_block_alloc_create.argtypes = [c.c_void_p, c.c_size_t]
    lib.dvs_block_alloc_destroy.restype  = None
    lib.dvs_block_alloc_destroy.argtypes = [c.c_void_p]

    # SolveProblem
    lib.solve_problem_init.restype  = c.c_void_p
    lib.solve_problem_init.argtypes = [c.c_void_p, c.c_size_t]
    lib.solve_problem_reset.restype  = None
    lib.solve_problem_reset.argtypes = [c.c_void_p]
    lib.solve_problem_destroy.restype  = None
    lib.solve_problem_destroy.argtypes = [c.c_void_p]

    lib.problem_add_var.restype  = c.c_uint32
    lib.problem_add_var.argtypes = [c.c_void_p, c.c_uint32,
                                    c.c_uint8, c.c_uint8,
                                    c.c_int64, c.c_int64]
    lib.problem_add_constraint.restype  = c.c_uint32
    lib.problem_add_constraint.argtypes = [c.c_void_p, c.c_uint32]
    lib.problem_add_source.restype  = c.c_uint32
    lib.problem_add_source.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]

    lib.problem_add_all_different.restype  = c.c_uint32
    lib.problem_add_all_different.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]

    lib.problem_add_soft_constraint.restype  = c.c_uint32
    lib.problem_add_soft_constraint.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.problem_add_dist.restype  = c.c_uint32
    lib.problem_add_dist.argtypes = [c.c_void_p, c.c_uint32,
                                     c.c_uint32, c.c_void_p]

    # Expression builders
    lib.expr_const.restype  = c.c_uint32
    lib.expr_const.argtypes = [c.c_void_p, c.c_int64, c.c_uint8]
    lib.expr_const_sized.restype  = c.c_uint32
    lib.expr_const_sized.argtypes = [c.c_void_p, c.c_int64, c.c_uint8, c.c_uint8]
    lib.expr_var.restype  = c.c_uint32
    lib.expr_var.argtypes = [c.c_void_p, c.c_uint32]
    lib.expr_binary.restype  = c.c_uint32
    lib.expr_binary.argtypes = [c.c_void_p, c.c_int32, c.c_uint32, c.c_uint32]
    lib.expr_unary.restype  = c.c_uint32
    lib.expr_unary.argtypes = [c.c_void_p, c.c_int32, c.c_uint32]
    lib.expr_ite.restype  = c.c_uint32
    lib.expr_ite.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32, c.c_uint32]
    lib.expr_in_range.restype  = c.c_uint32
    lib.expr_in_range.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32, c.c_uint32]
    lib.expr_in_set.restype  = c.c_uint32
    lib.expr_in_set.argtypes = [c.c_void_p, c.c_uint32,
                                c.c_uint32, c.c_void_p]
    lib.expr_extend.restype  = c.c_uint32
    lib.expr_extend.argtypes = [c.c_void_p, c.c_uint32,
                                c.c_uint8, c.c_uint8, c.c_uint8]
    lib.expr_extract.restype  = c.c_uint32
    lib.expr_extract.argtypes = [c.c_void_p, c.c_uint32,
                                 c.c_uint8, c.c_uint8]
    lib.expr_cast.restype  = c.c_uint32
    lib.expr_cast.argtypes = [c.c_void_p, c.c_uint32, c.c_uint8, c.c_uint8]

    # SolveCtx
    lib.dvs_solver_create.restype  = c.c_void_p
    lib.dvs_solver_create.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.dvs_solver_destroy.restype  = None
    lib.dvs_solver_destroy.argtypes = [c.c_void_p]
    lib.dvs_solver_compile.restype  = c.c_int
    lib.dvs_solver_compile.argtypes = [c.c_void_p, c.c_void_p]
    lib.dvs_solver_solve.restype  = c.c_int
    lib.dvs_solver_solve.argtypes = [c.c_void_p, c.c_void_p]  # ctx, SolveOpts*
    lib.dvs_solver_get_value.restype  = c.c_int64
    lib.dvs_solver_get_value.argtypes = [c.c_void_p, c.c_uint32]

    # Post-solve safety net: re-evaluates every constraint in the ORIGINAL
    # problem against the current assignment. Previously reachable only from
    # the SMT2 frontend, which left the randomization path with no way to check
    # that the model it just produced actually satisfies the problem submitted.
    # Third argument is a FILE* for diagnostics; None means "no output".
    lib.dvs_solver_validate_model.restype  = c.c_int
    lib.dvs_solver_validate_model.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p]

    lib.dvs_solver_add_constraint.restype  = c.c_int
    lib.dvs_solver_add_constraint.argtypes = [c.c_void_p, c.c_void_p]

    lib.dvs_solver_exclude_value.restype  = c.c_int
    lib.dvs_solver_exclude_value.argtypes = [c.c_void_p, c.c_uint32, c.c_int64]

    lib.dvs_solver_add_array_vars.restype  = c.c_int
    lib.dvs_solver_add_array_vars.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32,
                                          c.c_uint8, c.c_uint8,
                                          c.c_int64, c.c_int64]

    lib.dvs_solver_checkpoint.restype  = c.c_int
    lib.dvs_solver_checkpoint.argtypes = [c.c_void_p]
    lib.dvs_solver_restore.restype  = None
    lib.dvs_solver_restore.argtypes = [c.c_void_p, c.c_uint32]
    lib.dvs_solver_pin_var.restype  = c.c_int
    lib.dvs_solver_pin_var.argtypes = [c.c_void_p, c.c_uint32, c.c_int64]

    lib.dvs_solver_propagate_only.restype  = c.c_int
    lib.dvs_solver_propagate_only.argtypes = [c.c_void_p]

    # Reset / re-solve helpers
    lib.dvs_solver_reset.restype  = None
    lib.dvs_solver_reset.argtypes = [c.c_void_p]

    lib.dvs_solver_set_seed.restype  = None
    lib.dvs_solver_set_seed.argtypes = [c.c_void_p, c.c_uint64]

    lib.dvs_solver_get_values.restype  = None
    lib.dvs_solver_get_values.argtypes = [c.c_void_p, c.c_uint32,
                                      c.POINTER(c.c_uint32),
                                      c.POINTER(c.c_int64)]

    lib.dvs_solver_solve_n.restype  = c.c_int
    lib.dvs_solver_solve_n.argtypes = [c.c_void_p, c.c_uint32,
                                   c.c_uint32, c.POINTER(c.c_uint32),
                                   c.POINTER(c.c_int64),
                                   c.c_uint64, c.c_uint32]

    # Variable query helpers
    lib.dvs_var_lo32.restype  = c.c_int32
    lib.dvs_var_lo32.argtypes = [c.c_void_p, c.c_uint32]
    lib.dvs_var_hi32.restype  = c.c_int32
    lib.dvs_var_hi32.argtypes = [c.c_void_p, c.c_uint32]

    lib.dvs_prop_constraint_id.restype  = c.c_uint32
    lib.dvs_prop_constraint_id.argtypes = [c.c_void_p, c.c_uint32]

    # Placement propagators
    lib.prop_add_min_of_n_32.restype  = c.c_uint32
    lib.prop_add_min_of_n_32.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32,
                                          c.POINTER(c.c_uint32), c.c_uint8]
    lib.prop_add_max_of_n_32.restype  = c.c_uint32
    lib.prop_add_max_of_n_32.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32,
                                          c.POINTER(c.c_uint32), c.c_uint8]
    lib.prop_add_no_overlap_2d.restype  = c.c_uint32
    lib.prop_add_no_overlap_2d.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p,
                                            c.c_uint8]
    lib.dvs_solver_optimize.restype  = c.c_int
    lib.dvs_solver_optimize.argtypes = [c.c_void_p, c.c_uint32,
                                     c.c_void_p, c.c_void_p]
    lib.dvs_solver_set_value_selector.restype  = None
    lib.dvs_solver_set_value_selector.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p]

    # BV-SAT completeness engine (dvs_bbsolver). Takes a SolveProblem buffer
    # (the same one dvs_solver_compile consumes) and answers SAT/UNSAT
    # authoritatively via bit-blasting + kissat. See bvsat.py.
    lib.dvs_bbsolver_new.restype  = c.c_void_p
    lib.dvs_bbsolver_new.argtypes = [c.c_void_p, c.c_void_p]   # alloc, problem
    lib.dvs_bbsolver_free.restype  = None
    lib.dvs_bbsolver_free.argtypes = [c.c_void_p]
    lib.dvs_bbsolver_check.restype  = c.c_int                  # DVS_BB_SAT/UNSAT/...
    lib.dvs_bbsolver_check.argtypes = [c.c_void_p, c.c_uint64]  # bb, seed
    # DSE-2 soft-aware serve.
    lib.dvs_bbsolver_check_maxsat.restype  = c.c_int
    lib.dvs_bbsolver_check_maxsat.argtypes = [
        c.c_void_p, c.c_void_p, c.c_uint64,                    # alloc, problem, seed
        c.POINTER(c.c_void_p), c.c_void_p, c.c_uint32]         # out_bb, out_keep, keep_cap
    lib.dvs_bbsolver_set_soft_keep.restype  = None
    lib.dvs_bbsolver_set_soft_keep.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32]
    lib.dvs_bbsolver_value.restype  = c.c_int                  # 0 on success
    lib.dvs_bbsolver_value.argtypes = [c.c_void_p, c.c_uint32,
                                       c.POINTER(c.c_int64)]
    lib.dvs_bbsolver_value_wide.restype  = c.c_int             # 0 on success
    lib.dvs_bbsolver_value_wide.argtypes = [c.c_void_p, c.c_uint32,
                                            c.POINTER(c.c_uint64), c.c_uint32]
    lib.dvs_bbsolver_num_aig_ands.restype  = c.c_uint64
    lib.dvs_bbsolver_num_aig_ands.argtypes = [c.c_void_p]
    lib.dvs_bbsolver_num_sat_clauses.restype  = c.c_uint64
    lib.dvs_bbsolver_num_sat_clauses.argtypes = [c.c_void_p]
    lib.dvs_bbsolver_num_sat_vars.restype  = c.c_uint64
    lib.dvs_bbsolver_num_sat_vars.argtypes = [c.c_void_p]


def _load_lib() -> Optional[ctypes.CDLL]:
    """Return the cached CDLL handle, loading it on first call.

    Returns ``None`` when the library cannot be found on this host.
    """
    global _LIB_CACHE, _LOAD_ATTEMPTED
    if _LOAD_ATTEMPTED:
        return _LIB_CACHE

    _LOAD_ATTEMPTED = True
    lib_path = _find_library()
    if lib_path is None:
        return None

    try:
        lib = ctypes.CDLL(str(lib_path))
        _wire_argtypes(lib)
        _LIB_CACHE = lib
        return lib
    except OSError:
        return None


def _wire_builder_argtypes(lib: ctypes.CDLL) -> None:
    """Wire argtypes/restypes for the SolveProblemBuilder C API."""
    c = ctypes

    lib.dvs_builder_create.restype  = c.c_void_p
    lib.dvs_builder_create.argtypes = [c.c_uint32, c.c_void_p]

    lib.dvs_builder_reset.restype  = None
    lib.dvs_builder_reset.argtypes = [c.c_void_p]

    lib.dvs_builder_destroy.restype  = None
    lib.dvs_builder_destroy.argtypes = [c.c_void_p]

    lib.dvs_builder_virtual_used.restype  = c.c_uint32
    lib.dvs_builder_virtual_used.argtypes = [c.c_void_p]

    lib.dvs_builder_finalize.restype  = c.c_void_p
    lib.dvs_builder_finalize.argtypes = [c.c_void_p, c.POINTER(c.c_size_t)]

    lib.dvs_builder_free_problem.restype  = None
    lib.dvs_builder_free_problem.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t]

    lib.dvs_builder_alloc.restype  = c.c_uint32
    lib.dvs_builder_alloc.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.dvs_builder_expr_const.restype  = c.c_uint32
    lib.dvs_builder_expr_const.argtypes = [c.c_void_p, c.c_int64, c.c_uint8]
    lib.dvs_builder_expr_const_sized.restype  = c.c_uint32
    lib.dvs_builder_expr_const_sized.argtypes = [c.c_void_p, c.c_int64, c.c_uint8,
                                             c.c_uint8]

    lib.dvs_builder_expr_var.restype  = c.c_uint32
    lib.dvs_builder_expr_var.argtypes = [c.c_void_p, c.c_uint32]

    lib.dvs_builder_expr_binary.restype  = c.c_uint32
    lib.dvs_builder_expr_binary.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint32, c.c_uint32]

    lib.dvs_builder_expr_unary.restype  = c.c_uint32
    lib.dvs_builder_expr_unary.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.dvs_builder_expr_ite.restype  = c.c_uint32
    lib.dvs_builder_expr_ite.argtypes = [c.c_void_p,
                                     c.c_uint32, c.c_uint32, c.c_uint32]

    lib.dvs_builder_expr_in_range.restype  = c.c_uint32
    lib.dvs_builder_expr_in_range.argtypes = [c.c_void_p,
                                          c.c_uint32, c.c_uint32, c.c_uint32]

    lib.dvs_builder_expr_in_set.restype  = c.c_uint32
    lib.dvs_builder_expr_in_set.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint32, c.c_void_p]

    lib.dvs_builder_expr_in_ranges.restype  = c.c_uint32
    lib.dvs_builder_expr_in_ranges.argtypes = [c.c_void_p, c.c_uint32,
                                           c.c_uint32, c.c_void_p, c.c_void_p]

    lib.dvs_builder_expr_extend.restype  = c.c_uint32
    lib.dvs_builder_expr_extend.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint8, c.c_uint8, c.c_uint8]

    lib.dvs_builder_expr_cast.restype  = c.c_uint32
    lib.dvs_builder_expr_cast.argtypes = [c.c_void_p, c.c_uint32,
                                          c.c_uint8, c.c_uint8]

    lib.dvs_builder_expr_extract.restype  = c.c_uint32
    lib.dvs_builder_expr_extract.argtypes = [c.c_void_p, c.c_uint32,
                                         c.c_uint8, c.c_uint8]

    # Without explicit argtypes, ctypes passes the 64-bit builder pointer as a
    # C int, truncating it — harmless only when the builder happens to sit at a
    # low address, but a wild-pointer crash under ASAN / a high-address heap
    # (e.g. CI). Every other builder_expr_* is wired; concat was the one gap.
    # Without explicit argtypes, ctypes passes the 64-bit builder pointer as a
    # C int, truncating it — harmless only when the builder happens to sit at a
    # low address, but a wild-pointer crash under ASAN / a high-address heap
    # (e.g. CI). Every other builder_expr_* is wired; concat was the one gap.
    # Regression-guarded by pyvsc ve/unit/test_dvsolve_ctypes_wiring.py.
    lib.dvs_builder_expr_concat.restype  = c.c_uint32
    lib.dvs_builder_expr_concat.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint32, c.c_uint8]

    lib.dvs_builder_add_var.restype  = c.c_uint32
    lib.dvs_builder_add_var.argtypes = [c.c_void_p, c.c_uint32,
                                    c.c_uint8, c.c_uint8,
                                    c.c_int64, c.c_int64]

    lib.dvs_builder_add_constraint.restype  = c.c_uint32
    lib.dvs_builder_add_constraint.argtypes = [c.c_void_p, c.c_uint32]

    lib.dvs_builder_add_source.restype  = c.c_uint32
    lib.dvs_builder_add_source.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]

    lib.dvs_builder_add_all_different.restype  = c.c_uint32
    lib.dvs_builder_add_all_different.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]

    lib.dvs_builder_expr_array_select.restype  = c.c_uint32
    lib.dvs_builder_expr_array_select.argtypes = [c.c_void_p, c.c_uint32,
                                              c.c_uint32, c.c_uint32, c.c_uint32]

    lib.dvs_builder_expr_sum.restype  = c.c_uint32
    lib.dvs_builder_expr_sum.argtypes = [c.c_void_p, c.c_uint32,
                                     c.c_uint32, c.c_void_p]

    lib.dvs_builder_expr_countones.restype  = c.c_uint32
    lib.dvs_builder_expr_countones.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.dvs_builder_expr_clog2.restype  = c.c_uint32
    lib.dvs_builder_expr_clog2.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.dvs_builder_add_soft_constraint.restype  = c.c_uint32
    lib.dvs_builder_add_soft_constraint.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.dvs_builder_add_dist.restype  = c.c_uint32
    lib.dvs_builder_add_dist.argtypes = [c.c_void_p, c.c_uint32,
                                     c.c_uint32, c.c_void_p]
