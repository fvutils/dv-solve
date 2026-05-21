(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const word_addr (_ BitVec 16))
(declare-const burst_len (_ BitVec 8))
(declare-const beat_size (_ BitVec 2))
(declare-const end_addr (_ BitVec 17))

(assert (bvuge word_addr (_ bv0 16)))
(assert (bvule word_addr (_ bv65535 16)))
(assert (bvuge burst_len (_ bv0 8)))
(assert (bvule burst_len (_ bv255 8)))
(assert (bvuge beat_size (_ bv0 2)))
(assert (bvule beat_size (_ bv3 2)))
(assert (bvuge end_addr (_ bv0 17)))
(assert (bvule end_addr (_ bv65790 17)))

(assert (= end_addr (bvadd ((_ zero_extend 1) word_addr) ((_ zero_extend 9) burst_len))))  ; c_end_addr
(assert (bvule end_addr (_ bv65535 17)))  ; c_in_range
(assert (bvule ((_ zero_extend 6) beat_size) burst_len))  ; c_beat_fits

(check-sat)
(get-value (word_addr burst_len beat_size end_addr))
