; seed=2026100203 i=98 door=smt2 expect=sat got=unsat
(set-logic QF_BV)
(declare-const a (_ BitVec 1))
(declare-const x (_ BitVec 64))
(declare-const y (_ BitVec 32))
(assert (ite false true (bvule (_ bv0 64) (_ bv16068038427581503454 64))))
(check-sat)
