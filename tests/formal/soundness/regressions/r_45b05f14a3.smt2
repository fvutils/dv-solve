; seed=811 i=460 door=protocol[k=3 pin x=61] solved anyway expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 6))
(assert (bvugt x x))
(check-sat)
