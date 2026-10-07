# Plan: ship `dv-solve-smt2` in the wheel

Status: **in progress** — 2026-10-04. P1–P4 and P6 docs implemented; P5
(Windows) deferred. Target release: 0.2.3.

## Goal

`pip install dv-solve` puts a working `dv-solve-smt2` on `PATH`. With that,
a Verilator user can write

```bash
export VERILATOR_SOLVER="dv-solve-smt2 --interactive --mode=verilator"
```

with no source build, no `LD_LIBRARY_PATH`, and no knowledge of where pip put
the package.

## Where we are

- CMake builds `dv-solve-smt2` (`CMakeLists.txt:403`) and installs it to `bin`
  (`CMakeLists.txt:412`). The wheel never runs `cmake --install`, so that rule
  has no effect on it.
- The wheel contains only what `[[tool.ivpm-build.extra-data]]` stages into
  the package: the three shared libraries, the headers and the SV sources.
  `package-data` has no pattern that would match an executable.
- There is no `[project.scripts]` entry and no CLI module.
- The executable links `libdv_solve.so` dynamically with no RPATH, so even a
  copied binary only runs when `LD_LIBRARY_PATH` points at the library.
- The docs say so explicitly: `README.md:29` and
  `docs/site/getting-started/install.md:12` describe the executable as
  source-build only.
- Under MSVC, `DV_SOLVE_BUILD_TOOLS` is forced `OFF` (`CMakeLists.txt:106`).
  `smt2_main.c` uses `<unistd.h>` and `<pthread.h>`, and `smt2_frontend.c`
  uses `<sys/resource.h>`, so there is no Windows executable today.

## Precedent

**z3-solver** is the model. In `z3_solver-5.1.0.0` (manylinux x86_64):

- The executable is at `z3_solver-5.1.0.0.data/data/bin/z3`, next to the
  Python package's `z3/lib/libz3.so`.
- pip installs `.data/` contents into the environment's scheme directories,
  so `z3` lands in `<env>/bin` and is on `PATH` in an activated venv.
- No entry point and no Python launcher. The binary is a real executable.
- `z3` is statically linked against z3 itself (25 MB, the same size as
  `libz3.so`). It needs only system libraries: libstdc++, libm, libgcc_s,
  libpthread and libc. That is what lets it run from `bin/` without knowing
  where `site-packages` is.

Other wheels ship native binaries the same way, either in `.data/scripts`
(ruff, uv) or inside the package behind a Python `console_scripts` launcher
(cmake, ninja).

**In our own ecosystem there is no precedent for a separate binary wheel.**
The edapack `*-bin` packages (`verilator-bin`, `yosys-bin`) are GitHub release
tarballs that ivpm installs and puts on `PATH` through `export.envrc` /
`ivpm.yaml`. None of them is on PyPI. `yosys-bin` has a `pyproject.toml`, but
it only packages the dv-flow plugin, not the binaries.

## Size

Current local Release build, x86_64, stripped:

| Artifact | Raw | Compressed (gzip -9) |
|---|---|---|
| `dv-solve-smt2`, linked to the shared library | 141 KB | 64 KB |
| `libdv_solve.so` | 2.2 MB | 1.0 MB |
| `libdv_solve_dpi.so` | 2.2 MB | 1.0 MB |
| `libdv_solve_debug.so` | 2.2 MB | 1.0 MB |

A statically linked `dv-solve-smt2` should be about the size of
`libdv_solve.so` (that library already contains kissat and CaDiCaL): roughly
2.2 MB raw, or about 1 MB more per wheel once compressed.

The last published wheel (0.1.0) is 1.1 MB. z3-solver's wheel is 33 MB, and
PyPI's default limit is 100 MB per file. **Size does not justify a separate
wheel.** A separate wheel would also bring a second build matrix, a second
release and a version-matching rule, all for about 1 MB.

## Decisions

| # | Decision | Choice | Why |
|---|---|---|---|
| D1 | One wheel or two | One: the `dv-solve` wheel carries the binary | Size is negligible (see above), and one wheel means one release and nothing to keep in step |
| D2 | Native binary or Python launcher | Native binary, no launcher | Matches z3. No Python start-up per run. No dependency on the package being importable |
| D3 | How the binary finds `libdv_solve` | Static link: `dv_solve` compiled into the executable (`dv_solve_static`) | The relative path from `<env>/bin` to `site-packages/dv_solve` varies (venv vs `--user`, `lib` vs `lib64`, the Python version in the path), so an RPATH can't reliably reach it |
| D4 | Wheel location | `.data/scripts/`, not z3's `.data/data/bin/` | `scripts` installs to `bin/` on POSIX and `Scripts\` on Windows, which is on `PATH` in a venv. `data/bin` would land in `<venv>\bin` on Windows, which isn't. This keeps a later Windows port free of layout changes |
| D5 | Windows | Not in this change. The Windows wheel ships without the binary, as now | The tool needs POSIX headers. Port it separately (P5) |
| D6 | Mechanism for `.data/scripts` | New ivpm-build option (see P1) | Reusable by other ivpm-build projects. Avoids a hack in dv-solve's `setup.py` |

## Work

### P1 — ivpm-build: stage executables as wheel scripts (`fvutils/ivpm-build`)

- [ ] Add a config table, for example:

  ```toml
  [[tool.ivpm-build.scripts]]
  src = "build/bin/dv-solve-smt2{exeext}"
  optional = true   # missing on platforms that don't build it (Windows today)
  ```

  Add `{exeext}` to `expand_libvars`: `.exe` on Windows, empty elsewhere.
- [ ] In `build_wheel`, after `_run_cmake()`, resolve each entry. Pass the
  existing files to setuptools as distutils `scripts` (the
  `Distribution.scripts` list) for the duration of `_st.build_wheel`. Patch it
  in the same way `_platform_wheel_tag()` patches `has_ext_modules`.
  setuptools' `build_scripts` copies files that don't start with a `#!python`
  shebang unchanged and sets them executable. `bdist_wheel` puts them under
  `<dist>.data/scripts/`.
- [ ] Check that a `[project]` table in `pyproject.toml` doesn't make
  setuptools drop or reject a `scripts` list injected this way. If it does,
  stage the files into a temporary directory and use `data_files`-style
  injection targeting the scripts scheme instead.
- [ ] Missing source: warn if `optional = true`, otherwise fail the build.
  Don't copy the `extra-data` behaviour, which warns and keeps going for every
  missing file.
- [ ] Unit test: a toy CMake project that builds a `hello` executable. Build
  the wheel and check `*.data/scripts/hello` is present, executable
  (`external_attr`) and listed in `RECORD`.
- [ ] Release ivpm-build (tag). Note the published version for P2's floor.

### P2 — dv-solve: build a self-contained `dv-solve-smt2`

- [ ] CMake: add `add_library(dv_solve_static STATIC ${DVS_SOURCES})` with the
  same compile definitions as `dv_solve`, linked to kissat, `Threads` and
  `dvs_apply_cadical`. Link `dv-solve-smt2` against it instead of `dv_solve`.
  If compiling the sources a fourth time slows the build too much, refactor
  the targets around a shared `OBJECT` library. Linking to the static archive
  is fine for local builds too, so no separate build mode is needed.
- [ ] Build only `dv-solve-smt2` for the wheel. Today `DV_SOLVE_BUILD_TOOLS`
  also builds about 20 C test programs, which cost time on the emulated
  aarch64 runners. Either add `DVS_BUILD_SMT2` (default `ON`, independent of
  `DV_SOLVE_BUILD_TOOLS`), or have the backend pass
  `-DDV_SOLVE_BUILD_TOOLS=OFF -DDVS_BUILD_SMT2=ON`. Check how the backend
  passes CMake defines first.
- [ ] Strip the executable in Release builds (`-s` at link time, or a
  post-build `strip` on Linux and `strip -x` on macOS).
- [ ] Linux: confirm `readelf -d` shows only system libraries as `NEEDED`.
  With CaDiCaL, `libstdc++.so.6` is expected, and is on the manylinux
  allowlist.
- [ ] macOS: confirm `otool -L` shows only `/usr/lib/libSystem` and
  `libc++`, and `lipo -info` shows `x86_64 arm64`.
- [ ] `pyproject.toml`: add the `[[tool.ivpm-build.scripts]]` entry from P1
  and raise the `ivpm-build>=` floor to the release P1 produced.

### P3 — dv-solve: Python discovery helper

- [ ] Add `dv_solve.get_smt2_exe()`, following the existing `get_dpi_lib()`
  pattern. It returns an absolute path and raises a `RuntimeError` built by
  `missing_artifact_error` when nothing is found. Order:
  1. the override root, if set (`_resolve.override_root()`),
  2. the installed distribution: the `RECORD` entry that ends in
     `scripts/dv-solve-smt2[.exe]`, through
     `importlib.metadata.distribution("dv-solve").files` and
     `locate_file`; this works for venv, `--user` and system installs,
  3. source checkouts, the same candidates `_resolve._checkouts()` already
     walks (`build/bin/`, then `build/`),
  4. `shutil.which("dv-solve-smt2")` as a last resort.
- [ ] Add it to `resolve_report()` so deployment diagnostics show which
  binary was chosen.
- [ ] Optional: point `tests/formal/*` at the helper instead of the
  hard-coded `build/dv-solve-smt2` paths. That cleanup can come later.

### P4 — CI verification

- [ ] `tests/wheel_smoke.py`. On platforms where the binary is expected
  (everything except Windows for now):
  - `shutil.which("dv-solve-smt2")` resolves into the test environment's
    scripts directory (`sysconfig.get_path("scripts")`);
  - `dv-solve-smt2 --version` exits 0;
  - a trivial `.smt2` file (`(declare-const x (_ BitVec 8))
    (assert (bvugt x #x10)) (check-sat)`) prints `sat`;
  - `dv_solve.get_smt2_exe()` agrees with `which`.
  On Windows, assert the helper raises the documented error, so a future
  port is caught by a failing test rather than going unnoticed.
- [ ] Linux: after auditwheel repair, check from the build log or an
  unpacked wheel that auditwheel left the static binary alone or only
  adjusted it harmlessly.
- [ ] Optional end-to-end check: run the Verilator quickstart example
  against the installed binary in a job that already has Verilator.

### P5 — Windows port (branch `feat/windows-cadical`)

- [x] Replace `unistd.h`, `pthread.h` and `sys/resource.h` uses in
  `smt2_main.c` and `smt2_frontend.c` with portable code, or with small
  `#ifdef _WIN32` shims. The large-stack worker uses
  `dvs_thread_create_stack`; the depth guard sizes itself from
  `dvs_stack_size()` where there is no `RLIMIT_STACK`; `setenv`/`unsetenv`
  come from `msvc_compat.h`; stdout is binary, so replies end in `\n`.
- [x] Turn the tool on for MSVC. P1–P4 then pick it up with no layout
  changes, which is the reason for D4.
- [x] Link it against the static C runtime (`/MT`), with `/MT` copies of
  kissat and CaDiCaL: pip's `Scripts\` has no `VCRUNTIME140.dll`, and
  delvewheel bundles the runtime only for the package's DLLs.

### P6 — Docs and release

- [ ] `README.md:29` and `docs/site/getting-started/install.md:12`: the
  executable comes with `pip install dv-solve` on Linux and macOS.
- [ ] `quickstart-verilator.md`: drop "built from source", and change the
  example to `export VERILATOR_SOLVER="dv-solve-smt2 --interactive --mode=verilator"`.
- [ ] `docs/site/reference/cli.md`: say where the binary comes from and
  mention `dv_solve.get_smt2_exe()`.
- [ ] Bump `BASE` to 0.2.3, merge with `--no-ff`, tag `v0.2.3`, then check
  the published wheels: download one per platform, unzip it and confirm
  `.data/scripts/dv-solve-smt2` is there.

## Order and dependencies

```
P1 (ivpm-build, release) ──► P2 ──► P4 ──► P6 (release 0.2.3)
                              └──► P3 ──┘
P5 independent; can land in any later release
```

P2 and P3 can be developed against a local editable ivpm-build. Only the
dv-solve release waits on the ivpm-build release.

## Fallback if P1 stalls

dv-solve's `setup.py` already overrides `build_ext`. It can call
`setup(scripts=[...])` with the binary path whenever the file exists. Under
the backend, setup.py is evaluated again after CMake has run, so the file is
there in time. It works, but it isn't reusable and depends on when the
backend runs setup.py. Use it only to unblock a release.

## Risks

| Risk | Mitigation |
|---|---|
| setuptools ignores a `scripts` list injected alongside a PEP 621 `[project]` table | Check this early in P1. The fallback is `data_files` into the scripts scheme |
| Static link pulls in symbols that clash between kissat and CaDiCaL | The CaDiCaL localization (`cmake/cadical.cmake`) already produces a single object with only `ccadical_*` global, and the shared library links the same inputs, so no new clash is expected |
| auditwheel or delocate rewrites the binary in an unexpected way | P4 inspects the repaired wheel. A static binary has nothing for them to graft |
| Another installed package also provides `dv-solve-smt2` (for example, a source build on `PATH` ahead of the venv) | `get_smt2_exe()` prefers the wheel's own binary. Docs suggest `which dv-solve-smt2` when results look stale |
| Windows users expect the command | The Windows wheel's helper error says the executable isn't available on Windows yet (P5) |

## Not changing

- The shared libraries, headers and SV files stay where they are in the
  package.
- No `[project.scripts]` / `console_scripts` launcher (D2).
