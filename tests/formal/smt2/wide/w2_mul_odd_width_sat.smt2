(set-logic QF_BV)
(declare-const x (_ BitVec 100))
(assert (= (bvmul x #x0000000000000000000000003) #x00000000000000000000000ff))
(check-sat)
