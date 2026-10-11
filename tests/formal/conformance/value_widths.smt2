; get-value at widths 1, 7, 64, 65, 128, 129
; expect: sat values
; exit: 0
; values: z3
(set-logic QF_BV)
(declare-const a (_ BitVec 1))
(declare-const b (_ BitVec 7))
(declare-const c (_ BitVec 64))
(declare-const d (_ BitVec 65))
(declare-const e (_ BitVec 128))
(declare-const f (_ BitVec 129))
(assert (= a #b1))
(assert (= b #b1010101))
(assert (= c #xfedcba9876543210))
(assert (= d ((_ zero_extend 1) c)))
(assert (= e (concat c (bvnot c))))
(assert (= f (concat a e)))
(check-sat)
(get-value (a b c d e f (bvadd d d) (bvmul e #x0000000000000000000000000000000f) (bvlshr f (_ bv1 129))))
