; corpus for construct.inside_ranges from 210 captured calls, 1 distinct problems
; problem construct.inside_ranges#0 calls=210
; get-value a b
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 32))
(declare-fun b () (_ BitVec 8))
(assert (= #b1 (bvor (bvor (bvand (__Vbv (bvuge a #x00001000)) (__Vbv (bvule a #x00001fff))) (bvand (__Vbv (bvuge a #x80000000)) (__Vbv (bvule a #x8000ffff)))) (__Vbv (= a #xffff0000)))))
(assert (= #b1 (bvor (bvor (bvand (__Vbv (bvuge ((_ zero_extend 24) b) #x00000000)) (__Vbv (bvule ((_ zero_extend 24) b) #x00000009))) (bvand (__Vbv (bvuge ((_ zero_extend 24) b) #x00000064)) (__Vbv (bvule ((_ zero_extend 24) b) #x0000006d)))) (bvand (__Vbv (bvuge ((_ zero_extend 24) b) #x000000fa)) (__Vbv (bvule ((_ zero_extend 24) b) #x000000ff))))))
(assert (= #b1 (__Vbv (not (__Vbool (bvand (__Vbv (bvuge ((_ zero_extend 24) b) #x00000005)) (__Vbv (bvule ((_ zero_extend 24) b) #x00000006))))))))
; end
