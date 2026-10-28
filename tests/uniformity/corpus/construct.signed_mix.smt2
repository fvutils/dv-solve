; corpus for construct.signed_mix from 210 captured calls, 1 distinct problems
; problem construct.signed_mix#0 calls=210
; get-value a b c u
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 32))
(declare-fun b () (_ BitVec 8))
(declare-fun c () (_ BitVec 16))
(declare-fun u () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvsgt a #xfffffc18))))
(assert (= #b1 (__Vbv (bvslt a #x000003e8))))
(assert (= #b1 (__Vbv (bvslt ((_ sign_extend 24) b) #x00000000))))
(assert (= #b1 (__Vbv (bvsgt ((_ sign_extend 24) b) #xffffff9c))))
(assert (= #b1 (__Vbv (= ((_ sign_extend 16) c) (bvsub a ((_ sign_extend 24) b))))))
(assert (= #b1 (__Vbv (bvugt u #x64))))
(assert (= #b1 (__Vbv (bvsgt (bvadd ((_ zero_extend 24) u) a) #x00000000))))
; end
