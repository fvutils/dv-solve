; get-option on options we keep
; expect: true false
; exit: 2
(set-option :produce-models true)
(get-option :produce-models)
(get-option :print-success)
