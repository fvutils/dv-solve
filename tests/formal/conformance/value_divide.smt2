; get-value on divide terms, every value checked against z3
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
(get-value ((bvudiv a b) (bvurem a b) (bvudiv a c) (bvurem a c) (bvsdiv a b) (bvsrem a b) (bvsmod a b) (bvsdiv a c) (bvsrem a c) (bvsmod a c)))
