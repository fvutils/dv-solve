; corpus for control.ite_cond from 210 captured calls, 1 distinct problems
; problem control.ite_cond#0 calls=210
; get-value a kind
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun kind () (_ BitVec 2))
(assert (= #b1 (ite (__Vbool (__Vbv (= ((_ zero_extend 30) kind) #x00000000))) (__Vbv (bvult a #x0a)) (ite (__Vbool (__Vbv (= ((_ zero_extend 30) kind) #x00000001))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000032)) (__Vbv (bvule ((_ zero_extend 24) a) #x0000003c))) (__Vbv (bvugt a #xc8))))))
; end
