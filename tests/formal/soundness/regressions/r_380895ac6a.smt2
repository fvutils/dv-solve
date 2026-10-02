; seed=2026100200 i=625 door=smt2-incr expect=unsat got=timeout
(set-logic QF_BV)
(declare-const x (_ BitVec 63))
(declare-const y (_ BitVec 63))
(assert (bvsgt x x))
(assert (bvugt y x))
(assert (= y x))
(check-sat)
