; seed=811 i=983 door=protocol[k=6 pin z=12] solved anyway expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const z (_ BitVec 6))
(assert (= (bvor (bvadd z z) z) z))
(check-sat)
