; seed=2 i=1114 door=protocol[k=1 pin y=2] solved anyway expect=unsat got=sat bad-model
(set-logic QF_BV)
(declare-const z (_ BitVec 6))
(assert (xor (not true) (bvugt (_ bv1 6) z)))
(check-sat)
