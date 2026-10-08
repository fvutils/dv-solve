; exit status 1 for unsat
; expect: unsat
; exit: 1
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (distinct x x))
(check-sat)
