"""Verify dv_solve has no runtime Zuspec dependency."""
import sys


def test_import_no_zuspec():
    """dv_solve core modules must not trigger any zuspec.* import."""
    zuspec_keys = [k for k in sys.modules if k.startswith("zuspec")]
    for k in zuspec_keys:
        del sys.modules[k]

    class _BlockZuspec:
        def find_module(self, name, path=None):
            if name.startswith("zuspec"):
                raise ImportError(f"Blocked Zuspec import: {name}")
            return None

    sys.meta_path.insert(0, _BlockZuspec())
    try:
        import importlib
        import dv_solve
        import dv_solve.lib
        import dv_solve.problem
        import dv_solve.builder
        import dv_solve.ctx
        import dv_solve.partitioner
        import dv_solve.icl
        import dv_solve.state_graph
        import dv_solve.scheduling_graph
        import dv_solve.structural_solver
        import dv_solve.stream_solver
        import dv_solve.flow_constraint_store
        import dv_solve.buffer_inference
    finally:
        sys.meta_path.pop(0)
