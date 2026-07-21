(set-logic QF_BV)
(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))
(assert (= a #xff))(assert (= b #xff))(assert (= (bvmul a b) #x01))
(check-sat)
