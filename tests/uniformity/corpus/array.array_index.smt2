; corpus for array.array_index from 210 captured calls, 1 distinct problems
; problem array.array_index#0 calls=210
; get-value (select a #x00000000)(select a #x00000001)(select a #x00000002)(select a #x00000003)(select a #x00000004)(select a #x00000005)(select a #x00000006)(select a #x00000007) idx
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (Array (_ BitVec 32) (_ BitVec 8)))
(declare-fun idx () (_ BitVec 3))
(assert (= #b1 (__Vbv (= (select a ((_ zero_extend 29) idx)) #xaa))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000000 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000000) #x80)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000001 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000001) #x80)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000002 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000002) #x80)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000003 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000003) #x80)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000004 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000004) #x80)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000005 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000005) #x80)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000006 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000006) #x80)))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (= #x00000007 ((_ zero_extend 29) idx))))) (__Vbool (__Vbv (bvult (select a #x00000007) #x80)))))))
; end
