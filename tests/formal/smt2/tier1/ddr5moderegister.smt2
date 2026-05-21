(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const mra (_ BitVec 2))
(declare-const op_burst_len (_ BitVec 2))
(declare-const op_cas_lat (_ BitVec 5))
(declare-const op_rfu_7 (_ BitVec 1))
(declare-const op_rd_pre (_ BitVec 3))
(declare-const op_rfu_5 (_ BitVec 1))
(declare-const op_crc_en (_ BitVec 1))

(assert (bvuge mra (_ bv0 2)))
(assert (bvule mra (_ bv2 2)))
(assert (bvuge op_burst_len (_ bv0 2)))
(assert (bvule op_burst_len (_ bv3 2)))
(assert (bvuge op_cas_lat (_ bv0 5)))
(assert (bvule op_cas_lat (_ bv31 5)))
(assert (bvuge op_rfu_7 (_ bv0 1)))
(assert (bvule op_rfu_7 (_ bv1 1)))
(assert (bvuge op_rd_pre (_ bv0 3)))
(assert (bvule op_rd_pre (_ bv7 3)))
(assert (bvuge op_rfu_5 (_ bv0 1)))
(assert (bvule op_rfu_5 (_ bv1 1)))
(assert (bvuge op_crc_en (_ bv0 1)))
(assert (bvule op_crc_en (_ bv1 1)))

(assert (or (distinct mra (_ bv0 2)) (bvule op_burst_len (_ bv2 2))))  ; c_mr0_burst
(assert (or (distinct mra (_ bv0 2)) (bvule op_cas_lat (_ bv22 5))))  ; c_mr0_cas
(assert (or (distinct mra (_ bv0 2)) (= op_rfu_7 (_ bv0 1))))  ; c_mr0_rfu
(assert (or (distinct mra (_ bv1 2)) (bvule op_rd_pre (_ bv4 3))))  ; c_mr8_pre
(assert (or (distinct mra (_ bv1 2)) (= op_rfu_5 (_ bv0 1))))  ; c_mr8_rfu
(assert (or (= mra (_ bv0 2)) (= op_burst_len (_ bv0 2))))  ; c_zero_mr0_fields
(assert (or (= mra (_ bv0 2)) (= op_cas_lat (_ bv0 5))))  ; c_zero_mr0_fields
(assert (or (= mra (_ bv0 2)) (= op_rfu_7 (_ bv0 1))))  ; c_zero_mr0_fields
(assert (or (= mra (_ bv1 2)) (= op_rd_pre (_ bv0 3))))  ; c_zero_mr8_fields
(assert (or (= mra (_ bv1 2)) (= op_rfu_5 (_ bv0 1))))  ; c_zero_mr8_fields
(assert (or (= mra (_ bv2 2)) (= op_crc_en (_ bv0 1))))  ; c_zero_mr50_fields

(check-sat)
(get-value (mra op_burst_len op_cas_lat op_rfu_7 op_rd_pre op_rfu_5 op_crc_en))
