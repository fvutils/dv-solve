(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= (bvand x (_ bv15 128)) (_ bv16 128)))
(check-sat)
