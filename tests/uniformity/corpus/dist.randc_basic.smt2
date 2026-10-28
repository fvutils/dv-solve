; corpus for dist.randc_basic from 210 captured calls, 197 distinct problems
; problem dist.randc_basic#0 calls=14
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 4))
(declare-fun b () (_ BitVec 6))
(declare-fun c () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvult b #b101000))))
(assert (= #b1 (__Vbv (bvugt c ((_ repeat 1) (concat #x0 a))))))
; end
; problem dist.randc_basic#1 calls=1
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 4))
(declare-fun b () (_ BitVec 6))
(declare-fun c () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvult b #b101000))))
(assert (= #b1 (__Vbv (bvugt c ((_ repeat 1) (concat #x0 a))))))
(assert (not (= a (_ bv15 4))))
(assert (not (= b (_ bv14 6))))
; end
; problem dist.randc_basic#2 calls=1
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 4))
(declare-fun b () (_ BitVec 6))
(declare-fun c () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvult b #b101000))))
(assert (= #b1 (__Vbv (bvugt c ((_ repeat 1) (concat #x0 a))))))
(assert (not (= a (_ bv11 4))))
(assert (not (= a (_ bv15 4))))
(assert (not (= b (_ bv14 6))))
(assert (not (= b (_ bv15 6))))
; end
; problem dist.randc_basic#3 calls=1
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 4))
(declare-fun b () (_ BitVec 6))
(declare-fun c () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvult b #b101000))))
(assert (= #b1 (__Vbv (bvugt c ((_ repeat 1) (concat #x0 a))))))
(assert (not (= a (_ bv11 4))))
(assert (not (= a (_ bv12 4))))
(assert (not (= a (_ bv15 4))))
(assert (not (= b (_ bv14 6))))
(assert (not (= b (_ bv15 6))))
(assert (not (= b (_ bv27 6))))
; end
