; BMC d=8 for Fifo8x16 -- expects UNSAT
; Property: FIFO count is always in [0, 8] (4-bit count, max value 8).
; Exercises: Array store/select with 3-bit pointer, 4-bit count arithmetic.
(set-logic QF_AUFBV)

(declare-sort |Fifo_s| 0)

; 8-entry x 16-bit data storage (3-bit address)
(declare-fun |Fifo#data|    (|Fifo_s|) (Array (_ BitVec 3) (_ BitVec 16)))
; 3-bit write and read pointers (mod 8)
(declare-fun |Fifo#wr_ptr|  (|Fifo_s|) (_ BitVec 3))
(declare-fun |Fifo#rd_ptr|  (|Fifo_s|) (_ BitVec 3))
; 4-bit count (range 0..8)
(declare-fun |Fifo#count|   (|Fifo_s|) (_ BitVec 4))
; Push / pop enables from the environment
(declare-fun |Fifo#push_en| (|Fifo_s|) Bool)
(declare-fun |Fifo#pop_en|  (|Fifo_s|) Bool)
(declare-fun |Fifo#din|     (|Fifo_s|) (_ BitVec 16))

; Transition (guarded push/pop; no let bindings -- all inlined)
; do_push = push_en AND NOT full   where full  = (count == 8)
; do_pop  = pop_en  AND NOT empty  where empty = (count == 0)
(define-fun |Fifo_t| ((s |Fifo_s|) (n |Fifo_s|)) Bool
  (and
    ; data array updated on push
    (= (|Fifo#data| n)
       (ite (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))
            (store (|Fifo#data| s)
                   (|Fifo#wr_ptr| s)
                   (|Fifo#din| s))
            (|Fifo#data| s)))
    ; wr_ptr advances on push
    (= (|Fifo#wr_ptr| n)
       (ite (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))
            (bvadd (|Fifo#wr_ptr| s) #b001)
            (|Fifo#wr_ptr| s)))
    ; rd_ptr advances on pop
    (= (|Fifo#rd_ptr| n)
       (ite (and (|Fifo#pop_en| s) (not (= (|Fifo#count| s) #b0000)))
            (bvadd (|Fifo#rd_ptr| s) #b001)
            (|Fifo#rd_ptr| s)))
    ; count update: push-only +1, pop-only -1, both/neither stay
    (= (|Fifo#count| n)
       (ite (and (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))
                 (not (and (|Fifo#pop_en| s) (not (= (|Fifo#count| s) #b0000)))))
            (bvadd (|Fifo#count| s) #b0001)
            (ite (and (and (|Fifo#pop_en| s) (not (= (|Fifo#count| s) #b0000)))
                      (not (and (|Fifo#push_en| s) (not (= (|Fifo#count| s) #b1000)))))
                 (bvsub (|Fifo#count| s) #b0001)
                 (|Fifo#count| s))))))

(declare-const |state_0| |Fifo_s|)
(declare-const |state_1| |Fifo_s|)
(declare-const |state_2| |Fifo_s|)
(declare-const |state_3| |Fifo_s|)
(declare-const |state_4| |Fifo_s|)
(declare-const |state_5| |Fifo_s|)
(declare-const |state_6| |Fifo_s|)
(declare-const |state_7| |Fifo_s|)
(declare-const |state_8| |Fifo_s|)

; Initial state: count = 0, pointers = 0
(assert (= (|Fifo#count|  |state_0|) #b0000))
(assert (= (|Fifo#wr_ptr| |state_0|) #b000))
(assert (= (|Fifo#rd_ptr| |state_0|) #b000))

(assert (|Fifo_t| |state_0| |state_1|))
(assert (|Fifo_t| |state_1| |state_2|))
(assert (|Fifo_t| |state_2| |state_3|))
(assert (|Fifo_t| |state_3| |state_4|))
(assert (|Fifo_t| |state_4| |state_5|))
(assert (|Fifo_t| |state_5| |state_6|))
(assert (|Fifo_t| |state_6| |state_7|))
(assert (|Fifo_t| |state_7| |state_8|))

; Property (negated): count > 8 at any reachable state
(assert (or
  (bvugt (|Fifo#count| |state_1|) #b1000)
  (bvugt (|Fifo#count| |state_2|) #b1000)
  (bvugt (|Fifo#count| |state_3|) #b1000)
  (bvugt (|Fifo#count| |state_4|) #b1000)
  (bvugt (|Fifo#count| |state_5|) #b1000)
  (bvugt (|Fifo#count| |state_6|) #b1000)
  (bvugt (|Fifo#count| |state_7|) #b1000)
  (bvugt (|Fifo#count| |state_8|) #b1000)
))

(check-sat)
