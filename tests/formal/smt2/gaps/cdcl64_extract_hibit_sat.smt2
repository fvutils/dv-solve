(set-logic QF_BV)
(declare-const v (_ BitVec 64))
(assert (= ((_ extract 63 63) v) #b1))
(check-sat)
