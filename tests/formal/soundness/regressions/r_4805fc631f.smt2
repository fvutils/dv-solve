; seed=2026100201 i=293 door=smt2-incr[2] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 65))
(declare-const y (_ BitVec 65))
(assert true)
(assert false)
(check-sat)
