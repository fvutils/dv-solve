; corpus for arith.shift_width from 210 captured calls, 1 distinct problems
; problem arith.shift_width#0 calls=210
; get-value a k r
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 16))
(declare-fun k () (_ BitVec 5))
(declare-fun r () (_ BitVec 32))
(assert (= #b1 (__Vbv (= r (bvshl ((_ repeat 1) (concat #x0000 a)) ((_ zero_extend 27) k))))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 27) k) #x00000001)) (__Vbv (bvule ((_ zero_extend 27) k) #x00000010)))))
(assert (= #b1 (__Vbv (not (= ((_ zero_extend 16) ((_ extract 31 16) r)) #x00000000)))))
; end
