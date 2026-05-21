(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const r0_base (_ BitVec 40))
(declare-const r0_size (_ BitVec 34))
(declare-const r0_end (_ BitVec 40))
(declare-const r1_base (_ BitVec 40))
(declare-const r1_size (_ BitVec 34))
(declare-const r1_end (_ BitVec 40))
(declare-const r2_base (_ BitVec 40))
(declare-const r2_size (_ BitVec 34))
(declare-const r2_end (_ BitVec 40))
(declare-const r3_base (_ BitVec 40))
(declare-const r3_size (_ BitVec 34))
(declare-const r3_end (_ BitVec 40))
(declare-const r4_base (_ BitVec 40))
(declare-const r4_size (_ BitVec 34))
(declare-const r4_end (_ BitVec 40))
(declare-const r5_base (_ BitVec 40))
(declare-const r5_size (_ BitVec 34))
(declare-const r5_end (_ BitVec 40))
(declare-const r6_base (_ BitVec 40))
(declare-const r6_size (_ BitVec 34))
(declare-const r6_end (_ BitVec 40))
(declare-const r7_base (_ BitVec 40))
(declare-const r7_size (_ BitVec 34))
(declare-const r7_end (_ BitVec 40))

(assert (bvuge r0_base (_ bv0 40)))
(assert (bvule r0_base (_ bv1090921693183 40)))
(assert (bvuge r0_size (_ bv1073741824 34)))
(assert (bvule r0_size (_ bv8589934592 34)))
(assert (bvuge r0_end (_ bv1073741824 40)))
(assert (bvule r0_end (_ bv1099511627775 40)))
(assert (bvuge r1_base (_ bv0 40)))
(assert (bvule r1_base (_ bv1090921693183 40)))
(assert (bvuge r1_size (_ bv1073741824 34)))
(assert (bvule r1_size (_ bv8589934592 34)))
(assert (bvuge r1_end (_ bv1073741824 40)))
(assert (bvule r1_end (_ bv1099511627775 40)))
(assert (bvuge r2_base (_ bv0 40)))
(assert (bvule r2_base (_ bv1090921693183 40)))
(assert (bvuge r2_size (_ bv1073741824 34)))
(assert (bvule r2_size (_ bv8589934592 34)))
(assert (bvuge r2_end (_ bv1073741824 40)))
(assert (bvule r2_end (_ bv1099511627775 40)))
(assert (bvuge r3_base (_ bv0 40)))
(assert (bvule r3_base (_ bv1090921693183 40)))
(assert (bvuge r3_size (_ bv1073741824 34)))
(assert (bvule r3_size (_ bv8589934592 34)))
(assert (bvuge r3_end (_ bv1073741824 40)))
(assert (bvule r3_end (_ bv1099511627775 40)))
(assert (bvuge r4_base (_ bv0 40)))
(assert (bvule r4_base (_ bv1090921693183 40)))
(assert (bvuge r4_size (_ bv1073741824 34)))
(assert (bvule r4_size (_ bv8589934592 34)))
(assert (bvuge r4_end (_ bv1073741824 40)))
(assert (bvule r4_end (_ bv1099511627775 40)))
(assert (bvuge r5_base (_ bv0 40)))
(assert (bvule r5_base (_ bv1090921693183 40)))
(assert (bvuge r5_size (_ bv1073741824 34)))
(assert (bvule r5_size (_ bv8589934592 34)))
(assert (bvuge r5_end (_ bv1073741824 40)))
(assert (bvule r5_end (_ bv1099511627775 40)))
(assert (bvuge r6_base (_ bv0 40)))
(assert (bvule r6_base (_ bv1090921693183 40)))
(assert (bvuge r6_size (_ bv1073741824 34)))
(assert (bvule r6_size (_ bv8589934592 34)))
(assert (bvuge r6_end (_ bv1073741824 40)))
(assert (bvule r6_end (_ bv1099511627775 40)))
(assert (bvuge r7_base (_ bv0 40)))
(assert (bvule r7_base (_ bv1090921693183 40)))
(assert (bvuge r7_size (_ bv1073741824 34)))
(assert (bvule r7_size (_ bv8589934592 34)))
(assert (bvuge r7_end (_ bv1073741824 40)))
(assert (bvule r7_end (_ bv1099511627775 40)))

(assert (= ((_ zero_extend 1) r0_end) (bvadd ((_ zero_extend 1) r0_base) ((_ zero_extend 7) r0_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r1_end) (bvadd ((_ zero_extend 1) r1_base) ((_ zero_extend 7) r1_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r2_end) (bvadd ((_ zero_extend 1) r2_base) ((_ zero_extend 7) r2_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r3_end) (bvadd ((_ zero_extend 1) r3_base) ((_ zero_extend 7) r3_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r4_end) (bvadd ((_ zero_extend 1) r4_base) ((_ zero_extend 7) r4_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r5_end) (bvadd ((_ zero_extend 1) r5_base) ((_ zero_extend 7) r5_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r6_end) (bvadd ((_ zero_extend 1) r6_base) ((_ zero_extend 7) r6_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r7_end) (bvadd ((_ zero_extend 1) r7_base) ((_ zero_extend 7) r7_size))))  ; c_ends
(assert (bvule r0_end r1_base))  ; c_order
(assert (bvule r1_end r2_base))  ; c_order
(assert (bvule r2_end r3_base))  ; c_order
(assert (bvule r3_end r4_base))  ; c_order
(assert (bvule r4_end r5_base))  ; c_order
(assert (bvule r5_end r6_base))  ; c_order
(assert (bvule r6_end r7_base))  ; c_order
(assert (bvule r7_end (_ bv1099511627775 40)))  ; c_fits

(check-sat)
(get-value (r0_base r0_size r0_end r1_base r1_size r1_end r2_base r2_size r2_end r3_base r3_size r3_end r4_base r4_size r4_end r5_base r5_size r5_end r6_base r6_size r6_end r7_base r7_size r7_end))
