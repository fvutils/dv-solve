; corpus for arith.pow2 from 210 captured calls, 1 distinct problems
; problem arith.pow2#0 calls=210
; get-value k v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun k () (_ BitVec 5))
(declare-fun v () (_ BitVec 32))
(assert (= #b1 (__Vbv (not (= v #x00000000)))))
(assert (= #b1 (__Vbv (= (bvand v (bvsub v #x00000001)) #x00000000))))
(assert (= #b1 (__Vbv (= v (bvshl #x00000001 ((_ zero_extend 27) k))))))
(assert (= #b1 (__Vbv (bvuge k #b00100))))
; end
