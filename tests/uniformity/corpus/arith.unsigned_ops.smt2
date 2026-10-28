; corpus for arith.unsigned_ops from 210 captured calls, 1 distinct problems
; problem arith.unsigned_ops#0 calls=210
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 32))
(declare-fun b () (_ BitVec 32))
(declare-fun c () (_ BitVec 16))
(assert (= #b1 (__Vbv (bvugt a #x80000000))))
(assert (= #b1 (__Vbv (bvult b a))))
(assert (= #b1 (__Vbv (bvult (bvsub a b) #x000003e8))))
(assert (= #b1 (__Vbv (= c ((_ extract 15 0) (bvxor a b))))))
; end
