; seed=3 i=267 door=protocol[k=1 pin y=1247751566118870854] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 64))
(declare-const y (_ BitVec 64))
(declare-const z (_ BitVec 64))
(assert (= x (concat (_ bv0 17) (_ bv0 47))))
(check-sat)
