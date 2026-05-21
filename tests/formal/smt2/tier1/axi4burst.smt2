(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const addr_word (_ BitVec 10))
(declare-const addr_page (_ BitVec 4))
(declare-const len_m1 (_ BitVec 8))
(declare-const burst (_ BitVec 2))
(declare-const end_word (_ BitVec 11))

(assert (bvuge addr_word (_ bv0 10)))
(assert (bvule addr_word (_ bv1023 10)))
(assert (bvuge addr_page (_ bv0 4)))
(assert (bvule addr_page (_ bv15 4)))
(assert (bvuge len_m1 (_ bv0 8)))
(assert (bvule len_m1 (_ bv255 8)))
(assert (bvuge burst (_ bv0 2)))
(assert (bvule burst (_ bv2 2)))
(assert (bvuge end_word (_ bv1 11)))
(assert (bvule end_word (_ bv1024 11)))

(assert (= ((_ zero_extend 1) end_word) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) addr_word) ((_ zero_extend 3) len_m1))) (_ bv1 12))))  ; c_end_word
(assert (bvule end_word (_ bv1024 11)))  ; c_no_cross
(assert (or (distinct burst (_ bv2 2)) (= len_m1 (_ bv1 8)) (= len_m1 (_ bv3 8)) (= len_m1 (_ bv7 8)) (= len_m1 (_ bv15 8))))  ; c_wrap_len
(assert (or (distinct burst (_ bv0 2)) (bvule len_m1 (_ bv15 8))))  ; c_fixed_len

(check-sat)
(get-value (addr_word addr_page len_m1 burst end_word))
