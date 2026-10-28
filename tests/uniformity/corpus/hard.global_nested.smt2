; corpus for hard.global_nested from 210 captured calls, 1 distinct problems
; problem hard.global_nested#0 calls=210
; get-value m0.l.v m0.w m1.l.v m1.w x
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun m0.l.v () (_ BitVec 8))
(declare-fun m0.w () (_ BitVec 8))
(declare-fun m1.l.v () (_ BitVec 8))
(declare-fun m1.w () (_ BitVec 8))
(declare-fun x () (_ BitVec 8))
(assert (= #b1 (__Vbv (= x (bvadd m0.l.v m1.l.v)))))
(assert (= #b1 (__Vbv (bvugt m0.w m0.l.v))))
(assert (= #b1 (__Vbv (bvult m1.w m1.l.v))))
(assert (= #b1 (__Vbv (bvult m0.l.v m1.l.v))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) x) #x00000064)) (__Vbv (bvule ((_ zero_extend 24) x) #x000000c8)))))
(assert (= #b1 (__Vbv (not (= m0.w m1.w)))))
; end
