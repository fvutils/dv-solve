# Building against dv-solve

The `dv_solve` Python package can tell a build system where the native pieces
are, whether dv-solve was installed from a wheel or is being used from a source
checkout. Tools that compile C code against the solver, or load its DPI library
into a simulator, should ask the package rather than hard-code paths.

| Function | Returns |
|---|---|
| `dv_solve.get_libs()` | library names to link, for example `["dv_solve"]` (`-ldv_solve`) |
| `dv_solve.get_libdirs()` | directories holding the shared libraries (`-L`) |
| `dv_solve.get_incdirs()` | C include directories (`-I`) |
| `dv_solve.get_svdirs()` | directories holding the SystemVerilog packages `zsp_dpi_pkg.sv` and `zsp_randomizer_pkg.sv` |
| `dv_solve.get_dpi_lib()` | full path of the DPI shared library, for a simulator's `-sv_lib` option |
| `dv_solve.resolve_report()` | a dictionary describing what was found and where it looked, for diagnosing a failed lookup |

For example, a link line for a C program:

```bash
cc app.c $(python3 -c 'import dv_solve as d;
print(" ".join(["-I"+p for p in d.get_incdirs()] + ["-L"+p for p in d.get_libdirs()]
               + ["-l"+l for l in d.get_libs()]))')
```

The program must also find the library when it runs: add
`-Wl,-rpath,<dir>` for each directory from `get_libdirs()`, or put those
directories on `LD_LIBRARY_PATH`.
