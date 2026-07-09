; Sanitized from Verilator transcript: t_constraint_array_index
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun data () (Array (_ BitVec 32) (_ BitVec 4)))
(assert (= #b1 (__Vbv (bvule (bvadd (bvadd (bvadd (bvadd (bvadd #b00000000000000000000000000000000 (bvadd ((_ zero_extend 28) (select data #x00000000)) #x00000000)) (bvadd ((_ zero_extend 28) (select data #x00000001)) #x00000001)) (bvadd ((_ zero_extend 28) (select data #x00000002)) #x00000002)) (bvadd ((_ zero_extend 28) (select data #x00000003)) #x00000003)) (bvadd ((_ zero_extend 28) (select data #x00000004)) #x00000004)) #x00000032))))
(check-sat)
