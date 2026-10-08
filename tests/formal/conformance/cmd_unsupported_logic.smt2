; a logic we do not support
; expect: error unknown
; exit: 4
(set-logic QF_NIA)
(declare-const x Int)
(check-sat)
