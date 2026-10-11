; seed=811 i=320 door=builder[again] expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(assert (= (bvsub x (_ bv1 8)) (ite true x (_ bv0 8))))
(check-sat)
