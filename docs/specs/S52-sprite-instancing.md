# S52 — Sprite Instancing: one instance format, batched sprites, Sorting Layers and Sorting Groups, Sprite Batch, batch statistics

Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5A (with S41, S43 and S44). Depends on S41 (the
pixel-snap rule, PPU, shared-edge snapping; same wave, D10) and S44 (sheets and their pages through
`SpriteSheetPool`; same wave: until it lands a page is a texture and frames are rects). S45's atlas
pages (wave 5B) reach it through the same pool without a change here. Uses S07
(`Changed<Transform>`), S15 (render stats), S20 (instance streams), S24 (undo), S32 (async
pipelines); defines the FX slots that S43 (PALETTE_SWAP, same wave) and S48 (DITHER_FADE, DISSOLVE,
wave 5B) fill. Blocks S50 (tilemap chunks draw through it).
Read `README.md` and the ledger first.

## 1. Goal

Today every sprite is its own mesh, its own GPU buffers and its own draw call, and 2D draw order is
nothing but hierarchy order. A pixel-art game with a few thousand sprites (bullets, coins, enemies,
foliage) spends its frame in per-draw overhead, and runs out of buffers before it runs out of GPU. S52
draws sprites the way Unity's 2D renderer and spryte do: one compact instance format whose transform is
built in the vertex shader, consecutive compatible sprites merged into one instanced draw, the S41
snap rule applied identically to the old path, and every frame's batches and batch breaks visible to
people and agents. It adds the sort control 2D games expect from Unity, which batching needs to be
predictable: Sorting Layers, Order in Layer, Sorting Groups and a sort axis for top-down games.

## 2. Scope

In scope:
- Sorting: project Sorting Layers, per-renderer Sorting Layer and Order in Layer, the Sorting Group
  component, the sort axis (None / Y / custom), in 2D (no depth test) and 3D (transparent queue)
  passes.
- One sprite instance format and its vertex shader include; optional per-instance streams (rotation,
  colour, FX) chosen by shader variant; batching of consecutive compatible sprites sharing a texture
  page (S45); a bounded, pixel-safe lookahead reorder; frame coherence (no repack when nothing moved).
- The material instancing policy: which shader features read per-instance values (S43 palette row,
  S48 flash/fade/dissolve, the existing `library:sprite` FLASH), and the per-mesh fallback otherwise.
- The bulk API: the Sprite Batch component (retained or cleared each frame), its Lua and C++ API.
- Integration points: tilemap chunks (S50) as static instance batches; Particle Systems (S20 and
  S49's pixel particles, which keep their own instanced path) in the sort.
- Render statistics (batches, instances, draws saved, breaks by reason) through the existing
  `get_render_stats`, Lua `Profiler`, the stats overlay and a Scene view draw mode.
- Pixel-parity golden tests (batched path == per-mesh path) and budgets on mobile and web.

Out of scope:
- Several textures per batch (spryte's 8 units). One page per batch; atlases (S45) are how pages are
  shared. A sampler-array path would need constant-index sampling on GLES3 and would multiply every
  `library:sprite` feature. See §9.
- GPU-driven culling, indirect draws, storage buffers (README do-not-copy and platform rules).
- Batching 3D meshes (an SRP-Batcher equivalent), HTML UI (S16 has its own batching), the
  `UIComponent`/RectTransform UI, Points and Lines.
- Sprites whose world transform leaves the XY plane (rotated about X or Y, billboards) and sheared
  sprites: they keep the per-mesh path (a counted break, R18).
- Runtime atlas packing (S45 owns atlases). Text (S51 decides whether its glyph quads become Sprite
  Batch instances).

## 3. Current state (read, not built or run)

- **A sprite is a mesh.** `Sprite` derives `Mesh` (`engine/core/object/Sprite.h:17`).
  `MeshSystem::prepareSpriteMesh` (`engine/core/subsystem/MeshSystem.cpp:566-580`) gives it a DYNAMIC
  vertex buffer of POSITION, TEXCOORD1, NORMAL, TANGENT and COLOR (16 floats per vertex), and
  `createSprite` (`:621`) writes 4 vertices plus a 6-index buffer (`:767-776`). Frames are rects in
  `SpriteComponent::framesRect` (at most `MAX_SPRITE_FRAMES` 128, `engine/core/Engine.h:106`).
- **One draw per sprite.** `RenderSystem::drawMesh` (`engine/core/subsystem/RenderSystem.cpp:8604`)
  culls the mesh, uploads dirty buffers, writes its custom uniforms, builds its model/normal/MVP
  block, then for each submesh binds textures, `beginDraw`s its pipeline and draws
  (`drawSubmeshGeometry`, `:8415-8447`). Nothing merges consecutive meshes. Inferred cost of N
  visible sprites: N draw calls, N binding applies, at least 2N uniform blocks, and 2N GPU buffers.
  - 2N buffers: 10,000 sprites need 20,000 buffers, over the default pool of 16,384
    (`GpuResourceLimits::buffers`, `Engine.h:284`).
  - Uniforms: Vulkan and Metal round each block up to 256 bytes against a per-frame budget of 16 MB
    on Android, iOS and the web (`Engine.h:245-300`); 10,000 sprites take about 5 MB of it.
- **Instancing exists, for other things.** `InstancedMeshComponent` draws many copies of one mesh in
  one draw with a 96-byte instance (`InstanceRenderData`: matrix, colour, texture rect,
  `engine/core/component/InstancedMeshComponent.h:87`; attributes `i_matrix_col1..4`, `i_color`,
  `i_textureRect`, `engine/shaderlib/mesh.vert:117-125`). Particle Systems append their instances to
  per-frame paged streams with `sg_append_buffer` and draw one instanced draw per system
  (`RenderSystemParticles.cpp:1-8`, `ParticleStream`, `RenderSystem.h:2372-2385`). Instances bind at
  an offset because GLES3/WebGL2 has no base instance (`bindInstances` in `drawSubmeshGeometry`).
- **Draw order.** A camera with `depthTest = false` (the default 2D camera, `engine/core/Scene.cpp:258-271`;
  `CameraComponent.h:93-98`) draws in Transform order, which is the hierarchy order
  (`renderCameraPass`'s loop, `RenderSystem.cpp:16204-16316`). `sortsByDistance` is
  `depthTest && transparentSort` (`:665-667`). Only a distance-sorted pass has queues: opaque nearest
  first, transparent by `sortPriority`, then far to near, then submission order
  (`TransparentRenderComparison`/`OpaqueRenderComparison`, `RenderSystem.h:626-646`; built at
  `RenderSystem.cpp:16040-16083`). `MeshComponent::transparentSortPriority` and `opaqueSortPriority`
  come from the shader at load (`MeshComponent.h:503-504`). Particle Systems add `sortPriority` and
  `sortingFudge` (`engine/core/particles/ParticleEffect.h:456-457`). There are no Sorting Layers, no
  Order in Layer, no Sorting Group and no sort axis.
- **2D lights are one scene block.** `u_fs_lighting2d` holds the pass's up to `MAX_LIGHTS_2D` lights
  for every sprite (`engine/shaderlib/includes/lighting2d.glsl:70-76`), so lights never differ between
  two sprites of a pass.
- **`library:sprite`** (`engine/shaderlib/library/sprite.frag:1-45`, `sprite.vert`) has OUTLINE,
  HUE_SHIFT, FLASH, GRAYSCALE, COLOR_REPLACE, OVERLAY and GLOW, all as material uniforms
  (`flashAmount` among them), and already handles instancing for the frame rect (`sprite.vert:28-37`).
- **sokol-gfx.** 16 vertex attributes and 16 vertex buffer slots (`engine/libs/sokol/sokol_gfx.h:2043-2045`);
  `stride = 0` means "computed", not "constant" (`:3795-3810`); `step_rate` (instance divisor) is
  honoured on GL (`:11386-11399`), D3D11 (`:13943-13952`) and Metal (`:16066`) but not on Vulkan,
  which sets only the input rate (`:20417`, `:21575`); integer and half-float vertex formats exist
  (`:2494-2523`, integer attributes through `glVertexAttribIPointer`, `:9267`). The engine sets
  per-instance steps in `SokolObject.cpp:517`.
- **Statistics.** `get_render_stats` (`editor/mcp/tools/ProfilerTools.cpp:1028-1039`, group
  `profiling`) returns `draw_calls`, `instances`, `triangles`, passes, pipeline and binding applies
  (`renderStatsJson`, `:166-197`; `RenderStats`, `engine/core/render/RenderStats.h:155-186`); Lua
  `Profiler` exposes `drawCalls` (`engine/core/script/binding/ProfilerClassesLua.cpp:100`). Nothing
  reports batches or why a batch broke. The HTML UI's `HtmlUIRenderStats::runsMerged`
  (`engine/core/htmlui/HtmlUIRenderStats.h:28-31`) is the naming precedent.

## 4. Reference

| Source | What | Take | Improve |
|---|---|---|---|
| spryte (MIT, `/tmp/pixelart-research/spryte`) | `src/webgl2.ts:15-65` `VERT` | one instance format for quads and sprites; the transform algebra in the vertex shader under a batch transform (`u_linear`, `u_mt`) | our format is typed per stream (D2), not one overloaded vec4 set |
| | `src/webgl2.ts:67-81` `COPY_VERT`, `src/copies.ts:1-60` | position-only copies sharing everything else; interleaved records | becomes the core stream of Sprite Batch with the other streams absent (R26) |
| | `src/webgl2.ts:97-144` `EACH_VERT`, `:822-855` | optional per-copy scale, rotation, packed colour; an absent stream is a constant attribute (`vertexAttrib*`) | sokol has no constant attributes: shader variants instead (D3) |
| | `src/draw.ts:303-309`, `COPY_VERT` `floor(e + 0.5)` | one snap rule on CPU and GPU (`Math.round` and `floor(x + 0.5)` agree, both round half up) | the rule itself is S41's; we call S41's functions on both sides |
| | `src/panels.ts:119-132` `snapEdges` | shared-edge snapping of neighbouring quads | S41 snaps every vertex of a grid-aligned draw, so neighbours whose producers write bit-identical shared edges (S41 R13; S50's tile edges) stay joined without a flag (R11) |
| | `browser-test/checks.ts:1`, `:157-195` | the parity method: draw the same scene two ways, compare every pixel; exact when unrotated, a stated tolerance (8 px) for rotated copies because GPU `sin`/`cos` differ in the last bits | our rotation is `cos`/`sin` computed on the CPU (D2), so the tolerance applies only where the old path also differs; goldens on every backend we run |
| Unity 2D Renderer (behaviour only) | Sorting Layers, Order in Layer, Sorting Group, Transparency Sort Axis, SpriteRenderer Sort Point | names and semantics | — |
| Godot 3 2D batching (behaviour only) | item reordering with lookahead when rects do not overlap | the idea | ours is bounded, deterministic, and proved pixel-safe (D7) |

## 5. Requirements

### Functional — sorting

- **R1 Sorting Layers.** Project Settings > Tags and Layers > Sorting Layers: an ordered list of
  named layers with stable integer ids; "Default" (id 0) always exists and cannot be deleted. Drawing
  order follows the list order, not the id. Renaming or reordering never touches scenes.
- **R2 Per-renderer fields.** Every sprite-like renderer gets `sortingLayer` (layer id, default 0)
  and `sortingOrder` (Order in Layer, int16, default 0): `MeshComponent` (sprites, polygon meshes,
  legacy tilemaps, plain meshes), the Particle System renderer settings
  (`ParticleEffect.h:456-457`'s struct), the Sprite Batch (R26) and the Tilemap Layer (S50).
- **R3 2D order.** In a pass without depth test, the hierarchy is cut into runs at every UI subtree
  (a RectTransform root and everything under it). Inside a run, world renderers draw ordered by
  (Sorting Layer rank, Order in Layer, sort-axis distance when an axis is set, hierarchy index). UI
  subtrees keep their place and their own hierarchy order (their scissors and Canvas Groups depend on
  it). With every field at its default and no axis, the order is today's exactly (AC1).
- **R4 Sort axis.** Project setting `rendering.sprite_sort_axis`: `none` (default), `y` (higher
  world y draws first, the top-down convention) or `custom` with a vector; a Camera can override it
  (`sortAxisOverride`, default "Project"). The distance is measured at the renderer's pivot (Unity's
  Sort Point "Pivot"), for tilemap layers in Individual mode at each tile's anchor (S50).
- **R5 Sorting Group.** A Sorting Group component makes its subtree sort as one item, at the group's
  own Sorting Layer, Order in Layer and pivot; inside, the members sort by the same key among
  themselves. Groups nest. A disabled group is transparent to sorting.
- **R6 3D passes.** In a distance-sorted pass the transparent queue orders by (Sorting Layer rank,
  Order in Layer, `sortPriority`, distance, submission order); the opaque queue is unchanged. Defaults
  give today's order.
- **R7 Deterministic.** The order is a total order (the hierarchy index breaks every tie), so two runs
  of the same scene draw the same sequence on every backend.

### Functional — instance format and batching

- **R8 One instance format.** A batchable renderer becomes one instance of the format of D2: a core
  stream (always present) and up to three optional streams (rotation, colour, FX). The vertex shader
  builds the quad from it; the fragment shader is the renderer's own, unchanged.
- **R9 Absent streams cost nothing.** A batch carries only the streams one of its instances needs; a
  member without one writes the default (rotation 0, colour white, FX zero) for the batch. A scene with
  no tinted, rotated or flashing sprite never uploads a colour, rotation or FX byte.
- **R10 Batches.** Consecutive instances in draw order merge into one instanced draw when their batch
  keys are equal (D6): pipeline (shader variant, blend, depth and stencil state, cull), texture page
  and sampler, the material's non-instanced uniform values and textures, scissor, and render target.
  A batch holds at most 16,384 instances.
- **R11 Snapping.** The batched vertex stage honours S41's rule (S41 §6.2) under S41's `Pxs` variant
  bit, exactly as the per-mesh stages do: a grid-aligned instance (rotation stream absent, or its
  cos/sin a signed permutation) snaps every vertex with `pixelSnapClip` after the batch transform and
  the view-projection; an instance under S41's Rotate On Grid snaps its pivot with
  `pixelSnapCenterDelta`; under Steps90 the packer rounds the instance's rotation to 90° on the CPU
  with S41's `roundRotation90` before writing the rotation stream (S41 R10), and a static batch's
  model matrix (R15: a tilemap chunk, a retained Sprite Batch) follows the per-mesh rule of S41 R10
  (rounded under Steps90, its pivot snapped under Rotate On Grid). Rotated instances do snap: per
  vertex after rounding (Steps90) or at the pivot (Rotate On Grid). Shared edges stay joined
  because producers write bit-identical edge values (S41 R13: S50's tiles, sliced pieces). With no
  active Pixel Perfect Camera, nothing snaps on either path and the `Pxs` variants are never built.
- **R12 Frame clamp.** Flag `CLAMP_FRAME` clamps the UV to the frame inset by half a texel in the
  fragment stage (the `library:sprite` frame rect, `sprite.vert:28-37`), so a frame never samples its
  atlas neighbour at a fractional zoom. Tiles set it (S50).
- **R13 Lookahead.** In a pass without depth test, an instance may move earlier, past at most
  `rendering.sprite_batch_lookahead` items (default 32, 0 = off), to join an open batch when its
  conservative screen rectangle overlaps none of the items it jumps over and it does not cross a UI
  subtree or a Sorting Group boundary (D7). The pixels are identical to the unreordered order (AC4).
- **R14 Frame coherence.** When no instance of a run changed (S07 `Changed<Transform>`, renderer
  edits, material edits, camera when it matters) and the pass's cut is the same, the previous frame's
  batch list and stream contents are reused: no sort, no pack, no upload.
- **R15 Static batches.** A producer can hand the batcher a retained instance buffer with its own
  model matrix (a tilemap chunk, a retained Sprite Batch): drawn as one batch, uploaded only when its
  content changes, never repacked per frame.
- **R16 Culling.** A batched instance is frustum-culled by its quad bounds before packing; a static
  batch is culled by its bounds as a whole (S50 culls chunks).
- **R17 GPU objects.** A sprite drawn by the batcher creates no GPU buffers of its own. Its CPU quad
  stays (picking, S26; bounds). If it falls back to the per-mesh path (R18), its buffers are created
  in that frame, before its draw.
- **R18 Fallback and breaks.** A renderer that cannot be an instance draws on the per-mesh path as
  today, between the batches, in its sorted place. Every fallback and every batch break is counted by
  reason (D6): `texture`, `material`, `blend`, `stencil`, `scissor`, `capacity`, `ui_run`,
  `sorting_group`, `per_mesh` (with the cause: `material_not_instanceable`, `extra_pass`,
  `transform_3d`, `shear`, `deformed`, `custom_vertex`).
- **R19 Editor views too.** The Scene view and the Game view draw through the same batcher (the
  editor's selection outline and gizmos are unaffected: they read the CPU quad).
- **R20 Off switch.** Project setting `rendering.sprite_batching` (default on); off draws every sprite
  on the per-mesh path (the pixel-parity reference and a support switch).

### Functional — materials

- **R21 Instanceable materials.** The engine's default sprite material and `library:sprite` with any
  feature set are instanceable. A shader opts in with the manifest line `// @instanceable`; a
  shader without it, or one with a custom vertex stage that does not include the sprite instance
  vertex include, draws per mesh (`material_not_instanceable`).
- **R22 Per-instance properties.** A manifest property may name an FX slot,
  `// @property flashAmount range(0, 1) 0.75 "Flash amount" feature=FLASH instance=fx.y`. In a batch
  that property is read from the instance's FX stream, so sprites that differ only in it stay in one
  batch; every other property is part of the batch key.
- **R23 Slot assignment (family contract).** FX is four halfs, every neutral value 0: `fx.x` the S43
  PALETTE_SWAP palette row (an integer index stored exactly; 0 = the original colours), `fx.y` the
  flash amount (the existing `library:sprite` FLASH, which S52 M3 declares `instance=fx.y`), `fx.z`
  the S48 DITHER_FADE amount, `fx.w` the S48 DISSOLVE progress. S43 and S48 declare theirs with
  `instance=`. The four slots are then all taken; a feature needing more per-instance values is not
  instanceable and falls back (R18) until a later spec adds a stream.
- **R24 Material identity.** Two sprites with separate material copies but equal values batch: the
  key hashes values, not material object identity.
- **R24a Grown quads (S48 R7).** When a material's OUTLINE, DROP_SHADOW or GLOW grows the sprite's
  quad by its reach in texels, the instance vertex stage grows the quad and its UVs by the same reach
  (a material uniform, so part of the batch key) with the same arithmetic as the per-mesh builder;
  bounds, culling and D7's rectangles use the grown quad.
- **R25 2D lights.** Lights never break a batch (one block per pass, §3); Shadow Caster 2D shadows
  likewise.

### Functional — the bulk API

- **R26 Sprite Batch component.** A renderer that draws many sprites from one GameObject: a sheet
  from S44's `SpriteSheetPool` (an `.aseprite`; S45's `.spriteatlas` and sliced textures once S45
  lands) or a plain texture as its frame source; a material; Sorting Layer and Order; capacity;
  mode `Retained` (instances persist until changed) or `Clear Each Frame` (immediate mode: what the
  scripts add this frame is drawn once); space `Local` (instances move with the GameObject, through the
  batch transform) or `World`. Its instances use the format of D2 directly.
- **R27 Bulk writes.** Lua and C++ write whole streams at once from flat arrays (positions, frames,
  rotations, scales, colours, FX), plus per-instance `set`/`add`/`remove` for small edits; a write
  validates once per call, not per element (R43).
- **R28 Sorting inside a batch.** A Sprite Batch draws its instances in index order, or sorted by the
  sort axis (`sortInstances`), as one item in the pass sort. Its instances never interleave with other
  renderers (use separate batches or Individual tilemap mode for that).
- **R29 Frames by name.** A frame is an index into the batch's frame list, which the API fills from
  sheet frame names (S44's record), Sprite frames or pixel rects; an unknown name is a typed error at the call.
- **R30 Picking and bounds.** A Sprite Batch's bounds are its instances' (cached, refreshed when they
  change); the Scene view picks the batch as one GameObject.

### Functional — integration

- **R31 Tilemap chunks (S50).** S50's chunks are static batches (R15) of tile instances in layer
  space, with `CLAMP_FRAME`, per-cell flip/transpose bits (D2 flags) and edges S41 R13 makes
  bit-identical. Chunk-mode layers
  sort as one item per chunk at the layer's key; Individual-mode layers contribute one item per tile to
  the run's sort (S50).
- **R32 Particle Systems.** Particle Systems keep their own instanced path (S20; already one draw per
  system) and take part in the sort with their Sorting Layer and Order. S49's pixel particles do the
  same (S49 §2 keeps them off the Sprite Batch).
- **R33 Legacy tilemaps.** The legacy free tilemap keeps its mesh path and sorts as one item.
- **R34 Polygon meshes and plain meshes** sort (R2) but stay on the per-mesh path.

### Platform & backends

- **R35** The batched path runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan with vertex and
  fragment shaders only: per-instance vertex attributes (step function per instance, divisor 1),
  bindings at offsets (no base instance on GLES3/WebGL2), no storage buffers, no compute, no indirect
  draws, no `step_rate` other than 1 (Vulkan has none, §3).
- **R36** At most 8 vertex attributes (1 per-vertex corner + 4 core + 3 optional) and 5 vertex
  buffer slots per batch: within WebGL2's guaranteed 16 attributes and sokol's 16 slots.
- **R37** Integer attributes only where GLES3 has them (`uint` flags through `glVertexAttribIPointer`);
  half-float attributes (core in GLES3/WebGL2) for colour and FX.
- **R38** Shader variants per stream mask are created through S32 (async where available) and recorded
  in the warm-up list; a batch whose pipeline is still being created draws on the per-mesh path that
  frame instead of drawing nothing (S32's stand-in rule).
- **R39** Exported players (Compile and prebuilt, S30) carry the Sorting Layers table and the batching
  settings; disabled batching (R20) compiles nothing extra.

### Performance (release builds; "mobile" = Pixel 6a / iPhone 12 class, "web" = wasm in Chrome, one thread, on the same phone's browser for mobile web; "desktop" = the build server reference machine; `je-bench-sprites`)

- **R40 10k moving sprites, one atlas page.** 10,000 sprites 16×16 to 32×32 px, all moving every
  frame, at 1080p: draws ≤ 4 (one per page per 16,384 instances plus the camera's other draws); CPU
  (gather, sort, pack, upload) ≤ 2.0 ms mobile, ≤ 3.0 ms web, ≤ 0.5 ms desktop; GPU ≤ 4 ms mobile;
  60 fps sustained for 60 s on mobile and web. The "before" column (per-mesh path) is recorded in
  this spec.
- **R41 10k static sprites.** Nothing moved: ≤ 0.3 ms CPU mobile (R14), zero upload bytes.
- **R42 Mixed scene.** 2,000 sprites over 4 atlas pages with 10 Sorting Layers and a Y axis: draws ≤
  number of layer × page transitions in sorted order (reported by AC6), CPU ≤ 1.0 ms mobile.
- **R43 Lua bulk.** `SpriteBatch:setPositions` of 10,000 positions ≤ 1.0 ms mobile, ≤ 1.5 ms web;
  per-instance `set` ≤ 0.5 µs desktop per call.
- **R44 Memory.** Core instance 40 bytes, rotation +16, colour +8, FX +8. Per-frame stream bytes are
  within `streamStagingBytes` (16 MB mobile/web): 10,000 instances with every stream ≤ 720 KB.
- **R45 Disabled costs nothing.** A scene without sprites runs no batcher code past one empty check;
  with batching off (R20) nothing of S52 allocates. Sorting with all-default fields adds ≤ 2 % to
  today's 2D pass CPU (AC1 bench).
- **R46 No regression of the per-mesh path:** a fallback sprite costs what it costs today ± 5 %.

### Editor UX (human)

- **R47 Sorting Layers editor:** in Project Settings > Tags and Layers, a list with add, rename,
  delete (renderers on a deleted layer fall back to Default, with a warning naming how many) and drag
  to reorder; undoable like the other project settings rows.
- **R48 Renderer rows:** "Sorting Layer" (dropdown with "Add Sorting Layer...") and "Order in Layer"
  on Sprite, Mesh Polygon, Mesh (under Additional Settings), Particle System (Renderer), Sprite Batch
  and Tilemap Layer; multi-edit and undo as every other row.
- **R49 Sorting Group** in Add Component > Rendering, with "Sorting Layer", "Order in Layer".
- **R50 Batch view:** Scene view draw mode "Sprite Batches" tints each batch with a stable hashed
  colour and draws per-mesh sprites hatched; hovering a sprite shows "batch 12 of 40 · 316 instances ·
  broke: texture (atlas_b page 1 after atlas_a page 0)". The stats overlay (`DebugOverlay`) shows
  "Batches", "Instances batched", "Saved by batching".
- **R51 Sprite Batch Inspector:** source, material, mode, space, capacity, count (read-only), a
  frame list preview, Sorting rows.

### Agent UX (MCP, Lua, CLI)

- **R52 `get_render_stats`** gains `batches`: `{count, instances, draws_saved, per_mesh, breaks:
  {reason: count}, top_breaks: [{reason, cause, before, after}]}` (up to 8, entity references), and
  the parameter `explain_batches` (default false) that adds `list: [{index, instances, page, material,
  sorting_layer, first, last}]` up to `max_batches` (default 64). Also in `jasper://stats`.
- **R53 Sorting through existing tools:** Sorting Layers are `tags_and_layers.sorting_layers`
  (`get_project_settings`/`set_project_settings`; a list of names, ids kept by name); renderer fields
  are ordinary component properties for the existing component tools; the Sorting Group is an ordinary
  component. No new tool.
- **R54 Lua:**
  - `SortingLayer.getId(name)`, `SortingLayer.getName(id)`, `SortingLayer.getRank(id)`,
    `SortingLayer.list` (property).
  - Renderer properties `sortingLayer`, `sortingOrder` on `Sprite`, `Mesh`, `ParticleSystem`,
    `SpriteBatch`, `TilemapLayer` (S50).
  - `SpriteBatch` (§6.3); `Profiler.getRenderStats()` gains `batches`, `instancesBatched`,
    `drawsSaved`, `batchBreaks` (a table by reason).
- **R55 Agent guide:** `docs/mcp/guide/rendering-2d.md` (`jasper://docs/rendering-2d`): sorting rules,
  the break reasons and how to fix each, the Sprite Batch recipes; one line in the `## Pixel art` digest section of the
  guide.
- **R56 Determinism:** for the same scene and camera, `batches` and its `list` are identical on every
  run (R7, D7).

### Data & compatibility

- **R57** New fields (`sortingLayer`, `sortingOrder`, `sortAxisOverride`) and new components (Sorting
  Group, Sprite Batch) are optional: no scene format bump (`editor/StreamMigrations.h:17-20`). Old
  scenes load and draw in today's order (AC1).
- **R58** `project.yaml` gains `sortingLayers: [{id, name}]` (absent = only Default) and the
  `rendering.sprite_*` settings with their defaults; no project format bump.
- **R59** A renderer referencing an unknown layer id draws on Default and the load report says so
  (one line per scene, with the count).
- **R60** The cooked scene format (S30) carries the new fields through the generated field tables; the
  Compile build's factories set them (`RuntimeFields.gen.inc` regenerated).
- **R61** Sprite Batch instances saved with the scene (Retained mode, `saveInstances` on) are one
  base64 blob per stream (little-endian D2 records), so a 10,000-instance batch does not become 10,000
  YAML nodes; Clear Each Frame batches save no instances.

## 6. Design constraints & decisions

- **D1 Layout.** Batching is general 2D rendering, not pixel-art-only, so it lives with the renderer
  (a deviation from the ledger's `engine/core/pixelart/`, which keeps the snap rule, S41):
  - `engine/core/render/sprite/SpriteInstance.h` (the format, flags, pack helpers),
    `SpriteBatcher.{h,cpp}` (gather, key, sort, merge, lookahead, pack, stats),
    `InstanceStream.{h,cpp}` (the paged `sg_append_buffer` stream, extracted from `ParticleStream`,
    `RenderSystem.h:2372-2385`, shared with particles);
  - `engine/core/registry/SortingLayers.{h,cpp}` (ids, names, ranks; beside `TagManager`);
  - `engine/core/component/SortingGroupComponent.h`, `SpriteBatchComponent.h`;
    `engine/core/object/SpriteBatch.h` (the Lua-facing wrapper);
  - `engine/shaderlib/includes/sprite_instance_vs.glsl` (the vertex path), included by `mesh.vert`
    and `includes/surface_vs.glsl` under `HAS_SPRITE_INSTANCING`.
- **D2 The instance format.** One record layout for every producer (sprites, Sprite Batch, tile
  chunks); little-endian, tightly packed.

  | Stream | Attribute | Format | Bytes | Meaning |
  |---|---|---|---|---|
  | core | `i_pos` | FLOAT3 | 12 | quad origin (min corner, pivot applied) in batch space; with the rotation stream: the pivot |
  | core | `i_size` | FLOAT2 | 8 | quad size in batch units (scale folded in; negative = mirrored) |
  | core | `i_uv` | FLOAT4 | 16 | frame UV min and max in the page (flips folded in), computed on the CPU by the per-mesh path's own function, so both paths sample the same floats |
  | core | `i_flags` | UINT | 4 | bit 0 transpose (tiles), 1 `CLAMP_FRAME`, 2–3 tangent mirror signs (2D normal maps), 4–31 reserved (0) |
  | rotation | `i_rot` | FLOAT4 | 16 | cos θ, sin θ (computed on the CPU, as the per-mesh path's matrix), pivot x, y as fractions of the size |
  | colour | `i_color` | HALF4 | 8 | linear RGBA tint, HDR allowed (vertex colour × material colour of the per-mesh path) |
  | FX | `i_fx` | HALF4 | 8 | §R23 slots |

  Plus one static per-vertex stream, `a_corner` (the unit quad's 4 corners), and the shared 6-index
  quad buffer. Per draw: the batch model matrix, the pass view-projection, S41's snap uniforms (in the
  frame block), and the grown-quad reach (R24a) when the material has one.
- **D3 Absent streams are shader variants.** sokol has no constant vertex attributes; a stride-0 buffer
  is not one (sokol computes the stride, §3); a one-element buffer with a huge `step_rate` is not
  portable (Vulkan ignores it, §3); a full-length defaults buffer wastes bandwidth on mobile. So
  `INST_ROT`, `INST_COLOR` and `INST_FX` defines select the attributes and the defaults are constants
  in the shader: at most 8 vertex variants per material variant (16 with S41's `Pxs` bit), created
  lazily (R38). All of them compile the same fragment source. The engine property bits for them
  (`HAS_SPRITE_INSTANCING` and the stream mask) are appended to MESH's bits at landing, after or
  before S41's `Pxs` (same wave), with fixed names and no hard-coded numbers.
- **D4 Same fragment stage, same varyings.** `sprite_instance_vs.glsl` writes exactly the varyings the
  per-mesh vertex path writes (UV, colour, world position for 2D lights, the frame rect, normal
  (0, 0, 1) and the tangent with its mirror sign as `MeshSystem::createTilemap`/`createSprite`
  compute it, `MeshSystem.cpp:1074-1080`), so the fragment program, and with it every effect, is the
  per-mesh one. Parity (AC2) then only depends on positions and UVs, which D2 makes identical inputs.
- **D5 Ordering.** Per pass: `SpriteBatcher::gather` walks the Transform array once (the loop that
  draws today), producing items (a renderer, a static batch, a Sorting Group, a fallback, a UI subtree
  boundary) with a 64-bit key: rank (8 bits) | order + 32768 (16) | axis distance as an
  order-preserving float (32) | and the hierarchy index as the final tie-break, then a stable radix sort
  per run (R3). Groups sort recursively. 3D transparent queues extend `TransparentRenderComparison`
  with rank and order ahead of `sortPriority` (R6).
- **D6 Batch key and break reasons.** The key hashes: shader variant (with stream mask union),
  render state, page image and sampler, non-instanced material values and textures, stencil reference,
  scissor rect. When two consecutive items differ the first differing field is the break reason
  (`texture`, `material`, `blend`, `stencil`, `scissor`); `capacity`, `ui_run` and `sorting_group` are
  structural; `per_mesh` names the fallback cause:

  | Cause | When |
  |---|---|
  | `material_not_instanceable` | the shader lacks `@instanceable`, or an instanced property has no slot |
  | `extra_pass` | the material has a Before/After extra pass (`Material::extraPass`) |
  | `transform_3d` | the world transform rotates off the XY plane |
  | `shear` | non-uniform scale under a rotation |
  | `deformed` | lattice, VAT, skinning, morphs, a sprite grid > 1 |
  | `custom_vertex` | a material with its own vertex stage |
- **D7 Lookahead (R13).** An item may move before the k ≤ `lookahead` items preceding it in the run
  when its screen rectangle (after snapping, of the grown quad when R24a applies, expanded by one
  pixel for rasterization rules) overlaps none of them and
  its key equals the open batch's. Items never move across a UI subtree, a Sorting Group boundary or
  a fallback. Non-overlapping draws touch disjoint pixels, so order between them cannot change a pixel.
  The scan is O(n × lookahead) with the rectangles in a small ring, deterministic (no hashing order).
- **D8 Coherence (R14).** A run caches its sorted item list, batch list and stream offsets with a
  revision made of the members' change counters (S07), the camera's snapped position when the axis or
  culling depends on it, and the settings. Equal revision: draw the cached batches from the retained
  stream pages (kept two frames, as `ParticleStream`).
- **D9 Where it plugs in.** `renderCameraPass`'s 2D loop (`RenderSystem.cpp:16204-16316`) calls the
  batcher at each run boundary instead of `drawMesh` per sprite; UI drawing, scissors and Canvas
  Groups stay where they are. Distance-sorted passes batch consecutive transparent items in the queue
  order (no lookahead there). Shadow and G-buffer passes are unaffected (sprites cast no shadows,
  `sprite.frag:42-44`).
- **D10 Snap code is S41's.** The batched vertex shader calls S41's shader snap function and the
  per-mesh path calls S41's CPU function; S52 adds no rounding of its own. If S41 lands after S52
  M2, S52 ships with snapping off on both paths and S41 wires both.

### 6.1 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `get_render_stats` | `profiling` | `explain_batches`, `max_batches`; the result gains `batches` | +220 (description +120, schema +100) |
| `get_project_settings` / `set_project_settings` | core | none (the new keys are rows) | 0 |

- The `profiling` group's own limit is 7,000 characters (`tools/mcp/budget.json`, S15). If the
  measured group is over after the change, S52's PR raises that entry to the measured size, with the
  reason "S52: batch statistics on get_render_stats (extending it instead of a new tool)". No new
  tool, no core growth.
- The `profiling` entry (7,000) is S15's own cap, below `group_default` (15,000); a raise to the
  measured size stays below the default and is recorded in that entry's `spec` text, as conventions
  ("a spec's own budget is its group's `groups` entry") allow. No exception is added.
- Guide: one line in the family's `## Pixel art` digest section (S41 creates it; `## 2D and 3D` has no
  room): "2D draw order: Sorting Layers, Order in Layer, Sorting Group; batching and its breaks:
  get_render_stats batches, jasper://docs/rendering-2d" (≤ 140 bytes, the ledger's share).

### 6.2 Settings (Project scope; rows in `editor/settings/SettingsRows.cpp`, beside `tags_and_layers.layers`, `:2310`)

| Key | Type | Default | Meaning |
|---|---|---|---|
| `tags_and_layers.sorting_layers` | list of names | `["Default"]` | R1; ids are kept by name across edits |
| `rendering.sprite_batching` | bool | true | R20 |
| `rendering.sprite_batch_lookahead` | int 0..128 | 32 | R13 |
| `rendering.sprite_sort_axis` | none \| y \| custom | none | R4 |
| `rendering.sprite_sort_axis_vector` | vec3 | (0, 1, 0) | the custom axis |

### 6.3 API sketches

```cpp
namespace jasperengine {
    struct SpriteInstance {            // core stream, 40 bytes
        Vector3 pos; Vector2 size; Vector4 uv; uint32_t flags;
    };
    enum SpriteInstanceFlags : uint32_t { SPRITE_TRANSPOSE = 1, SPRITE_CLAMP_FRAME = 2, SPRITE_MIRROR_U = 4, SPRITE_MIRROR_V = 8 };
    enum class SpriteStream : uint8_t { Rotation = 1, Color = 2, Fx = 4 };

    // a producer's retained batch (S50 chunks, a retained Sprite Batch)
    struct StaticSpriteBatch {
        Matrix4 model; AABB worldBounds; const Texture* page; const Material* material;
        uint8_t streams; uint32_t count; uint64_t revision;   // a new revision re-uploads
        std::span<const SpriteInstance> core; std::span<const uint8_t> rotation, color, fx;
    };

    enum class BatchBreak : uint8_t { Texture, Material, Blend, Stencil, Scissor, Capacity, UiRun, SortingGroup, PerMesh };
    struct SpriteBatchStats {
        uint32_t batches = 0, instances = 0, drawsSaved = 0, perMesh = 0;
        uint32_t breaks[9] = {};
    };

    struct SortingGroupComponent { bool enabled = true; int32_t sortingLayer = 0; int16_t sortingOrder = 0; };

    enum class SpriteBatchMode : uint8_t { Retained, ClearEachFrame };
    enum class SpriteBatchSpace : uint8_t { Local, World };
    struct SpriteBatchComponent {
        bool enabled = true;
        SpriteBatchMode mode = SpriteBatchMode::Retained;
        SpriteBatchSpace space = SpriteBatchSpace::Local;
        AssetRef sheet; AssetRef texture;          // an S45 sheet, or a plain texture
        Material material;
        int32_t sortingLayer = 0; int16_t sortingOrder = 0;
        bool sortInstances = false; bool saveInstances = false;
        uint32_t capacity = 1024;
        // runtime: frames, streams, revision
    };
}
```

```lua
-- bullets, coins, stars: one GameObject, one draw
local batch = SpriteBatch(scene, entity)
batch.sheet = "sprites/fx.spriteatlas"       -- any S45 sheet; or batch.texture = "bullets.png"
local frame = batch:addFrame("bullet_a")      -- S45 frame name; addFrameRect(x, y, w, h) for a texture
batch.capacity = 4096
batch:setPositions(xy)                        -- flat {x1, y1, x2, y2, ...}; count = #xy / 2
batch:setFrames(frames)                       -- optional, else frame 0
batch:setRotations(r) ; batch:setScales(s)    -- radians; uniform or {sx, sy} pairs
batch:setColors(rgba)                         -- flat {r, g, b, a, ...} (linear) or Color values
batch:setFx("flash", amounts)                 -- "palette" | "flash" | "fade" | "dissolve"
local i = batch:add(x, y, frame)              -- small edits
batch:set(i, x, y) ; batch:remove(i) ; batch:clear()
print(batch.count)

sprite.sortingLayer = SortingLayer.getId("Foreground")
sprite.sortingOrder = 5
local stats = Profiler.getRenderStats()
print(stats.batches, stats.drawsSaved, stats.batchBreaks.texture)
```

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R3, R6, R45, R57) | `engine/tests/core/render/SpriteSortTest.cpp`: the item order for fixture scenes with every field at its default equals today's draw order (recorded from the base commit's `renderCameraPass` through a draw trace); Sorting Layers, Order, groups (nested), axis `y` and `custom` give the expected orders; UI subtrees stay in place. `SpriteBatchGpuTest.LegacySceneUnchanged`: the fixture `tools/fixtures` 2D scenes render pixel-identical before and after S52 (golden from the base commit). Bench `je-bench-sprites --sort-only`: ≤ 2 % over the base. |
| AC2 (R8–R12, D2, D4, R20) | `engine/tests/core/render/SpriteBatchGpuTest.cpp` `ParityAllStreams`: a 512×288 scene of 600 sprites (two atlas pages, mirrored, pivots, tints 0/0.25/0.5/1, `library:sprite` FLASH/OUTLINE/HUE_SHIFT, 2D lights with normal maps, S41 pixel-perfect on and off) drawn with batching on and off: unrotated 0 differing pixels; rotated ≤ 8 per 10,000 drawn pixels, and 0 on the glcore reference renderer. Goldens on glcore and Vulkan (`GRAPHIC_BACKEND=vulkan`), and the web gate (`GATE=sprite-parity tools/build.sh web-gate`, WebGL2). Red control: a build that rounds the origin with `std::round` instead of S41's function fails the pixel-perfect half. |
| AC3 (R11) | `SpriteBatchGpuTest.SharedEdges`: 64 adjacent pieces with bit-identical shared edges (S41 R13) at fractional camera offsets (0.25 px steps) under a Pixel Perfect Camera: no gap and no double-covered column (a coverage count image), batched and per-mesh; Rotate On Grid and Steps90 instances match the per-mesh pixels. |
| AC4 (R13, D7) | `SpriteBatchGpuTest.LookaheadIsPixelSafe`: 2,000 interleaved sprites of two pages, lookahead 0 vs 32: identical pixels, fewer draws with 32 (asserted counts); a crafted overlap is never reordered (unit test on `SpriteBatcher` with a trace). |
| AC5 (R14, R41) | `SpriteBatcherTest.Coherence`: a static scene packs and uploads once over 100 frames (counters); a moved sprite repacks its run only. |
| AC6 (R18, R52, R56) | `tools/mcp/tests/test_sprite_batching.py`: a scene with one case per break reason and per-mesh cause; `get_render_stats` names each in `breaks` and `top_breaks` with the right entities; `explain_batches` lists equal on two runs; `batches` absent-reason counts are 0 on a clean scene. Includes the profiling group hidden/listed assertion for the new parameter's tool. |
| AC7 (R21–R24) | `SpriteBatchGpuTest.InstancedProperties`: 100 sprites differing only in `flashAmount` draw in one batch and match the per-mesh pixels; differing in `outlineColor` break with `material`; a shader without `@instanceable` falls back with `material_not_instanceable`. S43/S48 add their slot cases to this test when they land. |
| AC8 (R26–R30, R43, R61) | `tools/mcp/tests/test_sprite_batch.py` (Lua through Play, `read_console`): positions, frames by sheet frame name, rotations, colours, FX; Clear Each Frame draws only this frame's; an unknown frame name errors at the call; the batch picks as one GameObject (`pick_at`); a saved Retained batch reloads with identical instances (blob). `engine/tests/core/script` Lua test for `SortingLayer`. Bench `je-bench-sprites --lua-bulk` within R43. |
| AC9 (R35–R38) | the GPU suites above pass on glcore and Vulkan; `tools/build.sh web`, `web-gate`, `tools/build.sh android` (emulator gate) pass; Metal and D3D11 by CI. `SpriteBatchGpuTest.PipelinePending`: with S32's async mode, a batch whose variant is not created yet draws per mesh, never blank. |
| AC10 (R40–R44) | `je-bench-sprites` (`engine/tests/core/bench/SpritesBench.cpp`, scene `tools/test-projects/sprite-bench`): desktop numbers by CI; mobile and web numbers measured once on a Pixel 6a and an iPhone 12 (Chrome and Safari) through the bench scene's on-screen report, recorded with the device and build in `docs/specs/evidence/s52/`; the before/after table is appended to this spec. |
| AC11 (R47–R51) | `tools/mcp/tests/test_sorting_layers_editor.py` (xvfb, isolated editor): add/rename/reorder/delete a Sorting Layer through the settings rows and undo; renderer rows and multi-edit; Sprite Batches draw mode screenshot (evidence); `screenshot` of the Scene view shows the batch tints. |
| AC12 (R58–R60) | `editor/tests/stream/StreamComponentTest.cpp`: the new fields and components round-trip, old fixtures re-save byte-identically when untouched; `CookedScene.RoundTripEveryComponent` covers Sorting Group and Sprite Batch; a smoke export (`tools/smoke-test.sh`) of a sorted scene matches its editor screenshot. |
| AC13 (§6.1, R55) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_protocol.py` (guide size); `tools/lua_api/tests` find `SpriteBatch`, `SortingLayer`; the doc test runs the Lua block of `docs/rendering-2d.md`. |

## 8. Dependencies & conflicts

- **Depends on.** S41 (same wave: the snap functions, CPU and shader, and the pixel-perfect camera
  uniforms; D10 says what happens if it is late). S44 (same wave: `SpriteSheetPool`, sheet pages and
  frame names; until it lands, a page is a texture and frames are rects). S45 (wave 5B) needs nothing
  here: its atlas pages come through S44's pool. S07 (change tracking for D8). S20 (the stream code D1
  extracts). S32 (async pipeline creation and the warm-up list).
- **Interfaces given.** S50: `StaticSpriteBatch`, the D2 flags, per-tile sort items for Individual
  mode. S43 (same wave) and S48 (wave 5B): the FX slots (R23) and the `instance=` manifest attribute.
  S42: Sprite Batch drawables for `check_pixel_art` (S42 R9). S51: Sprite Batch if it wants glyph
  quads batched.
- **Merge hotspots.** `RenderSystem.cpp` `renderCameraPass` (`:15855-16330`, also S13, S16, S20,
  S41), `RenderSystem.h` comparators, `MeshComponent.h` (S43, S48 may add material state),
  `engine/shaderlib/mesh.vert`, `includes/surface_vs.glsl`, `library/sprite.vert|frag` (S43, S48),
  `RenderSystemParticles.cpp` (S20, S49: the stream extraction), `SettingsRows.cpp`,
  `ProfilerTools.cpp`, `budget.json`, `RuntimeFields.gen.inc`.

## 9. Risks & defaults

- **One page per batch.** Projects without atlases batch poorly. Default: the break reason says
  `texture` with both pages, and the guide points to S45. A multi-page path is a later spec if
  measurements ask for it.
- **Variant count.** Up to 8 vertex variants per material variant. Default: lazy creation and the
  warm-up list; the bench reports pipelines created.
- **Half-float colour.** HDR tints need more than 8 bits; half precision changes an 8-bit sRGB tint by
  at most one level in rare cases. Default: the parity test uses exactly representable tints; a full
  float colour stream would cost 8 more bytes per instance and is not worth it.
- **Sorting inside UI.** World sprites under a RectTransform keep hierarchy order (R3). Default kept:
  UI layout is order-driven.
- **Lookahead and drawing outside the quad.** An effect that draws outside its quad without growing
  it would let lookahead reorder overlapping pixels. Default: S48 grows the quad for every such
  feature (S48 R7, our R24a); a shader that draws outside its quad any other way is not
  `@instanceable`.
- **Ledger location.** S52's code is under `engine/core/render/sprite/`, not `engine/core/pixelart/`
  (D1): batching serves all 2D content.

## Milestones

- **M1** Sorting Layers, renderer fields, Sorting Group, axis, the ordering in 2D and 3D passes, the
  editor rows, Lua `SortingLayer` (R1–R7, R47–R49, R53, R54 part, R57–R60); AC1, AC11 part, AC12.
- **M2** Instance format, the vertex include, batching of the default sprite material, fallbacks and
  break counting, stats in `get_render_stats` and Lua (R8–R12, R15–R20, R35–R39, R52); AC2, AC3, AC6,
  AC9. Needs S41's snap functions or ships with D10's rule.
- **M3** `library:sprite` and the instancing manifest, FX slots, FLASH's `instance=fx.y` (R21–R25);
  AC7. S43 (same wave) and S48 (wave 5B) then add their slot cases.
- **M4** Sprite Batch component, Lua and C++ bulk API (R26–R30); AC8.
- **M5** Lookahead, coherence, the Batch view (R13, R14, R50); AC4, AC5.
- **M6** Static batches for S50, particles in the sort, budgets, docs, guide (R31–R34, R40–R46, R55,
  R56); AC10, AC13.

## Deviations

None yet (spec-first stream; implementers append here).
