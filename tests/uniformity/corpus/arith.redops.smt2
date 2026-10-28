; corpus for arith.redops from 210 captured calls, 1 distinct problems
; problem arith.redops#0 calls=210
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(declare-fun c () (_ BitVec 8))
(assert (= #b1 (__Vbv (= (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor ((_ extract 0 0) a) ((_ extract 1 1) a)) ((_ extract 2 2) a)) ((_ extract 3 3) a)) ((_ extract 4 4) a)) ((_ extract 5 5) a)) ((_ extract 6 6) a)) ((_ extract 7 7) a)) #b1))))
(assert (= #b1 (__Vbv (= (__Vbv (= (bvnot ((_ extract 3 0) b)) #x0)) #b1))))
(assert (= #b1 (__Vbv (= (__Vbv (not (= c #x00))) #b1))))
(assert (= #b1 (__Vbv (not (__Vbool (__Vbv (not (= ((_ extract 7 4) c) #x0))))))))
(assert (= #b1 (__Vbv (not (= ((_ extract 7 4) b) #xf)))))
; end
