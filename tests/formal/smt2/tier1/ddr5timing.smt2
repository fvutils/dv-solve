(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const cmd_prev (_ BitVec 3))
(declare-const cmd (_ BitVec 3))
(declare-const ap_prev (_ BitVec 1))
(declare-const cancel_prev (_ BitVec 1))
(declare-const max_tccd (_ BitVec 4))
(declare-const tccd (_ BitVec 6))
(declare-const tccd_base (_ BitVec 6))

(assert (bvuge cmd_prev (_ bv0 3)))
(assert (bvule cmd_prev (_ bv4 3)))
(assert (bvuge cmd (_ bv0 3)))
(assert (bvule cmd (_ bv4 3)))
(assert (bvuge ap_prev (_ bv0 1)))
(assert (bvule ap_prev (_ bv1 1)))
(assert (bvuge cancel_prev (_ bv0 1)))
(assert (bvule cancel_prev (_ bv1 1)))
(assert (bvuge max_tccd (_ bv0 4)))
(assert (bvule max_tccd (_ bv8 4)))
(assert (bvuge tccd (_ bv2 6)))
(assert (bvule tccd (_ bv50 6)))
(assert (bvuge tccd_base (_ bv2 6)))
(assert (bvule tccd_base (_ bv50 6)))

(assert (= ((_ zero_extend 1) tccd) (bvadd ((_ zero_extend 1) tccd_base) ((_ zero_extend 3) max_tccd))))  ; c_tccd_sum
(assert (or (distinct cmd_prev (_ bv2 3)) (distinct cmd (_ bv2 3)) (bvuge tccd_base (_ bv8 6))))  ; c_mrw_mrw
(assert (or (distinct cmd_prev (_ bv2 3)) (= cmd (_ bv2 3)) (bvuge tccd_base (_ bv16 6))))  ; c_mrw_other
(assert (or (distinct cmd_prev (_ bv0 3)) (distinct cmd (_ bv0 3)) (bvuge tccd_base (_ bv8 6))))  ; c_act_act
(assert (or (distinct cmd_prev (_ bv0 3)) (= cmd (_ bv0 3)) (bvuge tccd_base (_ bv24 6))))  ; c_act_other
(assert (or (distinct cmd_prev (_ bv1 3)) (distinct cmd (_ bv1 3)) (bvuge tccd_base (_ bv8 6))))  ; c_rd_rd
(assert (or (distinct cmd_prev (_ bv1 3)) (distinct cmd (_ bv4 3)) (bvuge tccd_base (_ bv12 6))))  ; c_rd_pre
(assert (or (distinct cmd_prev (_ bv4 3)) (distinct cmd (_ bv4 3)) (bvuge tccd_base (_ bv2 6))))  ; c_pre_pre
(assert (or (distinct cmd_prev (_ bv4 3)) (= cmd (_ bv4 3)) (bvuge tccd_base (_ bv24 6))))  ; c_pre_other

(check-sat)
(get-value (cmd_prev cmd ap_prev cancel_prev max_tccd tccd tccd_base))
