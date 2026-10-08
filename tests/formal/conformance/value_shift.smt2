; get-value on shift terms, every value checked against z3
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
(get-value ((bvshl a #x03) (bvlshr a #x03) (bvashr a #x03) (bvshl a #x09) (bvlshr a #xff) (bvashr a #x20) ((_ rotate_left 3) a) ((_ rotate_right 11) a)))
