; Sanitized from Verilator transcript: t_constraint_shift_width
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun address () (_ BitVec 37))
(declare-fun size () (_ BitVec 4))
(assert (= #b1 (__Vbv (= (bvurem address (bvshl #b0000000000000000000000000000000000001 ((_ zero_extend 33) size))) #b0000000000000000000000000000000000000))))
(assert (= #b1 (__Vbv (= ((_ zero_extend 28) size) #x00000006))))
(check-sat)
