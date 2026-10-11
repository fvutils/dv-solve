; seed=811 i=709 door=protocol[k=4 pin x=53] solved anyway expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 6))
(declare-const y (_ BitVec 6))
(assert (bvugt x y))
(assert (= x y))
(check-sat)
