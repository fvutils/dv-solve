; exit status 0 for sat
; expect: sat
; exit: 0
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(check-sat)
