; get-model
; expect: sat model
; exit: 0
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(assert (= y #x2))
(check-sat)
(get-model)
