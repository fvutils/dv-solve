(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const op (_ BitVec 2))
(declare-const addr (_ BitVec 12))
(declare-const wdata (_ BitVec 8))
(declare-const rdata_exp (_ BitVec 8))
(declare-const cfg_reg (_ BitVec 4))
(declare-const cfg_val (_ BitVec 6))
(declare-const status (_ BitVec 2))

(assert (bvuge op (_ bv0 2)))
(assert (bvule op (_ bv2 2)))
(assert (bvuge addr (_ bv0 12)))
(assert (bvule addr (_ bv4095 12)))
(assert (bvuge wdata (_ bv0 8)))
(assert (bvule wdata (_ bv255 8)))
(assert (bvuge rdata_exp (_ bv0 8)))
(assert (bvule rdata_exp (_ bv255 8)))
(assert (bvuge cfg_reg (_ bv0 4)))
(assert (bvule cfg_reg (_ bv15 4)))
(assert (bvuge cfg_val (_ bv0 6)))
(assert (bvule cfg_val (_ bv63 6)))
(assert (bvuge status (_ bv0 2)))
(assert (bvule status (_ bv3 2)))

(assert (or (distinct op (_ bv0 2)) (= wdata (_ bv0 8))))  ; c_read
(assert (or (distinct op (_ bv0 2)) (= cfg_reg (_ bv0 4))))  ; c_read
(assert (or (distinct op (_ bv0 2)) (= cfg_val (_ bv0 6))))  ; c_read
(assert (or (distinct op (_ bv1 2)) (= rdata_exp (_ bv0 8))))  ; c_write
(assert (or (distinct op (_ bv1 2)) (= cfg_reg (_ bv0 4))))  ; c_write
(assert (or (distinct op (_ bv1 2)) (= cfg_val (_ bv0 6))))  ; c_write
(assert (or (distinct op (_ bv2 2)) (= addr (_ bv0 12))))  ; c_config
(assert (or (distinct op (_ bv2 2)) (= wdata (_ bv0 8))))  ; c_config
(assert (or (distinct op (_ bv2 2)) (= rdata_exp (_ bv0 8))))  ; c_config
(assert (bvugt (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) addr) ((_ zero_extend 5) wdata))) ((_ zero_extend 6) rdata_exp))) ((_ zero_extend 11) cfg_reg))) ((_ zero_extend 10) cfg_val)) (_ bv0 16)))  ; c_not_empty

(check-sat)
(get-value (op addr wdata rdata_exp cfg_reg cfg_val status))
