; seed=811 i=847 door=builder[again] expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 6))
(assert (bvult x x))
(assert (= x (_ bv0 6)))
(check-sat)
