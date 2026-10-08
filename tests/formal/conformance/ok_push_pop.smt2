; push and pop
; expect: sat unsat sat
; exit: 0
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(assert (= x #x1))
(check-sat)
(push 1)
(assert (= x #x2))
(check-sat)
(pop 1)
(check-sat)
