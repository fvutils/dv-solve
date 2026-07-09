; Sanitized from Verilator transcript: t_constraint_dist_range
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun x () (_ BitVec 4))
(assert (= #b1 (bvand (__Vbv (bvuge x #x0)) (__Vbv (bvule x #x9)))))
(check-sat)
