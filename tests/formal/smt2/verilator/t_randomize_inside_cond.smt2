; Sanitized from Verilator transcript: t_randomize_inside_cond
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun __Varg1 () (_ BitVec 32))
(assert (= #b1 (bvor (__Vbv (= __Varg1 #x0000000a)) (__Vbv (= __Varg1 #x00000014)))))
(check-sat)
