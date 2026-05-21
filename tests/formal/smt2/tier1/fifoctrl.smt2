(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const wr_ptr (_ BitVec 8))
(declare-const rd_ptr (_ BitVec 8))
(declare-const watermark_lo (_ BitVec 6))
(declare-const watermark_hi (_ BitVec 8))

(assert (bvuge wr_ptr (_ bv0 8)))
(assert (bvule wr_ptr (_ bv255 8)))
(assert (bvuge rd_ptr (_ bv0 8)))
(assert (bvule rd_ptr (_ bv255 8)))
(assert (bvuge watermark_lo (_ bv0 6)))
(assert (bvule watermark_lo (_ bv63 6)))
(assert (bvuge watermark_hi (_ bv192 8)))
(assert (bvule watermark_hi (_ bv255 8)))

(assert (distinct wr_ptr rd_ptr))  ; c_ptrs_differ
(assert (bvult ((_ zero_extend 2) watermark_lo) watermark_hi))  ; c_watermark

(check-sat)
(get-value (wr_ptr rd_ptr watermark_lo watermark_hi))
