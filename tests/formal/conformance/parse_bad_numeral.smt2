; 3ab is not a numeral
; expect: error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= ((_ extract 3ab 0) x) x))
(check-sat)
