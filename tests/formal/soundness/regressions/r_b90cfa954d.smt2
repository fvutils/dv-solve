; seed=707 i=1711 door=protocol[k=7 pin x=3654776311] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 33))
(declare-const y (_ BitVec 33))
(declare-const z (_ BitVec 32))
(assert (bvult (ite (bvult (_ bv0 32) z) x y) y))
(check-sat)
