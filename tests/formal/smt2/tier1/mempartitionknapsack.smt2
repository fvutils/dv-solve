(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const s0 (_ BitVec 10))
(declare-const s1 (_ BitVec 10))
(declare-const s2 (_ BitVec 10))
(declare-const s3 (_ BitVec 10))
(declare-const s4 (_ BitVec 10))
(declare-const s5 (_ BitVec 10))
(declare-const s6 (_ BitVec 10))
(declare-const s7 (_ BitVec 10))
(declare-const a0 (_ BitVec 1))
(declare-const a1 (_ BitVec 11))
(declare-const a2 (_ BitVec 11))
(declare-const a3 (_ BitVec 11))
(declare-const a4 (_ BitVec 11))
(declare-const a5 (_ BitVec 11))
(declare-const a6 (_ BitVec 11))
(declare-const a7 (_ BitVec 11))
(declare-const total (_ BitVec 12))

(assert (bvuge s0 (_ bv64 10)))
(assert (bvule s0 (_ bv512 10)))
(assert (bvuge s1 (_ bv64 10)))
(assert (bvule s1 (_ bv512 10)))
(assert (bvuge s2 (_ bv64 10)))
(assert (bvule s2 (_ bv512 10)))
(assert (bvuge s3 (_ bv64 10)))
(assert (bvule s3 (_ bv512 10)))
(assert (bvuge s4 (_ bv64 10)))
(assert (bvule s4 (_ bv512 10)))
(assert (bvuge s5 (_ bv64 10)))
(assert (bvule s5 (_ bv512 10)))
(assert (bvuge s6 (_ bv64 10)))
(assert (bvule s6 (_ bv512 10)))
(assert (bvuge s7 (_ bv64 10)))
(assert (bvule s7 (_ bv512 10)))
(assert (bvuge a0 (_ bv0 1)))
(assert (bvule a0 (_ bv0 1)))
(assert (bvuge a1 (_ bv64 11)))
(assert (bvule a1 (_ bv1984 11)))
(assert (bvuge a2 (_ bv128 11)))
(assert (bvule a2 (_ bv1984 11)))
(assert (bvuge a3 (_ bv192 11)))
(assert (bvule a3 (_ bv1984 11)))
(assert (bvuge a4 (_ bv256 11)))
(assert (bvule a4 (_ bv1984 11)))
(assert (bvuge a5 (_ bv320 11)))
(assert (bvule a5 (_ bv1984 11)))
(assert (bvuge a6 (_ bv384 11)))
(assert (bvule a6 (_ bv1984 11)))
(assert (bvuge a7 (_ bv448 11)))
(assert (bvule a7 (_ bv1984 11)))
(assert (bvuge total (_ bv2048 12)))
(assert (bvule total (_ bv2048 12)))

(assert (or (= s0 (_ bv64 10)) (= s0 (_ bv128 10)) (= s0 (_ bv256 10)) (= s0 (_ bv512 10))))  ; c_s0_vals
(assert (or (= s1 (_ bv64 10)) (= s1 (_ bv128 10)) (= s1 (_ bv256 10)) (= s1 (_ bv512 10))))  ; c_s1_vals
(assert (or (= s2 (_ bv64 10)) (= s2 (_ bv128 10)) (= s2 (_ bv256 10)) (= s2 (_ bv512 10))))  ; c_s2_vals
(assert (or (= s3 (_ bv64 10)) (= s3 (_ bv128 10)) (= s3 (_ bv256 10)) (= s3 (_ bv512 10))))  ; c_s3_vals
(assert (or (= s4 (_ bv64 10)) (= s4 (_ bv128 10)) (= s4 (_ bv256 10)) (= s4 (_ bv512 10))))  ; c_s4_vals
(assert (or (= s5 (_ bv64 10)) (= s5 (_ bv128 10)) (= s5 (_ bv256 10)) (= s5 (_ bv512 10))))  ; c_s5_vals
(assert (or (= s6 (_ bv64 10)) (= s6 (_ bv128 10)) (= s6 (_ bv256 10)) (= s6 (_ bv512 10))))  ; c_s6_vals
(assert (or (= s7 (_ bv64 10)) (= s7 (_ bv128 10)) (= s7 (_ bv256 10)) (= s7 (_ bv512 10))))  ; c_s7_vals
(assert (= ((_ zero_extend 5) total) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) (bvadd ((_ zero_extend 1) s0) ((_ zero_extend 1) s1))) ((_ zero_extend 2) s2))) ((_ zero_extend 3) s3))) ((_ zero_extend 4) s4))) ((_ zero_extend 5) s5))) ((_ zero_extend 6) s6))) ((_ zero_extend 7) s7))))  ; c_sum
(assert (= a0 (_ bv0 1)))  ; c_a0
(assert (= a1 (bvadd ((_ zero_extend 10) a0) ((_ zero_extend 1) s0))))  ; c_a1
(assert (= ((_ zero_extend 1) a2) (bvadd ((_ zero_extend 1) a1) ((_ zero_extend 2) s1))))  ; c_a2
(assert (= ((_ zero_extend 1) a3) (bvadd ((_ zero_extend 1) a2) ((_ zero_extend 2) s2))))  ; c_a3
(assert (= ((_ zero_extend 1) a4) (bvadd ((_ zero_extend 1) a3) ((_ zero_extend 2) s3))))  ; c_a4
(assert (= ((_ zero_extend 1) a5) (bvadd ((_ zero_extend 1) a4) ((_ zero_extend 2) s4))))  ; c_a5
(assert (= ((_ zero_extend 1) a6) (bvadd ((_ zero_extend 1) a5) ((_ zero_extend 2) s5))))  ; c_a6
(assert (= ((_ zero_extend 1) a7) (bvadd ((_ zero_extend 1) a6) ((_ zero_extend 2) s6))))  ; c_a7

(check-sat)
(get-value (s0 s1 s2 s3 s4 s5 s6 s7 a0 a1 a2 a3 a4 a5 a6 a7 total))
