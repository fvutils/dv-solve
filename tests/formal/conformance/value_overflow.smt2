; get-value on overflow terms, every value checked against z3
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
(check-sat)
(get-value ((bvuaddo a b) (bvsaddo a b) (bvumulo a b) (bvsmulo a b) (bvusubo b a) (bvssubo a b) (bvnego a) (bvsdivo a b)))
