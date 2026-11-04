# S54: LOD and cross-fade

Wave 5. Research: `docs/forge-meshopt-analysis/lod-crossfade.md` (the design; read it in full),
`jasper-render.md` §4, `meshopt-api.md` (LOD findings) and `jasper-ux-human.md` §6.3-6.4.
Works with S53 (baked levels, contracts C1 and C2) and S55 (per-instance fade, C3).

## 1. Goal

LOD works out of the box and never pops. New projects have LOD on. Levels come pre-built from the
model cook, so phones and browsers no longer simplify meshes at every launch. Level switches
cross-fade with a dithered screen-door transition that costs one extra draw per fading submesh and
keeps early-Z for everything else. Users see what LOD is doing: a LOD Level draw mode, LOD numbers
in Stats, and a contact sheet of the levels. Agents can capture and verify all of it.

## 2. Scope

In scope:
- **LOD defaults:** on in new projects.
- **Selection:** fixes, hysteresis, and a culled level ("Cull below").
- **Levels:** baked-level prefill, shadow LOD, instanced-mesh LOD, and skinned-mesh eligibility.
- **Dithered cross-fade.**
- **Scene view:** Draw Modes for Wireframe, Shaded Wireframe, LOD Level and Overdraw.
- **Surfaces:** Stats overlay lines, `capture_model_lods` (MCP), Lua.

Out of scope:
- Geomorphing (rejected, `lod-crossfade.md` §2.6).
- Alpha-blended fades and Unity's SpeedTree mode.
- Cluster LOD (`visibility-feasibility.md` E).

## 3. Current state

All of the following is verified in `lod-crossfade.md` §1 and `jasper-render.md` §4:
- **Builds:** `MeshLodPool` builds up to 4 index levels on a job at every load.
- **Selection:** stateless projected error (`RenderSystem.cpp` `selectMeshLod` ~8222), with no
  hysteresis and no fade.
- **Defaults:** scenes default `meshLodEnabled: false`, and every template ships it off
  (`editor/resources/templates/*/Main.scene`).
- **Shadows:** they select by the light's own projection and snap.
- **Bugs:**
  1. The level `error` includes attribute cost (`MeshLodPool.cpp:160-171`) but is documented and
     used as geometric deviation.
  2. A UV-only mesh gets the normal weights (`ATTRIBUTE_WEIGHTS`, `:20`).
  3. A mesh can stay LOD-pending forever when its source is refused, or after an editor
     `clearUnused` between job finish and poll (`RenderSystem.cpp:7258,7264,8168-8172`,
     `MeshLodPool.cpp:215`).
  4. `target_error` is `FLT_MAX`, so levels have no error budget.

## 4. Reference

- **Unity:** LOD Group (Fade Mode, Animate Cross-fading, Fade Transition Width, the cross-fade
  animation duration, Culled).
- **Unreal:** dithered LOD transition (0.5 s).
- **Godot and Bevy:** visibility-range dither.

Details and citations are in `lod-crossfade.md` §2.

## 5. Requirements

### Functional

- **R1. Defaults.**
  - Every new-project template sets `meshLodEnabled: true`.
  - The engine default for a scene created at runtime is on.
  - Existing scenes keep their saved value. The S53 Audit window offers "Turn on LODs in this
    scene" as a one-click, undoable fix.
- **R2. Builder fixes (contract C2).**
  - `MeshLodPool::buildLevels(source, recipe)` is a pure function used by both the runtime and the
    cook, versioned by `kMeshLodRecipe`.
  - `error` is the geometric deviation: the position-only error from the simplifier, or a sampled
    deviation, never the attribute-weighted one.
  - The attribute weights follow the attributes present.
  - `target_error` is capped per Reduction (S53 R10) through the recipe. Use `SimplifyErrorClamped`
    and `SimplifyPermissive` where they help (meshoptimizer v1.3).
  - Skinned meshes qualify, using `SimplifyRegularize` and keeping bone-weight attributes.
- **R3. Baked levels.** A cooked glb's `JE_mesh_lods` (C1) pre-fills the pool when `recipe`
  matches, with no runtime simplify. Otherwise the runtime builds the levels as today. A test
  proves no simplify job runs for cooked models.
- **R4. Pending can't stick.** Every refused, failed or cleared build resolves to "no LODs" or
  re-queues. `hasPendingMeshLods` always becomes false. This needs red tests for both cases.
- **R5. Selection.**
  - Screen-space error against the scene's threshold, times the mesh's LOD Bias, times the
    project-wide `lodBias` from `QualityResolver` (C4).
  - 10% hysteresis before returning to a finer level.
  - **Cull below:** a per-mesh "Cull Below Screen Height" (0 = never, Unity's Culled). The mesh is
    not drawn when its projected height falls under it, and it fades out when cross-fade is on.
- **R6. Cross-fade** as designed in `lod-crossfade.md` §4:
  - **Pattern:** an 8x8 Bayer screen-door pattern computed in ALU, with complementary keep sets
    from a shared k in 1..63, so every pixel is drawn by exactly one level.
  - **Variant:** a `USE_LOD_FADE` mesh variant with the discard at the end. It is used only by
    fading draws, which are drawn after the other opaque draws; all other draws keep their
    pipeline and early-Z.
  - **Timing:**
    - Time-based (Animate Cross-fading) by default, with the scene Cross-fade Duration of
      0.5 s.
    - Distance-based (Fade Transition Width 0.1) when Animate Cross-fading is off, and always for
      instanced meshes, carried per instance through S55's `lodFade` (C3).
  - **The fade snaps** when:
    - the mesh wasn't drawn last frame,
    - there was a camera cut (the error budget changes 2x in a frame),
    - the mesh is under 8 px tall,
    - the fade variant isn't ready (S32), or
    - the material uses a custom shader without the fade include.
  - **Clocks:** the fade clock is game time in Play, and editor time in Edit mode (the Scene view
    fades too).
- **R7. Passes.**
  - The depth pre-pass skips fading draws.
  - SSAO, scene-depth and G-buffer passes draw the dominant level.
  - Shadows snap, using the shadow LOD: the light's projected error times
    `quality.shadow_lod_bias`, which S55 sets (default one coarser level).
  - Picking uses LOD 0. Point-light faces, probes and render-to-texture cameras use the shadow or
    dominant rule, never all levels.
- **R8. Instanced meshes.** Instances select a level per instance, by distance band, and draw one
  bucket per level (as today). Fades are per instance through `lodFade`.
- **R9. Settings (Unity names).**
  - **Mesh component (LOD group):** Fade Mode (None, Cross Fade; default Cross Fade), Animate
    Cross-fading (on), Fade Transition Width (0.1), Cull Below Screen Height (0), plus the
    existing LOD Enabled and LOD Bias.
  - **Scene settings:** Cross Fade on/off (default on), Cross-fade Duration (0.5 s), plus the
    existing Mesh LOD and Threshold.
  - All edits are undoable. Tooltips state the cost: "fading objects draw twice for the
    duration".
- **R10. Zero when off.** With Cross Fade off on every loaded scene and mesh, no fade variant is
  collected, compiled or shipped (the S32 warm-up list and the export variant collector), and no
  extra draw happens. A test checks the variant list.

### Editor UX (human)

- **R11. Scene view Draw Mode** dropdown in the Scene view toolbar:
  - **Shaded:** today's view.
  - **Wireframe** and **Shaded Wireframe:** editor-only, through a line-list index buffer built on
    demand per visible mesh, since sokol has no fill mode.
  - **LOD Level:** tints by level. The colours are shared with S53's LOD bar: LOD 0 green, 1
    blue, 2 amber, 3 red, and fading draws striped.
  - **Overdraw:** additive, a heat ramp.
  - The mode is saved in the editor layout, and costs nothing in games.
- **R12. Stats overlay** (Game and Scene view):
  - Vertices, an LOD histogram ("LOD 0: 120, 1: 40, 2: 8, 3: 2"), objects fading this frame,
    culled-by-size count and mesh memory.
  - S55 adds draws and instances.
- **R13. Model Import inspector integration** (S53 R16): the inspector's Force LOD slider and the
  LOD table read the same levels and errors. "Switches below" uses R5 with the inspector's
  reference camera.

### Agent UX (MCP, Lua)

- **R14. `capture_model_lods`** (group `meshes`, read-only, allowed while playing):
  - **Parameters:** `path` (a model) or `entity`, `levels` (default all), `size`, `save_path`.
  - **Model path:** renders a CPU contact sheet of each level (Lambert plus base colour).
  - **Entity:** renders through the real renderer with Force LOD per level.
  - **Returns:** the image, plus per level its triangles, error (mesh units), sampled deviation in
    world units at the entity's scale, the screen height where it switches, and the PSNR
    against LOD 0.
- **R15. Output-only additions:**
  - `get_render_stats`: `lod_histogram`, `fading`, `culled_by_size` and `vertices`.
  - Scene and entity property tools already cover the R9 fields; make sure they are reachable and
    documented.
- **R16. Lua.**
  - `Mesh.fadeMode`, `Mesh.animateCrossFade`, `Mesh.fadeTransitionWidth`, `Mesh.cullBelow`,
    `Mesh.forceLod(level)` (-1 = automatic) and `Mesh.currentLod` (read-only).
  - `Scene.crossFade` and `Scene.crossFadeDuration`.
  - Documented in `docs/scripting.md`.
- **R17. Docs:** `docs/lod.md` (user docs), plus the `docs/mcp/guide/meshes.md` "LOD" section.

### Performance

- **R18. Per-frame cost:**
  - Selection plus fade state stays under 0.05 ms per 1,000 LOD meshes on the linux build (bench
    test).
  - A fading submesh costs one extra draw.
  - There is no per-frame allocation.

### Data and compatibility

- **R19. Old scenes load unchanged.** New fields get defaults, with no version bump unless the
  serializer needs one; if it does, add a migration. Old scenes with LOD off stay off.

## 6. Design constraints and decisions

- **The fade variant** keeps the base shader's uniform and texture slot layout, validated at load.
  A mismatch disables fading for that shader with a one-time warning.
- **Fading draws sort after non-fading opaque draws** in the opaque queue. S55's sort key reserves
  the bit.
- **One shared Bayer pattern,** not blue noise: it needs no texture slot, and its complementarity
  is exact.

## 7. Acceptance criteria

The tests listed in `lod-crossfade.md` §5 are the base set, with the red controls given there. In
addition:

| AC | Verification |
|---|---|
| AC1 | `LodFadeTest`: complementarity (every pixel drawn exactly once for all k), the state machine, and hysteresis (one fade over 120 oscillating frames; red with hysteresis 0). |
| AC2 | GPU goldens mid-fade at a fixed time on glcore and Vulkan, with a non-fading control whose pipeline has no discard (variant-key assertion). |
| AC3 | `MeshLodPoolTest`: the R2 bug fixes, each red on `main`; skinned qualifies; error is geometric (≤ sampled max deviation × 1.05). |
| AC4 | `MeshLodPendingTest`: R4, red on `main`. |
| AC5 | Baked prefill: no simplify job for a cooked model (with S53 merged). |
| AC6 | Zero-cost variant-list test (R10). |
| AC7 | MCP `test_meshes.py::capture_model_lods`, plus the stats fields. |
| AC8 | Screenshots of each Draw Mode and Stats from an isolated editor. |
| AC9 | Templates: a new project has LOD on (MCP create-project test). |
| AC10 | Web and android build, and the smoke test passes. |

## 8. Dependencies and conflicts

- **Doriax catch-up overlap:**
  - `render-correctness` adds foliage Near Fade and Min Draw Distance scale fades on instanced
    meshes. S54's per-instance LOD fade composes with it: both multiply.
  - `draw-perf` makes LOD polling free when scene LOD is off. S54 keeps that property for scenes
    that turn it off.
  - Shader variant bits are taken as the next free bit at landing, in landing order (the
    `USE_LOD_FADE` bit number is a name, not a reservation).

- **Depends on** S53 (C1, C2), S55 (C3), S56 (C4) and S32.
- **Hotspots:** `RenderSystem.cpp`, `RenderSystemShadows.cpp`, `mesh.frag`, `surface_vs.glsl`,
  `ShaderPool.cpp`, `MeshComponent.h`, `SceneSettings.h` and `InspectorWindow.cpp`.

## 9. Risks and defaults

- **Instanced fade:** S55's `lodFade` slot replaces the report's "matrix w" idea.
- **Shading cost:** the discarded pixels of a fading object are still shaded, which is accepted
  for 0.5 s per change.

## Decisions

Made by the implementer under the owner's product authority (brief: "pick what a Unity user
would expect, the safest default and the best UX").

- **Level error (R2).** A level's `error` is a sampled symmetric deviation: every source vertex and
  triangle centre against the level's surface, every level triangle's centre and edge midpoints
  against the source's, through a uniform triangle grid (exact nearest distances). Errors never
  shrink with the level, since selection walks the levels until one exceeds the budget.
- **Builder recipe (C2).** `MeshLodRecipe {levels, ratio, targetError, permissive}`, with
  `MeshLodRecipe::forReduction`: Preserve Detail 0.6 a level within 1 % of the mesh's size,
  Balanced 0.5 within 2 % (the runtime's), Smallest 0.35 within 5 % and permissive. Weights: normal
  0.5, UV 1.0, bone weights 1.0, each only when present; no `SimplifyErrorClamped` (selection reads the
  geometric error, so the budget is what keeps a flat mesh's UVs and normals, whose position error is none); a
  level that stalls on seams retries with `SimplifyPermissive`, UV seams protected;
  `SimplifyRegularize` for skinned meshes. `kMeshLodRecipe` is 2. `MeshLodSource` now names its
  attributes (`normals`, `uvs`, `skinWeights`) instead of one packed array, which is what caused
  bug 2: S53's cook fills those fields.
- **Who gets levels.** 3D triangle meshes of the built-in Mesh or Lit shader and depth program.
  Sprites, tilemaps and polygons (pixel-exact 2D), terrains (CDLOD) and meshes drawn with a fork or
  library shader keep LOD 0: a custom vertex stage can move the vertices (library water, wind,
  outlines), so a geometric error says nothing there; with LOD on by default a flat water plane
  would otherwise draw two triangles.
- **Fade value and lanes (C3).** A draw's fade is `-k/64` for the level faded from and
  `(64-k)/64` for the level faded to (negative fades out, positive fades in; the magnitude is the
  share of pixels it keeps). It travels in lanes no other reader uses, so the fade variant has its
  base's uniform blocks, textures and vertex inputs exactly (checked at load and by
  `LodFadeVariants.CompileForEveryBackendWithTheBaseBindings`): `pbrParams.normalMatrix[0].w`
  for a single draw (the same lane S55's batches carry it in, `i_objectNormal0.w`), and the first
  column's w of the instance matrix for an Instanced Mesh's fade copies. No new uniform block,
  stream or layout.
- **Instanced meshes (R8).** The main view's instance list holds, after its plain buckets, the
  copies of the instances inside a level's band (once per level), dominant copies first per
  level. The opaque queue has two entries for such a mesh: its plain draws in place, its fade
  buckets after the non-fading opaque draws. The depth passes draw plain and dominant copies; the
  depth pre-pass the plain ones only.
- **Cull Below Screen Height** is a share of the view's height (Unity's relative screen height),
  measured from the world bounds' largest side. A culled mesh still casts its shadow.
- **Fade clock.** `Engine::getUnscaledTime` while the scene runs (game time: holds while paused,
  exact with a fixed frame delta), `HtmlUIClock` in Edit mode. A frame's step is at most 0.25 s.
- **Snaps beyond the spec's list:** a forced level (`Mesh.forceLod`), and a change of the mesh's
  level set (its levels arriving or being rebuilt), so a fade never runs between levels that are
  no longer the ones it started with. The GPU test harness also waits for pending levels.
- **Shadow LOD (R7).** The light's error budget is two shadow texels times `SHADOW_LOD_COARSER`
  (2, about one level coarser than the camera's rule) over S56's `QualityResolver::shadowLodBias()`
  (default 1, above 1 keeps detail as LOD Bias does);
  point-light faces now select levels with a light-relative view like projective slots;
  render-to-texture and mirror cameras select one level for their own view; probe captures keep
  LOD 0 (one level, the best for a capture that is kept).
- **Quality values (C4).** LOD selection multiplies `QualityResolver::lodBias()` (S56: the project's
  `quality.lod_bias` times the device tier's) into each mesh's bias once, through `LodView::bias`;
  shadows divide by `shadowLodBias()`.
- **Draw Modes (R11).** Saved per scene with the workspace (`drawMode`, `showStats`), driven by MCP
  through `set_scene_camera {display: {draw_mode, show_stats}}`. Colours LOD 0 green, 1 blue,
  2 amber, 3 red (`lodLevelColor`, for S53's LOD bar too).
- **Stats (R12).** `vertices` counts the indices (or vertices) the draws run, times their
  instances; mesh memory is the GPU allocator's Meshes category.
- **Editor views** keep their own fade state per entity, so the Scene and Game views never fight
  over one mesh's fade.
- **Old scenes (R19).** A scene saved without `meshLodEnabled` loads with LOD off; a cooked scene
  of format 1.0 loads with the cross-fade defaults (format 1.1 adds the two settings at the end).

## Deviations

- **R1, Audit window fix:** the Audit window is S53's; the scene setting it would turn on is the
  undoable `mesh_lod_enabled` scene setting (Inspector, `set_scene_settings`).
- **R6, §4.2 shader:** the design's `u_fs_lodFade` block and per-instance matrix trick were replaced
  by the lanes above (see Decisions), which keep every base binding and need no extra uniform
  apply.
- **R7, probes:** reflection probe captures draw LOD 0, not a selected level.
- **R11, Wireframe:** the wires are the editor's line overlay, built on the CPU per visible
  submesh and level (cached, released when the mode is left, at most 250,000 edges nearest
  first) instead of a GPU line-list index buffer: sokol fixes the primitive type per pipeline, so
  a GPU path needed a second object per submesh. Skinned, morphed and deformed meshes show their
  rest pose's wires.
- **R11, Overdraw:** each mesh layer adds a constant dark orange (additive, no depth test or cull);
  the sky is left out, the clear colour stays.
- **R13:** S53's inspector reads the same levels; S54 provides `lodSwitchHeight` and
  `lodLevelColor` for it.
- **R14:** a model path reads glTF and GLB; OBJ and FBX are captured through an entity.
- **AC2 on Vulkan:** the mid-fade golden is compared per 8x8 tile, since the dither's phase is the
  backend's pixel origin (§4.1).
- **R2, `SimplifyErrorClamped`:** not used. It scales the attribute error down to the position
  error, which is none on a flat surface; with selection on the geometric error, the clamped
  budget would let a flat mesh lose its texture mapping at every distance. `SimplifyPermissive` is
  used where it helps (a level stalled on seams, and the Smallest reduction).
