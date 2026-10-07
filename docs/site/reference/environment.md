# Environment variables

`DV_ENGINE`
: Choose the solving engine: `cdcl` or `bitblast`. Unset means automatic.
  The `--engine` option does the same thing. See {doc}`../concepts/engines`.

`DV_LOG`
: Turn on `dv-solve-smt2`'s warnings and diagnostics. A value containing `/`
  or `.` is a file to append them to (use this under yosys-smtbmc, which reads
  standard error as part of the response stream); any other value, such as
  `1`, prints them to standard error. Unset, they are not printed.

Only the variables on this page are supported. dv-solve reads others for
internal testing and diagnosis; they can change or disappear without notice.
