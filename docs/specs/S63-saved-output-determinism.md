# S63 — Saved files do not depend on what was drawn or loaded

**Depends on.** BF20 (the same class of bug), S03. Touches `editor/Factory.cpp`, `engine/core/subsystem/AtmosphereRenderer.cpp`,
serialization and reflection.

## 1. Goal
Saving a scene twice gives the same bytes whether or not its textures finished loading or a frame was drawn between
saves. Generated code and scene files hold authored data only. Two known cases remain, and the spec adds a guard so the
next one is caught by a test.

## 2. Requirements
- R1. Sprite frames: `framesRect` is normalised only once the texture has loaded (`Factory.cpp`, `Sprite.cpp`), so generated
  code differs by load timing. After the fix the authored value is what is stored and generated, whatever the load state,
  and rendering gives the same pixels. Old files load unchanged.
- R2. Clouds: `AtmosphereRenderer::weatherTexture` (`:726`) writes `TextureWrap::REPEAT` into the component's texture during a
  draw, so a saved file depends on draw history. After the fix the wrap is applied at bind or sampler level or at decode, so
  a component's saved `weatherMap` never changes because of a draw, while clouds still tile.
- R3. Guard: an MCP or unit test opens each scene of a representative fixture set, saves, draws frames, waits for assets,
  saves again, and compares scene files and generated sources byte for byte. Any further case it finds is fixed here, or
  it is listed with a stated reason in Deviations; nothing is left silent.
- R4. No format bump unless needed; if needed, a migration per S03.

## 3. Acceptance
- AC1. Red-then-green tests for R1 and R2, each failing without its fix.
- AC2. R3's guard runs in the MCP suite and in `tools/check-changed` for serialization and factory changes.
- AC3. Rendering goldens unchanged for sprites and clouds. `tools/check-changed` green.

## Deviations

The guard (`tools/mcp/tests/test_saved_output_determinism.py`) compares 45 scenes of `tools/fixtures`. On the code before this
work it found these cases beyond R1 and R2, all fixed here:

- An auto-resizing camera's clips and aspect were the size of the view that drew last. A document and the generated code leave
  them out for every such camera (the rule was the Default Camera's in the code only). New reflection field `derivedIf`.
- A Light's `worldDirection` and a Mesh's or UI's `worldAABB` are no longer written (still read when a file has them).
- A Terrain's `ranges` under Auto Ranges follow the camera's far clip: not written and not generated. Ranges set by hand stay.
- A Terrain's height and blend maps had their wrap (and the height map its `releaseDataAfterLoad`) written by its first draw, the
  same bug as R2. Unlike the weather map these settings are not authored choices: the terrain needs them, so
  `MeshSystem::applyTerrainMapSettings` applies them on decode and before a save as well as on draw. A file saying otherwise
  re-saves with them, drawn or not. It draws the same: the draw forced them before binding before this work as well, and
  `test_terrain_maps_draw_the_same_whatever_wrap_an_old_file_says` shows Clamp and Repeat files give the same image.
- A Terrain has its two submeshes only after its first run: a save writes two either way, and `maxSubmeshes` counts them.
- A mesh a Sprite, Tilemap, Mesh Polygon, Terrain or Spline Extrude generates: its vertex counts and bounds are not written, and
  its geometry and attributes are not generated (the file never held them, the code held them when a frame had made them).
- `mesh.vertexCount` and a rigidbody's `needReloadBody`, `needUpdateColliders` and `newBody` are runtime state, no longer generated.
- A 3D scene with no Camera generates the Scene view's camera, whose field of view and clips came from its first draw. They are
  applied before a save (`Project::applyFirstDrawSettings`).

The Play snapshot is not a file: Stop puts a kept Mesh's `worldAABB` back from it, so it still holds what a frame computes
(`Stream::encodePlaySnapshot`, `reflect::PlaySnapshotEncodeScope`).

`Compat.CorpusRoundTrip` leaves out each of these only under the rule by which a save does, and compares Terrain scenes
(`Compat.TheFilterHidesOnlyWhatASaveLeavesOut` holds the filter to that). `mesh.vertexCount` is not in a file (only a submesh's is), and a
load sets it from the buffer when it is 0 (`RenderSystem`, `loadMesh`), so a game from a file and one from generated code start alike.

R4: no format bump. Everything left out is read when an old file has it and recomputed otherwise, so old files load unchanged.

What stays different, with the reason:

- **A UI element with an anchor preset.** The layout derives its anchors, size and position from the preset and the canvas, and
  those are saved so the element has a size before its first layout. A file that disagrees with the preset (written by hand, or
  saved under another canvas size) changes on its first draw, then stays the same. `AllComponents.scene` held random values there,
  so the fixture now holds what the preset gives; the goldens already did.
- **A model's submeshes without an override.** The file holds a copy of what the model gives, which the model's load replaces. A
  file older than the model's current contents changes on its first load, then stays the same. `Lobes.scene` held image ids of an
  old import with a `/tmp` path, so the fixture now holds the paths its golden has.
- **A 3D scene with no Camera whose Scene view is left Orthographic** generates the view's clips, which follow the shape of the view.
  It is the view's own state, not scene data; a scene with a Camera, or a perspective view, does not depend on the view.

`test_reflection.py`'s goldens stay the capture from before S08, as its header requires: the lines above are taken out of a golden
before the comparison (`scene_golden_as_now`, `code_golden_as_now`), not regenerated.

The guard reopens each scene from its file, as R3 says. A scene created and saved in one batch, never drawn, is not compared.
