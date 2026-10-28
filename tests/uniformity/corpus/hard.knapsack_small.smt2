; corpus for hard.knapsack_small from 210 captured calls, 1 distinct problems
; problem hard.knapsack_small#0 calls=210
; get-value t0 t1 t2 t3 t4 t5
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun t0 () (_ BitVec 1))
(declare-fun t1 () (_ BitVec 1))
(declare-fun t2 () (_ BitVec 1))
(declare-fun t3 () (_ BitVec 1))
(declare-fun t4 () (_ BitVec 1))
(declare-fun t5 () (_ BitVec 1))
(assert (= #b1 (__Vbv (bvsle (bvadd (bvadd (bvadd (bvadd (bvadd (bvmul ((_ zero_extend 31) t0) #x00000003) (bvmul ((_ zero_extend 31) t1) #x00000004)) (bvmul ((_ zero_extend 31) t2) #x00000005)) (bvmul ((_ zero_extend 31) t3) #x00000006)) (bvmul ((_ zero_extend 31) t4) #x00000007)) (bvmul ((_ zero_extend 31) t5) #x00000008)) #x00000014))))
(assert (= #b1 (__Vbv (bvsge (bvadd (bvadd (bvadd (bvadd (bvadd (bvmul ((_ zero_extend 31) t0) #x00000004) (bvmul ((_ zero_extend 31) t1) #x00000005)) (bvmul ((_ zero_extend 31) t2) #x00000006)) (bvmul ((_ zero_extend 31) t3) #x00000007)) (bvmul ((_ zero_extend 31) t4) #x00000008)) (bvmul ((_ zero_extend 31) t5) #x00000009)) #x00000016))))
; end
