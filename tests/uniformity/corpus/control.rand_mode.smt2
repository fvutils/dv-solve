; corpus for control.rand_mode from 210 captured calls, 210 distinct problems
; problem control.rand_mode#0 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(assert (= #b1 (__Vbv (= b (bvadd #x03 #x01)))))
; end
; problem control.rand_mode#1 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(assert (= #b1 (__Vbv (= b (bvadd #x06 #x01)))))
; end
; problem control.rand_mode#2 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(assert (= #b1 (__Vbv (= b (bvadd #x09 #x01)))))
; end
; problem control.rand_mode#3 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(assert (= #b1 (__Vbv (= b (bvadd #x0c #x01)))))
; end
