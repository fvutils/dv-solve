; corpus for arith.shift_align from 210 captured calls, 1 distinct problems
; problem arith.shift_align#0 calls=210
; get-value addr sz
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun sz () (_ BitVec 3))
(assert (= #b1 (__Vbv (= (bvshl (bvlshr addr ((_ zero_extend 29) sz)) ((_ zero_extend 29) sz)) addr))))
(assert (= #b1 (__Vbv (bvult addr #x00010000))))
(assert (= #b1 (__Vbv (bvule sz #b101))))
(assert (= #b1 (__Vbv (not (= addr #x00000000)))))
; end
