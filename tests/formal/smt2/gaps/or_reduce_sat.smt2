(set-logic QF_BV)
(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))
(assert (= (bvor a b) #xff))(assert (= a #x0f))
(check-sat)
