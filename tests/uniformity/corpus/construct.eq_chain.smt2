; corpus for construct.eq_chain from 210 captured calls, 1 distinct problems
; problem construct.eq_chain#0 calls=210
; get-value a b c d
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 16))
(declare-fun b () (_ BitVec 16))
(declare-fun c () (_ BitVec 16))
(declare-fun d () (_ BitVec 16))
(assert (= #b1 (__Vbv (= b (bvadd a #x0003)))))
(assert (= #b1 (__Vbv (= c (bvadd b #x0005)))))
(assert (= #b1 (__Vbv (= d (bvsub c #x0001)))))
(assert (= #b1 (__Vbv (bvult a #x03e8))))
; end
