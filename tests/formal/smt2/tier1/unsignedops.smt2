(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const addr (_ BitVec 32))
(declare-const offset (_ BitVec 4))
(declare-const result (_ BitVec 32))

(assert (bvuge addr (_ bv2147483648 32)))
(assert (bvule addr (_ bv4294967280 32)))
(assert (bvuge offset (_ bv0 4)))
(assert (bvule offset (_ bv15 4)))
(assert (bvuge result (_ bv2147483648 32)))
(assert (bvule result (_ bv4294967295 32)))

(assert (= ((_ zero_extend 1) result) (bvadd ((_ zero_extend 1) addr) ((_ zero_extend 29) offset))))  ; c_sum

(check-sat)
(get-value (addr offset result))
