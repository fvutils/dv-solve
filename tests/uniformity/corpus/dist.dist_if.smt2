; corpus for dist.dist_if from 210 captured calls, 210 distinct problems
; problem dist.dist_if#0 calls=1
; get-value len mode
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun len () (_ BitVec 8))
(declare-fun mode () (_ BitVec 1))
(assert (= #b1 (ite (__Vbool mode) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000005)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000040))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000004)))) (ite (__Vbool (__Vbv (bvule #x000000000000000b #x000000000000000c))) (__Vbv (= ((_ zero_extend 24) len) #x00000002)) (__Vbv (= ((_ zero_extend 24) len) #x00000007)))) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000081)) (__Vbv (bvule ((_ zero_extend 24) len) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000040)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000080)))) (ite (__Vbool (__Vbv (bvule #x0000000000000030 #x0000000000000041))) (__Vbv (= ((_ zero_extend 24) len) #x0000006b)) (__Vbv (= ((_ zero_extend 24) len) #x00000085)))))))
; end
; problem dist.dist_if#1 calls=1
; get-value len mode
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun len () (_ BitVec 8))
(declare-fun mode () (_ BitVec 1))
(assert (= #b1 (ite (__Vbool mode) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000005)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000040))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000004)))) (ite (__Vbool (__Vbv (bvule #x0000000000000028 #x000000000000000c))) (__Vbv (= ((_ zero_extend 24) len) #x00000003)) (__Vbv (= ((_ zero_extend 24) len) #x0000000e)))) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000081)) (__Vbv (bvule ((_ zero_extend 24) len) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000040)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000080)))) (ite (__Vbool (__Vbv (bvule #x0000000000000086 #x0000000000000041))) (__Vbv (= ((_ zero_extend 24) len) #x0000006f)) (__Vbv (= ((_ zero_extend 24) len) #x000000fb)))))))
; end
; problem dist.dist_if#2 calls=1
; get-value len mode
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun len () (_ BitVec 8))
(declare-fun mode () (_ BitVec 1))
(assert (= #b1 (ite (__Vbool mode) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000005)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000040))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000004)))) (ite (__Vbool (__Vbv (bvule #x0000000000000014 #x000000000000000c))) (__Vbv (= ((_ zero_extend 24) len) #x00000001)) (__Vbv (= ((_ zero_extend 24) len) #x00000030)))) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000081)) (__Vbv (bvule ((_ zero_extend 24) len) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000040)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000080)))) (ite (__Vbool (__Vbv (bvule #x0000000000000021 #x0000000000000041))) (__Vbv (= ((_ zero_extend 24) len) #x00000070)) (__Vbv (= ((_ zero_extend 24) len) #x000000b9)))))))
; end
; problem dist.dist_if#3 calls=1
; get-value len mode
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun len () (_ BitVec 8))
(declare-fun mode () (_ BitVec 1))
(assert (= #b1 (ite (__Vbool mode) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000005)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000040))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000001)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000004)))) (ite (__Vbool (__Vbv (bvule #x000000000000002c #x000000000000000c))) (__Vbv (= ((_ zero_extend 24) len) #x00000001)) (__Vbv (= ((_ zero_extend 24) len) #x00000022)))) (bvand (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000081)) (__Vbv (bvule ((_ zero_extend 24) len) #x000000ff))) (bvand (__Vbv (bvuge ((_ zero_extend 24) len) #x00000040)) (__Vbv (bvule ((_ zero_extend 24) len) #x00000080)))) (ite (__Vbool (__Vbv (bvule #x0000000000000076 #x0000000000000041))) (__Vbv (= ((_ zero_extend 24) len) #x0000004d)) (__Vbv (= ((_ zero_extend 24) len) #x000000c1)))))))
; end
