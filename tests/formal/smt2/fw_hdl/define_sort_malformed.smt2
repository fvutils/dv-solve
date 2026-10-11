; Malformed input: a define-sort whose name is not a symbol. See
; docs/fw_hdl_requests.md, R2.
;
; Expected: an `(error "...")` response and a nonzero exit status. z3 prints
;   (error "line 8 column 13: invalid sort definition, symbol expected")
; dv-solve 10e8bdc ignores the line, prints `sat` and exits 0.
(set-logic QF_BV)
(define-sort (_ BitVec 4) () (_ BitVec 4))
(declare-const a (_ BitVec 4))
(assert (= a #x1))
(check-sat)
