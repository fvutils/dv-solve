; corpus for arith.mul_div_64 from 210 captured calls, 1 distinct problems
; problem arith.mul_div_64#0 calls=210
; get-value a b p
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 64))
(declare-fun b () (_ BitVec 64))
(declare-fun p () (_ BitVec 64))
(assert (= #b1 (bvand (__Vbv (bvuge a #x0000000000000001)) (__Vbv (bvule a #x00000000ffffffff)))))
(assert (= #b1 (bvand (__Vbv (bvuge b #x0000000000000001)) (__Vbv (bvule b #x000000000000ffff)))))
(assert (= #b1 (__Vbv (= p (bvmul a b)))))
(assert (= #b1 (__Vbv (= (bvudiv p b) a))))
; end
