; corpus for construct.inside_set from 210 captured calls, 1 distinct problems
; problem construct.inside_set#0 calls=210
; get-value a b c
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 16))
(declare-fun c () (_ BitVec 32))
(assert (= #b1 (bvor (bvor (bvor (bvor (bvor (bvor (bvor (__Vbv (= ((_ zero_extend 24) a) #x00000001)) (__Vbv (= ((_ zero_extend 24) a) #x00000003))) (__Vbv (= ((_ zero_extend 24) a) #x00000005))) (__Vbv (= ((_ zero_extend 24) a) #x00000007))) (__Vbv (= ((_ zero_extend 24) a) #x0000000b))) (__Vbv (= ((_ zero_extend 24) a) #x0000000d))) (__Vbv (= ((_ zero_extend 24) a) #x00000011))) (__Vbv (= ((_ zero_extend 24) a) #x00000013)))))
(assert (= #b1 (bvor (bvor (bvor (__Vbv (= b #x0100)) (__Vbv (= b #x0200))) (__Vbv (= b #x0400))) (__Vbv (= b #x0800)))))
(assert (= #b1 (bvor (bvor (bvor (__Vbv (= c #xfffffff9)) (__Vbv (= c #x00000000))) (__Vbv (= c #x0000002a))) (__Vbv (= c #x000003e8)))))
; end
