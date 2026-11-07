# S65 — Models load whole: no primitive or material is dropped

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
- R3. No fixed capacity silently loses geometry. The Doriax catch-up program's `dc/mesh-memory`
  (`docs/superpowers/specs/2026-11-04-mesh-memory-design.md`) removes the capacities themselves, so this spec does not. It owns R1's
  tests and R2 and lands on top of mesh-memory, or merges with it if it lands first.
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
  bakes the nodes into one mesh, because a picture needs no entities; with `dc/mesh-memory`'s growing arrays it leaves
  nothing out (`ModelCapacityGpu.APreviewOffTheMainThreadKeepsEveryPrimitive`, which loads as a preview for this reason).
- D2. R1's OBJ test (`ModelLoadingThreadGpu.AnObjOfManyMaterialsLoadsWhole`, the OBJ line of `test_player_cli.py --only
  model-capacity`) was red while `MAX_SUBMESHES` (16) held a mesh, and is green on the merge of `dc/mesh-memory`, which removed
  the capacities. The merge dropped this spec's capacity report (the "bakes into one mesh ... left out" warning and its error
  in `MeshSystem::loadGLTF`): with no capacity there is nothing to report. `ModelCapacityGpuTest.cpp` is mesh-memory's.
- D3. R4's real limits here are a primitive's blend shapes (vertex inputs) and a file's vertex and index buffers (the GPU buffer
  pool). `MAX_BONES` is not one: the shader reads a palette of any size, and mesh-memory removes it. The editor shows them in the
  Console when the asset database first sees or re-reads a model or an FBX is converted. No modal dialog and no new MCP text: agents
  read them with `read_console`.
- D4. AC1 on the players: Lua has no thread, so a script reaches this path with `Engine.startAsyncThread` on the main thread. A real
  second thread is tested in `jasperengine-core-tests`, a build without the editor (`ModelLoadingThreadGpuTest`). In the
  single-thread web player `startAsyncThread` is a warning and the load is an ordinary one.
- D5. Fixed on the way (AC1 ran into it): `Engine.endAsyncThread` in a script's init hung the player, the commit waiting for a queue
  that the view's load starts on that same thread.
