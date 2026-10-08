; a name already declared cannot be declared again
; expect: error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const x (_ BitVec 8))
(check-sat)
