(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const cmd (_ BitVec 3))
(declare-const ba (_ BitVec 2))
(declare-const bg (_ BitVec 3))
(declare-const cid (_ BitVec 4))
(declare-const row_hi (_ BitVec 8))
(declare-const row_lo (_ BitVec 10))
(declare-const col (_ BitVec 9))
(declare-const ap (_ BitVec 1))

(assert (bvuge cmd (_ bv0 3)))
(assert (bvule cmd (_ bv4 3)))
(assert (bvuge ba (_ bv0 2)))
(assert (bvule ba (_ bv3 2)))
(assert (bvuge bg (_ bv0 3)))
(assert (bvule bg (_ bv7 3)))
(assert (bvuge cid (_ bv0 4)))
(assert (bvule cid (_ bv15 4)))
(assert (bvuge row_hi (_ bv0 8)))
(assert (bvule row_hi (_ bv255 8)))
(assert (bvuge row_lo (_ bv0 10)))
(assert (bvule row_lo (_ bv1023 10)))
(assert (bvuge col (_ bv0 9)))
(assert (bvule col (_ bv511 9)))
(assert (bvuge ap (_ bv0 1)))
(assert (bvule ap (_ bv1 1)))

(assert (or (distinct cmd (_ bv0 3)) (bvugt (bvadd ((_ zero_extend 3) row_hi) ((_ zero_extend 1) row_lo)) (_ bv0 11))))  ; c_act_row
(assert (or (distinct cmd (_ bv1 3)) (bvugt row_hi (_ bv0 8))))  ; c_rd_row
(assert (or (= cmd (_ bv1 3)) (= col (_ bv0 9))))  ; c_rd_col
(assert (or (= cmd (_ bv1 3)) (= ap (_ bv0 1))))  ; c_rd_ap

(check-sat)
(get-value (cmd ba bg cid row_hi row_lo col ap))
