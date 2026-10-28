; corpus for control.implication_set from 210 captured calls, 1 distinct problems
; problem control.implication_set#0 calls=210
; get-value len mode
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun len () (_ BitVec 8))
(declare-fun mode () (_ BitVec 3))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 29) mode) #x00000000)) (__Vbv (bvule ((_ zero_extend 29) mode) #x00000002)))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= ((_ zero_extend 29) mode) #x00000000))) (__Vbool (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000004))))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= ((_ zero_extend 29) mode) #x00000001))) (__Vbool (bvor (__Vbv (= ((_ zero_extend 24) len) #x00000008)) (__Vbv (= ((_ zero_extend 24) len) #x00000010))))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= ((_ zero_extend 29) mode) #x00000002))) (__Vbool (__Vbv (bvuge len #x64)))))))
; end
