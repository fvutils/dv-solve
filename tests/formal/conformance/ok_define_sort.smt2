; define-sort (fw-hdl R4)
; expect: sat
; exit: 0
(set-logic QF_BV)
(define-sort W () (_ BitVec 8))
(declare-const x W)
(assert (= x #x42))
(check-sat)
