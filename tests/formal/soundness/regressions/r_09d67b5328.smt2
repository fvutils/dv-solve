; seed=707 i=1584 door=builder[again] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 33))
(declare-const y (_ BitVec 33))
(declare-const z (_ BitVec 32))
(assert true)
(assert (bvugt (bvnot z) (concat (_ bv1048575 20) (_ bv0 12))))
(check-sat)
