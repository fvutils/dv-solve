; seed=1 i=363 door=protocol[k=4 pin u=3] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 2))
(declare-const y (_ BitVec 2))
(declare-const z (_ BitVec 2))
(declare-const u (_ BitVec 2))
(assert (xor (not true) (ite true true true)))
(check-sat)
