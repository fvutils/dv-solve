; Malformed input: one close-paren too many after an assert.
; See docs/fw_hdl_requests.md, R2.
;
; Expected: an error response, on stdout or stderr, and a nonzero exit status.
; z3 reports: line 11 column 18: invalid command.
; dv-solve 10e8bdc prints NOTHING, not even the check-sat result, and exits 0.
; A script that treats "no sat on stdout" as "proven" would report a proof.
; Note: this header has no parentheses; with some in it, dv-solve prints sat.
(set-logic QF_BV)
(declare-const a (_ BitVec 4))
(assert (= a #x1)))
(check-sat)
