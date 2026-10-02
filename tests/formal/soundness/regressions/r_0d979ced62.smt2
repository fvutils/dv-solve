; seed=7 i=601 door=smt2-steps[1] expect=sat got=unsat
(set-logic QF_BV)
(declare-const y (_ BitVec 6))
(assert (bvsle (_ bv0 6) y))
(assert (= y (_ bv1 6)))
(check-sat)
