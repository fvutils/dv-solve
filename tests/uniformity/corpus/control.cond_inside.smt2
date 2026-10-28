; corpus for control.cond_inside from 210 captured calls, 1 distinct problems
; problem control.cond_inside#0 calls=210
; get-value sel v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun sel () (_ BitVec 2))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (__Vbv (not (= ((_ zero_extend 30) sel) #x00000003)))))
(assert (= #b1 (__Vbv (=> (__Vbool (bvor (__Vbv (= ((_ zero_extend 30) sel) #x00000000)) (__Vbv (= ((_ zero_extend 30) sel) #x00000001)))) (__Vbool (bvand (__Vbv (bvuge ((_ zero_extend 24) v) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) v) #x0000000f))))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (not (__Vbool (bvor (__Vbv (= ((_ zero_extend 30) sel) #x00000000)) (__Vbv (= ((_ zero_extend 30) sel) #x00000001))))))) (__Vbool (bvand (__Vbv (bvuge ((_ zero_extend 24) v) #x00000080)) (__Vbv (bvule ((_ zero_extend 24) v) #x000000ff))))))))
; end
