(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const tlp_type (_ BitVec 2))
(declare-const length (_ BitVec 5))
(declare-const address (_ BitVec 16))
(declare-const first_be (_ BitVec 4))
(declare-const last_be (_ BitVec 4))
(declare-const tag (_ BitVec 8))
(declare-const fmt (_ BitVec 2))

(assert (bvuge tlp_type (_ bv0 2)))
(assert (bvule tlp_type (_ bv3 2)))
(assert (bvuge length (_ bv1 5)))
(assert (bvule length (_ bv16 5)))
(assert (bvuge address (_ bv0 16)))
(assert (bvule address (_ bv65535 16)))
(assert (bvuge first_be (_ bv1 4)))
(assert (bvule first_be (_ bv15 4)))
(assert (bvuge last_be (_ bv0 4)))
(assert (bvule last_be (_ bv15 4)))
(assert (bvuge tag (_ bv0 8)))
(assert (bvule tag (_ bv255 8)))
(assert (bvuge fmt (_ bv0 2)))
(assert (bvule fmt (_ bv3 2)))

(assert (or (bvuge tlp_type (_ bv2 2)) (= (bvurem address (_ bv4 16)) (_ bv0 16))))  ; c_addr_align
(assert (or (bvule tlp_type (_ bv1 2)) (bvule address (_ bv1023 16))))  ; c_cfg_range
(assert (or (distinct length (_ bv1 5)) (= last_be (_ bv0 4))))  ; c_be_single
(assert (or (bvule length (_ bv1 5)) (bvuge last_be (_ bv1 4))))  ; c_be_multi
(assert (= fmt tlp_type))  ; c_fmt_type

(check-sat)
(get-value (tlp_type length address first_be last_be tag fmt))
