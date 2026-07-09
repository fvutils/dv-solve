; Sanitized from Verilator transcript: t_randomize_union
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun union_instance () (_ BitVec 32))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 20) ((_ extract 11 0) ((_ extract 31 0) union_instance))) #x00000000)) (__Vbv (bvule ((_ zero_extend 20) ((_ extract 11 0) ((_ extract 31 0) union_instance))) #x00000fff)))))
(check-sat)
