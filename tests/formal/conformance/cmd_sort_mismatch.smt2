; equality of a 4-bit and an 8-bit term
; expect: error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(declare-const b (_ BitVec 8))
(assert (= x b))
(check-sat)
