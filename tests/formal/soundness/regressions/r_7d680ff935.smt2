; seed=707 i=1968 door=builder[again] expect=sat got=unsat
(set-logic QF_BV)
(declare-const z (_ BitVec 32))
(assert (distinct (bvlshr (_ bv1 32) (bvand z z)) (_ bv0 32)))
(check-sat)
