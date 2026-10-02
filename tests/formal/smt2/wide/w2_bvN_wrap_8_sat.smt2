(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (= x (_ bv300 8)))(assert (= x #x2c))
(check-sat)
