; corpus for array.assoc_basic from 210 captured calls, 1 distinct problems
; problem array.assoc_basic#0 calls=210
; get-value (select m #x00000000)(select m #x0000000a)(select m #x00000014)(select m #x0000001e)(select m #x00000028)
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun m () (Array (_ BitVec 32) (_ BitVec 8)))
(assert (= #b1 (__Vbv (= (select m #x00000000) #x01))))
(assert (= #b1 (__Vbv (= (select m #x0000000a) #x0b))))
(assert (= #b1 (__Vbv (= (select m #x00000014) #x15))))
(assert (= #b1 (__Vbv (= (select m #x0000001e) #x1f))))
(assert (= #b1 (__Vbv (= (select m #x00000028) #x29))))
; end
