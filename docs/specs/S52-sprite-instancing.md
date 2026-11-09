# S52 — 2D sorting, Sprite Batch, and pixel-art parameters inside Dynamic Batching

Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5A, re-scoped on 2026-11-07 onto the Doriax
catch-up program's `sprite-batching` (landed on `main` as `48cf20e75`, recorded in `885cddc46`; its
spec is `docs/superpowers/specs/2026-11-04-sprite-batching-design.md`, called "the batching spec"
below). The split is in `docs/doriax-catchup/LEDGER.md` ("Lead decisions across specs, Pixel-art
program"). Depends on S41 (`pixelSnapDraw`, `pixelSnapApply`, the `Pxs` bits; branch
`feature/s41-pixel-perfect-camera`) and works with S43 (Palette Swap row) and S48 (effect amounts, not
landed). Blocks S50 (tilemap chunks draw through §6.5's instance batches).
Read `README.md`, the ledger and the batching spec's Deviations first.

Evidence labels: **read** (I read the file at `main` `81ffe9f3d` or the named branch),
**inferred** (from reading code), **assumed**. Nothing here was built or run.

## 1. Goal

`main` now merges consecutive sprites, small meshes and UI quads into one draw (Dynamic Batching),
but a 2D game still has no draw order except the hierarchy, cannot draw thousands of bullets without
thousands of GameObjects, and loses batching as soon as it uses pixel-art features: a pixel-snapped
sprite, a `library:sprite` effect (hit flash, Palette Swap, S48's fades) or a palette row each draws
alone. S52 adds Unity's 2D sort controls (Sorting Layers, Order in Layer, Sorting Group, Transparency
Sort Axis) and feeds them into the batcher's sort buckets. It adds a Sprite Batch component that draws
many sprites from one GameObject with GPU instancing, through a bulk API. It also makes the pixel-art
per-sprite parameters batch-compatible through the batcher's hooks, with batched == unbatched pixel
for pixel.

## 2. Scope

In scope:
- **Sorting:** project Sorting Layers, per-renderer Sorting Layer and Order in Layer, the Sorting
  Group component, Transparency Sort Mode / Axis and the Sprite Sort Point, in 2D passes (no distance
  sort) and in the transparent queue of 3D passes. The sort feeds `BatchItem::sortKey`. Editor UI,
  undo, Lua, MCP through existing tools, exports, old-scene compatibility.
- **Sprite Batch:** a component on a Sprite GameObject that draws N instances of that Sprite's
  frames with one instanced draw per texture page, plus a bulk Lua/C++ API. It is also the retained
  instance batch S50's tilemap chunks use (§6.5).
- **Pixel-art parameters in batches:**
  - S41's pixel snap (`pixelSnapDraw` as a key section; later, a per-vertex pivot);
  - `library:sprite` admitted to the batcher (`@batchable`), with its per-sprite amounts (the
    S43 row, FLASH and the S48 slots) carried per vertex by an additive channel;
  - the frame rect OUTLINE and OVERLAY read, per vertex;
  - the same values per instance in Sprite Batch;
  - pixel-parity tests.

Out of scope. The landed batcher owns these, and S52 does not duplicate them:
- Dynamic batching itself: eligibility, keys, runs, baking, settled objects, lazy MVP, culling cache,
  content-shared immutable buffers, stream chunks, the deferred flush and lazy scissor, 2D reordering
  (REORDER mode) and depth-pass grouping (`engine/core/render/DrawBatcher.{h,cpp}`,
  `engine/core/subsystem/RenderSystemBatch.cpp`).
- Batch statistics and explanation: `get_render_stats` `batching` / `batch_breaks`, Game view Stats,
  Frame Debugger reasons, Profiler monitors, Project Settings > Graphics > Dynamic Batching,
  `Engine.dynamicBatching`. S52 only adds reasons to the existing enum (§6.4).
- Automatic GPU instancing of repeated meshes (S55).

Also out of scope (dropped from the old S52; see §6.8):
- the 40-byte instance format and its own vertex include;
- the lookahead reorder and frame coherence;
- a separate `batches` stats object;
- a Scene view "Sprite Batches" draw mode.

Also out of scope:
- Several textures per draw (texture arrays or sampler arrays): one page per draw. Atlases (S45)
  are how pages are shared.
- Compute, SSBOs, indirect draws (README do-not-copy and platform rules).
- Saving Sprite Batch instances in scenes: they are runtime data that scripts fill. Placed static
  art is S50's tilemap.
- Batching other library shaders: each can opt in later with `@batchable` and its own tests.

## 3. Current state

On `main` `81ffe9f3d`, read unless marked otherwise.

- **2D draw order is the hierarchy.**
  - A camera with `depthTest = false` (the default camera: `engine/core/Scene.cpp:172-173`) does
    not sort by distance (`sortsByDistance`, `RenderSystem.cpp:738-740`). It draws in Transform
    order: the loop at `RenderSystem.cpp:18262`, meshes through `submitMesh` (`:18372`), UI through
    `submitUI` (`:18380`), particles at `:18429`.
  - A distance-sorted pass has two queues: opaque (`OpaqueRenderComparison`,
    `RenderSystem.h:708-718`; drawn at `RenderSystem.cpp:18192-18200`) and transparent
    (`TransparentRenderComparison`, `RenderSystem.h:694-702`: `sortPriority`, then far to near,
    then submission order; sorted at `RenderSystem.cpp:18452`).
  - `sortPriority` exists on Line Renderer (`LineRendererComponent.h:115`), Trail Renderer
    (`TrailRendererComponent.h:77`), Points (`PointsComponent.h:95`) and the particle renderer
    (`ParticleEffect.h:456-457`, with `sortingFudge`).
  - There are no Sorting Layers, no Order in Layer, no Sorting Group and no sort axis (grep of
    `engine` and `editor`).
  - The Unity particle importer maps `m_SortingOrder` onto `sortPriority`
    (`editor/unity/UnityParticleImport.cpp:1478`).
- **The batcher's sort hook is unused.**
  - `BatchItem::sortKey` (`engine/core/render/DrawBatcher.h:144-151`; contract `:17-19`) is the
    draw's bucket. In REORDER mode a draw jumps back only over runs of its own bucket, and a run
    spanning buckets is never jumped over (`DrawBatcher.cpp:180-205`, equality only).
  - `submitMesh` / `submitUI` never set it (`RenderSystemBatch.cpp:707-790`, `:896-960`), so
    every draw is bucket 0. `BatchBreak::SORT_BUCKET` exists (`DrawBatcher.h:61`) but nothing
    produces it (**inferred**).
  - The colour pass uses REORDER for orthographic cameras along Z, else STRICT
    (`RenderSystem.cpp:18181-18188`).
- **What draws alone today.** `meshBatchForm` (`RenderSystemBatch.cpp:232-320`) refuses:
  - any manifest shader, `library:sprite` included (`submesh.manifest` →
    `INELIGIBLE_CUSTOM_SHADER`, `:254-256`);
  - any variant bit outside `BATCH_MESH_PROPERTIES` (`:41-63`; S41's `Pxs` is not in it).
  `uiBatchForm` allows UI bits 0-4 only (`:322-347`). The batching spec's Deviations, point 5,
  record that a pixel-snapped draw is drawn alone until S41 is wired in.
- **The per-vertex channel exists.**
  - The bake copies every vertex attribute it does not know as it is (`bakeMeshRecord`,
    `RenderSystemBatch.cpp:362-433`).
  - `setBatchAttributeRole(type, POINT | DIRECTION)` (`:145-153`) makes an attribute be placed
    like a position or a normal (`placeBatchRecord`, `:560-566`).
  - The key holds the pipeline id, origin cell, scissor and byte sections (`fillMeshKey`,
    `:599-660`): textures, colour factor, material params, alpha cutoff, UV sets, object lights.
  - The run draws through its head's `drawMesh` with `passBatchGeometry` (`RenderSystem.cpp:9955`).
    That path writes the head's custom uniforms (`writeCustomUniforms`, `RenderSystem.cpp:498`).
  - Documented for S52 in `docs/rendering/batching.md:86-97`.
- **`library:sprite`** (`engine/shaderlib/library/sprite.frag`, `sprite.vert`):
  - Every amount is a material uniform in `u_fs_customParams` (`sprite.frag:55-`; `flashAmount`
    `:21`, used `:217`).
  - With OUTLINE or OVERLAY the vertex stage passes the frame rect from the per-object
    `spriteParams.textureRect`, composed with `i_textureRect` under `HAS_INSTANCING`
    (`sprite.vert:26-37`).
  - On `feature/s43-palettes`, `paletteRow` is a plain property (`sprite.frag:37`, read as
    `customParams.paletteRow` at `:197`): no `instance=` yet.
- **S41 (branch, read).**
  - The vertex stages end with `gl_Position = pixelSnapApply(gl_Position, pbrParams.pixelSnapDraw.x,
    snapPivot)`. `snapPivot` is `mvpMatrix[3]` (the model origin), or `mvpMatrix * i_matrix_col4`
    under `HAS_INSTANCING` (`engine/shaderlib/mesh.vert:346-352`, the same in `surface_vs.glsl`,
    the depth stages and `ui.vert`).
  - The mode is per draw (`RenderSystem::pixelSnapDrawMode`, `RenderSystemPixelArt.cpp:369-373`):
    0 none, 1 every vertex (`pixelart::isGridAligned`), 2 the pivot. It is written into the
    object block by `applyObjectBlock` (`:375-386`).
  - Bits: `Pxs` = MESH 42, DEPTH 19, UI 5 on that branch, renumbered at landing (ledger rule).
- **Instancing.**
  - `InstancedMeshComponent` keeps an AoS `InstanceData` of about 140 bytes per instance
    (`InstancedMeshComponent.h:70-86`). It uploads `InstanceRenderData` (matrix, colour, texture
    rect: 96 bytes, `:88-92`) read as `i_matrix_col1..4`, `i_color`, `i_textureRect`
    (`surface_vs.glsl:175-186`; matrix rebuilt at `:380`, col1.w being S54's fade lane).
  - `library:sprite` supports it (`sprite.vert:33-37`). Lua adds instances one call at a time
    (`Mesh::addInstance`, `engine/core/object/Mesh.h:206-211`).
  - The batcher refuses entities with InstancedMeshComponent (`RenderSystemBatch.cpp:241`). S55
    does not auto-instance the sprites the batcher takes (S55 §10, read).
- **Settings and undo.**
  - Tags and Layers is a Project Settings category (`ProjectSettingsWindow.cpp:3218`; Layers
    foldout `:3289`) with rows `tags_and_layers.tags` / `.layers` (`SettingsRows.cpp:2803`,
    `:2844`). It has no undo command.
  - Other settings groups have one (`editor/command/type/DrawSubmissionSettingsCmd.h:15`).
- **Sprite geometry.** `MeshSystem::prepareSpriteMesh` (`MeshSystem.cpp:683-703`): POSITION,
  TEXCOORD1, NORMAL, TANGENT, COLOR, 64 bytes per vertex. The buffer is IMMUTABLE and shared by
  content.

## 4. Reference

| Source | What | Take | Improve |
|---|---|---|---|
| Unity 2D (behaviour only) | Sorting Layers (Tags and Layers), Renderer `sortingLayerID`/`sortingLayerName`/`sortingOrder`, Sorting Group, Graphics > Transparency Sort Mode / Axis, Sprite Renderer Sprite Sort Point | names, semantics, the sort precedence (layer, order, then distance or axis) | layers keep stable ids, so renaming or reordering never rewrites a scene |
| spryte (MIT, `/tmp/pixelart-research/spryte`) | `src/copies.ts:1-60`, `src/webgl2.ts:97-144`: bulk copies with optional per-copy streams | the bulk-array API shape (whole streams per call), optional streams that cost nothing when absent | streams are optional instance attributes on our existing instanced variant, not a new format |
| spryte | `browser-test/checks.ts:157-195` | the parity method: one scene drawn two ways, every pixel compared | 0 differing pixels, not a tolerance (§7) |
| The batching spec | Deviations "Hooks for pixel-art S52" 1-5 | the bucket, the per-vertex channel, the `pixelSnapDraw` key section | an additive per-vertex channel, so the vertex buffer never changes when a value animates (D5) |

## 5. Requirements

### Functional: sorting

- **R1 Sorting Layers.**
  - A project-wide ordered list of named layers with stable integer ids. "Default" (id 0) always
    exists and cannot be removed or renamed.
  - Drawing order follows list position (the layer's *value*), not the id. Renaming or reordering
    rewrites no scene or prefab.
- **R2 Renderer fields.**
  - `sortingLayer` (layer id, default 0) and `sortingOrder` (Order in Layer, int16, default 0) on
    every world renderer: MeshComponent (Sprite, MeshPolygon, Mesh, legacy Tilemap), Particle
    System renderer, Points, Line Renderer, Trail Renderer and Sprite Batch (it uses its Sprite's).
    S50's Tilemap Layer adds its own.
  - Sprite gets `spriteSortPoint` (`Center`, default as Unity, or `Pivot`).
- **R3 2D order.**
  - In a pass without distance sort, the hierarchy is cut into runs at UI subtrees (a
    RectTransform and everything under it). UI subtrees keep their place and their hierarchy
    order, because scissors, Canvas Groups and FLATTEN layers depend on it.
  - Inside a run, world renderers draw ordered by (layer value, Order in Layer, axis distance when
    the mode is Custom Axis, hierarchy index). With every field at its default and the mode
    Default, the order is today's exactly (AC1).
  - The scene-colour copy, video near planes and screen fluids still go before the first draw that
    needs them, in the new order.
- **R4 Transparency Sort Mode.** Project Settings > Graphics:
  - "Transparency Sort Mode": `Default` (no axis term) or `Custom Axis`;
  - "Transparency Sort Axis": a vec3, default (0, 1, 0), so a higher world y draws first (the
    top-down convention).
  The distance is `dot(axis, point)`, larger first. The point is the renderer's pivot, or its world
  bounds' centre when Sprite Sort Point is Center (for other renderers: the pivot). It applies to
  2D passes. Distance-sorted passes keep camera distance.
- **R5 Sorting Group.**
  - The component makes its subtree sort as one item at the group's Sorting Layer, Order in Layer
    and pivot. Inside, members sort among themselves by R3's key.
  - Groups nest. A disabled Sorting Group is transparent to sorting. A UI subtree inside a group
    stays a run boundary within the group.
- **R6 3D passes.**
  - The transparent queue orders by (layer value, Order in Layer, `sortPriority`, distance far
    first, submission order). A Sorting Group's members take the group's key and distance, then
    their own.
  - The opaque queue is unchanged (Unity's behaviour). Defaults give today's order.
- **R7 Batcher buckets.**
  - Each draw submitted to the batcher carries `BatchItem::sortKey` = its bucket (D2). Batching
    and REORDER work inside a bucket, as the landed contract says.
  - A bucket change that stops a join is counted as `sort_bucket` (the existing reason).
- **R8 Determinism.** The order is total (the hierarchy index breaks every tie). The same scene
  draws the same sequence on every run and backend.
- **R9 Disabled costs nothing.** A scene whose renderers all have default fields, with no enabled
  Sorting Group and mode Default, runs today's 2D loop unchanged. Its only extra cost is one flag
  computed in `RenderSystem::update` (D1).

### Functional: Sprite Batch

- **R10 Component.**
  - "Sprite Batch" (Add Component > Rendering, and GameObject > 2D Object > Sprite Batch) on a
    GameObject with a Sprite. It draws instances of that Sprite's frames: its texture or S44
    sheet, S45 atlas, frame names, pivot and material (any `library:sprite` features). The
    Sprite itself is then drawn only through its instances.
  - Fields:
    - `mode`: `Retained` (instances persist) or `Clear Each Frame` (what scripts add in a frame
      is drawn once);
    - `space`: `Local` (instances move with the GameObject) or `World`;
    - `capacity` (default 1024; the bulk calls grow it);
    - `sortInstances` (by the Transparency Sort Axis, else index order).
  - It cannot be added beside an InstancedMeshComponent: the error names the one to remove.
- **R11 Instances.**
  - An instance is a position (xyz), rotation (radians about z), scale (xy), frame index, colour
    (linear RGBA) and, optionally, the four FX slots (R19).
  - The frame gives size and pivot: the instance matches a Sprite showing that frame with that
    transform.
- **R12 Bulk API.**
  - Lua and C++ write whole streams from flat arrays: positions, rotations, scales, frames,
    colours, FX by slot name. Per-instance `add` / `set` / `remove` / `clear` cover small edits.
  - A call validates once (array length, frame range), not per element. An unknown frame name
    is a typed error at the call.
- **R13 Drawing.**
  - One instanced draw per run of consecutive instances on the same page, in index order (or
    sorted, R10).
  - It draws through the Sprite's own `HAS_INSTANCING` variant and its existing per-instance
    attributes. The only new attribute is the FX stream, present only once `setFx` was called.
  - It is one sort item (R3) at its Sprite's key and pivot. Its instances never interleave with
    other renderers.
- **R14 Uploads.**
  - Retained: the instance buffer is uploaded only when its content changed (a revision counter).
  - Clear Each Frame: instances are appended to the frame's stream (as particles do).
  - Nothing is packed or uploaded when the batch is culled, hidden or empty.
- **R15 Culling, bounds, picking.**
  - The batch is culled whole by its instances' bounds, refreshed when the instances change.
  - The Scene view picks it as one GameObject. `get_entity` reports `count`.
- **R16 Pixel snap per instance.**
  - Under an active Pixel Perfect Camera, each instance snaps by S41's rule. A grid-aligned
    instance (rotation a multiple of 90°, and the GameObject grid-aligned) snaps every vertex.
    Any other snaps its pivot.
  - Under Steps90 the packer rounds the rotation to 90° (S41 R10) and writes exact 0/±1.
  - The per-instance decision is in the instance matrix's exact zeros (D7). It costs no extra
    bytes.
- **R17 Static instance batches for S50.** The C++ type behind Sprite Batch (`SpriteInstanceBatch`,
  §6.5) can be owned by another producer: a retained buffer with its own model matrix, bounds and
  revision, drawn as one sort item. S50's chunks use it.

### Functional: pixel-art parameters in batches

- **R18 Pixel snap batches.**
  - S41's `Pxs` bits join the batcher's MESH and UI allowlists. Each draw's `pixelSnapDraw` mode
    is a key section (reason `pixel_snap`), so a run holds one mode. The run's draw uses its head's
    mode, and `pixelSnapApply` snaps each baked vertex.
  - Mode 1 (every vertex) batches from M2.
  - Mode 2 (pivot) batches once the sprite has a per-vertex pivot (R22). Until then, and always
    for UI, a mode-2 draw is drawn alone (`ineligible_pixel_snap_pivot`).
  - Steps90's rounded model matrix reaches the bake through `objectMatrices`, so both paths use it
    (AC6 checks).
- **R19 FX slots (family contract, kept).** Four floats, every neutral value 0:
  - `fx.x`: S43's Palette Swap row (an integer stored exactly; 0 = original colours);
  - `fx.y`: FLASH amount (`library:sprite`'s existing `flashAmount`);
  - `fx.z`: S48's DITHER_FADE amount;
  - `fx.w`: S48's DISSOLVE progress.
  A manifest property claims one with `instance=fx.<slot>`. S52 adds it to `flashAmount` and to
  S43's `paletteRow`. S48 adds `fx.z` and `fx.w` when it lands. Until then those two slots are
  carried and stay 0. A test shader proves the channel (AC8).
- **R20 Batchable manifest shaders.** A manifest shader with the line `// @batchable` passes
  `meshBatchForm` when its per-object inputs are covered:
  - textures: its `@texture` and `@palette` bindings join the `texture` key section;
  - uniform values: the custom parameter block, with the `instance=` fields zeroed, is a new key
    section (`custom_params`);
  - per-sprite amounts: the `instance=` fields travel per vertex (R21);
  - the frame rect: per vertex (R22).
  `library:sprite` declares it. A batchable variant that grows its quad in the vertex stage is not
  admitted: S48's grown quads must be built in the geometry (D6).
- **R21 Additive per-vertex FX channel.**
  - A sprite whose material enables a feature with an `instance=` property gets a FLOAT4
    attribute, `a_spriteFx`, written as zeros in its vertex buffer. The variant's `Sfx` bit reads
    it.
  - The shader reads each claimed amount as `uniform field + fx slot`:
    - drawn alone: the uniform carries the value and the attribute is 0;
    - batched: the bake writes the material's values into its copy of the attribute, and the
      run's draw writes those uniform fields as 0;
    - Sprite Batch with an FX stream: `i_spriteFx` carries them and the uniform fields are 0.
  - Changing an amount re-bakes the record on the CPU (it is compared every frame, like the
    colour factor). The GPU vertex buffer never changes for it. Neutral 0 makes the sum exact in
    float.
- **R22 Per-vertex frame rect and pivot.**
  - OUTLINE and OVERLAY sprites get a FLOAT4 attribute of the identity rect (0, 0, 1, 1)
    (`TEXTURERECT`, bit `Sfr`). The vertex stage composes `spriteParams.textureRect` with it, as it
    already composes `i_textureRect`. The bake writes the record's rect into it (a new role,
    `TEXTURE_RECT`), and the run draws with the whole rect: the composed rect is the same floats on
    both paths.
  - Pixel-snapped sprites under an active Pixel Perfect Camera get a FLOAT3 attribute `(0, 0, 0)`
    (`SNAPPIVOT`, POINT role, bit `Spv`). The snap pivot is `mvp * vec4(a_snapPivot, 1)`: the model
    origin drawn alone, the baked origin batched.
  - Without these attributes, the draws stay alone (`ineligible_frame_rect`,
    `ineligible_pixel_snap_pivot`).
- **R23 Parity.** For every case in AC5-AC9, an image drawn with Dynamic Batching on equals the
  image drawn with it off: 0 differing pixels on the reference renderers (glcore, Vulkan
  lavapipe), with geometry kept off pixel-centre edges where no Pixel Perfect Camera snaps it.

### Platform and backends

- **R24** All paths use vertex and fragment stages only, per-instance attributes with divisor 1,
  bindings at offsets (no base instance on GLES3/WebGL2), FLOAT attributes only, and flat varyings.
  No compute, SSBO, texture array or integer attribute. GL 3.3, GLES3/WebGL2, Metal, D3D11, Vulkan.
- **R25** The new variant bits (`Sfx`, `Sfr`, `Spv`) exist only for variants that need them. A
  project with no Pixel Perfect Camera and no `instance=` feature compiles no new variant. Exports
  carry the Sorting Layers table and the Transparency Sort settings, in both Compile and prebuilt.

### Performance

Measured on the bench machine and asserted through `perfSlack()` / `harness.over_budget` (counts
always). Mobile is a Pixel 6a / iPhone 12 class device and web is wasm in Chrome; mobile and web are
measured once by hand and recorded in `docs/specs/evidence/s52/`.

- **R26 Sorting.** 2,000 sprites in 10 layers with Custom Axis: the gather and sort add ≤ 0.2 ms
  desktop and ≤ 1.0 ms mobile. Defaults: no gather runs (R9, asserted by a counter).
- **R27 Sprite Batch.** 10,000 instances of one page, all moving every frame:
  - 1 draw;
  - pack and upload ≤ 0.5 ms desktop, ≤ 2 ms mobile;
  - 96 bytes per instance, + 16 with FX;
  - a static Retained batch uploads 0 bytes per frame.
  Lua `setPositions` of 10,000: ≤ 1.0 ms desktop, ≤ 3 ms mobile web.
- **R28 Pixel-art batching.** 1,000 snapped `library:sprite` sprites with FLASH animating on 100 of
  them (one sheet, mode 1): ≤ 2 colour-pass draws for them, and 0 GPU vertex-buffer writes caused by
  the flash (asserted by `Buffer::getWriteCount`).
- **R29** A draw that is batchable today costs what it costs now ± 5 % (the key gains at most one
  small section).

### Editor UX (human)

- **R30 Sorting Layers list.**
  - Project Settings > Tags and Layers > "Sorting Layers": add (+), rename inline, remove (−),
    drag to reorder. Default is locked.
  - Removing a layer still used in open scenes warns with the count; those renderers draw on
    Default.
  - Every edit is one undo step (`SortingLayersCmd`).
- **R31 Renderer rows.**
  - "Sorting Layer" (dropdown ending in "Add Sorting Layer...", which opens Tags and Layers) and
    "Order in Layer" on Sprite, Mesh Polygon, Mesh (Additional Settings), Particle System
    (Renderer), Points, Line Renderer and Trail Renderer (Additional Settings).
  - "Sprite Sort Point" on Sprite.
  - Multi-edit and undo work as for every property row.
- **R32 Sorting Group** in Add Component > Rendering: "Sorting Layer", "Order in Layer". Adding,
  removing and editing it are undoable.
- **R33 Sprite Batch Inspector.**
  - Rows: Mode, Space, Capacity, Sort Instances, "Instances" (read-only count).
  - When empty: "Filled by scripts: SpriteBatch:setPositions (docs/rendering/sprite-batch.md)".
  - The Sprite's Sorting rows apply to it.

### Agent UX (MCP, Lua, CLI)

- **R34 No new tool.**
  - Sorting Layers are the setting `tags_and_layers.sorting_layers` (an ordered list of names; ids
    kept by name) through `get_project_settings` / `set_project_settings`. The Graphics rows are
    `graphics.transparency_sort_mode` and `graphics.transparency_sort_axis`.
  - Renderer fields, Sorting Group and Sprite Batch are ordinary component properties for
    `add_component`, `set_properties` and `get_entity`. `sortingLayer` accepts a layer name or id,
    and reads back as `{id, name}`. An unknown name fails with the fix: "add it with
    set_project_settings tags_and_layers.sorting_layers".
- **R35 Stats.** `get_render_stats` keeps its schema. Its `batching.breaks` and `batch_breaks`
  gain the reasons of §6.4. `docs/mcp/guide/profiling.md` (Dynamic Batching section) names each
  with its fix.
- **R36 Guide.**
  - `docs/mcp/guide/rendering-2d.md` (`jasper://docs/rendering-2d`): the sort precedence, Sorting
    Groups, the axis, Sprite Batch recipes, and which pixel-art features batch.
  - One "Other areas" index entry in the agent guide: `rendering-2d`: `set_properties`,
    `jasper://docs/rendering-2d`.
  - Every new MCP text has an eval case (AC14).
- **R37 Lua (Unity names).**
  - `SortingLayer.layers`, `SortingLayer.nameToId(name)`, `SortingLayer.idToName(id)`,
    `SortingLayer.getLayerValue(id)`.
  - Renderer properties `sortingLayerId`, `sortingLayerName`, `sortingOrder` on `Sprite`, `Mesh`,
    `ParticleSystem`, `Points`, `LineRenderer`, `TrailRenderer`, and `SortingGroup`.
  - `SpriteBatch` (§6.6). C++ scripts get the same API.

### Data and compatibility

- **R38 No version bump.** Every scene key is new and optional (AGENTS.md): `sortingLayer`,
  `sortingOrder`, `spriteSortPoint`, the Sorting Group and the Sprite Batch components.
  - A pre-S52 scene fixture loads with defaults, draws in today's order and re-saves byte for
    byte when untouched (AC12).
  - A change that re-types or renames a stored key during implementation bumps the version with a
    migration in `editor/StreamMigrations.cpp`, a fixture in `editor/tests/fixtures/migrations/`
    and a test.
- **R39 project.yaml.**
  - New keys: `sortingLayers: [{id, name}]` (absent = Default only), `transparencySortMode`,
    `transparencySortAxis`.
  - A renderer referencing an unknown id draws on Default, and `get_load_report` shows one line per
    scene with the count.
- **R40 Exports.** Cooked scenes carry the new fields (`RuntimeFields.gen.inc` regenerated). The
  Compile factories set them. `PlayerConfig` carries the layer table and sort settings.
- **R41 Unity particle import.** New imports map `m_SortingOrder` to `sortingOrder` and
  `m_SortingLayerID` by name. Effects imported earlier keep `sortPriority`, which still sorts after
  the order (R6): nothing is rewritten.

## 6. Design constraints and decisions

- **D1 Where sorting plugs in.**
  - `RenderSystem::update` already visits every renderer. It ORs one per-scene flag,
    `sortingActive`, from each renderer's two fields, the enabled Sorting Group count and the
    project mode.
  - Off: the 2D loop at `RenderSystem.cpp:18262` runs as today.
  - On, the loop works in two steps per run (cut at UI subtrees):
    - Gather: it collects world renderers as items with a key. A Sorting Group is one item
      holding its members' sorted items.
    - Draw: it stable-sorts by the key, skipping the sort when `std::is_sorted` holds, then draws
      the items through the same per-type calls (`submitMesh`, `drawParticles`, ...).
  - The colour-copy and near-plane checks run on items in draw order.
  - The 3D transparent queue's `TransparentRenderData` gains `sortKey`, and
    `TransparentRenderComparison` compares it first.
- **D2 Keys and buckets.**
  - Item key, 64 bits: layer value (16) | Order in Layer + 32768 (16) | axis distance as an
    order-preserving float (32, 0 in mode Default), then the hierarchy index as the final tie-break.
  - Bucket (`BatchItem::sortKey`): layer value (16) | order + 32768 (16) | outermost enabled
    Sorting Group's sequence number in this pass + 1 (32; 0 outside groups).
  - Disjoint draws are order-independent, so a bucket of 0 for everything would also be
    pixel-safe. The landed contract is kept anyway: layers are where users expect batch breaks
    (Unity breaks there too), and a bucket boundary stops REORDER's overlap scan early. §9 records
    the measured alternative.
- **D3 Sorting Layers storage.**
  - `engine/core/registry/SortingLayers.{h,cpp}` (beside `TagManager`): ids, names, values, and
    a lookup from id to value in a flat array, read once per item.
  - The editor keeps the table in `Project` and pushes it to the engine on load and on each
    `SortingLayersCmd`.
- **D4 The batcher's changes are additive.** Each change is small and in its existing place:
  - `BATCH_MESH_PROPERTIES` and the UI allowlist gain `Pxs`, `Sfx`, `Sfr`, `Spv`.
  - `meshBatchForm` admits `@batchable` manifests and checks §5 R20's conditions.
  - `fillMeshKey` adds the `pixel_snap` and `custom_params` sections and the library textures.
  - The bake gains the `TEXTURE_RECT` and `FX` roles.
  - `drawBatchRun`'s head draw zeroes the `instance=` uniform fields (through `writeCustomUniforms`
    when `passBatchGeometry` is set) and writes the head's `pixelSnapDraw`.
  - Nothing else in `DrawBatcher` changes.
- **D5 Why additive, not "write the vertex buffer".** The batching spec suggested MeshSystem
  rewriting the sprite's vertex buffer when a value changes. An animated flash would then turn the
  sprite's shared immutable buffer private and dynamic, and upload it every frame. Additive
  `uniform + attribute` with neutral 0 keeps the buffer constant (zeros). The value lives where it
  already lives (the material), and the bake writes it into the CPU copy the run uploads anyway.
- **D6 Grown quads (S48 R7).** The batch bakes geometry; it cannot replay a vertex-stage
  growth. So a feature that draws outside the frame (S48's DROP_SHADOW, a grown OUTLINE or GLOW)
  grows the quad and its UVs in `MeshSystem`'s sprite geometry, and is batchable. A variant that
  grows in its vertex stage stays `ineligible_custom_shader`. S48 implements it this way (§8).
- **D7 Sprite Batch on GPU instancing: the choice.**
  - Against the landed dynamic batcher:
    - A bullet would have to be a GameObject (Transform, MeshComponent, its slot in `update()`).
    - It would pay about 650 bytes of batch record and 256 bytes of baked vertices per sprite,
      re-placed and uploaded every frame it moves.
    - An instance uploads 96 bytes and needs no entity.
  - Against S55:
    - S55 instances separate MeshComponent entities that share a mesh.
    - The ≤ 64-vertex built-in-shader sprites go to the batcher and never reach it.
    - It also needs one entity per sprite.
  - Against a new compact format (the old S52 D2, 40 bytes):
    - It would need a second vertex path and up to 8-16 variants per material variant to compile,
      warm (S32) and list in exports (S64).
    - Every `library:sprite` vertex hook would need a second implementation.
  - The existing `HAS_INSTANCING` path already has frames (`i_textureRect`), tints (`i_color`),
    `library:sprite`, 2D lights and S41's instanced pivot. So Sprite Batch packs straight into
    `InstanceRenderData` and adds one optional attribute (`i_spriteFx`).
  - 10,000 moving instances are 960 KB a frame, within the 16 MB mobile stream budget.
  - The compact format remains a later optimisation if R27's measurements ask for it.
- **D8 Per-instance snap without bytes.**
  - The packer decides alignment on the CPU (`pixelart::isGridAligned` of the GameObject's matrix
    times the instance's 2D linear part). For an aligned instance it writes the near-zero terms of
    its linear part as exact 0.
  - The instanced `Pxs` stage computes `mode = drawMode == 1 && the instance's linear part has
    that zero pattern ? 1 : (drawMode > 0 ? 2 : 0)`. Exact zeros compare exactly on every backend.
  - This also corrects InstancedMeshComponent: rotated instances under Rotate On Grid snap their
    pivot instead of every vertex. It is a deliberate behaviour change; its tests are updated in the
    same commit (§8).
- **D9 Code homes** (ledger "Code homes"):
  - `engine/core/render/sprite/SpriteInstanceBatch.{h,cpp}` (the retained or per-frame instance
    buffer, packing, bounds, revision);
  - `engine/core/component/SortingGroupComponent.h` and `SpriteBatchComponent.h`;
  - `engine/core/object/SpriteBatch.h` (the scripting wrapper);
  - `engine/core/registry/SortingLayers.{h,cpp}`;
  - shader changes in `library/sprite.{vert,frag}`, `includes/surface_vs.glsl`, `mesh.vert`, the
    depth stages and `ui.vert`;
  - `engine/core/subsystem/RenderSystemBatch.cpp` for D4.
- **D10 S41 landing order.** S52 starts after S41 is on `main` (its interface is final on its
  branch). If S41 slips, M1 and M4 land first, and R18/R22's snap parts wait. No rounding of S52's
  own anywhere.

### 6.1 MCP and the budget

| Tool | Change | Listing |
|---|---|---|
| `get_project_settings` / `set_project_settings` | rows `tags_and_layers.sorting_layers`, `graphics.transparency_sort_mode`, `graphics.transparency_sort_axis` (descriptions in `SettingsRows.cpp`, not the tool listing) | 0 |
| `add_component`, `set_properties`, `get_entity` | new components and properties are reflected; `sortingLayer` takes a name or id | 0 (schemas are generic) |
| `get_render_stats` | new reason values only | 0 |

`tool_budget.py --check tools/mcp/budget.json` must pass unchanged. If a description is trimmed,
the budget is lowered. Each new setting description, property description, error text, guide page
and index line gets a case in `tools/mcp/eval/cases.py` that fails without it (AC14).

### 6.2 Settings

| Key (rows in `editor/settings/SettingsRows.cpp`) | Type | Default | Meaning |
|---|---|---|---|
| `tags_and_layers.sorting_layers` | list of names | `["Default"]` | R1, ids kept by name across edits |
| `graphics.transparency_sort_mode` | `default` \| `custom_axis` | `default` | R4 |
| `graphics.transparency_sort_axis` | vec3 | (0, 1, 0) | R4 |

### 6.3 Shader contract

- **`Sfx`** (MESH): `in vec4 a_spriteFx` (or `i_spriteFx` under `HAS_INSTANCING`), passed to the
  fragment stage as `flat out vec4 v_spriteFx`. A manifest property `instance=fx.<s>` is read as
  `customParams.<name> + v_spriteFx.<s>`. Without `Sfx`, the shader reads `customParams.<name>`
  alone.
- **`Sfr`** (MESH): `in vec4 a_frameRect`. `v_frameRect = compose(spriteParams.textureRect,
  a_frameRect)`, the same arithmetic as the existing `i_textureRect` composition (`sprite.vert:33-37`).
- **`Spv`** (MESH, DEPTH): `in vec3 a_snapPivot`. `snapPivot = mvpMatrix * vec4(a_snapPivot, 1.0)`
  replaces `mvpMatrix[3]`.
- **Bits.** Appended after whatever is on `main` at landing, in landing order, with these names
  and a test pinning them (ledger rule). The shader cache version is bumped by one.

### 6.4 New batch break reasons

These are appended to `BatchBreak`, with stable names in `getBatchBreakName`:

| Reason | When |
|---|---|
| `pixel_snap` | the draw's `pixelSnapDraw` mode differs from the run's |
| `custom_params` | a batchable manifest's custom parameter bytes (without `instance=` fields) differ |
| `ineligible_pixel_snap_pivot` | a mode-2 draw without `a_snapPivot`, or a mode-2 UI element |
| `ineligible_frame_rect` | OUTLINE or OVERLAY without `a_frameRect` (a mesh that is not a sprite) |

### 6.5 `SpriteInstanceBatch` (Sprite Batch and S50)

```cpp
namespace jasperengine {
    // One page's instances drawn with a Sprite's HAS_INSTANCING variant (InstanceRenderData layout,
    // optional FX stream). Retained: uploaded when `revision` changes. Per-frame: appended to the
    // frame stream.
    class JASPERENGINE_API SpriteInstanceBatch {
    public:
        enum class Storage : uint8_t { Retained, PerFrame };
        void reserve(uint32_t capacity);
        void setCount(uint32_t count);
        std::span<InstanceRenderData> instances();       // written by the producer
        std::span<Vector4> fx();                         // empty until enableFx()
        void enableFx();
        void markChanged();                              // bumps the revision, refreshes bounds
        const AABB& localBounds() const;
        // page runs: [first, count) of instances sharing one texture page, in order
        void setPageRuns(std::span<const std::pair<uint32_t, uint32_t>> runs);
    };
}
```

### 6.6 API sketches

```cpp
namespace jasperengine {
    struct SortingGroupComponent { bool enabled = true; int32_t sortingLayer = 0; int16_t sortingOrder = 0; };

    enum class SpriteBatchMode : uint8_t { Retained, ClearEachFrame };
    enum class SpriteBatchSpace : uint8_t { Local, World };
    struct SpriteBatchComponent {
        SpriteBatchMode mode = SpriteBatchMode::Retained;
        SpriteBatchSpace space = SpriteBatchSpace::Local;
        uint32_t capacity = 1024;
        bool sortInstances = false;
        // runtime, not serialized: SoA streams, count, revision, the SpriteInstanceBatch
    };
}
```

```lua
-- bullets: one GameObject, one draw per page
local batch = SpriteBatch(scene, entity)          -- entity has a Sprite (sheet "fx.aseprite")
local bullet = batch:frameId("bullet_a")          -- the Sprite's frame by name (typed error if unknown)
batch:setPositions(xy)                            -- flat {x1, y1, x2, y2, ...} or {x, y, z} triples with stride=3
batch:setFrames(frames)                           -- optional, else frame 0
batch:setRotations(r) ; batch:setScales(s)        -- radians; uniform or {sx, sy} pairs
batch:setColors(rgba)                             -- flat linear {r, g, b, a, ...}
batch:setFx("flash", amounts)                     -- "palette" | "flash" | "fade" | "dissolve"
local i = batch:add(x, y, bullet) ; batch:set(i, x, y) ; batch:remove(i) ; batch:clear()
print(batch.count)

sprite.sortingLayerName = "Foreground"
sprite.sortingOrder = 5
print(SortingLayer.getLayerValue(sprite.sortingLayerId))
```

### 6.7 Particles, trails, points and UI in the sort

Particle Systems, Points, Line Renderer and Trail Renderer keep their own draw paths and are sort
items with their own fields. UI elements are not sort items (R3). HTML UI documents draw after the
pass as today.

### 6.8 What the old S52 had, decided

| Old item | Decision |
|---|---|
| One 40-byte instance format, rotation/colour/FX streams | replaced by `InstanceRenderData` + the optional `i_spriteFx` stream (D7) |
| Batching consecutive sprites, keys, fallbacks | the landed batcher's |
| Lookahead reorder (R13) | the landed REORDER mode, within D2's buckets |
| Frame coherence (R14) | the landed settled records |
| Batch stats, `explain_batches`, `jasper://stats` batches | the landed `batching` / `batch_breaks`; S52 adds reasons only |
| `@instanceable`, `instance=`, FX slots | kept as `@batchable` and `instance=` with the same slots (R19-R21) |
| Grown quads (R24a) | D6 |
| Static batches for S50 (R15, R31) | kept: `SpriteInstanceBatch` (R17) |
| Sprite Batch (R26-R30) | kept, on instancing; frames come from its Sprite; instances not saved |
| Sorting Layers, Order, Groups, sort axis (R1-R7) | kept, Unity's Graphics names for the axis |
| "Sprite Batches" Scene view draw mode (R50) | dropped: the landed Frame Debugger shows runs and reasons |
| `CLAMP_FRAME` | S50's own concern (a tile material define); not needed by S52 |

## 7. Acceptance criteria

GPU suites end in `Gpu` and run on glcore and Vulkan (lavapipe). "Red" is the run without the change
(named in the commit). Parity means 0 differing pixels (R23).

| AC | Verification |
|---|---|
| AC1 (R3, R6, R8, R9, R38) | `engine/tests/core/render/SpriteSortTest.cpp`: with defaults, the draw sequence of fixture 2D scenes equals `main`'s (a draw trace recorded from the base commit) and no gather runs (counter); layers, orders, nested groups, a disabled group, Custom Axis with Center and Pivot, UI subtrees in place, the 3D transparent precedence. Red: the gather forced on with a non-stable sort fails the tie cases. |
| AC2 (R7, D2) | `SpriteSortGpuTest.BucketsBound`: two layers of interleaved non-overlapping sprites of two textures: runs never span layers (`sort_bucket` breaks counted), images equal batching off. |
| AC3 (R10-R15) | `engine/tests/core/render/SpriteBatchGpuTest.cpp`: `DrawsOnePerPage` (10,000 instances, one page: 1 draw; two pages interleaved: draws = page runs); `MatchesSprites` (64 instances == 64 Sprite GameObjects with the same frames and transforms at pixel-aligned placements: parity); `ClearEachFrame` (drawn once); `RetainedUploadsOnce` (static batch: 0 upload bytes over 100 frames); `CulledWhole`. Red: none for new behaviour; `MatchesSprites` with the pivot taken from the frame's centre instead of its pivot fails. |
| AC4 (R16, D8) | `SpriteBatchGpuTest.SnapPerInstance`: under a Pixel Perfect Camera, unrotated, 90° and 30° instances at 0.25 px offsets (Rotate On Grid and Steps90) equal the same Sprites drawn alone. `InstancedMeshPixelSnapGpu` (updated, said in the commit): rotated instances now pivot-snap. Red: the zero-pattern test removed fails the 30° case. |
| AC5 (R18, mode 1) | `PixelArtBatchGpuTest.SnappedSpritesBatch`: 500 sprites under a Pixel Perfect Camera at fractional positions, one sheet: ≤ 2 runs, parity with batching off. Red: on `main` all 500 draw alone (`ineligible_shader_property`); with the `pixel_snap` key section removed, a mixed-mode scene (camera roll toggled per view) fails parity. |
| AC6 (R18 Steps90, R22 pivot) | `PixelArtBatchGpuTest.Steps90AndOnGrid`: Steps90-rounded sprites batch and match; Rotate On Grid rotated sprites with `a_snapPivot` batch (mode-2 runs) and match. Red: the pivot attribute's POINT role removed fails parity. |
| AC7 (R19-R21) | `PixelArtBatchGpuTest.FlashAmountsBatch`: 100 `library:sprite` FLASH sprites with amounts 0-1 in one run, parity; outline colour differing breaks with `custom_params`; `Buffer::getWriteCount` unchanged while the amounts animate (R28). Red: the run's draw not zeroing the uniform field fails parity (double flash). |
| AC8 (R19, S43, S48 optional) | `PaletteSwapGpuTest.Batched` (S43's AC11): 64 sprites with rows 0-3, one run, parity. `PixelArtBatchGpuTest.FxSlotsZW`: a test manifest shader claiming `fx.z`/`fx.w` and writing them to colour batches with parity. S48 adds its DITHER_FADE/DISSOLVE cases to this test when it lands. Same cases for Sprite Batch through `setFx`. |
| AC9 (R22 frame rect) | `PixelArtBatchGpuTest.OutlineOverlayBatch`: OUTLINE and OVERLAY sprites from an atlas, frames changing per sprite, batch and match; a MeshPolygon with OUTLINE is `ineligible_frame_rect`. Red: the `TEXTURE_RECT` role removed bleeds the neighbour frame and fails. |
| AC10 (R24, R25) | The GPU suites pass on glcore and Vulkan; `tools/build.sh web` and `WEB_GATE`'s sprite parity case (WebGL2); `tools/build.sh android` compiles. `ShaderVariantTest.S52BitsOnlyWhenUsed`: a project without a Pixel Perfect Camera or `instance=` feature has no `Sfx`/`Sfr`/`Spv` key in its export shader list. Metal and D3D11 are read only (no CI). |
| AC11 (R30-R33) | `tools/mcp/tests/test_sorting_layers_editor.py` (isolated editor, xvfb): add, rename, reorder and remove a Sorting Layer through UI probes, then undo each; renderer rows with multi-edit and undo; Add Component Sorting Group, then undo; screenshots of the Tags and Layers panel and a sorted scene kept outside the repo. |
| AC12 (R38-R41) | `editor/tests/stream/StreamSortingTest.cpp`: round trip of every new field and both components; the pre-S52 fixture `editor/tests/fixtures/sorting/pre-s52.scene` loads with defaults and re-saves byte for byte; unknown layer id → Default plus one load-report line. `CookedScene.RoundTripEveryComponent` and `RuntimeFields.*` cover the new fields; `UnityParticleImport` test maps `m_SortingOrder` to `sortingOrder`. |
| AC13 (R34, R37) | `tools/mcp/tests/test_sorting_2d.py` (`CI = True`): `set_project_settings` layers by name; `set_properties sortingLayer: "Foreground"`; an unknown name errors with the fix; `screenshot` order changes as expected; a Sprite Batch filled by `eval_lua` in Play reports `count` and draws (`get_render_stats` instances). Lua: `engine/tests/core/script/LuaSortingTest.cpp` and `LuaSpriteBatchTest.cpp` (bulk calls, typed errors). |
| AC14 (§6.1, R35, R36) | `tool_budget.py --check`, `gen_tool_docs.py --check`, `test_protocol.py` (guide size), `test_tool_conventions.py`; `tools/mcp/eval/cases.py` cases for each new text, each failing with its text removed (run per `tools/mcp/eval/README.md` on Orquester accounts); the doc test runs the Lua block of `docs/mcp/guide/rendering-2d.md`. |
| AC15 (R26-R29) | `SpriteSortBench`, `SpriteBatchBench`, `PixelArtBatchBench` (`[bench]` lines, `perfSlack`); counts asserted always. Mobile and web numbers measured once and recorded in `docs/specs/evidence/s52/`. |

## 8. Dependencies and conflicts

- **Depends on:**
  - the landed `sprite-batching` (`48cf20e75`), its hooks and their documentation
    (`docs/rendering/batching.md:86-97`);
  - S41 on `main` (`pixel_snap.glsl`, `pixelSnapDraw`, `pixelSnapApply`, `Pxs`; D10);
  - S43 on `main` for `paletteRow instance=fx.x` (AC8's first half);
  - S44 / S45 for sheet and atlas frames (Sprite Batch reads them through its Sprite; without
    them, frames are texture rects).
- **Interfaces given:**
  - S48: `instance=fx.z|w`, the `Sfx` channel, D6 (grown quads in geometry, recorded in S48's
    Deviations when it is implemented).
  - S50: `SpriteInstanceBatch` replaces the old `StaticSpriteBatch` / `SpriteInstance`. S50's
    re-scope records it; S50's R31 and D7 move to it, and `CLAMP_FRAME` stays S50's.
  - S42: Sprite Batch drawables for `check_pixel_art`.
  - S49: particles are sort items (§6.7).
- **Wording to align when this lands:** S43 and S48 say `@instanceable` and "S52's `i_fx` stream".
  The names here are `@batchable` and `a_spriteFx` / `i_spriteFx`, with the same slots. Each
  records it in its Deviations.
- **Behaviour changes, said in the commits:** D8 (instanced pivot snap under Rotate On Grid). Tests
  that pin per-sprite draw counts of pixel-snapped or `library:sprite` scenes change (they now
  batch).
- **Merge hotspots:**
  - `RenderSystem.cpp` (`renderCameraPass` 2D loop and transparent queue, `writeCustomUniforms`);
  - `RenderSystem.h` comparators;
  - `RenderSystemBatch.cpp`, `DrawBatcher.{h,cpp}` (reasons);
  - `MeshSystem.cpp` (`prepareSpriteMesh`);
  - `library/sprite.{vert,frag}` (S43, S48);
  - `surface_vs.glsl`, `mesh.vert`, the depth stages, `ui.vert` (S41, S54, S55);
  - `ShaderPool.cpp` bits;
  - `SettingsRows.cpp`, `ProjectSettingsWindow.cpp`, `Project.{h,cpp}`, `Generator.cpp`,
    `PlayerConfig`, `RuntimeFields.gen.inc`, `tools/mcp/eval/cases.py`.

## 9. Risks and defaults

- **Bucket per layer costs batches.** A foreground of atlas A over characters of atlas B gives
  three runs even where foreground pieces touch nothing in between. Default: keep D2. AC2's bench
  logs the draw count with and without buckets; a measured gain above 10 % on the bench scene is a
  follow-up proposal to the batcher's owner, not a change here.
- **Vertex growth.** `a_spriteFx` adds 16 bytes per vertex, `a_frameRect` 16 and `a_snapPivot` 12,
  only on sprites that use the feature. Default accepted. Their buffers stay shared by content, so
  sprites with equal geometry still share one.
- **Sorting gather cost** when one renderer leaves the defaults: every 2D run is gathered. Default:
  the `is_sorted` short cut and R26's budget. Per-run dirty tracking is a later optimisation if the
  bench asks.
- **`Center` sort point** reads world bounds, which `update()` refreshes on change only. Default: a
  stale centre for one frame after a frame size change is accepted, as Unity shows it too
  (**assumed**).
- **Exact-zero instance test (D8).** A user-written instance matrix (InstancedMeshComponent from
  Lua) with a 1e-9 term snaps by pivot. Default: documented. The engine's own packers write exact
  zeros.
- **S41 or S43 interface drift before landing.** Default: S52 lands after both. A changed name is
  recorded in Deviations.

## Milestones

- **M1 Sorting.** R1-R9, R30-R32, R34 (sorting part), R37 (sorting), R38-R41. AC1, AC2, AC11, AC12,
  AC13 (sorting).
- **M2 Pixel snap in batches, mode 1.** R18 mode 1, `pixel_snap`. AC5. Needs S41 on `main`.
- **M3 `library:sprite` batchable, FX channel.** R19-R21, `custom_params`, `@batchable`,
  `instance=` on `flashAmount` and `paletteRow`. AC7, AC8.
- **M4 Sprite Batch.** R10-R17, R33, R37 (SpriteBatch). AC3, AC4, AC8 (Sprite Batch half), AC13.
- **M5 Per-vertex frame rect and pivot.** R22, R18 mode 2. AC6, AC9.
- **M6 Docs, guide, budgets, benches.** R35, R36, R26-R29. AC10, AC14, AC15.

## Deviations

Recorded by the implementer (2026-11-08).

### Names and storage

- **S43's names reconciled.** S43 (and the old S52) said `@instanceable` and "S52's `i_fx` stream".
  The code has `// @batchable` and `instance=fx.<x|y|z|w>`, read in a fragment stage as
  `customParams.<member> + spriteFx(<slot>)` (`includes/sprite_fx.glsl`), carried by the vertex
  channel `a_spriteFx` (batched sprites) or the per-instance `i_spriteFx` (Sprite Batch). The slots
  are the ledger's. S43's Deviations say the same.
- **`@batchable framerect=A,B`.** Which features read the frame rect is declared in the manifest
  (`library:sprite`: `framerect=OUTLINE,OVERLAY`) instead of a rule about OUTLINE and OVERLAY in the
  engine, so another batchable shader can say it too.
- **Order in Layer is stored as `int32_t`**, clamped to -32768..32767 when read and written: every
  integer row of the reflection, the Inspector and MCP is an `int`. The key keeps its 16 bits (D2).
- **Sorting Layer ids** are 0..2^20 (the id-to-value lookup is a flat table). An id once given is
  never given again: `SortingLayers::getNextId` only grows and project.yaml keeps it
  (`sortingLayerNextId`, written once a layer was removed), so a removed layer's renderers draw on
  Default instead of landing on a later layer. A list of names keeps the ids of the names already
  in the table; `tags_and_layers.sorting_layers` reads `[{id, name}]` and takes names or
  `{id, name}` entries, so a rename keeps the id (R1). Unity's
  `GetLayerValueFromID` is relative to Default; `SortingLayer.getLayerValue` is the layer's place in
  the list (Default may move), the "value" of R1.
- **The suite of AC3/AC4 is `SpriteBatchComponentGpu`** (`SpriteBatchComponentGpuTest.cpp`):
  `SpriteBatchGpu` is the landed Dynamic Batching's suite. AC4's "updated `InstancedMeshPixelSnapGpu`"
  did not exist; `SpriteBatchComponentGpu.InstancedMeshRotatedInstancesSnapTheirPivot` checks the
  D8 behaviour change instead.
- **Typed errors in Lua** are the message's first word (`frame_range:`, `unknown_frame:`, `length:`,
  `fx_slot:`, `unknown_sorting_layer:`): a Lua error is a string.

### Sorting

- **The batcher (D4, "nothing else changes").** A run holds one bucket: an object of an equal key
  but another bucket opens a run and counts `sort_bucket` (R7, AC2 "runs never span layers"). Before,
  a consecutive object joined across buckets and only REORDER's jump-back respected them. A scene
  that does not sort is bucket 0 throughout, so nothing changes for it. `isIneligibleBreak` covers
  the two ineligible reasons appended after `DISABLED`.
- **Particle Systems in a sorted 2D pass draw in place** as sort items (R2, §6.7); a 2D pass of a
  scene that does not sort still draws them after the hierarchy, as before. A Trail Renderer on a
  GameObject with another renderer draws right after it, at that renderer's key; alone it sorts by
  its own fields.
- **3D transparent queue:** a member of a Sorting Group takes its outermost enabled group's layer,
  order and distance, then its own layer and order; a nested group is not an item of its own there
  (it is in 2D passes). With a group in the queue, an item alone and a whole group are units of one
  total order, (priority, distance far first, sequence); a group's priority is 0 (its members'
  priorities order them inside it). Without a group the queue sorts as before.
- **Unity layer ids** compare as their 32 bits: TagManager writes `uniqueID` unsigned, a renderer's
  `m_SortingLayerID` signed.
- **Unknown layer ids** (R39) are one load-report line per scene, kind `fallback`, fate `info`
  (nothing is dropped: the ids stay).
- **AC1's "draw trace recorded from the base commit"** is replaced by what makes it hold: with
  defaults nothing is gathered (the counter, `SpriteSortTest.DefaultsGatherNothing`,
  `SpriteSortGpu.DefaultsDrawTheHierarchyAndGatherNothing`) and the 2D loop is the one of the base
  commit; the batcher sees bucket 0 for every draw. No trace file was recorded.
- **Unity import (R41):** `m_SortingLayerID` is found by name through the Unity project's
  `ProjectSettings/TagManager.asset`; a layer the engine project lacks keeps Default with the note
  `sorting-layer-missing`.

### Sprite Batch

- **The FX stream** exists whenever the Sprite's material reads an `instance=` amount (not "once
  `setFx` was called"): the variant ('Sfx') is then known at load and a first `setFx` never reloads
  the mesh. A material that reads none has no stream (R27's 96 bytes; 112 with it).
- **Clear Each Frame** uploads into the batch's own stream buffer once per frame it draws (not the
  frame's shared stream): the same cost, one buffer per batch.
- **Capacity**: writing past it grows the GPU buffer by half again (a mesh reload), so a batch
  filled one instance a frame reloads now and then; the saved `capacity` is not rewritten.
- **Snapped instances carry their pivot**: in a snapping scene a Sprite Batch adds an 8-byte
  per-instance stream (`i_snapPivot`, the instance's position; 'Spv' on the instanced variant),
  because the matrix's origin also carries the frame's offset (`R·S·ft`) and a trimmed or offset
  frame turned off the grid would snap at the wrong point. R27's 96 bytes are 104 there.
- **R17's producer entry** is `RenderSystem::attachInstanceBatch(entity, batch)`: the batch draws as
  the instances of that GameObject's MeshComponent, whose Transform is its model matrix and whose
  place in the Hierarchy and renderer fields are its sort item (no model matrix stored twice).
- **`SpritePageRun` carries its page** (`{first, count, page}`; §6.5 had pairs), and
  `SpriteInstanceBatch::setPages` gives the pages' textures.
- **World space** packs the instance matrices through the inverse of the GameObject's matrix (the
  draw keeps the GameObject's MVP); a World batch is packed again when its GameObject moves.
- **Frames draw Simple**: an instance maps the Sprite's quad onto its frame's quad. A Sliced or
  lattice-gridded Sprite's instances are its mesh stretched onto the frame.
- **No `create_entity` type** for it (R34, no tool change): agents use `add_component spritebatch`;
  GameObject > 2D Object > Sprite Batch makes a Sprite with one.
- **Depth passes** draw a Sprite Batch instanced as the colour pass does; its pages bind as the depth
  program's texture.

### Pixel-art inputs

- **'Sfx', 'Sfr', 'Spv' follow the mesh**: MeshSystem gives a Sprite the attributes its material and
  scene read (`MeshComponent::batchAttributes`: an enabled `instance=` feature, a `framerect=`
  feature, a snapping scene), the variant takes the bit only when the mesh carries the attribute.
  MESH bits 46-48, DEPTH 20; shader cache v61 (main took v60 before the landing). The export's shader list
  (`ShaderVariantCollector`) computes the same inputs from the Sprite's material and Sprite Batch
  (`MeshSystem::spriteBatchAttributes`), so a scene saves the same list drawn or not.
- **`custom_params` is a 64-bit FNV-1a hash** of the custom blocks (the engine-written members,
  and with 'Sfx' the `instance=` members, zeroed): a block of a dozen vectors does not fit the key's
  256 bytes. The library textures and palettes join the `texture` section.
- **Steps90** rounds the bake's model through `pixelArtModel` (S41 rounds only the MVP, not
  `objectMatrices`), so a batched Steps90 sprite is placed as its draw alone.
- **The run's snap mode** is set from its head through `passSnapModeOverride` (colour, depth and UI
  draws): the run's MVP is the origin cell's, always on the grid.
- **InstancedMesh (D8), a deliberate behaviour change** (commit 4d5f2544f; its message did not say
  so, and history is not rewritten): in a snapping scene an Instanced Mesh's matrices get exact
  zeros (`pixelart::exactGridZeros`, relative 1e-6), and its instances turned off the grid now snap
  their pivot (mode 2) where before every vertex snapped. Pinned by
  `SpriteBatchComponentGpu.InstancedMeshRotatedInstancesSnapTheirPivot`. Elsewhere its matrices are
  as before. An Instanced Mesh sprite takes no 'Sfx' (its instances have no FX amounts).
- **'Spv' on every snapped sprite**, batching on or off: Engine.dynamicBatching can change while a
  game runs, and the attribute (12 bytes a vertex, a snapping scene only) spares every sprite a
  reload and a variant swap then; an export ships the twin for the same reason.
- **GPU Instancing's "never both"** leaves out a small single-submesh mesh of a `@batchable` library
  shader as it does one of the built-in shader: Dynamic Batching takes it.
- **The custom-values key section** is hashed again only when the mesh's custom blocks were written
  (`MeshComponent::customUniformWrites`), not every frame.

### MCP, docs and evidence

- **No guide index entry** (R36): the agent guide had 1,503 bytes of the 1,500 of headroom
  `tool_budget.py` keeps, so even `, rendering-2d` would cut into it. The route is where the agent
  meets it instead: the `tags_and_layers.sorting_layers` setting, the `sortingLayer` schema (a name,
  an id or `{id, name}`), the unknown-name error naming `set_project_settings`, and
  `jasper://docs/rendering-2d` in the docs listing.
- **Sprite Batch colours are sRGB**, as every wrapper's colour (Sprite's `setColor`): the instance
  colour is converted to linear when written, so 64 instances match 64 Sprites (AC3).
- **AC14's eval cases** are in `tools/mcp/eval/cases.py` (`guard_sorting_layer_route`,
  `bad_unknown_sorting_layer`, `bad_bullets_as_gameobjects`, `guard_sort_bucket_break`); the eval
  itself runs on owner-approved Orquester accounts and was not run by the implementer.
- **AC15's mobile and web numbers** need a device and were not measured; the desktop benches print
  their `[bench]` lines.
