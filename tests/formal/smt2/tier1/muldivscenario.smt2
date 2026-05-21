(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const unit_price (_ BitVec 14))
(declare-const quantity (_ BitVec 7))
(declare-const total (_ BitVec 20))

(assert (bvuge unit_price (_ bv100 14)))
(assert (bvule unit_price (_ bv10000 14)))
(assert (bvuge quantity (_ bv1 7)))
(assert (bvule quantity (_ bv100 7)))
(assert (bvuge total (_ bv100 20)))
(assert (bvule total (_ bv1000000 20)))

(assert (= total ((_ zero_extend 5) (bvmul ((_ zero_extend 1) unit_price) ((_ zero_extend 8) quantity)))))  ; c_total

(check-sat)
(get-value (unit_price quantity total))
