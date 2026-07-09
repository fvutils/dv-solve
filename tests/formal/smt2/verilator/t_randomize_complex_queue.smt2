; Sanitized from Verilator transcript: t_randomize_complex_queue
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun field () (_ BitVec 3))
(assert (= #b1 (bvor (bvor (__Vbv (= ((_ zero_extend 29) field) #x00000001)) (__Vbv (= ((_ zero_extend 29) field) #x00000002))) (__Vbv (= ((_ zero_extend 29) field) #x00000003)))))
(check-sat)
