; seed=707 i=15 door=builder[again] expect=sat got=unsat
(set-logic QF_BV)
(declare-const x (_ BitVec 32))
(declare-const y (_ BitVec 32))
(assert (bvule (bvxor (bvnot x) x) x))
(check-sat)
