; corpus for control.solve_before from 420 captured calls, 2 distinct problems
; problem control.solve_before#0 calls=210
; get-value s
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun s () (_ BitVec 1))
(declare-fun v () (_ BitVec 8))
(assert (= #b1 (__Vbv (=> (__Vbool s) (__Vbool (__Vbv (= ((_ zero_extend 24) v) #x00000000)))))))
; end
; problem control.solve_before#1 calls=210
; get-value s v
(set-logic QF_ABV)
(define-fun __Vbv ((b Bool)) (_ BitVec 1) (ite b #b1 #b0))
(define-fun __Vbool ((v (_ BitVec 1))) Bool (= #b1 v))
(declare-fun s () (_ BitVec 1))
(declare-fun v () (_ BitVec 8))
(assert (= s #b0))
(assert (= #b1 (__Vbv (=> (__Vbool s) (__Vbool (__Vbv (= ((_ zero_extend 24) v) #x00000000)))))))
; end
