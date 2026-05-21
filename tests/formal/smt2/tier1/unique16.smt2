(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const v0 (_ BitVec 5))
(declare-const v1 (_ BitVec 5))
(declare-const v2 (_ BitVec 5))
(declare-const v3 (_ BitVec 5))
(declare-const v4 (_ BitVec 5))
(declare-const v5 (_ BitVec 5))
(declare-const v6 (_ BitVec 5))
(declare-const v7 (_ BitVec 5))
(declare-const v8 (_ BitVec 5))
(declare-const v9 (_ BitVec 5))
(declare-const v10 (_ BitVec 5))
(declare-const v11 (_ BitVec 5))
(declare-const v12 (_ BitVec 5))
(declare-const v13 (_ BitVec 5))
(declare-const v14 (_ BitVec 5))
(declare-const v15 (_ BitVec 5))

(assert (bvuge v0 (_ bv0 5)))
(assert (bvule v0 (_ bv31 5)))
(assert (bvuge v1 (_ bv0 5)))
(assert (bvule v1 (_ bv31 5)))
(assert (bvuge v2 (_ bv0 5)))
(assert (bvule v2 (_ bv31 5)))
(assert (bvuge v3 (_ bv0 5)))
(assert (bvule v3 (_ bv31 5)))
(assert (bvuge v4 (_ bv0 5)))
(assert (bvule v4 (_ bv31 5)))
(assert (bvuge v5 (_ bv0 5)))
(assert (bvule v5 (_ bv31 5)))
(assert (bvuge v6 (_ bv0 5)))
(assert (bvule v6 (_ bv31 5)))
(assert (bvuge v7 (_ bv0 5)))
(assert (bvule v7 (_ bv31 5)))
(assert (bvuge v8 (_ bv0 5)))
(assert (bvule v8 (_ bv31 5)))
(assert (bvuge v9 (_ bv0 5)))
(assert (bvule v9 (_ bv31 5)))
(assert (bvuge v10 (_ bv0 5)))
(assert (bvule v10 (_ bv31 5)))
(assert (bvuge v11 (_ bv0 5)))
(assert (bvule v11 (_ bv31 5)))
(assert (bvuge v12 (_ bv0 5)))
(assert (bvule v12 (_ bv31 5)))
(assert (bvuge v13 (_ bv0 5)))
(assert (bvule v13 (_ bv31 5)))
(assert (bvuge v14 (_ bv0 5)))
(assert (bvule v14 (_ bv31 5)))
(assert (bvuge v15 (_ bv0 5)))
(assert (bvule v15 (_ bv31 5)))

(assert (distinct v0 v1))  ; c_unique_0
(assert (distinct v0 v2))  ; c_unique_0
(assert (distinct v0 v3))  ; c_unique_0
(assert (distinct v0 v4))  ; c_unique_0
(assert (distinct v0 v5))  ; c_unique_0
(assert (distinct v0 v6))  ; c_unique_0
(assert (distinct v0 v7))  ; c_unique_0
(assert (distinct v0 v8))  ; c_unique_0
(assert (distinct v0 v9))  ; c_unique_0
(assert (distinct v0 v10))  ; c_unique_0
(assert (distinct v0 v11))  ; c_unique_0
(assert (distinct v0 v12))  ; c_unique_0
(assert (distinct v0 v13))  ; c_unique_0
(assert (distinct v0 v14))  ; c_unique_0
(assert (distinct v0 v15))  ; c_unique_0
(assert (distinct v1 v2))  ; c_unique_1
(assert (distinct v1 v3))  ; c_unique_1
(assert (distinct v1 v4))  ; c_unique_1
(assert (distinct v1 v5))  ; c_unique_1
(assert (distinct v1 v6))  ; c_unique_1
(assert (distinct v1 v7))  ; c_unique_1
(assert (distinct v1 v8))  ; c_unique_1
(assert (distinct v1 v9))  ; c_unique_1
(assert (distinct v1 v10))  ; c_unique_1
(assert (distinct v1 v11))  ; c_unique_1
(assert (distinct v1 v12))  ; c_unique_1
(assert (distinct v1 v13))  ; c_unique_1
(assert (distinct v1 v14))  ; c_unique_1
(assert (distinct v1 v15))  ; c_unique_1
(assert (distinct v2 v3))  ; c_unique_2
(assert (distinct v2 v4))  ; c_unique_2
(assert (distinct v2 v5))  ; c_unique_2
(assert (distinct v2 v6))  ; c_unique_2
(assert (distinct v2 v7))  ; c_unique_2
(assert (distinct v2 v8))  ; c_unique_2
(assert (distinct v2 v9))  ; c_unique_2
(assert (distinct v2 v10))  ; c_unique_2
(assert (distinct v2 v11))  ; c_unique_2
(assert (distinct v2 v12))  ; c_unique_2
(assert (distinct v2 v13))  ; c_unique_2
(assert (distinct v2 v14))  ; c_unique_2
(assert (distinct v2 v15))  ; c_unique_2
(assert (distinct v3 v4))  ; c_unique_3
(assert (distinct v3 v5))  ; c_unique_3
(assert (distinct v3 v6))  ; c_unique_3
(assert (distinct v3 v7))  ; c_unique_3
(assert (distinct v3 v8))  ; c_unique_3
(assert (distinct v3 v9))  ; c_unique_3
(assert (distinct v3 v10))  ; c_unique_3
(assert (distinct v3 v11))  ; c_unique_3
(assert (distinct v3 v12))  ; c_unique_3
(assert (distinct v3 v13))  ; c_unique_3
(assert (distinct v3 v14))  ; c_unique_3
(assert (distinct v3 v15))  ; c_unique_3
(assert (distinct v4 v5))  ; c_unique_4
(assert (distinct v4 v6))  ; c_unique_4
(assert (distinct v4 v7))  ; c_unique_4
(assert (distinct v4 v8))  ; c_unique_4
(assert (distinct v4 v9))  ; c_unique_4
(assert (distinct v4 v10))  ; c_unique_4
(assert (distinct v4 v11))  ; c_unique_4
(assert (distinct v4 v12))  ; c_unique_4
(assert (distinct v4 v13))  ; c_unique_4
(assert (distinct v4 v14))  ; c_unique_4
(assert (distinct v4 v15))  ; c_unique_4
(assert (distinct v5 v6))  ; c_unique_5
(assert (distinct v5 v7))  ; c_unique_5
(assert (distinct v5 v8))  ; c_unique_5
(assert (distinct v5 v9))  ; c_unique_5
(assert (distinct v5 v10))  ; c_unique_5
(assert (distinct v5 v11))  ; c_unique_5
(assert (distinct v5 v12))  ; c_unique_5
(assert (distinct v5 v13))  ; c_unique_5
(assert (distinct v5 v14))  ; c_unique_5
(assert (distinct v5 v15))  ; c_unique_5
(assert (distinct v6 v7))  ; c_unique_6
(assert (distinct v6 v8))  ; c_unique_6
(assert (distinct v6 v9))  ; c_unique_6
(assert (distinct v6 v10))  ; c_unique_6
(assert (distinct v6 v11))  ; c_unique_6
(assert (distinct v6 v12))  ; c_unique_6
(assert (distinct v6 v13))  ; c_unique_6
(assert (distinct v6 v14))  ; c_unique_6
(assert (distinct v6 v15))  ; c_unique_6
(assert (distinct v7 v8))  ; c_unique_7
(assert (distinct v7 v9))  ; c_unique_7
(assert (distinct v7 v10))  ; c_unique_7
(assert (distinct v7 v11))  ; c_unique_7
(assert (distinct v7 v12))  ; c_unique_7
(assert (distinct v7 v13))  ; c_unique_7
(assert (distinct v7 v14))  ; c_unique_7
(assert (distinct v7 v15))  ; c_unique_7
(assert (distinct v8 v9))  ; c_unique_8
(assert (distinct v8 v10))  ; c_unique_8
(assert (distinct v8 v11))  ; c_unique_8
(assert (distinct v8 v12))  ; c_unique_8
(assert (distinct v8 v13))  ; c_unique_8
(assert (distinct v8 v14))  ; c_unique_8
(assert (distinct v8 v15))  ; c_unique_8
(assert (distinct v9 v10))  ; c_unique_9
(assert (distinct v9 v11))  ; c_unique_9
(assert (distinct v9 v12))  ; c_unique_9
(assert (distinct v9 v13))  ; c_unique_9
(assert (distinct v9 v14))  ; c_unique_9
(assert (distinct v9 v15))  ; c_unique_9
(assert (distinct v10 v11))  ; c_unique_10
(assert (distinct v10 v12))  ; c_unique_10
(assert (distinct v10 v13))  ; c_unique_10
(assert (distinct v10 v14))  ; c_unique_10
(assert (distinct v10 v15))  ; c_unique_10
(assert (distinct v11 v12))  ; c_unique_11
(assert (distinct v11 v13))  ; c_unique_11
(assert (distinct v11 v14))  ; c_unique_11
(assert (distinct v11 v15))  ; c_unique_11
(assert (distinct v12 v13))  ; c_unique_12
(assert (distinct v12 v14))  ; c_unique_12
(assert (distinct v12 v15))  ; c_unique_12
(assert (distinct v13 v14))  ; c_unique_13
(assert (distinct v13 v15))  ; c_unique_13
(assert (distinct v14 v15))  ; c_unique_14

(check-sat)
(get-value (v0 v1 v2 v3 v4 v5 v6 v7 v8 v9 v10 v11 v12 v13 v14 v15))
