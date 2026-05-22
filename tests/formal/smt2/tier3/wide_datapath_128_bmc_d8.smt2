; BMC d=8 for WideDatapath64 -- expects UNSAT
; Property: 64-bit ALU identity: (a + 0) = a for all a.
; No arrays -- ensures the array-aware translator handles wide BV (max 64 bits) correctly.
(set-logic QF_AUFBV)

(declare-sort |Wide_s| 0)
(declare-fun |Wide#a|      (|Wide_s|) (_ BitVec 64))
(declare-fun |Wide#result| (|Wide_s|) (_ BitVec 64))

(define-fun |Wide_t| ((s |Wide_s|) (n |Wide_s|)) Bool
  (= (|Wide#result| n)
     (bvadd (|Wide#a| s) #x0000000000000000)))

(declare-const |state_0| |Wide_s|)
(declare-const |state_1| |Wide_s|)
(declare-const |state_2| |Wide_s|)
(declare-const |state_3| |Wide_s|)
(declare-const |state_4| |Wide_s|)
(declare-const |state_5| |Wide_s|)
(declare-const |state_6| |Wide_s|)
(declare-const |state_7| |Wide_s|)
(declare-const |state_8| |Wide_s|)

(assert (|Wide_t| |state_0| |state_1|))
(assert (|Wide_t| |state_1| |state_2|))
(assert (|Wide_t| |state_2| |state_3|))
(assert (|Wide_t| |state_3| |state_4|))
(assert (|Wide_t| |state_4| |state_5|))
(assert (|Wide_t| |state_5| |state_6|))
(assert (|Wide_t| |state_6| |state_7|))
(assert (|Wide_t| |state_7| |state_8|))

; Negate: result at last step != a at previous step
(assert (not (= (|Wide#result| |state_8|) (|Wide#a| |state_7|))))

(check-sat)
