; corpus for construct.ne_set from 210 captured calls, 1 distinct problems
; problem construct.ne_set#0 calls=210
; get-value a b c d
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 4))
(declare-fun b () (_ BitVec 4))
(declare-fun c () (_ BitVec 4))
(declare-fun d () (_ BitVec 4))
(assert (= #b1 (__Vbv (not (= a b)))))
(assert (= #b1 (__Vbv (not (= a c)))))
(assert (= #b1 (__Vbv (not (= a d)))))
(assert (= #b1 (__Vbv (not (= b c)))))
(assert (= #b1 (__Vbv (not (= b d)))))
(assert (= #b1 (__Vbv (not (= c d)))))
(assert (= #b1 (__Vbv (not (= a #x0)))))
(assert (= #b1 (__Vbv (not (= d #xf)))))
; end
