(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const q0 (_ BitVec 3))
(declare-const q1 (_ BitVec 3))
(declare-const q2 (_ BitVec 3))
(declare-const q3 (_ BitVec 3))
(declare-const q4 (_ BitVec 3))
(declare-const q5 (_ BitVec 3))
(declare-const q6 (_ BitVec 3))
(declare-const q7 (_ BitVec 3))
(declare-const dp0 (_ BitVec 4))
(declare-const dp1 (_ BitVec 4))
(declare-const dp2 (_ BitVec 4))
(declare-const dp3 (_ BitVec 4))
(declare-const dp4 (_ BitVec 4))
(declare-const dp5 (_ BitVec 4))
(declare-const dp6 (_ BitVec 4))
(declare-const dp7 (_ BitVec 4))
(declare-const dm0 (_ BitVec 4))
(declare-const dm1 (_ BitVec 4))
(declare-const dm2 (_ BitVec 4))
(declare-const dm3 (_ BitVec 4))
(declare-const dm4 (_ BitVec 4))
(declare-const dm5 (_ BitVec 4))
(declare-const dm6 (_ BitVec 4))
(declare-const dm7 (_ BitVec 4))

(assert (bvuge q0 (_ bv0 3)))
(assert (bvule q0 (_ bv7 3)))
(assert (bvuge q1 (_ bv0 3)))
(assert (bvule q1 (_ bv7 3)))
(assert (bvuge q2 (_ bv0 3)))
(assert (bvule q2 (_ bv7 3)))
(assert (bvuge q3 (_ bv0 3)))
(assert (bvule q3 (_ bv7 3)))
(assert (bvuge q4 (_ bv0 3)))
(assert (bvule q4 (_ bv7 3)))
(assert (bvuge q5 (_ bv0 3)))
(assert (bvule q5 (_ bv7 3)))
(assert (bvuge q6 (_ bv0 3)))
(assert (bvule q6 (_ bv7 3)))
(assert (bvuge q7 (_ bv0 3)))
(assert (bvule q7 (_ bv7 3)))
(assert (bvuge dp0 (_ bv0 4)))
(assert (bvule dp0 (_ bv14 4)))
(assert (bvuge dp1 (_ bv0 4)))
(assert (bvule dp1 (_ bv14 4)))
(assert (bvuge dp2 (_ bv0 4)))
(assert (bvule dp2 (_ bv14 4)))
(assert (bvuge dp3 (_ bv0 4)))
(assert (bvule dp3 (_ bv14 4)))
(assert (bvuge dp4 (_ bv0 4)))
(assert (bvule dp4 (_ bv14 4)))
(assert (bvuge dp5 (_ bv0 4)))
(assert (bvule dp5 (_ bv14 4)))
(assert (bvuge dp6 (_ bv0 4)))
(assert (bvule dp6 (_ bv14 4)))
(assert (bvuge dp7 (_ bv0 4)))
(assert (bvule dp7 (_ bv14 4)))
(assert (bvuge dm0 (_ bv0 4)))
(assert (bvule dm0 (_ bv14 4)))
(assert (bvuge dm1 (_ bv0 4)))
(assert (bvule dm1 (_ bv14 4)))
(assert (bvuge dm2 (_ bv0 4)))
(assert (bvule dm2 (_ bv14 4)))
(assert (bvuge dm3 (_ bv0 4)))
(assert (bvule dm3 (_ bv14 4)))
(assert (bvuge dm4 (_ bv0 4)))
(assert (bvule dm4 (_ bv14 4)))
(assert (bvuge dm5 (_ bv0 4)))
(assert (bvule dm5 (_ bv14 4)))
(assert (bvuge dm6 (_ bv0 4)))
(assert (bvule dm6 (_ bv14 4)))
(assert (bvuge dm7 (_ bv0 4)))
(assert (bvule dm7 (_ bv14 4)))

(assert (= dp0 (bvadd ((_ zero_extend 1) q0) (_ bv0 4))))  ; c_dp
(assert (= dp1 (bvadd ((_ zero_extend 1) q1) (_ bv1 4))))  ; c_dp
(assert (= dp2 (bvadd ((_ zero_extend 1) q2) (_ bv2 4))))  ; c_dp
(assert (= dp3 (bvadd ((_ zero_extend 1) q3) (_ bv3 4))))  ; c_dp
(assert (= dp4 (bvadd ((_ zero_extend 1) q4) (_ bv4 4))))  ; c_dp
(assert (= dp5 (bvadd ((_ zero_extend 1) q5) (_ bv5 4))))  ; c_dp
(assert (= dp6 (bvadd ((_ zero_extend 1) q6) (_ bv6 4))))  ; c_dp
(assert (= dp7 (bvadd ((_ zero_extend 1) q7) (_ bv7 4))))  ; c_dp
(assert (= dm0 (bvadd ((_ zero_extend 1) q0) (_ bv7 4))))  ; c_dm
(assert (= dm1 (bvadd ((_ zero_extend 1) q1) (_ bv6 4))))  ; c_dm
(assert (= dm2 (bvadd ((_ zero_extend 1) q2) (_ bv5 4))))  ; c_dm
(assert (= dm3 (bvadd ((_ zero_extend 1) q3) (_ bv4 4))))  ; c_dm
(assert (= dm4 (bvadd ((_ zero_extend 1) q4) (_ bv3 4))))  ; c_dm
(assert (= dm5 (bvadd ((_ zero_extend 1) q5) (_ bv2 4))))  ; c_dm
(assert (= dm6 (bvadd ((_ zero_extend 1) q6) (_ bv1 4))))  ; c_dm
(assert (= dm7 (bvadd ((_ zero_extend 1) q7) (_ bv0 4))))  ; c_dm
(assert (distinct q0 q1))  ; c_col_0
(assert (distinct q0 q2))  ; c_col_0
(assert (distinct q0 q3))  ; c_col_0
(assert (distinct q0 q4))  ; c_col_0
(assert (distinct q0 q5))  ; c_col_0
(assert (distinct q0 q6))  ; c_col_0
(assert (distinct q0 q7))  ; c_col_0
(assert (distinct q1 q2))  ; c_col_1
(assert (distinct q1 q3))  ; c_col_1
(assert (distinct q1 q4))  ; c_col_1
(assert (distinct q1 q5))  ; c_col_1
(assert (distinct q1 q6))  ; c_col_1
(assert (distinct q1 q7))  ; c_col_1
(assert (distinct q2 q3))  ; c_col_2
(assert (distinct q2 q4))  ; c_col_2
(assert (distinct q2 q5))  ; c_col_2
(assert (distinct q2 q6))  ; c_col_2
(assert (distinct q2 q7))  ; c_col_2
(assert (distinct q3 q4))  ; c_col_3
(assert (distinct q3 q5))  ; c_col_3
(assert (distinct q3 q6))  ; c_col_3
(assert (distinct q3 q7))  ; c_col_3
(assert (distinct q4 q5))  ; c_col_4
(assert (distinct q4 q6))  ; c_col_4
(assert (distinct q4 q7))  ; c_col_4
(assert (distinct q5 q6))  ; c_col_5
(assert (distinct q5 q7))  ; c_col_5
(assert (distinct q6 q7))  ; c_col_6
(assert (distinct dp0 dp1))  ; c_dp_0
(assert (distinct dp0 dp2))  ; c_dp_0
(assert (distinct dp0 dp3))  ; c_dp_0
(assert (distinct dp0 dp4))  ; c_dp_0
(assert (distinct dp0 dp5))  ; c_dp_0
(assert (distinct dp0 dp6))  ; c_dp_0
(assert (distinct dp0 dp7))  ; c_dp_0
(assert (distinct dp1 dp2))  ; c_dp_1
(assert (distinct dp1 dp3))  ; c_dp_1
(assert (distinct dp1 dp4))  ; c_dp_1
(assert (distinct dp1 dp5))  ; c_dp_1
(assert (distinct dp1 dp6))  ; c_dp_1
(assert (distinct dp1 dp7))  ; c_dp_1
(assert (distinct dp2 dp3))  ; c_dp_2
(assert (distinct dp2 dp4))  ; c_dp_2
(assert (distinct dp2 dp5))  ; c_dp_2
(assert (distinct dp2 dp6))  ; c_dp_2
(assert (distinct dp2 dp7))  ; c_dp_2
(assert (distinct dp3 dp4))  ; c_dp_3
(assert (distinct dp3 dp5))  ; c_dp_3
(assert (distinct dp3 dp6))  ; c_dp_3
(assert (distinct dp3 dp7))  ; c_dp_3
(assert (distinct dp4 dp5))  ; c_dp_4
(assert (distinct dp4 dp6))  ; c_dp_4
(assert (distinct dp4 dp7))  ; c_dp_4
(assert (distinct dp5 dp6))  ; c_dp_5
(assert (distinct dp5 dp7))  ; c_dp_5
(assert (distinct dp6 dp7))  ; c_dp_6
(assert (distinct dm0 dm1))  ; c_dm_0
(assert (distinct dm0 dm2))  ; c_dm_0
(assert (distinct dm0 dm3))  ; c_dm_0
(assert (distinct dm0 dm4))  ; c_dm_0
(assert (distinct dm0 dm5))  ; c_dm_0
(assert (distinct dm0 dm6))  ; c_dm_0
(assert (distinct dm0 dm7))  ; c_dm_0
(assert (distinct dm1 dm2))  ; c_dm_1
(assert (distinct dm1 dm3))  ; c_dm_1
(assert (distinct dm1 dm4))  ; c_dm_1
(assert (distinct dm1 dm5))  ; c_dm_1
(assert (distinct dm1 dm6))  ; c_dm_1
(assert (distinct dm1 dm7))  ; c_dm_1
(assert (distinct dm2 dm3))  ; c_dm_2
(assert (distinct dm2 dm4))  ; c_dm_2
(assert (distinct dm2 dm5))  ; c_dm_2
(assert (distinct dm2 dm6))  ; c_dm_2
(assert (distinct dm2 dm7))  ; c_dm_2
(assert (distinct dm3 dm4))  ; c_dm_3
(assert (distinct dm3 dm5))  ; c_dm_3
(assert (distinct dm3 dm6))  ; c_dm_3
(assert (distinct dm3 dm7))  ; c_dm_3
(assert (distinct dm4 dm5))  ; c_dm_4
(assert (distinct dm4 dm6))  ; c_dm_4
(assert (distinct dm4 dm7))  ; c_dm_4
(assert (distinct dm5 dm6))  ; c_dm_5
(assert (distinct dm5 dm7))  ; c_dm_5
(assert (distinct dm6 dm7))  ; c_dm_6

(check-sat)
(get-value (q0 q1 q2 q3 q4 q5 q6 q7 dp0 dp1 dp2 dp3 dp4 dp5 dp6 dp7 dm0 dm1 dm2 dm3 dm4 dm5 dm6 dm7))
