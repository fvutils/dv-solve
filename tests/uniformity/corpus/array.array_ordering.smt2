; corpus for array.array_ordering from 210 captured calls, 1 distinct problems
; problem array.array_ordering#0 calls=210
; get-value (select a #x00000000)(select a #x00000001)(select a #x00000002)(select a #x00000003)(select a #x00000004)(select a #x00000005)(select a #x00000006)(select a #x00000007)
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (Array (_ BitVec 32) (_ BitVec 8)))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvult (select a #x00000000) (select a #x00000001))))))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvugt (select a #x00000001) (select a #x00000002))))))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvult (select a #x00000002) (select a #x00000003))))))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvugt (select a #x00000003) (select a #x00000004))))))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvult (select a #x00000004) (select a #x00000005))))))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvugt (select a #x00000005) (select a #x00000006))))))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvult (select a #x00000006) (select a #x00000007))))))))
(assert (= #b1 (__Vbv (=> (__Vbool #b0) (__Vbool (__Vbv (bvugt (select a #x00000007) (select a #x00000000))))))))
; end
