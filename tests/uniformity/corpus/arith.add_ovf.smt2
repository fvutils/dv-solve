; corpus for arith.add_ovf from 210 captured calls, 1 distinct problems
; problem arith.add_ovf#0 calls=210
; get-value a b s
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 8))
(declare-fun s () (_ BitVec 8))
(assert (= #b1 (__Vbv (= s (bvadd a b)))))
(assert (= #b1 (__Vbv (bvugt a #xc8))))
(assert (= #b1 (__Vbv (bvugt b #x64))))
; end
