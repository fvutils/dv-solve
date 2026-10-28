; corpus for dist.weighted from 210 captured calls, 209 distinct problems
; problem dist.weighted#0 calls=2
; get-value r v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun r () (_ BitVec 8))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (__Vbv (= ((_ zero_extend 24) v) #x00000000)))))))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000013))) (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000009))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000005 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) v) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000005 #x0000000000000003))) (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000005 #x0000000000000007))) (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (__Vbv (= ((_ zero_extend 24) v) #x00000003)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) r) #x00000003)) (__Vbv (= ((_ zero_extend 24) r) #x0000000e)))))
; end
; problem dist.weighted#1 calls=1
; get-value r v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun r () (_ BitVec 8))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (__Vbv (= ((_ zero_extend 24) v) #x00000000)))))))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000013))) (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000009))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) v) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000003))) (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000007))) (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (__Vbv (= ((_ zero_extend 24) v) #x00000003)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) r) #x00000003)) (__Vbv (= ((_ zero_extend 24) r) #x0000000c)))))
; end
; problem dist.weighted#2 calls=1
; get-value r v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun r () (_ BitVec 8))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (__Vbv (= ((_ zero_extend 24) v) #x00000000)))))))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000013))) (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000009))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) v) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000003))) (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000007))) (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (__Vbv (= ((_ zero_extend 24) v) #x00000003)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) r) #x00000003)) (__Vbv (= ((_ zero_extend 24) r) #x0000000b)))))
; end
; problem dist.weighted#3 calls=1
; get-value r v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun r () (_ BitVec 8))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000003)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (bvor (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (__Vbv (= ((_ zero_extend 24) v) #x00000000)))))))
(assert (= #b1 (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000013))) (bvand (__Vbv (bvuge ((_ zero_extend 24) r) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) r) #x00000009))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) v) #x00000000)) (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000003))) (__Vbv (= ((_ zero_extend 24) v) #x00000001)) (ite (__Vbool (__Vbv (bvule #x0000000000000004 #x0000000000000007))) (__Vbv (= ((_ zero_extend 24) v) #x00000002)) (__Vbv (= ((_ zero_extend 24) v) #x00000003)))))))
(assert (= #b1 (ite (__Vbool (__Vbv (bvule #x0000000000000002 #x0000000000000001))) (__Vbv (= ((_ zero_extend 24) r) #x00000007)) (__Vbv (= ((_ zero_extend 24) r) #x00000011)))))
; end
