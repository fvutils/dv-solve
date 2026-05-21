# Benchmark Test Design

## Goal

A single test function per scenario, parametrized over every available back-end.
Adding a back-end or a scenario never requires changing existing test code.

Replaces the ad-hoc per-back-end files in `tests/benchmark/` and the
SV-only tests in `tests/bench/sim/`.

---

## Shape of every benchmark test

```python
# tests/bench/test_mem_transaction.py

def _check(sol):
    assert sol["addr"] % 4 == 0
    assert 1 <= sol["length"] <= 16
    assert sol["addr"] + sol["length"] <= 256

@pytest.mark.bench
@pytest.mark.parametrize("solver", solvers(), ids=str)
def test_mem_transaction(solver, tmp_path):
    solver.bench(MemTransaction, validate=_check, tmp_path=tmp_path)
```

That is the entire test.  `solver` is the parametrized back-end object.
The test body never changes regardless of which back-end is under test.

Resulting test node IDs:

```
test_mem_transaction[python]
test_mem_transaction[native]
test_mem_transaction[bitwuzla]
test_mem_transaction[sim-vlt]
test_mem_transaction[sim-mti]
```

---

## The `Solver` contract

```python
# tests/bench/solvers/base.py
from typing import Callable, Protocol
from pathlib import Path

class Solver(Protocol):
    def __str__(self) -> str:
        """Return the parametrize ID: "python", "native", "sim-vlt", …"""
        ...

    def bench(
        self,
        cls: type,                          # the @zdc.dataclass class
        validate: Callable[[dict], None],   # raises AssertionError on bad solution
        tmp_path: Path,
        n_solutions: int = 1000,
        min_bench_ns: int = 1_000_000_000,
    ) -> None:
        """
        Run the randomization benchmark for *cls*, assert correctness via
        *validate*, then print and persist throughput results.

        *validate* receives a plain dict[str, int] of field values — the same
        shape regardless of back-end.

        Call pytest.skip() if this solver is unavailable on the current host
        or cannot handle *cls* (e.g. a required tool is not on PATH).
        """
        ...
```

`validate` always receives `dict[str, int]` so a single checker works across
all back-ends.  The solver is responsible for the `pytest.skip()` call when it
cannot run; the test function itself contains no availability logic.

### BenchResult (internal to each solver)

```python
from dataclasses import dataclass

@dataclass
class BenchResult:
    solver:          str
    scenario:        str
    n_solutions:     int
    bench_ns:        int

    @property
    def ns_per_solution(self) -> float:
        return self.bench_ns / max(self.n_solutions, 1)

    @property
    def solutions_per_s(self) -> float:
        return self.n_solutions / (self.bench_ns / 1e9) if self.bench_ns > 0 else 0.0

    def summary(self) -> str:
        return (
            f"[{self.solver} / {self.scenario}]  {self.n_solutions} solutions  "
            f"{self.bench_ns/1e6:.1f} ms  "
            f"{self.ns_per_solution/1e3:.1f} µs/sol  "
            f"{self.solutions_per_s:.0f} sol/s"
        )

    def save(self, results_dir: Path) -> None:
        import json
        results_dir.mkdir(parents=True, exist_ok=True)
        path = results_dir / f"{self.scenario}_{self.solver}.json"
        path.write_text(json.dumps({
            "solver":          self.solver,
            "scenario":        self.scenario,
            "n_solutions":     self.n_solutions,
            "bench_ns":        self.bench_ns,
            "ns_per_solution": self.ns_per_solution,
            "solutions_per_s": self.solutions_per_s,
        }, indent=2))
```

---

## Back-end implementations

### `PythonSolver` / `NativeSolver`

Both wrap `zdc.randomize()`.  The only difference is the `ZSP_SOLVER_BACKEND`
environment variable.

```python
# solvers/python_solver.py
import os, time
import zuspec.dataclasses as zdc

BATCH = 500

class PythonSolver:
    _backend_name = "python"

    def __str__(self): return self._backend_name

    def bench(self, cls, validate, tmp_path, n_solutions=1000, min_bench_ns=1_000_000_000):
        obj = cls()

        # warm-up (excluded from timing)
        for _ in range(min(100, n_solutions // 10)):
            zdc.randomize(obj)

        iters, t = 0, time.perf_counter_ns()
        while iters < n_solutions or (time.perf_counter_ns() - t) < min_bench_ns:
            zdc.randomize(obj)
            iters += 1

        elapsed = time.perf_counter_ns() - t

        sol = {f: getattr(obj, f)
               for f in vars(obj) if not f.startswith("_")}
        validate(sol)

        result = BenchResult(str(self), cls.__name__.lower(), iters, elapsed)
        result.save(RESULTS_DIR)
        print("\n" + result.summary())
```

`NativeSolver` is identical except `_backend_name = "native"` and it sets
`ZSP_SOLVER_BACKEND=native` (via `monkeypatch` or `os.environ`) before the
loop, and calls `pytest.skip()` if `zuspec.solver` is not importable or the
shared library is not present.

### `SimSolver`

Uses `SolvePerfHarnessGenerator` (already in `src/zuspec/solver/bench_harness.py`)
to auto-generate a complete SV harness from the `@dataclass`, then compiles and
runs it via DV Flow.  No hand-written `.sv` files are needed.

```python
# solvers/sim_solver.py
import shutil, re
from zuspec.solver.bench_harness import SolvePerfHarnessGenerator

_SIM_EXECUTABLES = {
    "verilator": "vlt",
    "vsim":      "mti",
    "iverilog":  "ivl",
    "vcs":       "vcs",
}

class SimSolver:
    def __init__(self, sim_id: str, executable: str):
        self._sim_id   = sim_id
        self._exe      = executable

    def __str__(self): return f"sim-{self._sim_id}"

    def bench(self, cls, validate, tmp_path, n_solutions=1000, min_bench_ns=1_000_000_000):
        if not shutil.which(self._exe):
            pytest.skip(f"{self._exe} not found on PATH")

        # Generate SV from the dataclass — no hand-written harness needed
        sv_text = SolvePerfHarnessGenerator().emit(cls, n_solutions=n_solutions)
        sv_file = tmp_path / "harness.sv"
        sv_file.write_text(sv_text)

        bench_ns, solutions = _dv_flow_run(tmp_path, self._sim_id, sv_file)

        for sol in solutions[-5:]:          # spot-check last few solutions
            validate(sol)

        result = BenchResult(str(self), cls.__name__.lower(),
                             len(solutions), bench_ns)
        result.save(RESULTS_DIR)
        print("\n" + result.summary())
```

### `BitwuzlaSolver`

Uses `RandSMT2Emitter` to generate SMT-LIB2 from the `@dataclass`, then runs
bitwuzla as a subprocess with varying `--seed` values.

```python
# solvers/bitwuzla_solver.py
import shutil, subprocess, tempfile, time

class BitwuzlaSolver:
    def __str__(self): return "bitwuzla"

    def bench(self, cls, validate, tmp_path, n_solutions=1000, min_bench_ns=1_000_000_000):
        bitwuzla = shutil.which("bitwuzla")
        if bitwuzla is None:
            pytest.skip("bitwuzla not found on PATH")

        try:
            from zuspec.be.fv.smt2.rand_emitter import RandSMT2Emitter, parse_get_value
        except ImportError:
            pytest.skip("zuspec-be-fv not available")

        smt2_text = RandSMT2Emitter().emit(cls, seed=0)
        smt_file  = tmp_path / "problem.smt2"
        smt_file.write_text(smt2_text)

        field_names = [f["name"] for f in zdc.extract_rand_fields(cls)]

        iters, failures, seed = 0, 0, 0
        t = time.perf_counter_ns()
        while iters < n_solutions or (time.perf_counter_ns() - t) < min_bench_ns:
            out = subprocess.run(
                [bitwuzla, "-s", str(seed), str(smt_file)],
                capture_output=True, text=True,
            ).stdout
            if out.startswith("sat"):
                sol = parse_get_value(out, field_names)
                if iters % max(1, n_solutions // 10) == 0:
                    validate(sol)          # periodic spot-check
            else:
                failures += 1
            iters += 1
            seed  += 1

        elapsed = time.perf_counter_ns() - t
        assert failures == 0, f"{failures} unsatisfiable results"

        result = BenchResult("bitwuzla", cls.__name__.lower(), iters, elapsed)
        result.save(RESULTS_DIR)
        print("\n" + result.summary())
```

---

## `conftest.py` — solver registry

```python
# tests/bench/conftest.py
from pathlib import Path
from .solvers.python_solver   import PythonSolver
from .solvers.native_solver   import NativeSolver
from .solvers.sim_solver      import SimSolver, _SIM_EXECUTABLES
from .solvers.bitwuzla_solver import BitwuzlaSolver

RESULTS_DIR = Path(__file__).parent / "results"

def solvers():
    """Return all solver instances.  Availability is checked inside bench()."""
    return [
        PythonSolver(),
        NativeSolver(),
        BitwuzlaSolver(),
        *[SimSolver(sim_id, exe) for exe, sim_id in _SIM_EXECUTABLES.items()],
    ]

def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "bench: throughput benchmark (use -m bench to run)",
    )
```

All solver instances are always collected; unavailable ones emit a skip at
runtime.  This means `pytest --collect-only` always shows the full matrix,
making it obvious which back-ends are exercised on a given host.

---

## Directory layout

```
tests/bench/
├── BENCH_DESIGN.md              # this document
├── conftest.py                  # solvers(), marker, RESULTS_DIR
├── solvers/
│   ├── __init__.py
│   ├── base.py                  # Solver Protocol + BenchResult
│   ├── python_solver.py         # ZSP_SOLVER_BACKEND=python
│   ├── native_solver.py         # ZSP_SOLVER_BACKEND=native
│   ├── sim_solver.py            # DV Flow + SolvePerfHarnessGenerator
│   └── bitwuzla_solver.py       # subprocess bitwuzla + RandSMT2Emitter
├── results/                     # JSON output: {scenario}_{solver}.json
├── test_mem_transaction.py      # ~8 lines
├── test_ethernet_hdr.py         # ~8 lines
├── test_bus_transaction.py      # ~8 lines
└── test_fifo_ctrl.py            # ~8 lines
```

The existing directories `tests/benchmark/` and `tests/bench/sim/` are
superseded by this structure and can be removed once migration is complete.
The hand-written SV corpus harnesses in `tests/bench/sim/data/corpus/` are
retired; `SolvePerfHarnessGenerator` produces equivalent SV automatically.

---

## Adding a new back-end

1. Create `solvers/my_solver.py` implementing the `Solver` protocol.
2. Add one line to `conftest.py`: `MyNewSolver()` in the `solvers()` list.
3. No existing test files change.

## Adding a new scenario

1. Create `test_<scenario>.py` (copy the 8-line template above).
2. Write a `_check(sol)` validator.
3. No existing files change.
