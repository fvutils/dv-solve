; corpus for dist.dist_weight_div from 210 captured calls, 210 distinct problems
; problem dist.dist_weight_div#0 calls=1
; get-value a k
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun k () (_ BitVec 4))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000010)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x0000000f))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 28) k) #x0000000f)) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 28) k) #x00000001)) (__Vbv (bvule ((_ zero_extend 28) k) #x0000000e))) (__Vbv (= ((_ zero_extend 28) k) #x00000000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000004))) (__Vbv (= ((_ zero_extend 24) a) #x0000000d)) (__Vbv (= ((_ zero_extend 24) a) #x000000c6)))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000001))) (__Vbv (= ((_ zero_extend 28) k) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000008))) (__Vbv (= ((_ zero_extend 28) k) #x00000003)) (__Vbv (= ((_ zero_extend 28) k) #x0000000f))))))
; end
; problem dist.dist_weight_div#1 calls=1
; get-value a k
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun k () (_ BitVec 4))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000010)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x0000000f))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 28) k) #x0000000f)) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 28) k) #x00000001)) (__Vbv (bvule ((_ zero_extend 28) k) #x0000000e))) (__Vbv (= ((_ zero_extend 28) k) #x00000000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000004))) (__Vbv (= ((_ zero_extend 24) a) #x00000005)) (__Vbv (= ((_ zero_extend 24) a) #x00000022)))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000001))) (__Vbv (= ((_ zero_extend 28) k) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000008))) (__Vbv (= ((_ zero_extend 28) k) #x00000006)) (__Vbv (= ((_ zero_extend 28) k) #x0000000f))))))
; end
; problem dist.dist_weight_div#2 calls=1
; get-value a k
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun k () (_ BitVec 4))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000010)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x0000000f))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 28) k) #x0000000f)) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 28) k) #x00000001)) (__Vbv (bvule ((_ zero_extend 28) k) #x0000000e))) (__Vbv (= ((_ zero_extend 28) k) #x00000000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000003 #x0000000000000004))) (__Vbv (= ((_ zero_extend 24) a) #x00000003)) (__Vbv (= ((_ zero_extend 24) a) #x000000c0)))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000008 #x0000000000000001))) (__Vbv (= ((_ zero_extend 28) k) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000008 #x0000000000000008))) (__Vbv (= ((_ zero_extend 28) k) #x00000007)) (__Vbv (= ((_ zero_extend 28) k) #x0000000f))))))
; end
; problem dist.dist_weight_div#3 calls=1
; get-value a k
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun k () (_ BitVec 4))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000010)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x0000000f))))))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 28) k) #x0000000f)) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 28) k) #x00000001)) (__Vbv (bvule ((_ zero_extend 28) k) #x0000000e))) (__Vbv (= ((_ zero_extend 28) k) #x00000000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000004))) (__Vbv (= ((_ zero_extend 24) a) #x0000000f)) (__Vbv (= ((_ zero_extend 24) a) #x00000083)))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000001))) (__Vbv (= ((_ zero_extend 28) k) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000008))) (__Vbv (= ((_ zero_extend 28) k) #x00000002)) (__Vbv (= ((_ zero_extend 28) k) #x0000000f))))))
; end
