; get-value on structure terms, every value checked against z3
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
(get-value (((_ extract 7 4) a) ((_ extract 0 0) a) (concat a b) ((_ zero_extend 4) a) ((_ sign_extend 4) a) ((_ repeat 3) a) (bvredor a) (bvredand a)))
