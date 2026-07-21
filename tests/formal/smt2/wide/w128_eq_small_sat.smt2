(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (= x (_ bv42 128)))
(check-sat)
