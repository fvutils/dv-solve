# Installation

dv-solve is two things: a C library, and a set of front ends built on it (a
Python package, a SystemVerilog DPI package and the `dv-solve-smt2`
executable). How you install depends on which of them you need.

| You need | Install from |
|---|---|
| The Python API | PyPI |
| The DPI library and SystemVerilog packages | PyPI (they are bundled in the wheel) |
| The C library and headers | PyPI, or a source build |
| The `dv-solve-smt2` executable | PyPI, or a source build |

## From PyPI

```bash
pip install dv-solve
```

dv-solve needs Python 3.10 or newer. Check the
[project page](https://pypi.org/project/dv-solve/) for the platforms the
current release publishes wheels for. On other platforms, build from source.

The wheel carries the native libraries (`libdv_solve`, `libdv_solve_dpi`),
the public C headers and the SystemVerilog packages. The package can tell a
build system where they are:

```python
import dv_solve

dv_solve.get_libdirs()   # directories holding the shared libraries
dv_solve.get_incdirs()   # C include directories
dv_solve.get_svdirs()    # SystemVerilog package directories
dv_solve.get_dpi_lib()   # path to the DPI shared library
dv_solve.get_smt2_exe()  # path to dv-solve-smt2
```

The wheel also installs the `dv-solve-smt2` executable into the
environment's scripts directory (`bin/`, or `Scripts\` on Windows), so it is
on `PATH` whenever the environment is active. It is statically linked, so it
runs without the Python package.

## From source

You need CMake 3.14 or newer and a C compiler. A C++ compiler is also needed
for the optional CaDiCaL SAT back end (see below).

```bash
git clone https://github.com/fvutils/dv-solve.git
cd dv-solve
cmake -S . -B build -DDVS_WITH_CADICAL=OFF
cmake --build build
```

This produces, in `build/`:

- `libdv_solve` — the solver library
- `libdv_solve_dpi` — the DPI library for SystemVerilog simulators
- `dv-solve-smt2` — the SMT-LIB2 solver executable

Check the executable runs:

```bash
build/dv-solve-smt2 --version
```

### With the CaDiCaL back end

dv-solve always includes the kissat SAT solver. CaDiCaL is an optional
second SAT back end, used for incremental solving. Its source is fetched by
[IVPM](https://github.com/fvutils/ivpm):

```bash
ivpm update -d use        # fetches CaDiCaL into ./packages
cmake -S . -B build
cmake --build build
```

### Using the source build from Python

To use a source build from Python, put `src/` on `PYTHONPATH`. The package
looks for the native library in the source tree's `build/` directory:

```bash
PYTHONPATH=src python3 -c "import dv_solve; print(dv_solve.get_libdirs())"
```
