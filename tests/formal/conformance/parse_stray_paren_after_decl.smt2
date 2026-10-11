; a stray ')' after a declaration
; expect: error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(declare-const z (_ BitVec 4)))
(assert (= x #x1))
(check-sat)
