; corpus for arith.mod_align from 210 captured calls, 1 distinct problems
; problem arith.mod_align#0 calls=210
; get-value a m
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 32))
(declare-fun m () (_ BitVec 8))
(assert (= #b1 (bvor (bvor (bvor (__Vbv (= ((_ zero_extend 24) m) #x00000003)) (__Vbv (= ((_ zero_extend 24) m) #x00000005))) (__Vbv (= ((_ zero_extend 24) m) #x00000007))) (__Vbv (= ((_ zero_extend 24) m) #x0000000c)))))
(assert (= #b1 (__Vbv (= (bvurem a ((_ zero_extend 24) m)) #x00000000))))
(assert (= #b1 (__Vbv (= (bvurem a #x00000010) (bvmul #x00000004 (bvurem ((_ zero_extend 24) m) #x00000004))))))
(assert (= #b1 (__Vbv (bvult a #x00100000))))
; end
