; corpus for construct.enum_basic from 210 captured calls, 1 distinct problems
; problem construct.enum_basic#0 calls=210
; get-value md op
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun md () (_ BitVec 2))
(declare-fun op () (_ BitVec 3))
(assert (= #b1 (__Vbv (or (= op (_ bv0 3)) (= op (_ bv1 3)) (= op (_ bv2 3)) (= op (_ bv3 3)) (= op (_ bv4 3))))))
(assert (= #b1 (__Vbv (or (= md (_ bv0 2)) (= md (_ bv1 2)) (= md (_ bv2 2))))))
(assert (= #b1 (__Vbv (not (= op #b000)))))
(assert (= #b1 (bvor (__Vbv (= md #b01)) (__Vbv (= md #b10)))))
; end
