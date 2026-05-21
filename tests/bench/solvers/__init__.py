"""Solver registry — all back-ends in one list.

Import ``solvers`` from here (or from ``conftest``) for use in
``@pytest.mark.parametrize("solver", solvers(), ids=str)``.

Set ``ZSP_SOLVERS`` to a comma-separated list of solver IDs to restrict
which backends are instantiated, e.g.::

    ZSP_SOLVERS=native,sim-mti pytest ...

Valid IDs: ``python``, ``native``, ``bitwuzla``, ``sim-mti``, ``sim-vlt``.
When the variable is unset, all solvers are returned.
"""
import os
from .python_solver   import PythonSolver
from .native_solver   import NativeSolver
from .sim_solver      import SimSolver, _SIM_EXECUTABLES
from .bitwuzla_solver import BitwuzlaSolver
from .c_exe_solver    import CExeSolver


def solvers():
    """Return solver instances filtered by the ``ZSP_SOLVERS`` env var.

    Each solver still calls ``pytest.skip()`` internally when its binary or
    library is unavailable, so skips appear in the result matrix.
    """
    all_solvers = [
        PythonSolver(),
        NativeSolver(),
        BitwuzlaSolver(),
        CExeSolver(),
        *[SimSolver(sim_id, exe) for exe, sim_id in _SIM_EXECUTABLES.items()],
    ]

    env = os.environ.get("ZSP_SOLVERS", "").strip()
    if not env:
        return all_solvers

    allowed = {s.strip() for s in env.split(",")}
    return [s for s in all_solvers if str(s) in allowed]
