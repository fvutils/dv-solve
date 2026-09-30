# Environment variables

`DV_ENGINE`
: Choose the solving engine: `cdcl` or `bitblast`. Unset means automatic.
  The `--engine` option does the same thing. See {doc}`../concepts/engines`.

`DV_LOG`
: The file that `dv-solve-smt2` appends diagnostics to when its standard
  output is not a terminal. Defaults to `/tmp/dv-solve.log`.

Only the variables on this page are supported. dv-solve reads others for
internal testing and diagnosis; they can change or disappear without notice.
