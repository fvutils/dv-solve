; a declaration made inside a push goes with the pop, and can be made again
; expect: sat
; exit: 0
(set-logic QF_BV)
(declare-const y (_ BitVec 4))
(push 1)
(declare-const x (_ BitVec 4))
(pop 1)
(declare-const x (_ BitVec 8))
(assert (= x #xff))
(check-sat)
