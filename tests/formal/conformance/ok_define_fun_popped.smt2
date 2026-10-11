; a define-fun made inside a push goes with the pop (B82)
; expect: sat
; exit: 0
(set-logic QF_BV)
(push 1)
(define-fun f () (_ BitVec 4) #x1)
(pop 1)
(define-fun f () (_ BitVec 4) #x2)
(assert (= f #x2))
(check-sat)
