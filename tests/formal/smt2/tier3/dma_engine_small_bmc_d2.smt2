; BMC d=2 for DmaEngineSmall -- expects SAT
; 4-channel scratch memory: Bool wr_en, 2-bit channel, 8-bit data.
; d=1: UNSAT (write-then-read same channel returns written value).
; d=2: SAT   (two sequential writes; assert stale value -- satisfiable).
(set-logic QF_AUFBV)

(declare-sort |DMA_s| 0)

; 4-channel scratch memory (2-bit channel select as array address)
(declare-fun |DMA#mem|     (|DMA_s|) (Array (_ BitVec 2) (_ BitVec 8)))
(declare-fun |DMA#wr_en|   (|DMA_s|) Bool)
(declare-fun |DMA#wr_ch|   (|DMA_s|) (_ BitVec 2))
(declare-fun |DMA#wr_data| (|DMA_s|) (_ BitVec 8))
(declare-fun |DMA#rd_ch|   (|DMA_s|) (_ BitVec 2))
(declare-fun |DMA#rd_data| (|DMA_s|) (_ BitVec 8))

(define-fun |DMA_t| ((s |DMA_s|) (n |DMA_s|)) Bool
  (and
    (= (|DMA#mem| n)
       (ite (|DMA#wr_en| s)
            (store (|DMA#mem| s) (|DMA#wr_ch| s) (|DMA#wr_data| s))
            (|DMA#mem| s)))
    (= (|DMA#rd_data| n)
       (select (|DMA#mem| n) (|DMA#rd_ch| s)))))

(declare-const |state_0| |DMA_s|)
(declare-const |state_1| |DMA_s|)
(declare-const |state_2| |DMA_s|)

(assert (|DMA_t| |state_0| |state_1|))
(assert (|DMA_t| |state_1| |state_2|))

; d=2 SAT: "ch0 content never changes without a write at step 0."
; Negate: no write at step_0, but select(mem_2, ch0) != select(mem_0, ch0).
; Satisfiable: choose wr_en_1=true, wr_ch_1=0, wr_data_1 != initial mem_0[0].
(assert (not (|DMA#wr_en| |state_0|)))
(assert (not (= (select (|DMA#mem| |state_2|) #b00)
               (select (|DMA#mem| |state_0|) #b00))))

(check-sat)
