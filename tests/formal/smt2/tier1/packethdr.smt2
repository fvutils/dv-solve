(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const src_port (_ BitVec 16))
(declare-const dst_port (_ BitVec 16))
(declare-const length (_ BitVec 11))

(assert (bvuge src_port (_ bv0 16)))
(assert (bvule src_port (_ bv65535 16)))
(assert (bvuge dst_port (_ bv0 16)))
(assert (bvule dst_port (_ bv65535 16)))
(assert (bvuge length (_ bv20 11)))
(assert (bvule length (_ bv1500 11)))

(assert (distinct src_port dst_port))  ; c_ports_differ
(assert (bvule ((_ zero_extend 5) length) src_port))  ; c_src_fits
(assert (bvule dst_port ((_ zero_extend 5) length)))  ; c_dst_fits

(check-sat)
(get-value (src_port dst_port length))
