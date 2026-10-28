; corpus for dist.dist_mixed from 210 captured calls, 82 distinct problems
; problem dist.dist_mixed#0 calls=24
; get-value addr sz
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun sz () (_ BitVec 3))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge addr #x00001000)) (__Vbv (bvule addr #xffffffff))) (bvand (__Vbv (bvuge addr #x00000000)) (__Vbv (bvule addr #x00000fff))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)))))))
(assert (= #b1 (__Vbv (bvult sz #b100))))
(assert (= #b1 (__Vbv (= (bvurem addr (bvshl #x00000001 ((_ zero_extend 29) sz))) #x00000000))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000007 #x0000000000000001))) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000007 #x0000000000000002))) (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000007 #x0000000000000006))) (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (__Vbv (= ((_ zero_extend 29) sz) #x00000003)))))))
; end
; problem dist.dist_mixed#1 calls=21
; get-value addr sz
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun sz () (_ BitVec 3))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge addr #x00001000)) (__Vbv (bvule addr #xffffffff))) (bvand (__Vbv (bvuge addr #x00000000)) (__Vbv (bvule addr #x00000fff))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)))))))
(assert (= #b1 (__Vbv (bvult sz #b100))))
(assert (= #b1 (__Vbv (= (bvurem addr (bvshl #x00000001 ((_ zero_extend 29) sz))) #x00000000))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000003 #x0000000000000001))) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000003 #x0000000000000002))) (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000003 #x0000000000000006))) (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (__Vbv (= ((_ zero_extend 29) sz) #x00000003)))))))
; end
; problem dist.dist_mixed#2 calls=20
; get-value addr sz
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun sz () (_ BitVec 3))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge addr #x00001000)) (__Vbv (bvule addr #xffffffff))) (bvand (__Vbv (bvuge addr #x00000000)) (__Vbv (bvule addr #x00000fff))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)))))))
(assert (= #b1 (__Vbv (bvult sz #b100))))
(assert (= #b1 (__Vbv (= (bvurem addr (bvshl #x00000001 ((_ zero_extend 29) sz))) #x00000000))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000006 #x0000000000000001))) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000006 #x0000000000000002))) (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000006 #x0000000000000006))) (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (__Vbv (= ((_ zero_extend 29) sz) #x00000003)))))))
; end
; problem dist.dist_mixed#3 calls=19
; get-value addr sz
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun sz () (_ BitVec 3))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge addr #x00001000)) (__Vbv (bvule addr #xffffffff))) (bvand (__Vbv (bvuge addr #x00000000)) (__Vbv (bvule addr #x00000fff))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)))))))
(assert (= #b1 (__Vbv (bvult sz #b100))))
(assert (= #b1 (__Vbv (= (bvurem addr (bvshl #x00000001 ((_ zero_extend 29) sz))) #x00000000))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000008 #x0000000000000001))) (__Vbv (= ((_ zero_extend 29) sz) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000008 #x0000000000000002))) (__Vbv (= ((_ zero_extend 29) sz) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000008 #x0000000000000006))) (__Vbv (= ((_ zero_extend 29) sz) #x00000002)) (__Vbv (= ((_ zero_extend 29) sz) #x00000003)))))))
; end
