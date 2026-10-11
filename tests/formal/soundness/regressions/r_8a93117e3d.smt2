; seed=811 i=1080 door=protocol[k=1 pin x=4] solved anyway expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(declare-const z (_ BitVec 4))
(assert (bvuge y z))
(assert (or (= z x) (= y (_ bv1 4))))
(assert (bvuge (_ bv0 4) (bvneg y)))
(check-sat)
