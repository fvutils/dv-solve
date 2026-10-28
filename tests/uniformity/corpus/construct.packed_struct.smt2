; corpus for construct.packed_struct from 210 captured calls, 1 distinct problems
; problem construct.packed_struct#0 calls=210
; get-value c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun c () (_ BitVec 20))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 19 19) c)) (__Vbool (__Vbv (bvugt ((_ zero_extend 24) ((_ extract 15 8) c)) #x00000000)))))))
(assert (= #b1 (__Vbv (bvult ((_ extract 15 8) c) #x64))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 29) ((_ extract 18 16) c)) #x00000001)) (__Vbv (bvule ((_ zero_extend 29) ((_ extract 18 16) c)) #x00000005)))))
(assert (= #b1 (__Vbv (= ((_ zero_extend 24) ((_ extract 7 0) c)) (bvudiv ((_ zero_extend 24) ((_ extract 15 8) c)) #x00000004)))))
; end
