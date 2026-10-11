; seed=707 i=516 door=builder[again] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 33))
(declare-const y (_ BitVec 33))
(declare-const z (_ BitVec 32))
(assert (= x (bvand y (_ bv33554428 33))))
(check-sat)
