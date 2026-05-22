; BMC d=2 for WideDatapath64 -- expects UNSAT
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

(assert (|Wide_t| |state_0| |state_1|))
(assert (|Wide_t| |state_1| |state_2|))

; Negate: result at last step != a at previous step
(assert (not (= (|Wide#result| |state_2|) (|Wide#a| |state_1|))))

(check-sat)
