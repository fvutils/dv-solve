; corpus for control.constraint_mode from 210 captured calls, 1 distinct problems
; problem control.constraint_mode#0 calls=210
; get-value a
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvugt a #x64))))
; end
