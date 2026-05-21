(set-option :seed 0)
(set-option :produce-models true)
(set-logic QF_BV)

(declare-const r0_base (_ BitVec 31))
(declare-const r0_size (_ BitVec 26))
(declare-const r0_end (_ BitVec 31))
(declare-const r1_base (_ BitVec 31))
(declare-const r1_size (_ BitVec 26))
(declare-const r1_end (_ BitVec 31))
(declare-const r2_base (_ BitVec 31))
(declare-const r2_size (_ BitVec 26))
(declare-const r2_end (_ BitVec 31))
(declare-const r3_base (_ BitVec 31))
(declare-const r3_size (_ BitVec 26))
(declare-const r3_end (_ BitVec 31))
(declare-const r4_base (_ BitVec 31))
(declare-const r4_size (_ BitVec 26))
(declare-const r4_end (_ BitVec 31))
(declare-const r5_base (_ BitVec 31))
(declare-const r5_size (_ BitVec 26))
(declare-const r5_end (_ BitVec 31))
(declare-const r6_base (_ BitVec 31))
(declare-const r6_size (_ BitVec 26))
(declare-const r6_end (_ BitVec 31))
(declare-const r7_base (_ BitVec 31))
(declare-const r7_size (_ BitVec 26))
(declare-const r7_end (_ BitVec 31))
(declare-const r8_base (_ BitVec 31))
(declare-const r8_size (_ BitVec 26))
(declare-const r8_end (_ BitVec 31))
(declare-const r9_base (_ BitVec 31))
(declare-const r9_size (_ BitVec 26))
(declare-const r9_end (_ BitVec 31))
(declare-const r10_base (_ BitVec 31))
(declare-const r10_size (_ BitVec 26))
(declare-const r10_end (_ BitVec 31))
(declare-const r11_base (_ BitVec 31))
(declare-const r11_size (_ BitVec 26))
(declare-const r11_end (_ BitVec 31))
(declare-const s01 (_ BitVec 31))
(declare-const s012 (_ BitVec 31))
(declare-const s0123 (_ BitVec 31))
(declare-const s01234 (_ BitVec 31))
(declare-const s012345 (_ BitVec 31))
(declare-const s0to6 (_ BitVec 31))
(declare-const s0to7 (_ BitVec 31))
(declare-const s0to8 (_ BitVec 31))
(declare-const s0to9 (_ BitVec 31))
(declare-const s0to10 (_ BitVec 31))

(assert (bvuge r0_base (_ bv0 31)))
(assert (bvule r0_base (_ bv1610612736 31)))
(assert (bvuge r0_size (_ bv1048576 26)))
(assert (bvule r0_size (_ bv33554432 26)))
(assert (bvuge r0_end (_ bv0 31)))
(assert (bvule r0_end (_ bv2147483647 31)))
(assert (bvuge r1_base (_ bv0 31)))
(assert (bvule r1_base (_ bv1610612736 31)))
(assert (bvuge r1_size (_ bv1048576 26)))
(assert (bvule r1_size (_ bv33554432 26)))
(assert (bvuge r1_end (_ bv0 31)))
(assert (bvule r1_end (_ bv2147483647 31)))
(assert (bvuge r2_base (_ bv0 31)))
(assert (bvule r2_base (_ bv1610612736 31)))
(assert (bvuge r2_size (_ bv1048576 26)))
(assert (bvule r2_size (_ bv33554432 26)))
(assert (bvuge r2_end (_ bv0 31)))
(assert (bvule r2_end (_ bv2147483647 31)))
(assert (bvuge r3_base (_ bv0 31)))
(assert (bvule r3_base (_ bv1610612736 31)))
(assert (bvuge r3_size (_ bv1048576 26)))
(assert (bvule r3_size (_ bv33554432 26)))
(assert (bvuge r3_end (_ bv0 31)))
(assert (bvule r3_end (_ bv2147483647 31)))
(assert (bvuge r4_base (_ bv0 31)))
(assert (bvule r4_base (_ bv1610612736 31)))
(assert (bvuge r4_size (_ bv1048576 26)))
(assert (bvule r4_size (_ bv33554432 26)))
(assert (bvuge r4_end (_ bv0 31)))
(assert (bvule r4_end (_ bv2147483647 31)))
(assert (bvuge r5_base (_ bv0 31)))
(assert (bvule r5_base (_ bv1610612736 31)))
(assert (bvuge r5_size (_ bv1048576 26)))
(assert (bvule r5_size (_ bv33554432 26)))
(assert (bvuge r5_end (_ bv0 31)))
(assert (bvule r5_end (_ bv2147483647 31)))
(assert (bvuge r6_base (_ bv0 31)))
(assert (bvule r6_base (_ bv1610612736 31)))
(assert (bvuge r6_size (_ bv1048576 26)))
(assert (bvule r6_size (_ bv33554432 26)))
(assert (bvuge r6_end (_ bv0 31)))
(assert (bvule r6_end (_ bv2147483647 31)))
(assert (bvuge r7_base (_ bv0 31)))
(assert (bvule r7_base (_ bv1610612736 31)))
(assert (bvuge r7_size (_ bv1048576 26)))
(assert (bvule r7_size (_ bv33554432 26)))
(assert (bvuge r7_end (_ bv0 31)))
(assert (bvule r7_end (_ bv2147483647 31)))
(assert (bvuge r8_base (_ bv0 31)))
(assert (bvule r8_base (_ bv1610612736 31)))
(assert (bvuge r8_size (_ bv1048576 26)))
(assert (bvule r8_size (_ bv33554432 26)))
(assert (bvuge r8_end (_ bv0 31)))
(assert (bvule r8_end (_ bv2147483647 31)))
(assert (bvuge r9_base (_ bv0 31)))
(assert (bvule r9_base (_ bv1610612736 31)))
(assert (bvuge r9_size (_ bv1048576 26)))
(assert (bvule r9_size (_ bv33554432 26)))
(assert (bvuge r9_end (_ bv0 31)))
(assert (bvule r9_end (_ bv2147483647 31)))
(assert (bvuge r10_base (_ bv0 31)))
(assert (bvule r10_base (_ bv1610612736 31)))
(assert (bvuge r10_size (_ bv1048576 26)))
(assert (bvule r10_size (_ bv33554432 26)))
(assert (bvuge r10_end (_ bv0 31)))
(assert (bvule r10_end (_ bv2147483647 31)))
(assert (bvuge r11_base (_ bv0 31)))
(assert (bvule r11_base (_ bv1610612736 31)))
(assert (bvuge r11_size (_ bv1048576 26)))
(assert (bvule r11_size (_ bv33554432 26)))
(assert (bvuge r11_end (_ bv0 31)))
(assert (bvule r11_end (_ bv2147483647 31)))
(assert (bvuge s01 (_ bv0 31)))
(assert (bvule s01 (_ bv2147483647 31)))
(assert (bvuge s012 (_ bv0 31)))
(assert (bvule s012 (_ bv2147483647 31)))
(assert (bvuge s0123 (_ bv0 31)))
(assert (bvule s0123 (_ bv2147483647 31)))
(assert (bvuge s01234 (_ bv0 31)))
(assert (bvule s01234 (_ bv2147483647 31)))
(assert (bvuge s012345 (_ bv0 31)))
(assert (bvule s012345 (_ bv2147483647 31)))
(assert (bvuge s0to6 (_ bv0 31)))
(assert (bvule s0to6 (_ bv2147483647 31)))
(assert (bvuge s0to7 (_ bv0 31)))
(assert (bvule s0to7 (_ bv2147483647 31)))
(assert (bvuge s0to8 (_ bv0 31)))
(assert (bvule s0to8 (_ bv2147483647 31)))
(assert (bvuge s0to9 (_ bv0 31)))
(assert (bvule s0to9 (_ bv2147483647 31)))
(assert (bvuge s0to10 (_ bv0 31)))
(assert (bvule s0to10 (_ bv2147483647 31)))

(assert (= ((_ zero_extend 1) r0_end) (bvadd ((_ zero_extend 1) r0_base) ((_ zero_extend 6) r0_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r1_end) (bvadd ((_ zero_extend 1) r1_base) ((_ zero_extend 6) r1_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r2_end) (bvadd ((_ zero_extend 1) r2_base) ((_ zero_extend 6) r2_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r3_end) (bvadd ((_ zero_extend 1) r3_base) ((_ zero_extend 6) r3_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r4_end) (bvadd ((_ zero_extend 1) r4_base) ((_ zero_extend 6) r4_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r5_end) (bvadd ((_ zero_extend 1) r5_base) ((_ zero_extend 6) r5_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r6_end) (bvadd ((_ zero_extend 1) r6_base) ((_ zero_extend 6) r6_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r7_end) (bvadd ((_ zero_extend 1) r7_base) ((_ zero_extend 6) r7_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r8_end) (bvadd ((_ zero_extend 1) r8_base) ((_ zero_extend 6) r8_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r9_end) (bvadd ((_ zero_extend 1) r9_base) ((_ zero_extend 6) r9_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r10_end) (bvadd ((_ zero_extend 1) r10_base) ((_ zero_extend 6) r10_size))))  ; c_ends
(assert (= ((_ zero_extend 1) r11_end) (bvadd ((_ zero_extend 1) r11_base) ((_ zero_extend 6) r11_size))))  ; c_ends
(assert (bvule r0_end r1_base))  ; c_order
(assert (bvule r1_end r2_base))  ; c_order
(assert (bvule r2_end r3_base))  ; c_order
(assert (bvule r3_end r4_base))  ; c_order
(assert (bvule r4_end r5_base))  ; c_order
(assert (bvule r5_end r6_base))  ; c_order
(assert (bvule r6_end r7_base))  ; c_order
(assert (bvule r7_end r8_base))  ; c_order
(assert (bvule r8_end r9_base))  ; c_order
(assert (bvule r9_end r10_base))  ; c_order
(assert (bvule r10_end r11_base))  ; c_order
(assert (bvule r11_end (_ bv2147483647 31)))  ; c_fits
(assert (bvule r0_size r1_size))  ; c_size_order
(assert (bvule r1_size r2_size))  ; c_size_order
(assert (bvule r2_size r3_size))  ; c_size_order
(assert (bvule r3_size r4_size))  ; c_size_order
(assert (bvule r4_size r5_size))  ; c_size_order
(assert (bvule r5_size r6_size))  ; c_size_order
(assert (bvule r6_size r7_size))  ; c_size_order
(assert (bvule r7_size r8_size))  ; c_size_order
(assert (bvule r8_size r9_size))  ; c_size_order
(assert (bvule r9_size r10_size))  ; c_size_order
(assert (bvule r10_size r11_size))  ; c_size_order
(assert (= s01 ((_ zero_extend 4) (bvadd ((_ zero_extend 1) r0_size) ((_ zero_extend 1) r1_size)))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s012) (bvadd ((_ zero_extend 1) s01) ((_ zero_extend 6) r2_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s0123) (bvadd ((_ zero_extend 1) s012) ((_ zero_extend 6) r3_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s01234) (bvadd ((_ zero_extend 1) s0123) ((_ zero_extend 6) r4_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s012345) (bvadd ((_ zero_extend 1) s01234) ((_ zero_extend 6) r5_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s0to6) (bvadd ((_ zero_extend 1) s012345) ((_ zero_extend 6) r6_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s0to7) (bvadd ((_ zero_extend 1) s0to6) ((_ zero_extend 6) r7_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s0to8) (bvadd ((_ zero_extend 1) s0to7) ((_ zero_extend 6) r8_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s0to9) (bvadd ((_ zero_extend 1) s0to8) ((_ zero_extend 6) r9_size))))  ; c_sum_chain
(assert (= ((_ zero_extend 1) s0to10) (bvadd ((_ zero_extend 1) s0to9) ((_ zero_extend 6) r10_size))))  ; c_sum_chain
(assert (= (bvadd ((_ zero_extend 1) s0to10) ((_ zero_extend 6) r11_size)) (_ bv201326592 32)))  ; c_total

(check-sat)
(get-value (r0_base r0_size r0_end r1_base r1_size r1_end r2_base r2_size r2_end r3_base r3_size r3_end r4_base r4_size r4_end r5_base r5_size r5_end r6_base r6_size r6_end r7_base r7_size r7_end r8_base r8_size r8_end r9_base r9_size r9_end r10_base r10_size r10_end r11_base r11_size r11_end s01 s012 s0123 s01234 s012345 s0to6 s0to7 s0to8 s0to9 s0to10))
