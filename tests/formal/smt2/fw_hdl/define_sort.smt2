; A sort alias, as Yosys's `write_smt2 -stbv` writes a module's state. See
; docs/fw_hdl_requests.md, R4.
;
; Expected: sat (z3, boolector and bitwuzla answer sat). dv-solve 10e8bdc
; prints `unknown` and exits 2, with no diagnostic.
(set-logic QF_BV)
(define-sort |m_s| () (_ BitVec 4))
(define-fun |m_n q| ((state |m_s|)) (_ BitVec 2) ((_ extract 1 0) state))
(declare-fun s0 () |m_s|)
(assert (= (|m_n q| s0) #b10))
(check-sat)
