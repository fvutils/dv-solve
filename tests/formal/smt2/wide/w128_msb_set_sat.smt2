(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= ((_ extract 127 127) x) #b1))
(check-sat)
