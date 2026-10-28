; corpus for construct.range from 210 captured calls, 1 distinct problems
; problem construct.range#0 calls=210
; get-value a b c d
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun a () (_ BitVec 8))
(declare-fun b () (_ BitVec 16))
(declare-fun c () (_ BitVec 32))
(declare-fun d () (_ BitVec 32))
(assert (= #b1 (bvand (__Vbv (bvuge ((_ zero_extend 24) a) #x0000000a)) (__Vbv (bvule ((_ zero_extend 24) a) #x000000c8)))))
(assert (= #b1 (__Vbv (bvugt b ((_ zero_extend 8) a)))))
(assert (= #b1 (__Vbv (bvult b #x03e8))))
(assert (= #b1 (__Vbv (bvuge c #x000186a0))))
(assert (= #b1 (__Vbv (bvule c #x00030d40))))
(assert (= #b1 (__Vbv (not (= c #x000249f0)))))
(assert (= #b1 (__Vbv (bvsge d #xffffffce))))
(assert (= #b1 (__Vbv (bvsle d #x00000032))))
(assert (= #b1 (__Vbv (not (= d #x00000000)))))
; end
