; 2. is not a decimal
; expect: error unknown
; exit: 4
(set-info :x 2.)
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(check-sat)
