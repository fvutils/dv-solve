# Environment variables

`DV_ENGINE`
: Choose the solving engine: `cdcl` or `bitblast`. Unset means automatic.
  The `--engine` option does the same thing. See {doc}`../concepts/engines`.

`DV_CHECK_MODEL`
: `1` turns the model check on, `0` turns it off. Unset, it is on, except in
  `--mode=verilator`. See {doc}`../guides/smt2-solver`.

`DV_LOG`
: Turn on `dv-solve-smt2`'s warnings and diagnostics. A value containing `/`
  or `.` is a file to append them to (use this under yosys-smtbmc, which reads
  standard error as part of the response stream); any other value, such as
  `1`, prints them to standard error. Unset, they are not printed.

`DV_STATS`
: Count solves and write a stats file at process exit. The value is the file
  (`%p` is replaced by the process id), or `1` for
  `dv_solve_stats.<pid>.json`. Unset or `0`, nothing is counted. See
  {doc}`../guides/solve-statistics`.

Only the variables on this page are supported. dv-solve reads others for
internal testing and diagnosis; they can change or disappear without notice.
