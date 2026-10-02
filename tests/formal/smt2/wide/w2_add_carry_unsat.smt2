(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= (bvadd x #x00000000000000000000000000000001) #x00000000000000010000000000000000))(assert (= ((_ extract 127 64) x) #x0000000000000001))
(check-sat)
