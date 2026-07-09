; Sanitized from Verilator transcript: t_constraint_cond
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun i () (_ BitVec 1))
(declare-fun y () (_ BitVec 32))
(assert (= #b1 (__Vbv (=> (__Vbool i) (__Vbool (__Vbv (= y #x00000000)))))))
(check-sat)
