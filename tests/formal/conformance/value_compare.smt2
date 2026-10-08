; get-value on compare terms, every value checked against z3
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
(get-value ((bvult a b) (bvule a a) (bvugt a b) (bvuge b a) (bvslt a b) (bvsle a b) (bvsgt a b) (bvsge a b) (= a b) (distinct a b c) (bvcomp a b)))
