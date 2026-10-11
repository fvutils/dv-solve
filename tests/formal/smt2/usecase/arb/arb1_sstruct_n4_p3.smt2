; UC-ARB-1 (struct variant): next() picks a ready channel of maximal priority.
; n_ch = 4, prio 3 bits, channel index 2 bits. Expected: unsat (property holds).
(set-logic QF_BV)
(set-info :status unsat)
(declare-const busy (_ BitVec 4)) (declare-const stop (_ BitVec 4))
(declare-const err  (_ BitVec 4)) (declare-const mode (_ BitVec 4))
(declare-const hreq (_ BitVec 4)) (declare-const paused Bool)
(declare-const p0 (_ BitVec 3)) (declare-const p1 (_ BitVec 3))
(declare-const p2 (_ BitVec 3)) (declare-const p3 (_ BitVec 3))
(declare-const last (_ BitVec 2))
; ready(i) = busy & !stop & !err & (!mode | has_req)   -- bitwise over the 4 channels
(define-fun rdy () (_ BitVec 4) (bvand busy (bvnot stop) (bvnot err) (bvor (bvnot mode) hreq)))
(define-fun r ((i (_ BitVec 2))) Bool (= ((_ extract 0 0) (bvlshr rdy ((_ zero_extend 2) i))) #b1))
(define-fun pr ((i (_ BitVec 2))) (_ BitVec 3)
  (ite (= i #b00) p0 (ite (= i #b01) p1 (ite (= i #b10) p2 p3))))
; best = max prio over ready channels; has = any ready
(define-fun m01 () (_ BitVec 3) (ite (and (r #b01) (or (not (r #b00)) (bvugt p1 p0))) p1 p0))
(define-fun m012 () (_ BitVec 3) (ite (and (r #b10) (or (not (or (r #b00) (r #b01))) (bvugt p2 m01))) p2 m01))
(define-fun best () (_ BitVec 3) (ite (and (r #b11) (or (not (or (r #b00) (r #b01) (r #b10))) (bvugt p3 m012))) p3 m012))
(define-fun has () Bool (not (= rdy #x0)))
; round-robin scan from last+1 for the first ready channel at prio best
(define-fun ok ((i (_ BitVec 2))) Bool (and (r i) (= (pr i) best)))
(define-fun c1 () (_ BitVec 2) (bvadd last #b01)) (define-fun c2 () (_ BitVec 2) (bvadd last #b10))
(define-fun c3 () (_ BitVec 2) (bvadd last #b11)) (define-fun c4 () (_ BitVec 2) last)
(define-fun win () (_ BitVec 2) (ite (ok c1) c1 (ite (ok c2) c2 (ite (ok c3) c3 c4))))
(define-fun granted () Bool (and (not paused) has))
; negated property: a grant goes to a non-ready channel, or some ready channel beats it
(define-fun beats ((i (_ BitVec 2))) Bool (and (r i) (bvugt (pr i) (pr win))))
(assert (and granted (or (not (r win)) (beats #b00) (beats #b01) (beats #b10) (beats #b11))))
(check-sat)
