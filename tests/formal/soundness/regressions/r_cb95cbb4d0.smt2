; seed=811 i=924 door=builder[again] expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 8))
(declare-const y (_ BitVec 4))
(assert (= (bvnot x) (bvneg x)))
(check-sat)
