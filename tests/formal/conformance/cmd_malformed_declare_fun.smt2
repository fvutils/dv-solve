; declare-fun without its argument sorts
; expect: error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(declare-fun z (_ BitVec 4))
(check-sat)
