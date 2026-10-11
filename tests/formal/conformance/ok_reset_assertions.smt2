; reset-assertions drops the assertions and keeps the declarations (B77)
; expect: unsat sat
; exit: 0
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(assert (= x #x2))
(check-sat)
(reset-assertions)
(assert (= x #x2))
(check-sat)
