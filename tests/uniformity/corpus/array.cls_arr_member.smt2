; corpus for array.cls_arr_member from 210 captured calls, 1 distinct problems
; problem array.cls_arr_member#0 calls=210
; get-value e.0.v e.1.v e.2.v e.3.v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun e.0.v () (_ BitVec 8))
(declare-fun e.1.v () (_ BitVec 8))
(declare-fun e.2.v () (_ BitVec 8))
(declare-fun e.3.v () (_ BitVec 8))
(assert (= #b1 (__Vbv (bvult e.0.v #x64))))
(assert (= #b1 (__Vbv (=> (__Vbool #b0) (__Vbool (__Vbv (bvugt e.0.v e.3.v)))))))
(assert (= #b1 (__Vbv (bvult e.1.v #x64))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvugt e.1.v e.0.v)))))))
(assert (= #b1 (__Vbv (bvult e.2.v #x64))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvugt e.2.v e.1.v)))))))
(assert (= #b1 (__Vbv (bvult e.3.v #x64))))
(assert (= #b1 (__Vbv (=> (__Vbool #b1) (__Vbool (__Vbv (bvugt e.3.v e.2.v)))))))
; end
