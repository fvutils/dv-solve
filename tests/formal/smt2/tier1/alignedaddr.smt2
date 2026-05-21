(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const addr (_ BitVec 16))
(declare-const mask (_ BitVec 4))
(declare-const masked (_ BitVec 4))

(assert (bvuge addr (_ bv0 16)))
(assert (bvule addr (_ bv65535 16)))
(assert (bvuge mask (_ bv15 4)))
(assert (bvule mask (_ bv15 4)))
(assert (bvuge masked (_ bv0 4)))
(assert (bvule masked (_ bv15 4)))

(assert (= ((_ zero_extend 12) masked) (bvurem addr (_ bv16 16))))  ; c_mask_result
(assert (= masked (_ bv0 4)))  ; c_aligned

(check-sat)
(get-value (addr mask masked))
