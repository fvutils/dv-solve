(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const x (_ BitVec 7))

(assert (bvuge x (_ bv0 7)))
(assert (bvule x (_ bv99 7)))


(check-sat)
(get-value (x))
