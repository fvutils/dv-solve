; Sanitized from Verilator transcript: t_randc_constraint
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun value () (_ BitVec 4))
(assert (= #b1 (__Vbv (bvuge ((_ zero_extend 28) value) #x00000003))))
(assert (= #b1 (__Vbv (bvule ((_ zero_extend 28) value) #x0000000a))))
(check-sat)
