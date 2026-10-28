; corpus for array.array_reduction from 210 captured calls, 1 distinct problems
; problem array.array_reduction#0 calls=210
; get-value (select a #x00000000)(select a #x00000001)(select a #x00000002)(select a #x00000003)(select a #x00000004)(select a #x00000005)
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (Array (_ BitVec 32) (_ BitVec 8)))
(assert (= #b1 (__Vbv (= (bvxor (bvxor (bvxor (bvxor (bvxor (select a #x00000000) (select a #x00000001)) (select a #x00000002)) (select a #x00000003)) (select a #x00000004)) (select a #x00000005)) #x5a))))
(assert (= #b1 (__Vbv (= (bvor (bvor (bvor (bvor (bvor (select a #x00000000) (select a #x00000001)) (select a #x00000002)) (select a #x00000003)) (select a #x00000004)) (select a #x00000005)) #xff))))
(assert (= #b1 (__Vbv (= (bvand (bvand (bvand (bvand (bvand (select a #x00000000) (select a #x00000001)) (select a #x00000002)) (select a #x00000003)) (select a #x00000004)) (select a #x00000005)) #x00))))
; end
