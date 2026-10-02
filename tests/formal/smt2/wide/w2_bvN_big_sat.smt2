(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= x (_ bv340282366920938463463374607431768211455 128)))(assert (= ((_ extract 0 0) x) #b1))
(check-sat)
