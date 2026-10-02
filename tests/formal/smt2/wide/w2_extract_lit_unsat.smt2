(set-logic QF_BV)
(declare-const y (_ BitVec 8))
(assert (= y ((_ extract 71 64) #x0000000000000012ffffffffffffffff)))(assert (= y #xff))
(check-sat)
