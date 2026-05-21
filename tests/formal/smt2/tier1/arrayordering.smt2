(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const v0 (_ BitVec 7))
(declare-const v1 (_ BitVec 7))
(declare-const v2 (_ BitVec 7))
(declare-const v3 (_ BitVec 7))
(declare-const v4 (_ BitVec 7))
(declare-const v5 (_ BitVec 7))
(declare-const v6 (_ BitVec 7))
(declare-const v7 (_ BitVec 7))
(declare-const v8 (_ BitVec 7))
(declare-const v9 (_ BitVec 7))
(declare-const q0 (_ BitVec 5))
(declare-const q1 (_ BitVec 5))
(declare-const q2 (_ BitVec 5))
(declare-const q3 (_ BitVec 5))
(declare-const q4 (_ BitVec 5))
(declare-const q5 (_ BitVec 5))
(declare-const q6 (_ BitVec 5))
(declare-const q7 (_ BitVec 5))
(declare-const q8 (_ BitVec 5))
(declare-const q9 (_ BitVec 5))

(assert (bvuge v0 (_ bv50 7)))
(assert (bvule v0 (_ bv100 7)))
(assert (bvuge v1 (_ bv50 7)))
(assert (bvule v1 (_ bv100 7)))
(assert (bvuge v2 (_ bv50 7)))
(assert (bvule v2 (_ bv100 7)))
(assert (bvuge v3 (_ bv50 7)))
(assert (bvule v3 (_ bv100 7)))
(assert (bvuge v4 (_ bv50 7)))
(assert (bvule v4 (_ bv100 7)))
(assert (bvuge v5 (_ bv50 7)))
(assert (bvule v5 (_ bv100 7)))
(assert (bvuge v6 (_ bv50 7)))
(assert (bvule v6 (_ bv100 7)))
(assert (bvuge v7 (_ bv50 7)))
(assert (bvule v7 (_ bv100 7)))
(assert (bvuge v8 (_ bv50 7)))
(assert (bvule v8 (_ bv100 7)))
(assert (bvuge v9 (_ bv50 7)))
(assert (bvule v9 (_ bv100 7)))
(assert (bvuge q0 (_ bv10 5)))
(assert (bvule q0 (_ bv20 5)))
(assert (bvuge q1 (_ bv10 5)))
(assert (bvule q1 (_ bv20 5)))
(assert (bvuge q2 (_ bv10 5)))
(assert (bvule q2 (_ bv20 5)))
(assert (bvuge q3 (_ bv10 5)))
(assert (bvule q3 (_ bv20 5)))
(assert (bvuge q4 (_ bv10 5)))
(assert (bvule q4 (_ bv20 5)))
(assert (bvuge q5 (_ bv10 5)))
(assert (bvule q5 (_ bv20 5)))
(assert (bvuge q6 (_ bv10 5)))
(assert (bvule q6 (_ bv20 5)))
(assert (bvuge q7 (_ bv10 5)))
(assert (bvule q7 (_ bv20 5)))
(assert (bvuge q8 (_ bv10 5)))
(assert (bvule q8 (_ bv20 5)))
(assert (bvuge q9 (_ bv10 5)))
(assert (bvule q9 (_ bv20 5)))

(assert (= v0 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q0)))))  ; c_div5
(assert (= v1 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q1)))))  ; c_div5
(assert (= v2 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q2)))))  ; c_div5
(assert (= v3 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q3)))))  ; c_div5
(assert (= v4 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q4)))))  ; c_div5
(assert (= v5 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q5)))))  ; c_div5
(assert (= v6 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q6)))))  ; c_div5
(assert (= v7 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q7)))))  ; c_div5
(assert (= v8 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q8)))))  ; c_div5
(assert (= v9 ((_ zero_extend 1) (bvmul (_ bv5 6) ((_ zero_extend 1) q9)))))  ; c_div5
(assert (bvult v0 v1))  ; c_ascending
(assert (bvult v1 v2))  ; c_ascending
(assert (bvult v2 v3))  ; c_ascending
(assert (bvult v3 v4))  ; c_ascending
(assert (bvugt v5 v6))  ; c_descending
(assert (bvugt v6 v7))  ; c_descending
(assert (bvugt v7 v8))  ; c_descending
(assert (bvugt v8 v9))  ; c_descending

(check-sat)
(get-value (v0 v1 v2 v3 v4 v5 v6 v7 v8 v9 q0 q1 q2 q3 q4 q5 q6 q7 q8 q9))
