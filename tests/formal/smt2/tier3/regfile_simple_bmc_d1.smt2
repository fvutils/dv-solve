; BMC d=1 for RegfileSimple -- expects UNSAT
; Property: write-then-read at the same address returns the written value.
; Exercises: declare-fun returning (Array BV BV), store, select, array equality.
(set-logic QF_AUFBV)

(declare-sort |RegfileSimple_s| 0)

; 8-entry x 8-bit register file as an array field of the state
(declare-fun |RegfileSimple#mem|     (|RegfileSimple_s|) (Array (_ BitVec 3) (_ BitVec 8)))
(declare-fun |RegfileSimple#wr_en|   (|RegfileSimple_s|) Bool)
(declare-fun |RegfileSimple#wr_addr| (|RegfileSimple_s|) (_ BitVec 3))
(declare-fun |RegfileSimple#wr_data| (|RegfileSimple_s|) (_ BitVec 8))
(declare-fun |RegfileSimple#rd_addr| (|RegfileSimple_s|) (_ BitVec 3))
(declare-fun |RegfileSimple#rd_data| (|RegfileSimple_s|) (_ BitVec 8))

; Transition: if wr_en, update mem at wr_addr; rd_data reflects updated mem
(define-fun |RegfileSimple_t| ((s |RegfileSimple_s|) (n |RegfileSimple_s|)) Bool
  (and
    (= (|RegfileSimple#mem| n)
       (ite (|RegfileSimple#wr_en| s)
            (store (|RegfileSimple#mem| s)
                   (|RegfileSimple#wr_addr| s)
                   (|RegfileSimple#wr_data| s))
            (|RegfileSimple#mem| s)))
    (= (|RegfileSimple#rd_data| n)
       (select (|RegfileSimple#mem| n) (|RegfileSimple#rd_addr| s)))))

(declare-const |state_0| |RegfileSimple_s|)
(declare-const |state_1| |RegfileSimple_s|)

(assert (|RegfileSimple_t| |state_0| |state_1|))

; Negate property at the last transition:
; wr_en at state_0, rd_addr_0 == wr_addr_0, but rd_data_1 != wr_data_0
(assert (|RegfileSimple#wr_en|   |state_0|))
(assert (= (|RegfileSimple#rd_addr| |state_0|) (|RegfileSimple#wr_addr| |state_0|)))
(assert (not (= (|RegfileSimple#rd_data| |state_1|)
               (|RegfileSimple#wr_data| |state_0|))))

(check-sat)
