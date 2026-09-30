; A word-aligned memory access that stays inside a 256-byte window.
(set-logic QF_BV)
(set-option :produce-models true)
(declare-const addr (_ BitVec 32))
(declare-const len  (_ BitVec 8))
(assert (= ((_ extract 1 0) addr) #b00))                     ; addr % 4 == 0
(assert (bvuge len #x01))                                    ; 1 <= len
(assert (bvule len #x10))                                    ;      len <= 16
(assert (bvult (bvadd addr ((_ zero_extend 24) len)) #x00000100))
(check-sat)
(get-value (addr len))
