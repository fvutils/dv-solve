; seed=811 i=213 door=builder[again] expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 3))
(declare-const y (_ BitVec 3))
(declare-const z (_ BitVec 6))
(assert (= (_ bv0 3) (bvnot (_ bv0 3))))
(check-sat)
