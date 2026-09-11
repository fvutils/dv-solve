(set-logic QF_BV)
(declare-const a (_ BitVec 8))(declare-const b (_ BitVec 8))(declare-const c (_ BitVec 8))
(assert (= (bvxor a b c) #xff))(assert (= a #x0f))(assert (= b #xf0))
(check-sat)
