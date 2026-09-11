; Regression: (not (= x y)) between two 64-bit vars installed a 32-bit ne
; propagator (no width check at the negated-compare compile site) -> the 32-bit
; propagator truncated the 64-bit bounds and never reached fixpoint, so
; solver_propagate spun forever (never reaching the conflict/time budget).
; Fixed by making that site width-aware. Expect: sat.
(set-logic QF_BV)
(declare-fun x () (_ BitVec 64))
(declare-fun y () (_ BitVec 64))
(assert (not (= x y)))
(assert (bvugt x #xfffffffffffffff0))
(check-sat)
