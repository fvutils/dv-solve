; corpus for construct.width_mix from 210 captured calls, 1 distinct problems
; problem construct.width_mix#0 calls=210
; get-value m n w
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun m () (_ BitVec 12))
(declare-fun n () (_ BitVec 4))
(declare-fun w () (_ BitVec 40))
(assert (= #b1 (__Vbv (bvugt m ((_ repeat 1) (concat #x00 n))))))
(assert (= #b1 (__Vbv (bvult m #x190))))
(assert (= #b1 (__Vbv (= ((_ extract 39 12) w) #x0000005))))
(assert (= #b1 (__Vbv (= ((_ extract 11 0) w) m))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 28) n) #x00000002)) (__Vbv (bvule ((_ zero_extend 28) n) #x0000000c)))))
; end
