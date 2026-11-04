# S51 — Cheap Mesh update flags (measure first)

**Depends on.** S07, S08. Touches `editor/Catalog.cpp:398`, `editor/Stream.cpp:5401`. Reference: sound-layer cleanup design §5.A.4 and
`test_cleanup_group_a.py::test_a3_stop_decodes_changed_meshes_cheaply`, in `je-wt/snd-ro`.

## 1. Goal
Stop, undo and prefab propagation pay `Catalog::getChangedUpdateFlags` for every Mesh decode over a live Mesh. On the old
branch it cost about 1.5 ms per call. Measure it on today's code. If it is still expensive, make it microseconds with the same results.

## 2. Requirements
- R1. Measure: per-call cost, and Stop time on a 600-box scene, on a quiet machine, on main. Record numbers in Deviations.
- R2. If a call costs more than 50 µs: fix it. Decide flags by comparing what they depend on rather than encoding whole components,
  returning exactly the same flags as before, proven by a test over many cases (old result vs new, using the existing code as oracle
  during the test's development and fixed cases afterwards).
- R3. Also check the same cost for other component types on the path. A Play that only moves a GameObject doesn't decode its Mesh (BF13 C-3).
- R4. If R1 shows it is already cheap: add the budget test and close with the evidence, without changing the code.

## 3. Acceptance
- AC1. A CPU-ratio test (Stop on 600 boxes vs a baseline) that fails if the cost returns, robust under load (CPU time, not wall clock).
- AC2. Same flags proven. `tools/check-changed` green.
