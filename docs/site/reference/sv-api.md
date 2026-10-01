# SystemVerilog API

dv-solve's SystemVerilog API is two packages, which call the solver through
DPI:

`dvs_dpi_pkg`
: Imports the solver's DPI functions. A problem is compiled once into a
  handle and then solved as many times as you like.

`dvs_randomizer_pkg`
: A base class, `dvs_randomizer`, that wraps the handle for randomizing one
  kind of object.

The packages are `dvs_dpi_pkg.sv` and `dvs_randomizer_pkg.sv`, in the
directory `dv_solve.get_svdirs()` returns. The functions are implemented in
the library `dv_solve.get_dpi_lib()` returns, which the simulator must load.
{doc}`../guides/systemverilog-dpi` shows the whole flow, and
{doc}`../guides/packaging` describes the lookup functions.

## `dvs_dpi_pkg`

The functions take and return only scalars, strings and `chandle`s, so the
package works on any simulator with DPI-C.

### Compiling a problem

```systemverilog
function chandle dvs_dpi_compile_b64(input string b64_data);
```

Compile a problem and return a handle to it. `b64_data` is the problem as a
base64 string, produced by the Python builder's `finalize_bytes()` (see
{doc}`../guides/systemverilog-dpi`) with the same dv-solve version.

Returns `null` if the string is not valid base64, memory runs out, or
compiling fails or already shows the problem has no solution.

```systemverilog
function int dvs_dpi_n_uncompiled_h(input chandle ctx);
```

The number of constraints compiling could not take; normally 0. When it is
positive, {ref}`dvs_dpi_solve_h <sv-solve>` checks every solution against
the whole problem and returns 3 for one that breaks a constraint. Returns −1
for a `null` handle.

```systemverilog
function void dvs_dpi_release_h(input chandle ctx);
```

Free the handle and everything it holds.

(sv-solve)=
### Solving

```systemverilog
function int dvs_dpi_solve_h(input chandle ctx, input longint seed);
```

Search for a solution. Each call starts from the constraints and the active
pins, never from the previous solution. Ties are broken at random, so
solutions spread evenly over the solution space.

The seed selects the solution: the same seed from the same state gives the
same solution. A seed of 0 continues from the handle's own random state, so
successive calls still differ. Passing `$urandom()` ties the results to the
simulator's seed.

| Return | Meaning |
|---|---|
| 0 | A solution was found. |
| 1 | No solution exists. |
| 2 | The search gave up before deciding. |
| 3 | A solution was found, but it breaks a constraint that compiling could not take (see `dvs_dpi_n_uncompiled_h`). Treat it as a failure. |
| −1 | The handle is `null`, or every checkpoint slot is in use. |

```systemverilog
function longint dvs_dpi_get_value_h(input chandle ctx, input int var_id);
```

The value of variable `var_id` in the last solution. Returns 0 if the last
solve did not return 0, or `var_id` is out of range. Cast the result to the
field's width: `obj.len = 8'(dvs_dpi_get_value_h(h, LEN));`.

### Pins and checkpoints

```systemverilog
function int dvs_dpi_pin_var_h(input chandle ctx, input int var_id, input longint value);
```

Fix a variable to a value for every following solve, until a restore to a
checkpoint taken before the pin. This is the DPI equivalent of
SystemVerilog `rand_mode(0)`.

| Return | Meaning |
|---|---|
| 0 | Pinned. |
| −1 | The handle is `null` or `var_id` is out of range. |
| −2 | The value contradicts the constraints or the other pins. |

```systemverilog
function int dvs_dpi_checkpoint_h(input chandle ctx);
```

Save the current pins. Returns a checkpoint index (0 or more), or −1 if the
handle is `null` or 31 checkpoints are already open.

```systemverilog
function void dvs_dpi_restore_h(input chandle ctx, input int cp);
```

Undo every pin applied since checkpoint `cp`. Checkpoints taken after `cp`
are discarded; `cp` itself is kept and can be restored again.

## `dvs_randomizer_pkg`

```systemverilog
virtual class dvs_randomizer #(type T = int);
```

A base class for randomizing objects of class `T`. A subclass supplies the
problem and copies solutions into objects; the base class compiles the
problem on the first call and reuses it.

A subclass implements:

`pure virtual function string get_problem_b64();`
: Return the problem as a base64 string, as for `dvs_dpi_compile_b64`.

`pure virtual function void apply_solution(T obj, chandle ctx);`
: Copy the solution into `obj`'s fields, reading each with
  `dvs_dpi_get_value_h(ctx, id)`.

It then calls:

`virtual function int randomize_obj(T obj, longint seed = 0);`
: Solve, and on success apply the solution to `obj`. Returns the
  `dvs_dpi_solve_h` code; `obj` is changed only when it is 0. Stops the
  simulation with `$fatal` if the problem does not compile.

`function void cleanup();`
: Release the compiled problem. A later `randomize_obj()` compiles it again.
