; corpus for dist.dist_range from 210 captured calls, 210 distinct problems
; problem dist.dist_range#0 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 16))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000064)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000063))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000009)))))))
(assert (= #b1 (bvor (__Vbv (= b #xffff)) (bvor (bvand (__Vbv (bvuge b #x0001)) (__Vbv (bvule b #x03e8))) (__Vbv (= b #x0000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x00000000000000fb #x0000000000000032))) (__Vbv (= ((_ zero_extend 24) a) #x00000003)) (ite (__Vbool (__Vbv (bvule #x00000000000000fb #x000000000000008c))) (__Vbv (= ((_ zero_extend 24) a) #x00000048)) (__Vbv (= ((_ zero_extend 24) a) #x00000082))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x00000000000000e2 #x0000000000000001))) (__Vbv (= b #x0000)) (ite (__Vbool (__Vbv (bvule #x00000000000000e2 #x0000000000001f41))) (__Vbv (= b #x00ac)) (__Vbv (= b #xffff))))))
; end
; problem dist.dist_range#1 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 16))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000064)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000063))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000009)))))))
(assert (= #b1 (bvor (__Vbv (= b #xffff)) (bvor (bvand (__Vbv (bvuge b #x0001)) (__Vbv (bvule b #x03e8))) (__Vbv (= b #x0000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x00000000000000e0 #x0000000000000032))) (__Vbv (= ((_ zero_extend 24) a) #x00000008)) (ite (__Vbool (__Vbv (bvule #x00000000000000e0 #x000000000000008c))) (__Vbv (= ((_ zero_extend 24) a) #x0000004f)) (__Vbv (= ((_ zero_extend 24) a) #x00000079))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000001846 #x0000000000000001))) (__Vbv (= b #x0000)) (ite (__Vbool (__Vbv (bvule #x0000000000001846 #x0000000000001f41))) (__Vbv (= b #x01ca)) (__Vbv (= b #xffff))))))
; end
; problem dist.dist_range#2 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 16))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000064)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000063))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000009)))))))
(assert (= #b1 (bvor (__Vbv (= b #xffff)) (bvor (bvand (__Vbv (bvuge b #x0001)) (__Vbv (bvule b #x03e8))) (__Vbv (= b #x0000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x000000000000006c #x0000000000000032))) (__Vbv (= ((_ zero_extend 24) a) #x00000004)) (ite (__Vbool (__Vbv (bvule #x000000000000006c #x000000000000008c))) (__Vbv (= ((_ zero_extend 24) a) #x00000035)) (__Vbv (= ((_ zero_extend 24) a) #x000000bb))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x00000000000012f9 #x0000000000000001))) (__Vbv (= b #x0000)) (ite (__Vbool (__Vbv (bvule #x00000000000012f9 #x0000000000001f41))) (__Vbv (= b #x0060)) (__Vbv (= b #xffff))))))
; end
; problem dist.dist_range#3 calls=1
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 16))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000064)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000ff))) (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000063))) (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) a) #x00000009)))))))
(assert (= #b1 (bvor (__Vbv (= b #xffff)) (bvor (bvand (__Vbv (bvuge b #x0001)) (__Vbv (bvule b #x03e8))) (__Vbv (= b #x0000))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000124 #x0000000000000032))) (__Vbv (= ((_ zero_extend 24) a) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000124 #x000000000000008c))) (__Vbv (= ((_ zero_extend 24) a) #x00000045)) (__Vbv (= ((_ zero_extend 24) a) #x0000007e))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x000000000000011c #x0000000000000001))) (__Vbv (= b #x0000)) (ite (__Vbool (__Vbv (bvule #x000000000000011c #x0000000000001f41))) (__Vbv (= b #x01d9)) (__Vbv (= b #xffff))))))
; end
