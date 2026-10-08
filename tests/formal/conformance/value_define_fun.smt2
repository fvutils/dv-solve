; get-value on define-fun applications
; expect: sat values
; exit: 0
; values: z3
(set-logic QF_BV)
(declare-const a (_ BitVec 8))
(declare-const b (_ BitVec 8))
(declare-const c (_ BitVec 8))
(declare-const p Bool)
(assert (= a #xb5))
(assert (= b #x2c))
(assert (= c #x00))
(assert p)
(define-fun f ((u (_ BitVec 8)) (v (_ BitVec 8))) (_ BitVec 8) (bvadd (bvmul u #x03) v))
(define-fun g ((u (_ BitVec 8))) Bool (bvult u #x80))
(check-sat)
(get-value ((f a b) (f (f a b) c) (g a) (g (f b b))))
