; sort-typed declarations made inside a push go with the pop, and can be made again (B116)
; expect: sat
; exit: 0
(set-logic QF_UFBV)
(declare-sort S 0)
(push 1)
(declare-fun s0 () S)
(declare-fun f (S) (_ BitVec 4))
(pop 1)
(declare-fun s0 () S)
(declare-fun f (S) (_ BitVec 8))
(assert (= (f s0) #xff))
(check-sat)
