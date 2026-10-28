; corpus for construct.enum_cond from 210 captured calls, 1 distinct problems
; problem construct.enum_cond#0 calls=210
; get-value addr cmd len
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun cmd () (_ BitVec 32))
(declare-fun len () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= cmd (_ bv0 32)) (= cmd (_ bv1 32)) (= cmd (_ bv2 32))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= cmd #x00000000))) (__Vbool (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000010))))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= cmd #x00000001))) (__Vbool (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000040))))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= cmd #x00000002))) (__Vbool (bvand (__Vbv (= ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvult addr #x00000100))))))))
(assert (= #b1 (__Vbv (= ((_ extract 1 0) addr) #b00))))
; end
