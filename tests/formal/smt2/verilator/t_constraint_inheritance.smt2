; Sanitized from Verilator transcript: t_constraint_inheritance
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun x () (_ BitVec 32))
(assert (= #b1 (__Vbv (bvsgt x #x00000000))))
(check-sat)
