; a declare-sort value passed on by name through nested functions (B84: looped forever)
; expect: sat values
; exit: 0
; values: z3
(set-logic QF_UFBV)
(declare-sort |m_s| 0)
(declare-fun |m#0| (|m_s|) (_ BitVec 4))
(define-fun |g| ((t |m_s|) (state |m_s|)) (_ BitVec 4) (bvsub (|m#0| state) (|m#0| t)))
(define-fun |f| ((state |m_s|) (t |m_s|)) (_ BitVec 4) (|g| state t))
(define-fun |h| ((state |m_s|) (u |m_s|)) (_ BitVec 4) (|f| u state))
(declare-fun s0 () |m_s|)
(declare-fun s1 () |m_s|)
(assert (= (|h| s0 s1) #x3))
(assert (= (|m#0| s0) #x5))
(check-sat)
(get-value ((|m#0| s0) (|m#0| s1)))
