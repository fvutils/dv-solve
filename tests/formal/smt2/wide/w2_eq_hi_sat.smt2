(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= x #x8000000000000001ffffffffffffffff))(assert (= ((_ extract 127 64) x) #x8000000000000001))
(check-sat)
