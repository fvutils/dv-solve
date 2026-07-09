; Sanitized from Verilator transcript: t_constraint_solve_before
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun data () (_ BitVec 8))
(declare-fun mode () (_ BitVec 3))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 29) mode) #x00000000)) (__Vbv (bvule ((_ zero_extend 29) mode) #x00000003)))))
(assert (= #b1 (ite (__Vbool (__Vbv (= ((_ zero_extend 29) mode) #x00000000))) (__Vbv (= data #x00)) (ite (__Vbool (__Vbv (= ((_ zero_extend 29) mode) #x00000001))) (bvand (__Vbv (bvuge data #x01)) (__Vbv (bvule data #x0f))) (__Vbv (bvult data #x80))))))
(check-sat)
