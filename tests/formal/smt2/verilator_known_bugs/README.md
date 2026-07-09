# Quarantined fixtures — known dv-solve bugs (not run by test_cross_check)

- **t_constraint_operators.smt2**: operator kitchen-sink. dv-solve returns a
  wrong `unsat` (z3: `sat`) due to a **sign_extend-to-64-bit** bitblast bug —
  `(= ((_ sign_extend 32) x) (_ bv5 64))` is wrongly unsatisfiable (sign_extend
  to 63 bits and zero_extend to 64 both work). Unrelated to the signed div/rem/
  mod work that surfaced it. See docs/verilator_coverage_test_plan.md §8.I.
  Restore to ../verilator/ once that bug is fixed.
