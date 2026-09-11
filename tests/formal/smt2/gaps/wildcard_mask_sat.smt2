(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (= (bvand x #x0f) #x0a))
(check-sat)
