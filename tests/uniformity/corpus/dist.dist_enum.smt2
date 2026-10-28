; corpus for dist.dist_enum from 210 captured calls, 10 distinct problems
; problem dist.dist_enum#0 calls=24
; get-value k n
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun k () (_ BitVec 2))
(declare-fun n () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= k (_ bv0 2)) (= k (_ bv1 2)) (= k (_ bv2 2))))))
(assert (= #b1 (bvor (__Vbv (= k #b10)) (bvor (__Vbv (= k #b01)) (__Vbv (= k #b00))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= k #b10))) (__Vbool (__Vbv (bvugt n #xc8)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000003 #x0000000000000006))) (__Vbv (= k #b00)) (ite (__Vbool (__Vbv (bvule #x0000000000000003 #x0000000000000009))) (__Vbv (= k #b01)) (__Vbv (= k #b10))))))
; end
; problem dist.dist_enum#1 calls=23
; get-value k n
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun k () (_ BitVec 2))
(declare-fun n () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= k (_ bv0 2)) (= k (_ bv1 2)) (= k (_ bv2 2))))))
(assert (= #b1 (bvor (__Vbv (= k #b10)) (bvor (__Vbv (= k #b01)) (__Vbv (= k #b00))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= k #b10))) (__Vbool (__Vbv (bvugt n #xc8)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000009 #x0000000000000006))) (__Vbv (= k #b00)) (ite (__Vbool (__Vbv (bvule #x0000000000000009 #x0000000000000009))) (__Vbv (= k #b01)) (__Vbv (= k #b10))))))
; end
; problem dist.dist_enum#2 calls=23
; get-value k n
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun k () (_ BitVec 2))
(declare-fun n () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= k (_ bv0 2)) (= k (_ bv1 2)) (= k (_ bv2 2))))))
(assert (= #b1 (bvor (__Vbv (= k #b10)) (bvor (__Vbv (= k #b01)) (__Vbv (= k #b00))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= k #b10))) (__Vbool (__Vbv (bvugt n #xc8)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x000000000000000a #x0000000000000006))) (__Vbv (= k #b00)) (ite (__Vbool (__Vbv (bvule #x000000000000000a #x0000000000000009))) (__Vbv (= k #b01)) (__Vbv (= k #b10))))))
; end
; problem dist.dist_enum#3 calls=22
; get-value k n
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun k () (_ BitVec 2))
(declare-fun n () (_ BitVec 8))
(assert (= #b1 (__Vbv (or (= k (_ bv0 2)) (= k (_ bv1 2)) (= k (_ bv2 2))))))
(assert (= #b1 (bvor (__Vbv (= k #b10)) (bvor (__Vbv (= k #b01)) (__Vbv (= k #b00))))))
(assert (= #b1 (__Vbv (=> (__Vbool (__Vbv (= k #b10))) (__Vbool (__Vbv (bvugt n #xc8)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000007 #x0000000000000006))) (__Vbv (= k #b00)) (ite (__Vbool (__Vbv (bvule #x0000000000000007 #x0000000000000009))) (__Vbv (= k #b01)) (__Vbv (= k #b10))))))
; end
