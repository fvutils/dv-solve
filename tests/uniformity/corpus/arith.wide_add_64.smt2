; corpus for arith.wide_add_64 from 210 captured calls, 1 distinct problems
; problem arith.wide_add_64#0 calls=210
; get-value a b s
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 64))
(declare-fun b () (_ BitVec 64))
(declare-fun s () (_ BitVec 64))
(assert (= #b1 (__Vbv (= s (bvadd a b)))))
(assert (= #b1 (__Vbv (bvugt a #xf000000000000000))))
(assert (= #b1 (__Vbv (bvult b #x1000000000000000))))
(assert (= #b1 (__Vbv (bvugt b #x00000000000003e8))))
; end
