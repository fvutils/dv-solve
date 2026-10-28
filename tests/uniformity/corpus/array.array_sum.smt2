; corpus for array.array_sum from 210 captured calls, 1 distinct problems
; problem array.array_sum#0 calls=210
; get-value (select a #x00000000)(select a #x00000001)(select a #x00000002)(select a #x00000003)(select a #x00000004)(select a #x00000005)(select a #x00000006)(select a #x00000007)
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (Array (_ BitVec 32) (_ BitVec 16)))
(assert (= #b1 (__Vbv (bvult (select a #x00000000) #x03e8))))
(assert (= #b1 (__Vbv (bvult (select a #x00000001) #x03e8))))
(assert (= #b1 (__Vbv (bvult (select a #x00000002) #x03e8))))
(assert (= #b1 (__Vbv (bvult (select a #x00000003) #x03e8))))
(assert (= #b1 (__Vbv (bvult (select a #x00000004) #x03e8))))
(assert (= #b1 (__Vbv (bvult (select a #x00000005) #x03e8))))
(assert (= #b1 (__Vbv (bvult (select a #x00000006) #x03e8))))
(assert (= #b1 (__Vbv (bvult (select a #x00000007) #x03e8))))
(assert (= #b1 (__Vbv (= (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (select a #x00000000) (select a #x00000001)) (select a #x00000002)) (select a #x00000003)) (select a #x00000004)) (select a #x00000005)) (select a #x00000006)) (select a #x00000007)) #x0bb8))))
; end
