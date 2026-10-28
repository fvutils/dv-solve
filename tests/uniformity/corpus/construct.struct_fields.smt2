; corpus for construct.struct_fields from 210 captured calls, 1 distinct problems
; problem construct.struct_fields#0 calls=210
; get-value crc h
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun crc () (_ BitVec 16))
(declare-fun h () (_ BitVec 28))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) ((_ extract 23 16) h)) #x00000004)) (__Vbv (bvule ((_ zero_extend 24) ((_ extract 23 16) h)) #x0000003c)))))
(assert (= #b1 (__Vbv (not (= ((_ zero_extend 28) ((_ extract 27 24) h)) #x00000000)))))
(assert (= #b1 (__Vbv (not (= ((_ extract 7 0) h) ((_ extract 15 8) h))))))
(assert (= #b1 (__Vbv (= crc ((_ repeat 1) (concat ((_ extract 7 0) h) ((_ extract 15 8) h)))))))
; end
