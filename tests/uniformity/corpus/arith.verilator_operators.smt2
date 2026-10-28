; corpus for arith.verilator_operators from 210 captured calls, 1 distinct problems
; problem arith.verilator_operators#0 calls=210
; get-value a b c d e
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(declare-fun c () (_ BitVec 8))
(declare-fun d () (_ BitVec 8))
(declare-fun e () (_ BitVec 8))
(assert (= #b1 (__Vbv (= (bvand a b) #x0f))))
(assert (= #b1 (__Vbv (= (bvor a c) #xff))))
(assert (= #b1 (__Vbv (= (bvxor a d) #x55))))
(assert (= #b1 (__Vbv (= e (bvnot a)))))
(assert (= #b1 (__Vbv (not (= b a)))))
(assert (= #b1 (__Vbv (not (= c #xff)))))
; end
