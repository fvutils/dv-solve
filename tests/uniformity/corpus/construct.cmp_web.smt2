; corpus for construct.cmp_web from 210 captured calls, 1 distinct problems
; problem construct.cmp_web#0 calls=210
; get-value a b c d e f
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 16))
(declare-fun b () (_ BitVec 16))
(declare-fun c () (_ BitVec 16))
(declare-fun d () (_ BitVec 16))
(declare-fun e () (_ BitVec 16))
(declare-fun f () (_ BitVec 16))
(assert (= #b1 (__Vbv (bvult a b))))
(assert (= #b1 (__Vbv (bvult b c))))
(assert (= #b1 (__Vbv (bvult c d))))
(assert (= #b1 (__Vbv (bvult (bvadd a #x000a) e))))
(assert (= #b1 (__Vbv (bvult e d))))
(assert (= #b1 (__Vbv (bvugt f b))))
(assert (= #b1 (__Vbv (bvult f e))))
(assert (= #b1 (__Vbv (bvult d #x1388))))
(assert (= #b1 (__Vbv (bvugt a #x0064))))
; end
