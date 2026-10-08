; define-sort without its parameter list
; expect: error unknown
; exit: 4
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(declare-const y (_ BitVec 4))
(define-sort A (_ BitVec 4))
(check-sat)
