(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const value (_ BitVec 8))
(declare-const count (_ BitVec 1))

(assert (bvuge value (_ bv0 8)))
(assert (bvule value (_ bv255 8)))
(assert (bvuge count (_ bv1 1)))
(assert (bvule count (_ bv1 1)))

(assert (bvugt value (_ bv0 8)))  ; c_onehot

(check-sat)
(get-value (value count))
