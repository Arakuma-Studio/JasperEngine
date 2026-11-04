# S71 — Cheap Mesh update flags (measure first)

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

## Deviations

**R1, measured on main before the change (server H, Release, CPU time; H was busy with other jobs, so CPU time and ratios in
the same editor stand in for a quiet machine, as the wave's brief allows).**
- `Catalog::getChangedUpdateFlags`: 1083 µs a call for a default Mesh (1752 properties: 16 submesh slots), so R2 applies.
  Other components with a default value: Layout Group 85 µs, the rest under 25 µs; with their lists filled, Rigidbody 56 µs,
  Terrain 55 µs (`ChangedUpdateFlagsBench.EveryComponentStaysUnderTheBudget` prints each one).
- Stop on 600 boxes, main-thread CPU: 1.50 s after a Play that changed each Mesh, 1.54 s after one that moved each box, 1.49 to
  1.79 s after one that changed nothing, 0.65 to 0.74 s rebuilding the 600 a Play destroyed (ratios 2.2, 2.27, 2.2).

**After.** 1.3 to 1.5 µs a call for a Mesh, under 2 µs for every other component. Stop, in the same conditions: 1.32 to 1.47 times
rebuilding the boxes after a Play that changed each Mesh, 0.95 to 1.04 after one that moved each. The test's own figures, totals of
five rounds on a server at load 25 to 50: 1.52 and 1.03 (red control, the old flags and every Mesh decoded: 2.2 and 2.3).
Nothing depends on the Mesh's field layout or submesh capacity (`dc/mesh-memory` changes both): the walk reads the reflection
tables and their array operations, and the tests size the slots through the same operations.

**Decisions.**
1. The spec calls the moved Mesh "BF13 C-3". BF13 did not port it: it is item 2.C3 of the sound-layer cleanup design
   (`restorePlayComponents` on the old branch), and on main every Stop decoded and reloaded every Mesh, whatever Play did. So R3's
   sentence was not yet true of the code, and is implemented here, with a narrower mechanism than the old branch's:
   `Stream::decodeEntity`/`decodeComponents` take `keepSameMeshes`, which only `Project::restorePlaySnapshot` sets. A live Mesh that
   encodes as the snapshot has it, but for `worldAABB`, takes the snapshot's `worldAABB` and is not decoded. This also keeps the Meshes
   of a Play that changed nothing.
2. R3 is fixed for every component instead of Mesh alone: `reflect::changedUpdateFlags` (`editor/reflect/Properties.cpp`) walks the
   reflection tables as `listStruct` does, reduced once per struct to what can add a flag. The reference's plan of Mesh offsets
   would have left Layout Group and the lists of the others as they were. Two additions to the tables: `FieldDesc::peek` reads an
   out-of-line value without making it, and `ComponentDesc::extraUpdateFlags` (none for a Model, whose bone mappings carry no
   flags) leaves extra properties unlisted when they cannot add anything. Script and Text Effect still list their extra
   properties, whose flags depend on the entry.
3. The test keeps the old code as a permanent oracle in `ChangedUpdateFlagsTest.cpp` beside the fixed cases the spec asks for, so a
   property added later is checked too. It changes every property of every component, with empty and filled lists, lists of different
   lengths, NaN, script properties that change type, and random pairs.
4. The flags are as before, so a Mesh decoded over a live one still always gets `Mesh_Reload | Mesh_Texture | Shader_Uniforms`:
   its Custom properties count as changed (R2 asks for the same flags). Undo and prefab propagation reload the Meshes they decode.
5. A cost of (1): the comparison encodes the live Mesh, which costs about what its decode costs. A moved Mesh pays the comparison
   instead of the decode, so Stop's CPU after a move is the same with or without it (1.0 times rebuilding; a red control with cheap flags
   and no comparison also gave 1.0). A changed Mesh pays both: 1.47 against that 1.0, about 0.6 ms a Mesh. What the comparison
   buys is no Mesh reload at the next frame. Under llvmpipe that reload did not show (the first Scene view screenshot after Stop
   cost 0.20 s of main-thread CPU either way). It was not measured on a real GPU driver. So AC1's CPU ratio cannot see a moved
   Mesh decoded with cheap flags (tried: a ratio of moved against changed Stops passed its red control under load). That is
   checked exactly instead: `UndoProjectTest.StopKeepsAMeshAPlayOnlyMoved`, through `Project::restorePlaySnapshot` with a box's geometry.
6. Decoding a Mesh over a live one no longer leaves its 16 slots with an allocated default extra pass and PBR block (reading them
   allocated them). Inferred harmless from reading the code: an unset value reads and compares as its defaults, and the Inspector's
   listing still allocates them, so nothing can depend on either state.
7. Outside the spec, because it blocked the gate: `CanvasGroupTest.PerformanceBudgets` skipped itself above a load of half the cores, and
   a skip fails `tools/check-changed`, so no change that selects it could pass on a busy server. Its wall-clock budgets now go through
   `perfSlack()`, as `AGENTS.md` asks.
