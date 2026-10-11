; seed=707 i=693 door=builder[again] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 16))
(declare-const y (_ BitVec 16))
(assert (= (bvand y x) (_ bv31 16)))
(check-sat)
