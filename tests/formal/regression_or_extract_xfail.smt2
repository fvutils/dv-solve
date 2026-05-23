; xfail: dv-solve currently returns unsat where z3 returns sat.
; Trigger: an OR-of-extract-equalities asserted incrementally
; after a satisfying check-sat. The compile path falls into
; the _bool_to_var fallback in _compile_constraint, but _bool_to_var
; doesn't recognize (= ((_ extract H L) v) k) and returns
; EXPR_NULL — so the constraint is dropped from the propagator
; graph entirely. Search then continues against a state where
; u was previously pinned to #b1, and somehow returns unsat.
; Likely root: the dropped OR plus residual state in the
; incremental builder produces a phantom contradiction.
;
; Blocks yosys-sby end-to-end (yosys-smtbmc emits exactly this
; shape after each BMC step). Tracked for fix in a follow-up;
; see docs/cdcl_followup_plan.md.
(set-logic QF_BV)
(declare-fun u () (_ BitVec 1))
(assert (= u #b1))
(check-sat)
(assert (or (= ((_ extract 0 0) u) #b1) (= ((_ extract 0 0) u) #b1)))
(check-sat)
; Expected: sat / sat
; Actual: sat / unsat
