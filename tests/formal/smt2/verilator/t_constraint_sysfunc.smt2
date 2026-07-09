; Sanitized from Verilator transcript: t_constraint_sysfunc
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun value () (_ BitVec 8))
(assert (= #b1 (bvand (__Vbv (not (= value #x00))) (__Vbv (= (bvand value (bvsub value #x01)) #x00)))))
(check-sat)
