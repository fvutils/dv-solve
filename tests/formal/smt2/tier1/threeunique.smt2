(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const a (_ BitVec 7))
(declare-const b (_ BitVec 7))
(declare-const c (_ BitVec 7))

(assert (bvuge a (_ bv0 7)))
(assert (bvule a (_ bv100 7)))
(assert (bvuge b (_ bv0 7)))
(assert (bvule b (_ bv100 7)))
(assert (bvuge c (_ bv0 7)))
(assert (bvule c (_ bv100 7)))

(assert (distinct a b))  ; c_all_different
(assert (distinct b c))  ; c_all_different
(assert (distinct c a))  ; c_all_different

(check-sat)
(get-value (a b c))
