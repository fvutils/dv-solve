; Sanitized from Verilator transcript: t_constraint_struct
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun packed_struct () (_ BitVec 40))
(assert (= #b1 (__Vbv (= ((_ extract 39 32) packed_struct) #xa0))))
(assert (= #b1 (bvand (__Vbv (bvsge ((_ extract 31 0) packed_struct) #x00000000)) (__Vbv (bvsle ((_ extract 31 0) packed_struct) #x00000064)))))
(check-sat)
