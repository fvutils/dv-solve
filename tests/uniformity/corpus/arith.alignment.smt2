; corpus for arith.alignment from 210 captured calls, 1 distinct problems
; problem arith.alignment#0 calls=210
; get-value addr len
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun addr () (_ BitVec 32))
(declare-fun len () (_ BitVec 32))
(assert (= #b1 (bvor (bvor (bvor (bvor (bvor (bvor (__Vbv (= len #x00000001)) (__Vbv (= len #x00000002))) (__Vbv (= len #x00000004))) (__Vbv (= len #x00000008))) (__Vbv (= len #x00000010))) (__Vbv (= len #x00000020))) (__Vbv (= len #x00000040)))))
(assert (= #b1 (__Vbv (= (bvand addr (bvsub len #x00000001)) #x00000000))))
(assert (= #b1 (__Vbv (bvule (bvadd addr len) #x00002000))))
(assert (= #b1 (__Vbv (bvuge addr #x00001000))))
; end
