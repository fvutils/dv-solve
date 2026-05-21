(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const a (_ BitVec 8))
(declare-const b (_ BitVec 8))
(declare-const sum_ab (_ BitVec 9))
(declare-const masked (_ BitVec 4))

(assert (bvuge a (_ bv10 8)))
(assert (bvule a (_ bv200 8)))
(assert (bvuge b (_ bv10 8)))
(assert (bvule b (_ bv200 8)))
(assert (bvuge sum_ab (_ bv20 9)))
(assert (bvule sum_ab (_ bv400 9)))
(assert (bvuge masked (_ bv0 4)))
(assert (bvule masked (_ bv15 4)))

(assert (= sum_ab (bvadd ((_ zero_extend 1) a) ((_ zero_extend 1) b))))  ; c_sum
(assert (bvult a b))  ; c_order
(assert (= ((_ zero_extend 4) masked) (bvand a (_ bv15 8))))  ; c_masked
(assert (bvule masked (_ bv5 4)))  ; c_masked_bound

(check-sat)
(get-value (a b sum_ab masked))
