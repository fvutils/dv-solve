; corpus for construct.xor_parity from 210 captured calls, 1 distinct problems
; problem construct.xor_parity#0 calls=210
; get-value data lo p
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun data () (_ BitVec 32))
(declare-fun lo () (_ BitVec 8))
(declare-fun p () (_ BitVec 1))
(assert (= #b1 (__Vbv (= p (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor (bvxor ((_ extract 0 0) data) ((_ extract 1 1) data)) ((_ extract 2 2) data)) ((_ extract 3 3) data)) ((_ extract 4 4) data)) ((_ extract 5 5) data)) ((_ extract 6 6) data)) ((_ extract 7 7) data)) ((_ extract 8 8) data)) ((_ extract 9 9) data)) ((_ extract 10 10) data)) ((_ extract 11 11) data)) ((_ extract 12 12) data)) ((_ extract 13 13) data)) ((_ extract 14 14) data)) ((_ extract 15 15) data)) ((_ extract 16 16) data)) ((_ extract 17 17) data)) ((_ extract 18 18) data)) ((_ extract 19 19) data)) ((_ extract 20 20) data)) ((_ extract 21 21) data)) ((_ extract 22 22) data)) ((_ extract 23 23) data)) ((_ extract 24 24) data)) ((_ extract 25 25) data)) ((_ extract 26 26) data)) ((_ extract 27 27) data)) ((_ extract 28 28) data)) ((_ extract 29 29) data)) ((_ extract 30 30) data)) ((_ extract 31 31) data))))))
(assert (= #b1 (__Vbv (= p #b1))))
(assert (= #b1 (__Vbv (= lo ((_ extract 7 0) data)))))
(assert (= #b1 (__Vbv (= ((_ extract 0 0) lo) #b0))))
; end
