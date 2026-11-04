# Meshes

Mesh rendering for agents: what to set, how to check it.

## Draw submission

- **GPU Instancing** (project setting `graphics.gpu_instancing`, on by default; Project Settings >
  Graphics): opaque Mesh draws of the same mesh, material and shader become one instanced draw,
  in the camera pass, the depth pre-passes, the shadow maps and the reflection probes. The image
  is the same as with it off. Off submits every draw alone.
- A Mesh opts out with `set_properties` `{"mesh": {"allowGpuInstancing": false}}` (Inspector:
  Allow GPU Instancing; Lua `Mesh.allowGpuInstancing`).
- What never instances: skinned, morphing, lattice and Vertex Animation meshes, transparent
  submeshes, extra passes, sprites and tilemaps, terrains, InstancedMeshes, mirrors, forked
  shaders, and meshes of up to 64 vertices with the built-in shader (dynamic batching's).
  A per-object difference (its own colour, lights or reflection probe) splits a batch.
- Shadow maps, point light faces, reflection probes and render-to-texture cameras take their detail
  levels with `quality.shadow_lod_bias` (S56: below 1 coarser) when the scene's Mesh Detail (LOD)
  is on.
- A terrain with `staticShadowCaster: true` keeps its shadow in the static shadow cache while the
  camera moves.

## Checking the cost

`get_render_stats` (the `profiling` tool group) splits the frame:

- `draw_calls_by_pass`: `main` (the cameras' colour passes), `shadow`, `depth` (depth pre-pass,
  G-buffer), `other`; `draw_calls` and `triangles` keep every pass.
- `main_triangles`, `shadow_triangles`.
- `instanced_draws` and `batched_instances`: the instanced draws GPU Instancing made and the draws
  they replaced. 0 with many copies of one mesh: check the setting, the copies' materials and
  `allowGpuInstancing`.
- `visible`, `culled`: mesh draws the passes' visible lists kept and culled.
- `uniform_bytes` and `uniform_bytes_kept` (constant blocks not applied again),
  `buffer_upload_bytes`.

Lua: `scene:getRenderStats()` gives the same counters (camelCase: `mainDrawCalls`,
`shadowTriangles`, `instancedDraws`...).
