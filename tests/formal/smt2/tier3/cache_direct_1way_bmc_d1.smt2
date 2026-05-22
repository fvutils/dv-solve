; BMC d=1 for CacheDirectOneWay -- expects UNSAT
; Property: after writing a cache line, the data array contains the written value.
; Exercises: 4-bit index arrays for data (8-bit), valid (1-bit), tag (4-bit).
(set-logic QF_AUFBV)

(declare-sort |Cache_s| 0)

; 16-line cache: 4-bit index -> data/valid/tag
(declare-fun |Cache#data|  (|Cache_s|) (Array (_ BitVec 4) (_ BitVec 8)))
(declare-fun |Cache#valid| (|Cache_s|) (Array (_ BitVec 4) (_ BitVec 1)))
(declare-fun |Cache#tag|   (|Cache_s|) (Array (_ BitVec 4) (_ BitVec 4)))

; Processor request signals
(declare-fun |Cache#req_en|    (|Cache_s|) Bool)
(declare-fun |Cache#req_we|    (|Cache_s|) Bool)
(declare-fun |Cache#req_addr|  (|Cache_s|) (_ BitVec 8))   ; 4 tag + 4 index
(declare-fun |Cache#req_data|  (|Cache_s|) (_ BitVec 8))

; Transition: on write, fill the addressed line
; idx = extract(3,0)(req_addr), tag = extract(7,4)(req_addr)
(define-fun |Cache_t| ((s |Cache_s|) (n |Cache_s|)) Bool
  (and
    (= (|Cache#data| n)
       (ite (and (|Cache#req_en| s) (|Cache#req_we| s))
            (store (|Cache#data| s)
                   ((_ extract 3 0) (|Cache#req_addr| s))
                   (|Cache#req_data| s))
            (|Cache#data| s)))
    (= (|Cache#valid| n)
       (ite (and (|Cache#req_en| s) (|Cache#req_we| s))
            (store (|Cache#valid| s)
                   ((_ extract 3 0) (|Cache#req_addr| s))
                   #b1)
            (|Cache#valid| s)))
    (= (|Cache#tag| n)
       (ite (and (|Cache#req_en| s) (|Cache#req_we| s))
            (store (|Cache#tag| s)
                   ((_ extract 3 0) (|Cache#req_addr| s))
                   ((_ extract 7 4) (|Cache#req_addr| s)))
            (|Cache#tag| s)))))

(declare-const |state_0| |Cache_s|)
(declare-const |state_1| |Cache_s|)

; Initially all lines invalid
(assert (= (|Cache#valid| |state_0|) ((as const (Array (_ BitVec 4) (_ BitVec 1))) #b0)))

(assert (|Cache_t| |state_0| |state_1|))

; Property (negated): write at state_0, but data array at state_1 has wrong value
(assert (|Cache#req_en|  |state_0|))
(assert (|Cache#req_we|  |state_0|))
(assert (not (= (select (|Cache#data| |state_1|)
                        ((_ extract 3 0) (|Cache#req_addr| |state_0|)))
               (|Cache#req_data| |state_0|))))

(check-sat)
