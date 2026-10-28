; corpus for control.implication_chain_8 from 210 captured calls, 1 distinct problems
; problem control.implication_chain_8#0 calls=210
; get-value f k
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun f () (_ BitVec 8))
(declare-fun k () (_ BitVec 4))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 0 0) f)) (__Vbool ((_ extract 1 1) f))))))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 1 1) f)) (__Vbool ((_ extract 2 2) f))))))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 2 2) f)) (__Vbool ((_ extract 3 3) f))))))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 3 3) f)) (__Vbool ((_ extract 4 4) f))))))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 4 4) f)) (__Vbool ((_ extract 5 5) f))))))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 5 5) f)) (__Vbool ((_ extract 6 6) f))))))
(assert (= #b1 (__Vbv (=> (__Vbool ((_ extract 6 6) f)) (__Vbool ((_ extract 7 7) f))))))
(assert (= #b1 (__Vbv (not (= ((_ zero_extend 24) f) #x00000000)))))
(assert (= #b1 (__Vbv (= ((_ zero_extend 24) (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (bvadd (bvand f #x01) (bvlshr (bvand f #x02) #x01)) (bvlshr (bvand f #x04) #x02)) (bvlshr (bvand f #x08) #x03)) (bvlshr (bvand f #x10) #x04)) (bvlshr (bvand f #x20) #x05)) (bvlshr (bvand f #x40) #x06)) (bvlshr (bvand f #x80) #x07))) ((_ zero_extend 28) k)))))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 28) k) #x00000002)) (__Vbv (bvule ((_ zero_extend 28) k) #x00000005)))))
; end
