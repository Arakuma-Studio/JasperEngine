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
