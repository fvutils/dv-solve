; seed=1 i=579 door=builder[lcg=0] expect=sat got=unsat
(set-logic QF_BV)
(declare-const y (_ BitVec 64))
(declare-const z (_ BitVec 64))
(assert (bvuge y (bvshl z (ite true z (_ bv0 64)))))
(check-sat)
