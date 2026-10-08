; pop with nothing pushed
; expect: error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(pop 1)
(check-sat)
