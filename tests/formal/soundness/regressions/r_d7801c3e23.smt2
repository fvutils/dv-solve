; seed=3 i=473 door=protocol[k=2 pin y=88129105] solved anyway expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const x (_ BitVec 64))
(assert (bvugt x (concat (bvneg (_ bv1 12)) (_ bv0 52))))
(check-sat)
