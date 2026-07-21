(set-logic QF_BV)
(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))
(assert (= (bvand a b) #xff))(assert (= a #x0f))
(check-sat)
