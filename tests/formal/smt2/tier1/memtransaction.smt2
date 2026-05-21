(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const addr (_ BitVec 8))
(declare-const length (_ BitVec 5))
(declare-const end_addr (_ BitVec 9))

(assert (bvuge addr (_ bv0 8)))
(assert (bvule addr (_ bv252 8)))
(assert (bvuge length (_ bv1 5)))
(assert (bvule length (_ bv16 5)))
(assert (bvuge end_addr (_ bv1 9)))
(assert (bvule end_addr (_ bv268 9)))

(assert (= end_addr (bvadd ((_ zero_extend 1) addr) ((_ zero_extend 4) length))))  ; c_end
(assert (bvule end_addr (_ bv256 9)))  ; c_no_overflow

(check-sat)
(get-value (addr length end_addr))
