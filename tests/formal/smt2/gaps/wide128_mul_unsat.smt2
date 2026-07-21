(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= (bvmul x (_ bv2 128)) (_ bv1 128)))
(check-sat)
