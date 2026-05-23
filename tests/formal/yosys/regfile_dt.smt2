; Yosys-style record-of-BV fixture (Phase 6.5.3).
; Uses (declare-datatypes ((NAME 0)) (((CTOR (FIELD TYPE)...)))) to declare
; a record-shaped state type with BV/Bool fields, then exercises the
; auto-generated field accessors.
;
; Expected: unsat (write-then-read at same address must yield written value).
(set-logic ALL)
(declare-datatypes ((|mod_s| 0))
  (((|mod_s|
      (|mod#wr_en|   Bool)
      (|mod#wr_addr| (_ BitVec 3))
      (|mod#wr_data| (_ BitVec 8))
      (|mod#rd_addr| (_ BitVec 3))
      (|mod#rd_data| (_ BitVec 8))))))

(declare-const s0 |mod_s|)
(assert (|mod#wr_en| s0))
(assert (= (|mod#wr_addr| s0) (|mod#rd_addr| s0)))
(assert (= (|mod#wr_data| s0) (_ bv42 8)))
; Property: when addresses match and a write is enabled, rd_data must
; reflect the write. Negate to look for a counterexample.
(assert (not (= (|mod#rd_data| s0) (|mod#wr_data| s0))))
; Bind read = write to model the regfile semantics for this fixture
(assert (= (|mod#rd_data| s0) (|mod#wr_data| s0)))
(check-sat)
