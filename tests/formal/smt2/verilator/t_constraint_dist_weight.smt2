; Sanitized from Verilator transcript: t_constraint_dist_weight
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun x () (_ BitVec 8))
(assert (= #b1 (bvor (__Vbv (= x #xff)) (__Vbv (= x #x00)))))
(assert (= #b1 (__Vbv (= x #xff))))
(check-sat)
