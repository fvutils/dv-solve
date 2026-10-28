; corpus for arith.bitslice from 210 captured calls, 1 distinct problems
; problem arith.bitslice#0 calls=210
; get-value hi lo w
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun hi () (_ BitVec 4))
(declare-fun lo () (_ BitVec 4))
(declare-fun w () (_ BitVec 32))
(assert (= #b1 (__Vbv (= ((_ extract 31 28) w) hi))))
(assert (= #b1 (__Vbv (= ((_ extract 3 0) w) lo))))
(assert (= #b1 (__Vbv (bvugt hi lo))))
(assert (= #b1 (__Vbv (= ((_ extract 15 8) w) ((_ repeat 1) (concat hi lo))))))
(assert (= #b1 (__Vbv (= ((_ extract 27 16) w) #xabc))))
; end
