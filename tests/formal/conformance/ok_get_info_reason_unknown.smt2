; an unsupported sort is an honest unknown, and says why
; expect: unknown info
; exit: 2
(set-logic QF_BV)
(declare-const x Int)
(check-sat)
(get-info :reason-unknown)
