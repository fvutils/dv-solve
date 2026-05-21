(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const a (_ BitVec 7))
(declare-const b (_ BitVec 7))

(assert (bvuge a (_ bv0 7)))
(assert (bvule a (_ bv100 7)))
(assert (bvuge b (_ bv0 7)))
(assert (bvule b (_ bv100 7)))

(assert (or (bvuge a (_ bv20 7)) (bvuge b (_ bv10 7))))  ; c_lo
(assert (or (bvuge a (_ bv20 7)) (bvule b (_ bv30 7))))  ; c_lo
(assert (or (bvule a (_ bv50 7)) (bvuge b (_ bv30 7))))  ; c_hi
(assert (or (bvule a (_ bv50 7)) (bvule b (_ bv50 7))))  ; c_hi

(check-sat)
(get-value (a b))
