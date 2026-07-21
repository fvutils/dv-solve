(set-logic QF_BV)
(declare-const x (_ BitVec 128))
(assert (bvult x (_ bv100 128)))(assert (bvugt x (_ bv50 128)))
(check-sat)
