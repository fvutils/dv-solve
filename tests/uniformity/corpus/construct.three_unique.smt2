; corpus for construct.three_unique from 210 captured calls, 1 distinct problems
; problem construct.three_unique#0 calls=210
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(declare-fun c () (_ BitVec 8))
(assert (= #b1 (__Vbv (not (= b c)))))
(assert (= #b1 (__Vbv (not (= a c)))))
(assert (= #b1 (__Vbv (not (= a b)))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000007)))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) b) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) b) #x00000007)))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) c) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) c) #x00000007)))))
(assert (= #b1 (__Vbv (= (bvadd (bvadd a b) c) #x0c))))
; end
