; Sanitized from Verilator transcript: t_randc_enum_constraint
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun color () (_ BitVec 3))
(assert (= #b1 (__Vbv (or (= color (_ bv0 3)) (= color (_ bv1 3)) (= color (_ bv2 3)) (= color (_ bv3 3)) (= color (_ bv4 3))))))
(assert (= #b1 (__Vbv (not (= color #b100)))))
(check-sat)
