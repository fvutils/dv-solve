; corpus for construct.bool_logic from 210 captured calls, 1 distinct problems
; problem construct.bool_logic#0 calls=210
; get-value a b c d e x
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 1))
(declare-fun b () (_ BitVec 1))
(declare-fun c () (_ BitVec 1))
(declare-fun d () (_ BitVec 1))
(declare-fun e () (_ BitVec 1))
(declare-fun x () (_ BitVec 8))
(assert (= #b1 (bvand (bvor a b) (__Vbv (not (__Vbool (bvand a b)))))))
(assert (= #b1 (__Vbv (= c (bvxor a d)))))
(assert (= #b1 (bvor (bvand e (__Vbv (not (__Vbool c)))) (__Vbv (bvugt x #xc8)))))
(assert (= #b1 (__Vbv (=> (__Vbool (bvand b d)) (__Vbool (__Vbv (bvult x #xdc)))))))
; end
