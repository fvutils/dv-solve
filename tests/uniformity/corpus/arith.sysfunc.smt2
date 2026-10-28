; corpus for arith.sysfunc from 210 captured calls, 1 distinct problems
; problem arith.sysfunc#0 calls=210
; get-value lg n
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun lg () (_ BitVec 5))
(declare-fun n () (_ BitVec 16))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 16) n) #x00000002)) (__Vbv (bvule ((_ zero_extend 16) n) #x00000400)))))
(assert (= #b1 (__Vbv (= lg ((_ extract 4 0) (ite (__Vbool (__Vbv (bvult (bvsub ((_ zero_extend 16) n) #x00000001) #x00000002))) #x00000001 (ite (__Vbool (__Vbv (bvult (bvsub ((_ zero_extend 16) n) #x00000001) #x00000004))) #x00000002 (ite (__Vbool (__Vbv (bvult (bvsub ((_ zero_extend 16) n) #x00000001) #x00000008))) #x00000003 (ite (__Vbool (__Vbv (bvult (bvsub ((_ zero_extend 16) n) #x00000001) #x00000010))) #x00000004 #x00000005)))))))))
(assert (= #b1 (__Vbv (bvult ((_ zero_extend 16) n) #x00000011))))
; end
