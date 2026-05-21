(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const x (_ BitVec 8))

(assert (bvuge x (_ bv0 8)))
(assert (bvule x (_ bv255 8)))

(assert (bvuge x (_ bv100 8)))  ; c_hard

(check-sat)
(get-value (x))
