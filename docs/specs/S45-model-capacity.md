# S45 — Models load whole: no primitive or material is dropped

**Depends on.** BF19, S30/S34 (prebuilt players, capacities). Touches `engine/core/subsystem/MeshSystem.cpp`, `engine/core/util/`
(`HybridArray`), OBJ loader, `editor/Exporter*.cpp`.

## 1. Goal
A model looks the same in every build and on every thread. Today a many-node glTF loaded on a game's loading thread in a
shipped player is baked into one mesh and everything past `MAX_SUBMESHES` (16) is dropped with one warning; an OBJ with
too many materials is cut off with an error. Users expect the model they imported to appear whole. This is the bar in Unity.

## 2. Requirements
- R1. Red tests: a many-node glTF loaded off the main thread in a build with fixed capacities (the shipped-player case), and
  an OBJ with more materials than `MAX_SUBMESHES`.
- R2. Loading on a game's loading thread gives the same hierarchy and pixels as loading on the main thread. The bake into
  one mesh is no longer a lossy fallback: either the node tree is prepared off-thread and committed on the main thread, or
  an equivalent. Choose the simplest design that is correct on all backends.
- R3. No fixed capacity silently loses geometry. A mesh's submeshes can exceed `MAX_SUBMESHES` in every build (heap
  spill past the inline capacity is acceptable; the inline case stays allocation-free and costs nothing more). Same for
  `MAX_EXTERNAL_BUFFERS` where a model can hit it. An export's capacity table stays valid; prebuilt players need no rebuild
  per project for this.
- R4. Where a real limit remains (GPU, format), the error names the model, the count and the limit, and the editor shows it
  at import, not only at runtime.
- R5. Budget: a project with no large models keeps its player size and frame cost, within S34's and S07's budgets.
- R6. Old scenes and generated code keep working (`engine/project` template, prebuilt player variants).

## 3. Acceptance
- AC1. R1 red-then-green, on the Linux prebuilt player and the web player at least.
- AC2. Screenshot comparison: same model main thread vs loading thread, within golden tolerance.
- AC3. Vulkan and GLES checked through existing lanes. S34's size check unchanged or within budget.
- AC4. `tools/check-changed`, smoke test, docs.

## Deviations
- D1. Design (R2's "simplest design"). A game's loading thread owns a scene the frames do not run, so it runs the main thread's
  code on it (the same entities, no commit step). For a scene the frames do run, it only parses the file into `ModelPool`
  and the main thread makes the tree in a coming frame (`MeshSystem::prepareLoadOnMainThread`, `MainThreadQueue`): `loadModel`
  returns true before those entities exist. Only a thumbnail (off the main thread with `setImageDecodeMaxDimension`) still
  bakes the nodes into one mesh, now with nothing left out, because a picture needs no entities.
- D2. R3 by a new `SpillArray` in `util/HybridArray.h`, not a change to `HybridArray`: bones, tiles and sprite frames keep their
  fixed arrays and their size. An index access in the inline case costs a branch more than `values[i]`: "costs nothing more"
  holds for allocations, not for that branch (measured in the report of this stream; within S07's budget).
- D3. A compiled export still raises `MAX_SUBMESHES` and `MAX_EXTERNAL_BUFFERS` from its scenes (the capacity table stays valid,
  R3), which now only moves more of a mesh inline; the prebuilt players ignore those two and build method no longer takes them
  as a reason to compile.
- D4. R4's real limits are a skin's bones (`MAX_BONES`: not a GPU limit but a build's, it remains because the spec keeps it out
  of R3's list and a compiled export raises it), a primitive's blend shapes (vertex inputs) and a file's vertex and index
  buffers (the GPU buffer pool). The editor shows them in the Console when the asset database first sees or re-reads a model
  or an FBX is converted, and agents get `model_limits` from `import_files` and `get_asset_info`. No modal dialog.
- D5. AC1 on the players: Lua has no thread, so a script reaches this path with `Engine.startAsyncThread` on the main thread
  (`tools/ci/test_player_cli.py --only model-capacity`). A real second thread is tested in `jasperengine-core-tests`, a build of
  the same fixed capacities (`ModelCapacityGpuTest`). In the single-thread web player `startAsyncThread` is a warning and the
  load is an ordinary one.

