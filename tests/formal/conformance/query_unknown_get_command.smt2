; an unsupported query does not poison
; expect: error sat
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(get-proof)
(assert (= x #x1))
(check-sat)
