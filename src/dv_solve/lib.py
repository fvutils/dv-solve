"""ctypes handle for libdv_solve.so.

Call ``_load_lib()`` to obtain the cached CDLL handle (or None when the
library is not available).  The first successful call also wires all
argtypes/restypes so callers never have to do it themselves.
"""
from __future__ import annotations

import ctypes
import os
import platform
from pathlib import Path
from typing import Optional

# ------------------------------------------------------------------ #
# Module-level cache                                                   #
# ------------------------------------------------------------------ #

_LIB_CACHE: Optional[ctypes.CDLL] = None
_LOAD_ATTEMPTED = False


def _lib_patterns() -> list[str]:
    """Glob patterns for the dv_solve shared library on the current platform."""
    system = platform.system()
    if system == "Windows":
        return ["dv_solve.dll", "libdv_solve.dll"]
    if system == "Darwin":
        return ["libdv_solve.dylib"]
    # Linux / other ELF platforms: bare name plus any soname suffix.
    return ["libdv_solve.so", "libdv_solve.so.*"]


def _candidate_paths() -> list[Path]:
    """Return ordered list of directories to search for the solver library."""
    candidates: list[Path] = []

    # 1. Explicit override
    explicit = os.environ.get("ZSP_SOLVER_PATH")
    if explicit:
        candidates.append(Path(explicit))

    # 2. LD_LIBRARY_PATH entries
    ld_path = os.environ.get("LD_LIBRARY_PATH", "")
    for d in ld_path.split(":"):
        if d:
            candidates.append(Path(d))

    # 3. The installed-package directory itself (binary wheel layout): the
    #    native lib is bundled next to __init__.py (and optionally in a lib/
    #    subdir). This is what makes a binary wheel work with no source tree.
    pkg_dir = Path(__file__).parent
    candidates.append(pkg_dir)
    candidates.append(pkg_dir / "lib")

    # 4. Common build directories relative to this file:
    #    packages/dv-solve/src/dv_solve/lib.py
    #    → packages/dv-solve/  (up 3 levels from lib.py: dv_solve/ -> src/ ->
    #    dv-solve/). Was up 4 levels, which landed at packages/ and never found
    #    packages/dv-solve/build, so the lib only loaded via ZSP_SOLVER_PATH /
    #    LD_LIBRARY_PATH (Phase E / E4-2).
    #    Includes the CMake install layout (build/lib, build/lib64) produced by
    #    the ivpm-build backend's `ninja install`, as well as the bare build dir
    #    produced by a plain `cmake -B build`.
    pkg_root = Path(__file__).parent.parent.parent
    for build_name in ("build", "_build", "build_release", "cmake-build-release"):
        candidates.append(pkg_root / build_name)
        candidates.append(pkg_root / build_name / "lib")
        candidates.append(pkg_root / build_name / "lib64")

    # 5. pytest build dir pattern used by conftest.py
    tmp_prefix = Path("/tmp")
    for d in tmp_prefix.glob("pytest-*/zsp_build*"):
        candidates.append(d)

    return candidates


def _find_library() -> Optional[Path]:
    """Search candidate directories and return the first matching library."""
    patterns = _lib_patterns()
    for d in _candidate_paths():
        if not d.is_dir():
            continue
        for pattern in patterns:
            hits = sorted(d.glob(pattern), key=lambda p: len(p.name))
            if hits:
                return hits[0]
    return None


def _library_not_found_error() -> RuntimeError:
    """Build an actionable error for when the native library is unavailable."""
    patterns = " / ".join(_lib_patterns())
    searched = "\n".join("  - %s" % d for d in _candidate_paths())
    return RuntimeError(
        "dv-solve native library (%s) not found — the solver is unavailable.\n"
        "Searched:\n%s\n"
        "Fixes:\n"
        "  - Install a binary wheel:  pip install dv-solve\n"
        "  - Build from source (needs CMake + a C compiler):\n"
        "        cmake -S . -B build -G Ninja -DCMAKE_INSTALL_PREFIX=build\n"
        "        ninja -C build install\n"
        "  - Or point ZSP_SOLVER_PATH at the directory containing the library."
        % (patterns, searched)
    )


def _wire_argtypes(lib: ctypes.CDLL) -> None:
    """Set argtypes and restypes on every exported function."""
    _wire_builder_argtypes(lib)
    c = ctypes

    # zsp_block_alloc
    lib.zsp_block_alloc_create.restype  = c.c_void_p
    lib.zsp_block_alloc_create.argtypes = [c.c_void_p, c.c_size_t]
    lib.zsp_block_alloc_destroy.restype  = None
    lib.zsp_block_alloc_destroy.argtypes = [c.c_void_p]

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

    # SolveCtx
    lib.solver_create.restype  = c.c_void_p
    lib.solver_create.argtypes = [c.c_void_p, c.c_size_t, c.c_void_p]
    lib.solver_destroy.restype  = None
    lib.solver_destroy.argtypes = [c.c_void_p]
    lib.solver_compile.restype  = c.c_int
    lib.solver_compile.argtypes = [c.c_void_p, c.c_void_p]
    lib.solver_solve.restype  = c.c_int
    lib.solver_solve.argtypes = [c.c_void_p, c.c_void_p]  # ctx, SolveOpts*
    lib.solver_get_value.restype  = c.c_int64
    lib.solver_get_value.argtypes = [c.c_void_p, c.c_uint32]

    lib.solver_add_constraint.restype  = c.c_int
    lib.solver_add_constraint.argtypes = [c.c_void_p, c.c_void_p]

    lib.solver_exclude_value.restype  = c.c_int
    lib.solver_exclude_value.argtypes = [c.c_void_p, c.c_uint32, c.c_int64]

    lib.solver_add_array_vars.restype  = c.c_int
    lib.solver_add_array_vars.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32,
                                          c.c_uint8, c.c_uint8,
                                          c.c_int64, c.c_int64]

    lib.solver_checkpoint.restype  = c.c_int
    lib.solver_checkpoint.argtypes = [c.c_void_p]
    lib.solver_restore.restype  = None
    lib.solver_restore.argtypes = [c.c_void_p, c.c_uint32]

    lib.solver_propagate_only.restype  = c.c_int
    lib.solver_propagate_only.argtypes = [c.c_void_p]

    # Reset / re-solve helpers
    lib.solver_reset.restype  = None
    lib.solver_reset.argtypes = [c.c_void_p]

    lib.solver_set_seed.restype  = None
    lib.solver_set_seed.argtypes = [c.c_void_p, c.c_uint64]

    lib.solver_get_values.restype  = None
    lib.solver_get_values.argtypes = [c.c_void_p, c.c_uint32,
                                      c.POINTER(c.c_uint32),
                                      c.POINTER(c.c_int64)]

    lib.solver_solve_n.restype  = c.c_int
    lib.solver_solve_n.argtypes = [c.c_void_p, c.c_uint32,
                                   c.c_uint32, c.POINTER(c.c_uint32),
                                   c.POINTER(c.c_int64),
                                   c.c_uint64, c.c_uint32]

    # Variable query helpers
    lib.zsp_var_lo32.restype  = c.c_int32
    lib.zsp_var_lo32.argtypes = [c.c_void_p, c.c_uint32]
    lib.zsp_var_hi32.restype  = c.c_int32
    lib.zsp_var_hi32.argtypes = [c.c_void_p, c.c_uint32]

    lib.zsp_prop_constraint_id.restype  = c.c_uint32
    lib.zsp_prop_constraint_id.argtypes = [c.c_void_p, c.c_uint32]

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
    lib.solver_optimize.restype  = c.c_int
    lib.solver_optimize.argtypes = [c.c_void_p, c.c_uint32,
                                     c.c_void_p, c.c_void_p]
    lib.solver_set_value_selector.restype  = None
    lib.solver_set_value_selector.argtypes = [c.c_void_p, c.c_void_p, c.c_void_p]

    # BV-SAT completeness engine (zsp_bbsolver). Takes a SolveProblem buffer
    # (the same one solver_compile consumes) and answers SAT/UNSAT
    # authoritatively via bit-blasting + kissat. See bvsat.py.
    lib.zsp_bbsolver_new.restype  = c.c_void_p
    lib.zsp_bbsolver_new.argtypes = [c.c_void_p, c.c_void_p]   # alloc, problem
    lib.zsp_bbsolver_free.restype  = None
    lib.zsp_bbsolver_free.argtypes = [c.c_void_p]
    lib.zsp_bbsolver_check.restype  = c.c_int                  # ZSP_BB_SAT/UNSAT/...
    lib.zsp_bbsolver_check.argtypes = [c.c_void_p, c.c_uint64]  # bb, seed
    # DSE-2 soft-aware serve.
    lib.zsp_bbsolver_check_maxsat.restype  = c.c_int
    lib.zsp_bbsolver_check_maxsat.argtypes = [
        c.c_void_p, c.c_void_p, c.c_uint64,                    # alloc, problem, seed
        c.POINTER(c.c_void_p), c.c_void_p, c.c_uint32]         # out_bb, out_keep, keep_cap
    lib.zsp_bbsolver_set_soft_keep.restype  = None
    lib.zsp_bbsolver_set_soft_keep.argtypes = [c.c_void_p, c.c_void_p, c.c_uint32]
    lib.zsp_bbsolver_value.restype  = c.c_int                  # 0 on success
    lib.zsp_bbsolver_value.argtypes = [c.c_void_p, c.c_uint32,
                                       c.POINTER(c.c_int64)]
    lib.zsp_bbsolver_value_wide.restype  = c.c_int             # 0 on success
    lib.zsp_bbsolver_value_wide.argtypes = [c.c_void_p, c.c_uint32,
                                            c.POINTER(c.c_uint64), c.c_uint32]
    lib.zsp_bbsolver_num_aig_ands.restype  = c.c_uint64
    lib.zsp_bbsolver_num_aig_ands.argtypes = [c.c_void_p]
    lib.zsp_bbsolver_num_sat_clauses.restype  = c.c_uint64
    lib.zsp_bbsolver_num_sat_clauses.argtypes = [c.c_void_p]
    lib.zsp_bbsolver_num_sat_vars.restype  = c.c_uint64
    lib.zsp_bbsolver_num_sat_vars.argtypes = [c.c_void_p]


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

    lib.builder_create.restype  = c.c_void_p
    lib.builder_create.argtypes = [c.c_uint32, c.c_void_p]

    lib.builder_reset.restype  = None
    lib.builder_reset.argtypes = [c.c_void_p]

    lib.builder_destroy.restype  = None
    lib.builder_destroy.argtypes = [c.c_void_p]

    lib.builder_virtual_used.restype  = c.c_uint32
    lib.builder_virtual_used.argtypes = [c.c_void_p]

    lib.builder_finalize.restype  = c.c_void_p
    lib.builder_finalize.argtypes = [c.c_void_p, c.POINTER(c.c_size_t)]

    lib.builder_free_problem.restype  = None
    lib.builder_free_problem.argtypes = [c.c_void_p, c.c_void_p, c.c_size_t]

    lib.builder_alloc.restype  = c.c_uint32
    lib.builder_alloc.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.builder_expr_const.restype  = c.c_uint32
    lib.builder_expr_const.argtypes = [c.c_void_p, c.c_int64, c.c_uint8]

    lib.builder_expr_var.restype  = c.c_uint32
    lib.builder_expr_var.argtypes = [c.c_void_p, c.c_uint32]

    lib.builder_expr_binary.restype  = c.c_uint32
    lib.builder_expr_binary.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint32, c.c_uint32]

    lib.builder_expr_unary.restype  = c.c_uint32
    lib.builder_expr_unary.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.builder_expr_ite.restype  = c.c_uint32
    lib.builder_expr_ite.argtypes = [c.c_void_p,
                                     c.c_uint32, c.c_uint32, c.c_uint32]

    lib.builder_expr_in_range.restype  = c.c_uint32
    lib.builder_expr_in_range.argtypes = [c.c_void_p,
                                          c.c_uint32, c.c_uint32, c.c_uint32]

    lib.builder_expr_in_set.restype  = c.c_uint32
    lib.builder_expr_in_set.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint32, c.c_void_p]

    lib.builder_expr_in_ranges.restype  = c.c_uint32
    lib.builder_expr_in_ranges.argtypes = [c.c_void_p, c.c_uint32,
                                           c.c_uint32, c.c_void_p, c.c_void_p]

    lib.builder_expr_extend.restype  = c.c_uint32
    lib.builder_expr_extend.argtypes = [c.c_void_p, c.c_uint32,
                                        c.c_uint8, c.c_uint8, c.c_uint8]

    lib.builder_expr_extract.restype  = c.c_uint32
    lib.builder_expr_extract.argtypes = [c.c_void_p, c.c_uint32,
                                         c.c_uint8, c.c_uint8]

    lib.builder_add_var.restype  = c.c_uint32
    lib.builder_add_var.argtypes = [c.c_void_p, c.c_uint32,
                                    c.c_uint8, c.c_uint8,
                                    c.c_int64, c.c_int64]

    lib.builder_add_constraint.restype  = c.c_uint32
    lib.builder_add_constraint.argtypes = [c.c_void_p, c.c_uint32]

    lib.builder_add_source.restype  = c.c_uint32
    lib.builder_add_source.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]

    lib.builder_add_all_different.restype  = c.c_uint32
    lib.builder_add_all_different.argtypes = [c.c_void_p, c.c_uint32, c.c_void_p]

    lib.builder_expr_array_select.restype  = c.c_uint32
    lib.builder_expr_array_select.argtypes = [c.c_void_p, c.c_uint32,
                                              c.c_uint32, c.c_uint32, c.c_uint32]

    lib.builder_expr_sum.restype  = c.c_uint32
    lib.builder_expr_sum.argtypes = [c.c_void_p, c.c_uint32,
                                     c.c_uint32, c.c_void_p]

    lib.builder_expr_countones.restype  = c.c_uint32
    lib.builder_expr_countones.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.builder_expr_clog2.restype  = c.c_uint32
    lib.builder_expr_clog2.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.builder_add_soft_constraint.restype  = c.c_uint32
    lib.builder_add_soft_constraint.argtypes = [c.c_void_p, c.c_uint32, c.c_uint32]

    lib.builder_add_dist.restype  = c.c_uint32
    lib.builder_add_dist.argtypes = [c.c_void_p, c.c_uint32,
                                     c.c_uint32, c.c_void_p]
