; Regression for pop-doesn't-drop-aux-problems bug fixed in this commit.
; Before fix: 3rd check-sat got downgraded to "unknown" because the model
; validator still saw the (not a) constraint that was popped off the stack.
; Run via `build/dv-solve-smt2 tests/formal/regression_pop_aux.smt2`.
(set-logic QF_BV)
(declare-fun a () Bool)
(assert a)
(check-sat)
(push 1)
(assert (not a))
(check-sat)
(pop 1)
(check-sat)
