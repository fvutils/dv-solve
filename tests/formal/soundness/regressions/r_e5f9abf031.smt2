; seed=2026100200 i=196 door=builder[lcg=0] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 64))
(declare-const y (_ BitVec 64))
(assert (bvule x (ite true x y)))
(check-sat)
