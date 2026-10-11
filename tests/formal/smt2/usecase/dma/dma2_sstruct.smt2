; UC-DMA-2: one do_chunk() call as a single SPL step. Obligation: if the step
; completes the transfer (done=1) then no words remain. Expected: sat --
; the witness is candidate bug fw-hdl spl-formal.md section 5.1; see docs/usecase_benchmark_design.md section 4.2 (stall with CHK_SZ==0).
(set-logic QF_BV)
(set-info :status sat)
(declare-const chk_sz (_ BitVec 9))   ; SZ.CHK_SZ
(declare-const w_rem  (_ BitVec 12))  ; words remaining (TOT_SZ is 12 bits)
(declare-const stall  Bool)           ; swptr_stall() fires at some word j
(declare-const j      (_ BitVec 12))  ; ...after moving j words
(declare-const berr   Bool)           ; a bus error or host STOP ends the chunk via fail()
(declare-const ars    Bool)           ; cap_ars & CSR.ars & !stop
(assert (bvugt w_rem #x000))          ; an armed channel has work
(define-fun n () (_ BitVec 12)
  (ite (= chk_sz #b000000000) w_rem
       (ite (bvult ((_ zero_extend 3) chk_sz) w_rem) ((_ zero_extend 3) chk_sz) w_rem)))
(assert (=> stall (bvult j n)))
(define-fun moved () (_ BitVec 12) (ite stall j n))
(define-fun w_rem1 () (_ BitVec 12) (bvsub w_rem moved))
; exit logic as written: return early iff chunked and words remain; else complete
(define-fun early () Bool (and (not (= chk_sz #b000000000)) (not (= w_rem1 #x000))))
(define-fun done () Bool (and (not berr) (not early) (not ars)))
(assert (and done (not (= w_rem1 #x000))))
(check-sat)
(get-value (chk_sz w_rem stall j))
