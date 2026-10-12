; a sort-typed constant already declared cannot be declared again (B116)
; expect: error unknown
; exit: 4
(set-logic QF_UFBV)
(declare-sort S 0)
(declare-fun s0 () S)
(declare-fun s0 () S)
(check-sat)
