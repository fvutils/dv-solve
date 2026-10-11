; a function's let does not capture the caller's argument (B78)
; expect: unsat
; exit: 1
(set-logic QF_BV)
(declare-const x (_ BitVec 4))
(define-fun f ((p (_ BitVec 4))) (_ BitVec 4) (let ((t #x0)) (bvadd p t)))
(assert (let ((t x)) (not (= (f t) x))))
(check-sat)
