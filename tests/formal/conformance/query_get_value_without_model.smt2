; get-value after unsat
; expect: unsat error
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (distinct x x))
(check-sat)
(get-value (x))
