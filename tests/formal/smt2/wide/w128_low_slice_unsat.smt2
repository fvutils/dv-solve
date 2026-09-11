(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= ((_ extract 63 0) x) (_ bv0 64)))(assert (= ((_ extract 63 0) x) (_ bv1 64)))
(check-sat)
