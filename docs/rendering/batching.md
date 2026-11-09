# Dynamic Batching

Consecutive Sprites, small meshes and UI elements that share a material draw as one draw call.
The image is the one they make drawn one by one: their vertices are placed on the CPU and drawn
together with the pipeline, textures and uniforms of the first. It is on by default
(**Project Settings > Graphics > Dynamic Batching**, `graphics.dynamic_batching`); a game can
change it while it runs with `Engine.dynamicBatching` (Lua) or `Engine::setDynamicBatching`
(C++), and it goes back to the project's setting when Play stops. Spec:
[2026-11-04-sprite-batching-design.md](../superpowers/specs/2026-11-04-sprite-batching-design.md).

## What batches

- **Meshes**: Sprites, MeshPolygons with indices, primitives and any mesh of one submesh with at
  most 64 vertices, 16-bit indexed triangles, drawn with the built-in mesh shader. Unlit, 2D-lit
  (Light 2D, Shadow 2D) and lit (pixel and vertex lights, received shadows, fog, SSAO) variants
  all batch.
- **UI elements**: Images, Panels, Buttons and Texts of at most 256 vertices drawn with the
  built-in UI shader.
- **Every pass that draws them**: the colour pass, the camera's depth pre-pass, the scene-depth
  pass of scene inputs and the atmosphere, and shadow casters. The G-buffer pass of SSR draws
  them one by one.
- **GPU Instancing** (S55) takes the mesh draws that did not batch: a batched draw is never
  instanced, and an object drawn alone is instanced as before.

Two objects join one draw when they come one after the other in the pass's order (the hierarchy
order in 2D, the sorted queues in 3D) and share: the shader variant and render state, every
texture and sampler, the clip rect, the 4,096-unit origin cell of their positions (centred on the world origin), and the values
the draw does not take from their vertices (below).

What a draw takes from each object's vertices, so it never breaks a batch:

- its transform (positions, normals and tangents are placed in world space, relative to the
  origin cell: precision far from the world origin is the object's own);
- its sprite frame (the texture rect goes into the UVs);
- its colour (`Mesh.color`, a Sprite's colour, a UI element's colour faded by its Canvas Groups),
  when the shader variant reads vertex colours (Sprites and UI always do).

What must be equal: metallic, roughness, emission and alpha cutoff of lit materials, the alpha
cutoff of alpha-masked ones, the UV set of each map, the lights reaching a lit object (and its
ambient), the colour of a Text without a Text Effect.

**2D cameras reorder.** An orthographic camera looking along Z (every 2D and UI camera) lets a draw
join an earlier batch when its bounds touch nothing drawn in between: `coin, enemy, coin, enemy`
of two textures that do not overlap is two draws, not four. Overlapping draws keep their order.

**Pixel art** (S52): Sprites under a Pixel Perfect Camera batch, rotated ones too (a batch holds
one snap rule: corners or pivot); `library:sprite` batches, its FLASH amount and Palette Swap row
per sprite, its other values equal; OUTLINE and OVERLAY batch on Sprites. Draws batch only within
one Sorting Layer, Order in Layer and Sorting Group ([2D sorting](2d-sorting.md)).

**Never batched** (drawn alone, counted with the reason): custom shaders and library shaders other
than `library:sprite`, materials
with an extra pass, image-based lighting and reflection probes, mirrors, skinned and morphed
meshes, lattices, Vertex Animation, mesh LODs, more than one submesh, render-target textures,
fake billboards, instanced meshes, tilemaps and terrains (they have their own one-draw paths),
Points, Lines, Trails, Particle Systems and HTML UI. Anything that does not batch also ends the
batch before it.

## Settled objects and cost

An object whose geometry, frame, colour and transform did not change since its last frame keeps
its placed vertices: drawing it costs a copy of them (256 bytes for a quad) and a comparison of
its key with the batch's. A moving one is placed again (four vertex transforms for a quad). Under a
scrolling camera a batched mesh is not given a model-view-projection matrix at all; it is made
when the mesh draws alone.

Direct writes count: a script or C++ code writing a material's colour, a texture rect, a transform
or a mesh's vertices (with `needUpdateBuffer`) shows the next frame, batched or not.

Batched vertices go to the GPU in stream buffers sized from the frame's need (a frame that needs
more creates another at once; the next frame uses one of the total size); quads use one shared
index buffer and upload no indices. With batching off nothing is allocated.

## GPU buffers: no sprite ceiling

Immutable vertex and index buffers are shared by their content: every Sprite of one shape (size,
pivot, flip, texture size) draws from one vertex buffer and every quad from one index buffer, so
20,000 sprites of ten shapes hold about twenty buffers, not 40,000 (the pool holds 16,384). A
sprite whose geometry changes takes the buffer of its new bytes; one that changes twice within 30
frames (an animated size) gets a dynamic buffer of its own.

## Seeing it

- The Game view's **Stats** show `Batches` and `Saved by batching`.
- **Window > Analysis > Frame Debugger** with the draw list on shows each `Dynamic Batch (N
  objects)`; hover one for why it could not be batched with the draw before it.
- The Profiler's monitors `Batches` and `Saved by batching`.
- MCP: `get_render_stats` returns `batching` (counters and breaks by reason) and, with
  `batch_breaks: N`, the first N breaks with their entities; `get_gpu_passes include_draws` marks
  batch draws (`docs/mcp/guide/profiling.md`).

## For engine code

- **Sort buckets.** `BatchItem::sortKey` (`engine/core/render/DrawBatcher.h`) is the bucket of a
  draw; a draw never moves past one of another bucket and a run holds one bucket. A scene that does
  not sort puts every draw in bucket 0; Sorting Layers, Order in Layer and Sorting Groups put
  `(layer value, order, outermost group)` there (`RenderSystemSorting.cpp`).
- **Per-object values in vertices.** A per-object shader value that must not break batches lives
  in a vertex attribute of the mesh, the same at each of its vertices: the bake copies attributes
  it does not know as they are. `RenderSystem::setBatchAttributeRole(type, POINT)` bakes one like
  a position (an object-space point: a sprite's `a_snapPivot`), `DIRECTION` like a normal.
  A value kept as a material uniform is part of the batch key instead.
- **Batchable library shaders.** A manifest with `// @batchable [framerect=A,B]` batches: its
  textures and its custom block (the engine-written members left out) are in the key; a property
  with `instance=fx.<x|y|z|w>` is read as `customParams.<member> + spriteFx(<slot>)`
  (`includes/sprite_fx.glsl`): drawn alone the attribute `a_spriteFx` is 0, batched the bake writes
  the material's values into it and the run's uniform is 0 ('Sfx'). The features of `framerect=`
  read the frame rect per vertex (`a_frameRect`, 'Sfr'). Sprites get these attributes from
  MeshSystem when their material reads them; a mesh without them draws alone
  (`ineligible_frame_rect`).
