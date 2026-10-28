; corpus for control.soft_relaxation from 210 captured calls, 1 distinct problems
; problem control.soft_relaxation#0 calls=210
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvugt a #x0a))))
(assert (= #b1 (__Vbv (bvult a #x28))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) b) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) b) #x00000003)))))
; end
