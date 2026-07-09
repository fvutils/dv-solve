; Sanitized from Verilator transcript: t_randc_wide_constraint
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun value () (_ BitVec 16))
(assert (= #b1 (__Vbv (bvuge ((_ zero_extend 16) value) #x00000000))))
(check-sat)
