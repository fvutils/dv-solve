; Malformed input: one `)` too many after declare-const. See
; docs/fw_hdl_requests.md, R2.
;
; Expected: an `(error "...")` response (stdout or stderr) and a nonzero exit
; status. z3 prints
;   (error "line 9 column 30: invalid command, '(' expected")
; then carries on. dv-solve 10e8bdc ignores the stray `)`, prints `sat` with no
; diagnostic, and exits 0.
(set-logic QF_BV)
(declare-const a (_ BitVec 4)))
(assert (= a #x1))
(check-sat)
