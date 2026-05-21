(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const p0 (_ BitVec 7))
(declare-const p1 (_ BitVec 7))
(declare-const p2 (_ BitVec 7))
(declare-const p3 (_ BitVec 7))
(declare-const p4 (_ BitVec 7))
(declare-const p5 (_ BitVec 7))
(declare-const p6 (_ BitVec 7))
(declare-const s01 (_ BitVec 8))
(declare-const s012 (_ BitVec 8))
(declare-const s0123 (_ BitVec 9))
(declare-const s01234 (_ BitVec 9))
(declare-const s012345 (_ BitVec 9))

(assert (bvuge p0 (_ bv10 7)))
(assert (bvule p0 (_ bv80 7)))
(assert (bvuge p1 (_ bv10 7)))
(assert (bvule p1 (_ bv80 7)))
(assert (bvuge p2 (_ bv10 7)))
(assert (bvule p2 (_ bv80 7)))
(assert (bvuge p3 (_ bv10 7)))
(assert (bvule p3 (_ bv80 7)))
(assert (bvuge p4 (_ bv10 7)))
(assert (bvule p4 (_ bv80 7)))
(assert (bvuge p5 (_ bv10 7)))
(assert (bvule p5 (_ bv80 7)))
(assert (bvuge p6 (_ bv10 7)))
(assert (bvule p6 (_ bv80 7)))
(assert (bvuge s01 (_ bv20 8)))
(assert (bvule s01 (_ bv160 8)))
(assert (bvuge s012 (_ bv30 8)))
(assert (bvule s012 (_ bv240 8)))
(assert (bvuge s0123 (_ bv40 9)))
(assert (bvule s0123 (_ bv320 9)))
(assert (bvuge s01234 (_ bv50 9)))
(assert (bvule s01234 (_ bv400 9)))
(assert (bvuge s012345 (_ bv60 9)))
(assert (bvule s012345 (_ bv480 9)))

(assert (= s01 (bvadd ((_ zero_extend 1) p0) ((_ zero_extend 1) p1))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s012) (bvadd ((_ zero_extend 1) s01) ((_ zero_extend 2) p2))))  ; c_sum_chain
(assert (= s0123 (bvadd ((_ zero_extend 1) s012) ((_ zero_extend 2) p3))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s01234) (bvadd ((_ zero_extend 1) s0123) ((_ zero_extend 3) p4))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s012345) (bvadd ((_ zero_extend 1) s01234) ((_ zero_extend 3) p5))))  ; c_sum_chain
(assert (= (bvadd ((_ zero_extend 1) s012345) ((_ zero_extend 3) p6)) (_ bv200 10)))  ; c_total
(assert (bvule p0 p1))  ; c_ordered
(assert (bvule p1 p2))  ; c_ordered
(assert (bvule p2 p3))  ; c_ordered
(assert (bvule p3 p4))  ; c_ordered
(assert (bvule p4 p5))  ; c_ordered
(assert (bvule p5 p6))  ; c_ordered

(check-sat)
(get-value (p0 p1 p2 p3 p4 p5 p6 s01 s012 s0123 s01234 s012345))
