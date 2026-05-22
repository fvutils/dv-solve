; BMC d=4 for RegfileAddrAlias -- expects UNSAT
; Property: writing address A then reading address B (A != B) returns old B value.
; Exercises: store/select non-aliasing proof.
(set-logic QF_AUFBV)

(declare-sort |RegfileAlias_s| 0)

(declare-fun |RegfileAlias#mem|     (|RegfileAlias_s|) (Array (_ BitVec 3) (_ BitVec 8)))
(declare-fun |RegfileAlias#wr_en|   (|RegfileAlias_s|) Bool)
(declare-fun |RegfileAlias#wr_addr| (|RegfileAlias_s|) (_ BitVec 3))
(declare-fun |RegfileAlias#wr_data| (|RegfileAlias_s|) (_ BitVec 8))
(declare-fun |RegfileAlias#rd_addr| (|RegfileAlias_s|) (_ BitVec 3))
(declare-fun |RegfileAlias#rd_data| (|RegfileAlias_s|) (_ BitVec 8))

(define-fun |RegfileAlias_t| ((s |RegfileAlias_s|) (n |RegfileAlias_s|)) Bool
  (and
    (= (|RegfileAlias#mem| n)
       (ite (|RegfileAlias#wr_en| s)
            (store (|RegfileAlias#mem| s)
                   (|RegfileAlias#wr_addr| s)
                   (|RegfileAlias#wr_data| s))
            (|RegfileAlias#mem| s)))
    (= (|RegfileAlias#rd_data| n)
       (select (|RegfileAlias#mem| n) (|RegfileAlias#rd_addr| s)))))

(declare-const |state_0| |RegfileAlias_s|)
(declare-const |state_1| |RegfileAlias_s|)
(declare-const |state_2| |RegfileAlias_s|)
(declare-const |state_3| |RegfileAlias_s|)
(declare-const |state_4| |RegfileAlias_s|)

(assert (|RegfileAlias_t| |state_0| |state_1|))
(assert (|RegfileAlias_t| |state_1| |state_2|))
(assert (|RegfileAlias_t| |state_2| |state_3|))
(assert (|RegfileAlias_t| |state_3| |state_4|))

; Negate property at last transition:
; wr_en, wr_addr != rd_addr, but rd_data != old mem[rd_addr]
(assert (|RegfileAlias#wr_en|   |state_3|))
(assert (not (= (|RegfileAlias#rd_addr| |state_3|)
               (|RegfileAlias#wr_addr| |state_3|))))
; rd_data in next state should equal old mem[rd_addr]; negate:
(assert (not (= (|RegfileAlias#rd_data| |state_4|)
               (select (|RegfileAlias#mem| |state_3|)
                       (|RegfileAlias#rd_addr| |state_3|)))))

(check-sat)
