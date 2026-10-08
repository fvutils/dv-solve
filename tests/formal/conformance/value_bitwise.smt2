; get-value on bitwise terms, every value checked against z3
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
(get-value ((bvand a b) (bvor a b) (bvxor a b) (bvnot a) (bvnand a b) (bvnor a b) (bvxnor a b) (bvand a b #x0f) (bvor a b c)))
