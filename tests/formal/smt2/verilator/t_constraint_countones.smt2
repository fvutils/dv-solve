; Sanitized from Verilator transcript: t_constraint_countones
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun x () (_ BitVec 5))
(assert (= #b1 (__Vbv (= ((_ zero_extend 27) (bvadd (bvadd (bvadd (bvadd (bvand x #b00001) (bvlshr (bvand x #b00010) #b00001)) (bvlshr (bvand x #b00100) #b00010)) (bvlshr (bvand x #b01000) #b00011)) (bvlshr (bvand x #b10000) #b00100))) #x00000001))))
(check-sat)
