(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= x #xffffffffffffffffffffffffffffffff))
(check-sat)
