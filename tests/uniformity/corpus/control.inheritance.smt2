; corpus for control.inheritance from 210 captured calls, 1 distinct problems
; problem control.inheritance#0 calls=210
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvult a #x32))))
(assert (= #b1 (__Vbv (bvult a #x32))))
(assert (= #b1 (__Vbv (bvugt b a))))
(assert (= #b1 (__Vbv (bvult b #x3c))))
; end
