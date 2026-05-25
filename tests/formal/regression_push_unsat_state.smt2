; Regression for "push contradiction leaves stale state surviving pop" bug
; (Phase 10 item 2, originally "cv14 cover-mode false-UNSAT").
;
; Repro: outer asserts redundant + contradicting in a push so the inner
; check goes UNSAT during solver_solve (not at compile time — the AND/
; bit_slice propagators don't catch it until search-time BCP). Then pop
; and re-check: should be sat (only outer asserts remain), but dv
; returned unsat because two layers of state weren't restored cleanly.
;
; Root cause (two interacting bugs):
;
; 1. solver_solve at zsp_search.c:341 overwrites level_marks[0] to seal
;    "level-0 baseline" for its restarts/bounds_shave probing. Inside a
;    push scope, this leaves level_marks[m->decision_level] pointing
;    at a post-push state — and solver_restore's trail_backtrack stops
;    there instead of walking back to m->trail_top. Two pre-checkpoint
;    trail entries survive the pop, leaving bounds tightened.
;
; 2. trail_backtrack walks all watcher chains clearing PROP_FLAG_ENTAILED
;    so post-backtrack propagation can re-fire. But solver_restore only
;    NULL'd the prop_refs[] slots of post-cp propagators (didn't detach
;    them from watcher chains). Those stale post-cp props' var ids
;    might be reused for a new variable in the post-pop scope; clearing
;    their ENTAILED bit lets them fire against the wrong variable.
;
; Fixes (this commit):
; - solver_restore restores level_marks[m->decision_level] from the
;   saved checkpoint values (m->trail_top, m->trail_count) before
;   calling trail_backtrack.
; - trail_backtrack's watcher-chain ENTAILED-clear skips dead props
;   (identified by prop_refs[p->prop_id] != ref).
;
; Expected: 2 lines, "unsat" then "sat", exit 0.
(set-logic QF_BV)
(declare-fun u6 () (_ BitVec 1))
(declare-fun u7 () (_ BitVec 1))
(assert (= ((_ extract 0 0) u6) #b1))
(assert (= ((_ extract 0 0) u7) #b0))
(push 1)
;; AND of redundant (u6=1 already entailed) + contradicting (u7=1
;; vs outer u7=0). The redundant conjunct is the key — without it
;; the contradiction is caught at compile time and the buggy
;; codepath (solver_solve overwriting mark[0]) isn't reached.
(assert (and (= ((_ extract 0 0) u6) #b1) (= ((_ extract 0 0) u7) #b1)))
(check-sat)
(pop 1)
;; Declare a fresh var post-pop. Its var id (4) reuses an id that
;; was a failed-push aux during the inner compile, so any stale
;; propagator left in u26's would-be watcher chains would fire
;; against u26 as if it were the old aux. With the trail_backtrack
;; fix, stale props stay ENTAILED and don't trip.
(declare-fun u26 () (_ BitVec 1))
(assert (= u26 u6))
(push 1)
(assert (= ((_ extract 0 0) u26) #b1))
(check-sat)
