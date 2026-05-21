(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const base_addr (_ BitVec 10))
(declare-const aligned (_ BitVec 10))

(assert (bvuge base_addr (_ bv0 10)))
(assert (bvule base_addr (_ bv1023 10)))
(assert (bvuge aligned (_ bv0 10)))
(assert (bvule aligned (_ bv1023 10)))

(assert (= ((_ zero_extend 1) aligned) (bvmul ((_ zero_extend 1) (bvudiv base_addr (_ bv4 10))) (_ bv4 11))))  ; c_aligned

(check-sat)
(get-value (base_addr aligned))
