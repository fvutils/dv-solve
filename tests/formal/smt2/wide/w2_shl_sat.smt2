(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= (bvshl #x00000000000000000000000000000001 x) #x80000000000000000000000000000000))
(check-sat)
