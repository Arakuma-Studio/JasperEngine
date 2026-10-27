# Vertex Animation: baked vertex and bone animation textures for crowds (VAT)

Status: draft for owner review. Owner direction: "both vertices and bones; mobile and web first, PC too; import Houdini
VATs; best UX for humans and AI agents". Branch `vertex-animation` (from main). Research behind every decision: the
research thread of 2026-10-27 (15 Unity VAT tools/repos, OpenVAT, SideFX Labs VAT 3, keijiro/HdrpVatExample,
Bonjour-Interactive-Lab/Unity3D-VATUtils, the SideFX "VAT in Unity" tutorial project, and two engine maps: rendering
and animation/editor/MCP). Summary of what was taken from where is in section 11.

## 1. Goal and non-goals

Today a crowd of skinned characters costs one CPU pose evaluation and one skinning matrix upload per character, and
instanced meshes share one pose (`RenderSystem.cpp:3398`, all instances read the same `bonesMatrix`). After this work:

- Any animation the engine can play (an `.anim` clip, a model clip `hero.gltf#Walk`, an entity Animation, a pose
  animation, lattice tracks, a hierarchy of moving parts) **bakes** into a **Vertex Animation** asset (`.vat`) that
  plays on the GPU with no CPU pose work: thousands of characters, each with its own clip, time, speed and crossfade,
  in **one instanced draw per material**.
- Two bake modes, one runtime:
  - **Vertices**: per-vertex position (offset from rest) and tangent frame per frame. Bakes anything that deforms:
    skin + morphs + lattices + cloth-like motion, and imported simulations.
  - **Bones**: per-frame bone matrices, skinned on the GPU from a texture. Kilobytes instead of megabytes, exact
    normals, attachment sockets for free. Skinned characters and rigid hierarchies (each moving part is a bone).
  - **Auto** (default) picks Bones when the deformation is skin-only or rigid, Vertices otherwise.
- **Imports**: SideFX Labs (Houdini) VAT 3 soft-body, rigid-body and dynamic-remeshing (fluid) exports, and OpenVAT
  (Blender) exports, are re-packed into the same `.vat` format at import, so one runtime path plays everything.
- Mobile and web first: every default fits OpenGL ES 3 / WebGL2 guarantees (2048 texture width, RGBA16F/RGBA32F
  sampled with `texelFetch`, no float filtering, no compute, no storage buffers); desktop presets raise the limits.
- Humans get a baker window, Inspector "Bake" entries where animations already live, a `.vat` asset inspector with
  staleness and rebake, Create Crowd, and a Lua API. Agents get one MCP tool (preview without writing, then write), a
  CLI, `check --vat`, `get_asset_info` and a guide, within the tool-definition budget.

Non-goals (now): an animator state machine/graph (Lua drives clips), root motion, IK or ragdoll on baked characters,
picking/physics against the deformed shape (rest pose, as lattices), motion vectors (the engine has none; the shader
function is pure in time, so a later motion-vector pass evaluates it again at a per-instance `prevTime` captured last
frame, reset to the current time on seek/restart), GPU-driven culling/indirect draws, Houdini
particle-sprite mode (section 4.6 reserves it), per-instance events by default (opt-in, 3.7).

Backward compatibility is absolute: no existing scene, prefab, shader fork, MCP call or test changes behaviour.
Meshes without a Vertex Animator never get the new shader variants.

## 2. The asset

### 2.1 Two files: `<name>.vat` (JSON) and `<name>.vat.glb` (generated)

- `<name>.vat`: canonical UTF-8 JSON (the `.anim` writer rules: 2-space indent, fixed key order, shortest round-trip
  numbers, deterministic bytes). This is the asset people drag, the Inspector shows and agents read/write. It holds
  the clip table, events, sockets, bounds, layout description and the bake **recipe** (sources + settings + source
  hashes) used for staleness and Rebake.
- `<name>.vat.glb`: the render mesh plus the baked payload, written by the baker/importer only ("generated, do not
  edit" in the Project window tooltip and the guide). glTF so the existing model pipeline loads the mesh, materials
  and textures; the payload is a buffer view referenced by the extension `JASPER_vertex_animation`
  `{version, payload: {bufferView, format, width, height}, layoutHash}`.
- Why two files: the JSON stays small, reviewable and agent-editable like `.anim`; the binary needs no base64; the
  mesh loads through `MeshSystem::loadGLTF` unchanged except for two attributes (3.2).

```json
{
  "format": "jasperengine.vat",
  "version": 1,
  "guid": "…",
  "mode": "bones",
  "mesh": "hero.vat.glb",
  "layout": {"precision": "float", "width": 2048, "height": 37, "samples": 2950, "bones": 41, "vertices": 5120,
             "normals": true, "layout_hash": "9c1f…"},
  "bounds": {"min": [-0.61, 0, -0.55], "max": [0.63, 1.92, 0.71]},
  "clips": [
    {"name": "Walk", "start": 0, "frames": 31, "fps": 30, "wrap": "loop", "speed": 1, "interpolate": true,
     "bounds": {"min": [-0.4, 0, -0.5], "max": [0.4, 1.9, 0.6]},
     "events": [{"time": 0.25, "function": "onFootstep", "args": ["left"]}]},
    {"name": "Idle", "start": 31, "frames": 60, "fps": 30, "wrap": "loop", "speed": 1, "interpolate": true}
  ],
  "sockets": [{"name": "RightHand", "path": "Armature/Hips/…/RightHand"}],
  "recipe": {
    "sources": [{"kind": "model", "path": "assets/models/hero.gltf", "clips": ["Walk", "Idle"], "hash": "sha1:…"}],
    "mode": "auto", "fps": 30, "preset": "mobile_web", "normals": true, "precision": "auto", "tolerance_mm": 1
  },
  "stats": {"bytes": 1810432, "max_error_mm": 0.21, "mean_error_mm": 0.04}
}
```

- **Editable by hand / Inspector / agents** (lenient, warnings with JSON paths, the `.anim` rules): clip `name`,
  `wrap` (`loop|once|ping_pong|clamp_forever`), `speed`, `interpolate`, `events`, the `sockets` list order and names,
  and removing whole clips (their rows stay in the payload until the next Rebake; the Inspector says so).
- **Generated** (`layout`, clip `start`/`frames`/`fps`/`bounds`, `bounds`, `stats`, `mode`, `mesh`): the loader
  compares `layout.layout_hash` with the glb extension's `layoutHash`; a mismatch, or a generated field changed in a
  way the payload can't honour, fails to load with "generated field X changed: rebake (Inspector > Rebake, or
  bake_vertex_animation {rebake})". Unknown top-level keys are preserved.
- A Unity/Houdini file given as `.vat` is refused with a pointer to the importer (4.6).
- Rename/move/delete keep the pair together (Project window, `move_asset`/`delete_asset`, AssetDatabase GUID
  tracking of `.vat` like `.anim`).

### 2.2 Payload layout (both modes: one 2D texture, linear addressing)

One texture per asset, width `W` (preset: 2048 for `mobile_web`, 4096 for `desktop`), height as needed, rows packed
linearly so any count fits until `W × W` texels:

```
texel(t) = ivec2(t % W, t / W)
vertices: t = (frame * V + vertex) * S + k      S = 1 (positions), +1 tangent frame, +1 colour (imports), k < S
bones:    t = (frame * B + bone)   * 2 + k      2 texels = rotation quaternion, translation + uniform scale
```

`frame` is global: clip `start` + local frame, clips packed back to back (no padding, no per-clip textures), so every
character playing any clip of the asset shares one texture, one material and one instanced draw.

- **Vertices** (`S` = 1 + normals + colors; colors only from imports, 4.6.1): texel 0 = position **offset from the
  rest position** (`.xyz`, `.w` unused, written 0); texel 1 = the tangent frame as a unit quaternion mapping (+X, +Y, +Z) to (tangent, bitangent, normal); the tangent sign stays the
  static attribute's `w`. The baker enforces hemisphere continuity per vertex across consecutive frames so linear
  interpolation then normalize is valid. Imports with changing topology (fluid) store absolute positions over a zero
  rest (`"rest": "none"` in the clip; 4.6).
- **Bones**: the skin matrix `inverse(meshModel) * jointModel * inverseBind` (the formula at `RenderSystem.cpp:12646`)
  per bone per frame, decomposed into a unit quaternion (texel 0) and translation + uniform scale (texel 1), always
  RGBA32F (tiny). The shader nlerps the quaternions (hemisphere continuity enforced at bake) and lerps translation and
  scale, then rebuilds the matrix: unlike lerping matrices this never shrinks a fast-spinning piece between frames
  (keijiro/HdrpVatExample's rigid lerp artifact), and costs 2 texels instead of 3. A skin matrix that doesn't
  decompose within tolerance (non-uniform scale or shear on a bone) makes Auto pick Vertices (`nonuniform_bone_scale`).
  Rigid hierarchies and Houdini rigid pieces: each moving node/piece is one bone, its vertices weight 1.
- **Precision**: Vertices default to RGBA16F (offsets keep half precision near 0.5 mm per metre of motion);
  `precision: "auto"` measures the quantization error against the exact bake and switches to RGBA32F when it exceeds
  `tolerance_mm` (default 1 mm, scaled by the model's size: 0.05% of the bounds diagonal when larger). Reported in
  `stats` and the baker UI. Reserved third option `unorm16` (RGBA16UI read with `usampler2D`, normalized to the clip
  bounds: exact to range/65535 at the same 8 bytes per texel, core on GLES3/WebGL2) if half proves too coarse.
- **Sampling**: never filtered. `NEAREST`, clamp, no mips, `texelFetch`, two frames read and mixed in the shader.
  Works on every backend without `OES_texture_float_linear`; no bleeding between clips or wrapped rows.
- **Loop seams**: loop clips sample `frames = round(length * fps)` frames at `i * length / frames` (no duplicate end
  frame; the shader interpolates last → first); other wraps sample `frames + 1` so the last frame is exactly `length`.

### 2.3 The render mesh in `.vat.glb`

- Every renderer under the source root merges into **one mesh**, one submesh per material (materials and textures
  copied from the source), so a character is one draw per material however many parts it had.
- Vertices mode: skin and morph targets removed; rest = bind pose (or the first frame when the source has no bind
  pose). Bones mode: `JOINTS_0`/`WEIGHTS_0` kept with joint indices remapped to the baked bone order; no skeleton
  nodes are written.
- Custom attribute `_VAT_INDEX` (float, exact to 2^24): the vertex's row in the payload. Not `gl_VertexIndex`:
  D3D11's `SV_VertexID` ignores base vertex, `gl_VertexIndex` is local per glTF primitive here, and re-indexing/LOD
  would break it. LOD meshes from `MeshLodPool` reuse the vertices (`MeshLodPool.cpp:162-179`), so every LOD shares
  the payload.
- Mesh bounds = the union over every frame of every clip, so culling is right even for tools that don't know VAT.
- Node transforms are baked into the vertices before baking (the loader's `bakingFlatten` would otherwise move
  vertices after the bake, `MeshSystem.cpp:5290`): the glb has one node with an identity transform.

### 2.4 Limits (validated before baking, 4.7)

| Preset | Width | Max texels | Memory warning |
|---|---|---|---|
| `mobile_web` (default) | 2048 | 2048 × 2048 (4.2 M) | > 32 MB |
| `desktop` | 4096 | 4096 × 8192 (33.5 M) | > 256 MB |

At runtime a payload taller than `SystemRender::getFeatures().maxTextureSize2D` doesn't upload: the mesh draws at rest
and one error names the asset and the device limit.

## 3. Runtime (engine)

### 3.1 Data and pool

- `engine/core/animation/VertexAnimation.{h,cpp}`: immutable `VertexAnimationData` (mode, layout, clips with events,
  socket tracks, bounds) and `VertexAnimationIO` (parse/write the JSON; one parser for engine, editor, MCP, CLI).
- `VertexAnimationPool` (`engine/core/pool/`): `shared_ptr<const VertexAnimation>` keyed by normalized `.vat` path,
  loads through `Data::open` (resource packs work), holds the GPU texture, `invalidate(path)` for rebakes,
  `clearUnused`, registered with the other pools.
- `VertexAnimationRuntime::evaluate(state, now)`: the **same frame math as the shader** in C++ (section 3.4), used for
  `normalizedTime`, `isPlaying`, events, sockets and tests (parity test against the shader, 8).

### 3.2 Engine prerequisites (each is its own task, useful beyond VAT)

1. **Float texture upload**: `SokolTexture::createTexture` maps only RGBA8/R8/R16 and leaves `pixelFormat`
   uninitialised for float formats (`SokolTexture.cpp:294-309`). Add immutable RGBA16F/RGBA32F (and R/RG variants),
   `TextureData` float awareness (bytes per channel), no mips for float. Fixes undefined behaviour today.
2. **CPU skinning evaluator** `engine/core/render/CpuSkin.{h,cpp}`: full-mesh linear blend skinning (positions,
   normals, tangents) from `MeshComponent` data and `bonesMatrix`, morph targets (positions/normals/tangents with
   `morphWeights`), and stage-0/1 lattice via `lattice::deformVertex` (`LatticeSolver.h:71`). `pose::PoseSkin` caps at
   ~6000 vertices and positions only; this replaces nothing, it is new. Parity test against the GPU readback.
3. **Mesh attributes**: `MeshSystem` reads `_VAT_INDEX` and, for glbs with the extension, `JOINTS_0`/`WEIGHTS_0`
   without a skin (`MeshSystem.cpp:5376-5444`), new `AttributeType::VAT_INDEX` (`Render.h:172-212`).
4. **Per-instance animation data**: two optional per-instance vec4 attributes `i_vat0`, `i_vat1` appended to
   `InstanceRenderData` only when the instanced mesh has a Vertex Animator (stride 24 → 32 floats; others unchanged).
   Instances are re-sorted per view (`InstanceSort.h:15-30`), so `gl_InstanceIndex` can't index anything: the data
   travels with the instance. Touch list: `InstancedMeshComponent.h:14-27`, `MeshSystem.cpp:6797-6811`,
   `RenderSystem.cpp:11520-11523`, `Render.h:199-204`, `ShaderData.cpp:100-111`, Lua `Mesh.h:155-169` (+ bindings),
   Catalog/Stream/Factory/EntityCloner instance I/O.
5. **Bounds override**: `MeshComponent::animatedBounds` (AABB, empty = none) used instead of `verticesAABB` for the
   world AABB, the visibility precheck and instance spheres (`RenderSystem.cpp:13085, 12363, 7427, 11473`). Set
   from the clip bounds of the clips in use, plus the mesh's own `verticesAABB` while a drawn state is at rest
   (no clip: the clips' bounds don't hold the rest pose); the asset bounds when neither is known.

### 3.3 Vertex Animator component

`VertexAnimatorComponent` (display "Vertex Animator", Add Component > Animation, after Pose Animator; Lua
`VertexAnimator`, `object:getVertexAnimator()`, nil when absent; scene YAML `vertexanimator:`). ComponentType appended
at the end. Goes on the GameObject whose Model is the `.vat.glb` (dragging a `.vat` into the scene creates exactly
that: a GameObject with Model = `<name>.vat.glb` and Vertex Animator = `<name>.vat`).

Authored fields: `animation` (`.vat` path + GUID), `defaultClip` (empty = first), `playAutomatically` (true),
`speed` (1), `randomStart` (true: each instance, and a single entity, starts at a random time of its first clip,
seeded by entity id + instance index, so crowds never march in lockstep), `defaultFadeTime` (0.2),
`instanceEvents` (false, 3.7), `attachments` (`[{socket, entity}]`, 3.7).

Runtime (not serialized): resolved asset, per-entity state A/B (clip, start, speed, fade), per-instance states
(parallel to `InstancedMesh.instances`), epoch, last event times, load state (PoseAnimator retry pattern).

When `animation`'s mesh doesn't match the entity's mesh (vertex count + layout hash), the Inspector warns
`mesh_mismatch` with a one-click fix ("Use hero.vat.glb") and the mesh draws at rest.

### 3.4 Playback model (CPU writes only on change)

Per character two tracks, A (current) and B (fading in or blended), each `{clip, start, speed}`, plus `{fadeStart,
fadeEnd}` or a fixed blend weight with phase sync. The GPU computes everything from one global time, so a crowd of
looping characters costs **zero CPU work and zero uploads per frame**; a `play`/`crossFade` writes 32 bytes.

```
tau  = speed == 0 ? start : (now - start) * speed             // vatLocalTime: speed 0 holds at tau = start
f    = tau * clip.fps                                         // highp float
loop:      f = mod(f, frames)            f1 = mod(f0 + 1, frames)
once/clamp:f = clamp(f, 0, frames - 1)   f1 = min(f0 + 1, frames - 1)
ping_pong: f = triangle(f, frames - 1)   f1 = next on the same leg
f0 = floor(f); a = interpolate ? f - f0 : 0
value(A) = mix(sample(clip.start + f0), sample(clip.start + f1), a)
result   = mix(value(A), value(B), weightB(now))               // weightB from the fade or the fixed blend
```

- **Time**: `now` is the scene's play time minus an epoch the CPU rebases every 2048 s (then rewrites every start
  once), keeping float precision at milliseconds forever (flipbook's `startTime` precedent). In edit mode time stands
  still unless the Inspector's **Preview** is on (3.6).
- **Crossfade**: `crossFade(name, fade)` moves A→B with a linear weight over `fade`; at the end B becomes A.
  **Blend** `blend(a, b, w)` holds both with a fixed weight and phase sync (B's normalized time follows A's), for
  walk/run by speed.
- `once` clips finish at their last frame; `onFinish` fires from the CPU mirror; `queue(name)` plays next.
- **Held** tracks (`stop()`, speed 0) are written once as speed 0 with `start` = the held local time, so a paused
  crowd costs nothing per frame either. Events cross `(previous now, now]` on a double clock: once per crossing.
- Requests made inside `onEvent`/`onFinish` apply at once (at that update's time) and draw that frame. A disabled
  animator writes the rest state once (entity and instances); `Mesh:updateInstance` keeps an instance's
  `vat0/vat1`.
- Single (non-instanced) entities carry the same state in the uniform block instead of instance attributes.

### 3.5 Shader integration (mirrors the Lattice deformer end to end)

- Variant bits: MESH 33 `Vat` → `HAS_VAT`, 34 `Vab` → `HAS_VAT_BONES`; DEPTH 16/17; GBUFFER 14/15
  (`ShaderPool.cpp:1436-1530` builders, `:588-630` names, `ShaderVariantCompiler.cpp:362-433` defines). Either bit
  drops `HAS_SKINNING`/`HAS_MORPHTARGET` (the terrain/lattice exclusion pattern). Not in `getFallbackProperties`.
- `engine/shaderlib/includes/vat.glsl` (modelled on `lattice.glsl:68-87`): `u_vatTexture` + `u_vat_smp` with
  `@image_sample_type unfilterable_float` / `@sampler_type nonfiltering`; floats-only block `u_vs_vat` (time, W, V or
  B, S, entity state, clip table of up to 64 `vec4(start, frames, fps, flags)`); `vatDeform(inout pos, inout normal,
  inout tangent)` for Vertices and `vatBoneMatrix(...)` feeding the existing `getSkinPosition/Normal` math for Bones.
  All `highp`.
- Called first in every vertex entry point, where morph/skin run today: `mesh.vert`, `depth.vert`, `gbuffer.vert`,
  `includes/surface_vs.glsl`, `includes/surface_depth_vs.glsl`, `library/outline_vs.glsl`. Stage-1 lattices and the
  `SURFACE_DISPLACE` hooks still apply afterwards. Identical expressions in every pass (`invariant gl_Position`) so
  shadows and the depth pre-pass match; a mutation test removes the call from `depth.vert` and must fail.
- C++: `TextureShaderType::VAT`, `UniformBlockType::{VS,DEPTH_VS,GBUFFER_VS}_VAT` (`Render.h:224-306`), names
  (`ShaderData.cpp:201, 328`), slots cached per submesh (`RenderSystem.cpp:5806/6007/6161/7075`, reset `:9712`),
  `applyVat()` next to `applyLattice()` (`:88`) called at `:7802/8099/8851/7188`, `inputs.vat` in
  `getSubmeshShaderInputs` (`:3447`), variant collection (`ShaderVariantCollector.cpp:462-512`) and the library
  matrix test (`ShaderLibraryMatrix.cpp:94-103`) so exported builds ship the variants.
- Budget: one texture + one sampler, inside the 16-sampler limit (Bones mode replaces the bone texture: net zero).
- Cost per vertex (texel fetches): Vertices 2 (positions) / 4 (with tangent frame), ×2 during a crossfade; Bones 16
  (4 influences × 2 texels × 2 frames; 4 for weight-1 rigid pieces), ×2 during a crossfade. The guide's table tells which to pick when.

### 3.6 Culling, LOD, preview

- `animatedBounds` (3.2.5) from the clips in use; instance spheres from the same.
- LOD: the existing LOD pipeline works unchanged (shared vertices). Phase 3 adds a per-LOD **update interval**
  (distant characters advance their frame every N frames).
- Edit mode: the Inspector **Preview** toggle animates the GameObject (and its instances) in the Scene view through
  `PreviewOwner`; `App::stopTransientPreviews` stops it on play/save/switch. Off by default so scene screenshots are
  deterministic.

### 3.7 Events, sockets, attachments

- Events: per clip, copied from the source `.anim` events at bake (Rebake keeps hand edits by clip name), fired from
  the CPU mirror for single entities exactly like Animation Player events (`onEvent` subscribers, then the Lua method
  on the entity's scripts; the weight ≥ 0.5 dominant-state rule). For instanced crowds only when `instanceEvents` is
  on: `onInstanceEvent(index, function, args)`, cost O(instances) per frame (documented).
- Sockets: chosen nodes (any bone in Bones mode; any node of the source hierarchy in Vertices mode) baked as transform
  tracks in the payload's tail rows (CPU-readable copy kept in the pool). `getSocketPose(name[, instance])` returns
  position/rotation in the GameObject's space at the current time, blended through crossfades.
- `attachments: [{socket, entity}]`: the listed child entities follow their socket every frame (single entities).
  Instanced attachments (a weapon per instance as a second instanced mesh) are phase 3.

### 3.8 Lua API

`VertexAnimator`: `play(name)`, `play(name, fade)`, `crossFade(name, fade)`, `blend(a, b, weight)`, `queue(name)`,
`stop()`, `isPlaying([name])`, `time`, `normalizedTime`, `speed`, `currentClip`, `getClipNames()`,
`getClipLength(name)`, `getSocketPose(name)`, `onEvent(fn(name, args))`, `onFinish(fn(clip))`, `preview` (editor).
Instanced: `playInstance(i, name[, fade])`, `crossFadeInstance(i, name, fade)`, `setInstanceSpeed(i, s)`,
`setInstanceTime(i, t)`, `getInstanceClip(i)`, `getInstanceNormalizedTime(i)`, `getSocketPose(name, i)`,
`playAll(name[, fade])`, `randomizeTimes(seed)`, `onInstanceEvent(fn(i, name, args))`, `onInstanceFinish(fn(i,
clip))`. Bindings in `engine/core/script/binding/`, stubs via `generate_lua_stubs.py` (no unresolved lines).

### 3.9 Cloning, prefabs, export

EntityCloner copies authored fields and resets runtime; prefab overrides on scalar fields and `attachments`; Factory
emits authored fields; `Project.cpp:672,837` and `ProjectPrefabPaths.cpp:413` list `.vat` + `.vat.glb` as shipped
assets; the exporter already copies every file; the shader variant collector adds VAT variants for every mesh with a
Vertex Animator.

## 4. Baking and importing (editor, `editor/vat/`)

### 4.1 Sources (all normalised to clips on a hierarchy, no files written)

| Source | How |
|---|---|
| `.anim` clip(s) + a model or prefab to play them on | `ActionSystem::previewPlayer` |
| Model clips `hero.gltf#Walk` | `extractModelSubClip` (in memory) |
| Animation Player on a GameObject (its clip list) | its states |
| Entity Animation | `convertEntityAnimation` |
| Pose animation (+ the Pose Animator's model) | `pose::sample` frames → `bakeChannels` |
| Lattice tracks | clip `lattice` channel (deformation baked through `CpuSkin` lattice step) |
| Houdini Labs VAT 3, OpenVAT | importers (4.6), re-packed |

### 4.2 Bake pipeline

1. Suspend previews on the target (`PoseEditSession::suspendActiveFor`, `PreviewOwner::acquire`), as
   `LatticeBake.cpp:1019-1039`.
2. Per clip, per sample time: `ActionSystem::previewPlayer` poses the hierarchy (main thread, time-sliced with a
   per-frame budget like `PlacementSession::advance`), compute bone matrices with the render formula, then:
   Vertices → `CpuSkin` (skin + morph + lattice) → offsets + tangent frames; Bones → matrices; sockets → transforms.
3. `restorePlayerPreview`. Encode (half/float, hemisphere continuity, bounds per clip and total) on a worker thread.
4. Measure: quantization error (Vertices), interpolation error at mid-frames against an exact mid-frame sample
   (both modes; suggests a higher fps when large), texture size, memory, per-clip memory.
5. Auto mode: Bones when every vertex is explained by skin + rigid parts (no morph/lattice motion above tolerance),
   else Vertices; the report says why.

### 4.3 Session and job

`VatBakeSession` (pattern: `texsynth/SynthesisSession`): spec → `check(spec)` (validation, 4.7) → `estimate(spec)`
(size/memory before any work) → run (progress stages "Posing clip 2 of 5", Cancel) → preview result (in memory) →
`write`. The same session serves the window, Inspector actions, CLI and MCP.

### 4.4 Outputs, existing files, undo

- Default output `<source dir>/<source stem>.vat` (+ `.vat.glb`), a source dir outside the assets folder mapped to
  the same folder under it; the PoseBaker existing-file rule: a file baked from the same recipe source (kind, path,
  scene and the same clips) is rewritten and keeps its GUID (Rebake); any other existing file of that name makes
  the bake take a free name ("hero 2.vat"); an explicit path that exists is refused unless `overwrite`. Images the
  source holds inside are stored inside the `.vat.glb`. Placing needs a 3D scene; a crowd's GameObject is unrotated
  and unscaled at the source's position, its instances carrying the source's rotation and scale.
- `place`: `none` (files only; default from the window and Project), `beside` (default from Inspector actions: a new
  GameObject "<name> (VAT)" with the source's transform, selected), `replace` (the source GameObject is disabled and
  the VAT one takes its place), `crowd` (`{count, area, seed}`, 5.4).
- One undo step: `CreateVatCmd` (the `CreateClipCmd` pattern: files written in `execute`, undo deletes them only if
  their bytes are unchanged, nested scene commands for `place`). Files-only uses `WriteFilesCmd` on the project
  history.

### 4.5 Staleness and Rebake

The recipe stores each source's content hash (and for model clips the model file hash). The `.vat` Inspector shows
"Source changed since baking: Rebake" with the changed source named; Rebake re-runs the recipe and keeps GUID, clip
names, events, speeds and wraps (by clip name). `check --vat` reports `vat-source-changed`, `vat-source-missing`,
`vat-mesh-mismatch`, `vat-layout-mismatch`.

### 4.6 Importers: Houdini Labs VAT 3 and OpenVAT

Both decode on the CPU at import and re-pack into the 2.2 layout, writing `.vat` + `.vat.glb` with
`recipe.sources[0].kind = "houdini_vat3" | "openvat"` (Rebake re-imports when the files change).

- **Houdini Labs VAT 3** (soft, rigid, dynamic remeshing; particle sprites reserved): soft → Vertices; rigid → Bones
  (one bone per piece); dynamic remeshing → Vertices with no rest and no interpolation. Details in 4.6.1.
- **OpenVAT**: `<name>_vat.(exr|png)`, optional `<name>_vnrm.*`, `<name>-remap_info.json`, and the mesh with the
  `VAT_UV` layer (the last TEXCOORD). Block layout, per-channel min/max remap from `os-remap`, Blender Z-up → Y-up
  `(x, z, −y)`, packed normals in the lower half or the separate file, clips from `animations` (global frames made
  relative). Its Blender add-on is GPL: we read its files, we copy none of its code.
- Missing pieces are errors with fixes ("rotation texture not found next to the mesh: export with Normals enabled, or
  pass rotation"). Float images need a reader: add tinyexr (BSD-3) for EXR; PNG16 multi-channel via a 16-bit stb
  path (today 16-bit RGB(A) drops to 8 bits, `TextureData.cpp:197-206`).

#### 4.6.1 Houdini VAT 3 decoding details

Source of truth: the SideFX tutorial project (`unity_projectfiles.zip`: Unity package with the four VAT 3.0 Shader
Graphs, example exports of each mode, and `VAT3_Tutorial_Simulations.hip`), decoded from the graphs and checked
against the EXR/PNG/FBX data. Decoding happens on the CPU at import; the runtime never sees Houdini's encoding.

**Files** (ROP export folder): `geo/<asset>_mesh.fbx`, `tex/<asset>_{pos,pos2,rot,col,col2,lookup}.<exr|png|tga>`,
and metadata from the first available of: `unity/<asset>_mat.mat` (Unity YAML: `m_Floats`, keywords, and the shader
GUID naming the mode), the legacy `<asset>_data.json` (`Bound Min/Max X/Y/Z`, `Houdini FPS`, `Two Position
Textures`, `Axis System`), else the mesh's raw bounding box. Fallbacks: mode from the textures and UV sets present;
frame count = active height / rows per frame (distinct UV1 V values other than the guard value); FPS asked when
missing (the importer UI and the MCP `fps` argument).

**Metadata in the bounds.** The exporter rounds bounds to 0.1 and stores flags in the next digit. Decode in double
precision with snapping (float32 gives `-81.0000038`, whose fractional part reads as 1, not 0):
- `HDR = fract(bMax.z*10) >= 0.5` (else LDR: values remapped by the bounds),
- active ratios `Ur = 1 - (ceil(bMin.z*10) - bMin.z*10)`, `Vr = 1 - fract(-bMax.x*10)`,
- position-alpha scale `1 / (1 - fract(bMax.y*10))` (piece/particle scale),
- `fract(-bMin.x*10)`: rigid LDR rotation-alpha range, or for fluid `>= 0.5` = 16-bit lookup (÷2048) else 8-bit
  (÷255).
Cross-check the ratios against the texture size and the UV row count; disagreement is an error with the numbers.

**Addressing.** Per vertex the VAT texel comes from UV1 (UV0 for fluid): `u = uv.x*Ur`, row from the top =
`((1 - uv.y) + f/N) * Vr * H` for frame `f` (0-based). Rows are read in file order (EXR scanline 0 = frame 1). The
six guard vertices (UV V ≤ 0.1, at the bound corners, there only to force the bounding box) and their triangles are
dropped from our mesh; our bounds come from the decoded data.

**Positions.** `p = pos.rgb (+ pos2.rgb * 0.01 when Two Position Textures)`; LDR: `p * (bMax - bMin) + bMin`.

**Per mode → our layout:**
- **Soft body** → Vertices. Values are offsets from the FBX rest position (frame 1 is zero). Normal and tangent from
  the rotation quaternion `q` (HDR raw, LDR `(t - 0.5) * 2`): `N = rotate(q, (0,1,0))`, `T = rotate(q, (-1,0,0))`
  with `rotate(q,v) = v + 2·cross(q.xyz, cross(q.xyz, v) + q.w·v)`; re-encoded as our tangent-frame quaternion.
  "Compressed normals" (no rotation texture): the 10-bit spheremap in position alpha (`x = a*1024; hi = floor(x/32);
  e = vec2(hi, x - hi*32)/31.5*4 - 2; N = (e.x*g, 1 - |e|²/2, e.y*g)` with `g = sqrt(1 - |e|²/4)`), tangent from
  the mesh. Two-sided materials (the cloth example) keep their double-sided render state.
- **Rigid body** → Bones, one bone per piece. Rest pivot from the extra UV sets: `(-uv2.x, uv3.x, 1 - uv3.y)` in
  Unity terms (Houdini `uv3`/`uv4`); piece position from the position texture (absolute); orientation is a
  smallest-three quaternion: rotation RGB = three components, the dropped component's index = `floor(pos.a * 4)`,
  piece scale = `(1 - fract(pos.a * 4)) * alphaScale` (`Decode_Quaternion`: index 1 → `(w, y, z, x)`, 2 → `(x, -w, z,
  -y)`, 3 → `(x, y, -w, -z)`, else `(x, y, z, w)`, `w = sqrt(1 - |xyz|²)`). Bone `b` at frame `f`: rotation `q`,
  translation `piecePos - rotate(q, pivot) * scale`, uniform scale. Rotation alpha holds the angle to the next frame
  (in turns, sign = flip q1); when it exceeds 60° the importer **upsamples** (inserts frames with Houdini's slerp,
  `q = (q0·sin(θ - θa) + sign·q1·sin(θa)) / sin θ`) so our nlerp stays faithful, and reports the new frame count.
  "Smooth trajectories" (`P + V t + ½ A t²`, acceleration in the colour texture) are honoured by the same upsampling.
  "Stretch by velocity" needs non-uniform scale: dropped with `stretch_by_velocity_ignored`.
- **Dynamic remeshing (fluid)** → Vertices with `rest: none`, `interpolate: false`. Every mesh vertex is a triangle
  corner whose per-frame texel comes from the lookup table: `L = lookup(uv0, f)`, texel = `(L.x + L.y/D, 1 - (L.z +
  L.w/D))`, absolute positions, normals from the rotation texture as soft body. The re-pack stores one position per
  corner per frame, which is larger than Houdini's packed form (57,216 corners × 100 frames × 2 texels = 11.4 M texels
  for the tutorial fluid: desktop preset only; `too_large` names it). A lookup-table layout that keeps Houdini's
  packing is reserved for a later version if fluids on mobile matter.
- **Particle sprites**: reserved (needs camera-facing cards; the engine's instanced billboards may serve).

**Colour texture** (`col`): optional third texel per sample in Vertices mode (`layout.colors`, RGBA16F) feeding the
vertex-colour input, imported when present (rigid pieces' colour, fluid surface UVs from colour RG are not
supported: `fluid_uv_from_color_ignored`). Baking from the engine never writes it.

**Axes and units.** Unity-target exports (`coordsys 2`, `scale 1`) are mirrored in X relative to the engine's
right-handed Y-up: apply `S = diag(-1, 1, 1)`: positions and remapped values `x = -x`; quaternions `(x, -y, -z, w)`;
the tangent reference `(-1,0,0)` becomes `(1,0,0)`; pivots use `(uv2.x, uv3.x, 1 - uv3.y)` without the negation.
Unreal-target exports (`coordsys 4`, `scale 100`, cm) are converted from the JSON `Axis System` and `÷100`; when the
target can't be determined the import stops with "re-export with Target Engine = Unity, or pass axis". The tutorial's
cloth ROP is still set to Unreal (its sticky note: "Set Target Engine before rendering!"), so both must work.

**FBX reading** must bypass the normal model import (`editor/util/FbxImporter.cpp` + `GlbBuilder.cpp`), which would
break VAT meshes four ways: it applies the FBX unit scale (`UnitScaleFactor 1` = cm → ×0.01), flips V (`1 - v`),
keeps only TEXCOORD_0/1 (rigid needs four UV sets), and drops tangents. The importer reads the FBX with ufbx
directly: no unit conversion, raw V, all UV sets, tangents, float32 UVs, no welding/reordering/recentring, then writes
our `.vat.glb` with `_VAT_INDEX` (where the VAT UVs are no longer needed).

**Textures**: EXR (float32/half, ZIP/PIZ, RGB treated as alpha 1) through tinyexr, PNG/TGA 8-bit (LDR, hi/lo pairs,
the lookup table; PNG sRGB chunks ignored: data is linear). Fixtures for tests: the four tutorial exports (license:
SideFX tutorial assets; kept out of the repo unless SideFX's terms allow, tests skip with a message when absent) plus
small synthetic exports generated by a test script that writes each mode's encoding.

### 4.7 Validation (shared by window, Inspector, MCP, CLI)

Errors (bake refused) and warnings, each with a `kind`, a sentence and a fix:
`too_large` (texels over the preset: fixes ordered by effect: lower fps, drop normals, Bones mode, fewer clips,
desktop preset), `no_animation`, `unbound_tracks` (from the clip binding report), `vertex_limit` (> 2^24),
`bone_limit` (> 65,535 joints, Bones mode: use Vertices; VAT bones live in the payload, not the 128-matrix skinning array), `topology_changes` (only imports), `large_interpolation_error`
(raise fps or turn interpolation off for that clip), `precision_switched` (half → float), `memory_high`, `mixed_rest`
(source has no bind pose: first frame used), `materials_not_vat_ready` (a project shader fork older than VAT draws
at rest: re-fork or switch to the library shader).

## 5. Editor UI

### 5.1 Vertex Animation Baker window

Window > Animation > **Vertex Animation Baker** (App wiring as Texture Synthesis: `App.cpp:242/274/613-619/788`,
docking, `PanelVisibilitySettings`). One column, top to bottom:

1. **Source**: drop zone/field (model, prefab, GameObject, `.anim`, Houdini/OpenVAT export). Shows what it found
   ("hero.gltf: 1 skinned mesh, 41 bones, 5,120 vertices, 6 clips").
2. **Clips**: checklist with per-row frames, length, wrap, per-clip fps override; "All / None"; for `.anim` sources a
   "Play on" model field.
3. **Settings**: Mode (Auto/Vertices/Bones, with the Auto decision shown), Sample rate (default 30), Normals,
   Precision (Auto/Half/Float), Preset (Mobile & Web / Desktop), Sockets (pick bones/nodes; "Add Selected").
4. **Estimate** (live, before baking): texture size, memory total and per clip, fetches per vertex, a bar against the
   preset limit, validation rows with their fixes as buttons ("Lower to 20 fps").
5. **Preview**: a dual viewport (source vs VAT) with play/scrub/clip dropdown and an error heatmap toggle, using the
   in-memory result after a bake run.
6. **Output**: path, Place (Files only / Beside / Replace / Crowd…), **Bake** (progress + Cancel replace it while
   running).

Double-clicking a `.vat` opens the window with its recipe loaded ("Rebake" instead of "Bake").

### 5.2 Inspector

- **Bake ▾ › Vertex Animation…** on Animation Player, Pose Animator and Lattice Modifier sections and on clip rows of
  the model asset view (`drawModelClipsAsset`, `InspectorWindow.cpp:1422`), through a hooks struct like
  `PoseInspectorHooks` (the Inspector doesn't link the baker). Opens the window prefilled; Shift-click bakes with the
  last settings.
- **Vertex Animator** section: Animation field, Clip dropdown with ▶ Preview, Speed, Random Start, Play
  Automatically, Default Fade, Instance Events, Attachments; while previewing a time scrubber; warnings (`mesh_mismatch`
  with its fix, device limit).
- **`.vat` asset view**: header (mode, vertices/bones, texture size, memory, error), clips table (name, frames, fps,
  length, wrap, speed, events count, bytes; name/wrap/speed/interpolate editable, events open the clip's event list),
  sockets, staleness banner + **Rebake**, **Open in Baker**, **Create Crowd…**.

### 5.3 Project window

`.vat` file type with icon; `.vat.glb` shown with a "generated" badge; context menu on models (`.gltf/.glb/.fbx`) and
`.anim`: **Bake Vertex Animation…**; on Houdini/OpenVAT exports (`*-remap_info.json`, a Houdini JSON, or a mesh with
sibling VAT textures): **Import as Vertex Animation…**; drag a `.vat` into Scene/Hierarchy creates the GameObject
(3.3); rename/move/delete keep the pair.

### 5.4 Create Crowd

Dialog from the `.vat` view or the baker's Place: count, area (box/disc, size, on the selected surface or the ground
plane), minimum spacing, random yaw, random scale range, clip weights (e.g. 70% Walk, 30% Idle), random speed range,
random start (on), seed, Instance Events. Creates one GameObject with Model + Instanced Mesh + Vertex Animator; one
undo step. Phase 3 adds a crowd brush (paint instances in the Scene view, reusing the Physics Placement surface
snapping).

## 6. MCP (agents)

Budget: tool definitions are 261 characters under `TOTAL_LIMIT` (107,841 of 108,102); instructions are full
(16,384 bytes). One new tool, paid by trims measured with `tools/mcp/tool_budget.py` (candidates: move field lists of
`synthesize_texture`, `edit_lattice`, `edit_colliders`, pose tools to their guides); a cap for the new tool in
`test_html_ui_tools.py` `LIMITS`; `TOTAL_LIMIT` only lowered. Instructions: one short line in "2D and 3D" paid by a
trim.

- **`bake_vertex_animation`**: `source` (GameObject id/path, model, `model#Clip`, `.anim` list with `on`, Houdini or
  OpenVAT export path), `clips`, `mode` (auto|vertices|bones), `fps`, `normals`, `precision`, `preset`, `sockets`,
  `output`, `overwrite`, `rebake` (a `.vat` path: re-run its recipe), `place` (none|beside|replace|`{crowd: {count,
  area, seed}}`), `wait`, `timeout_seconds`.
  - Without `output` (and without `rebake`): **preview only**, nothing written: the report `{mode, mode_reason,
    vertices, bones, clips[{name, frames, fps, bytes}], texture{width, height, format}, bytes, max_error_mm,
    mean_error_mm, fetches_per_vertex, warnings[{kind, message, fix}]}` and a contact-sheet image (source vs VAT at 4
    times per clip, like `render_pose_animation`). Calling again with the same arguments plus `output` writes exactly
    that bake.
  - With `output`: writes, one undo step named in the result text, returns the report plus `{vat, mesh, entity?}`.
  - Long bakes become jobs (`get_job`/`cancel_job`), the `synthesize_texture` pattern.
- Existing tools: `add_component`/`set_properties` `vertexanimator` (animation path validated: ".vat file, e.g.
  assets/vat/hero.vat"); `get_asset_info` on `.vat` (clips, layout, stats, staleness, users); `list_assets` type
  `vertex_animation`; `write_file` validates `.vat` (refuses generated-field edits with the rebake message);
  `move_asset`/`delete_asset` keep the pair; `focus_window` `vertex_animation_baker`; `eval_lua` for runtime control.
- Guide `docs/mcp/guide/vertex-animation.md` → `jasper://docs/vertex-animation`: when to use which mode (fetch-cost
  table), the preview → write loop, recipes ("bake hero's Walk/Run/Idle for web and spawn 500 out of sync",
  "convert a crowd of Animation Players", "import a Houdini destruction", "attach a sword to RightHand"), warnings
  and fixes, Lua.

## 7. CLI and checks

- `jasperengine-editor bake-vat --project . --source <path> [--clips Walk,Run] [--mode auto|vertices|bones]
  [--fps 30] [--normals on|off] [--preset mobile_web|desktop] [--output assets/vat/hero.vat] [--overwrite]
  [--rebake assets/vat/hero.vat] [--dry-run] [--format text|json]`; headless with the `PlacementCli` pattern
  (`PlacementCli.cpp:506-539`), exit codes 0/1/2; `--dry-run` = the MCP preview report without the image.
- `check --vat` (+ default set, `--files`, hook mode): staleness, mesh/layout mismatch, missing sources, device
  limits per preset; `docs/mcp/checks.md`.

## 8. Tests

- Engine gtests (`engine/tests/vat`, option `JASPERENGINE_VAT_TESTS`): IO round trip byte-identical, leniency
  warnings, generated-field refusal, layout addressing at row wraps, frame math for every wrap (C++ mirror) incl.
  loop seam and epoch rebasing, crossfade/blend weights, event crossing, socket sampling, `CpuSkin` vs hand-computed
  skin/morph/lattice, half/float encode error bounds, quaternion hemisphere continuity.
- GPU readback (`engine/tests/htmlui`, `TestVatRender.cpp` like `TestLatticeRender.cpp`): skinned render vs VAT
  render of the hero fixture at the same times, Vertices and Bones, single and instanced with different clips per
  instance, shadow pass matches (mutation test), crossfade midpoint, GLES path.
- Editor gtests: `VatBakerTest` (hero Walk/Idle; morph fixture; lattice fixture; rigid hierarchy → Bones; Auto
  decisions; existing-file rule; recipe hashes; Rebake keeps edits), importers (Houdini soft/rigid/fluid fixtures
  from the tutorial project, OpenVAT fixture) decode vs reference positions, Stream/Factory/Cloner round trips.
- MCP `test_bake_vertex_animation.py`: preview report + image, write, overwrite/free name, job + cancel, undo scopes,
  rebake, place modes incl. crowd, `get_asset_info`, `write_file` validation, play → per-instance motion differs,
  events into Lua; `test_bake_vat_cli.py`; `test_checks.py` `--vat`; budget caps, `test_docs`, `test_stubs`.
- Performance (`tools/performance`): 2,000 hero instances, 3 clips, desktop and the web build; recorded, not gated.

## 9. Documentation

`docs/vertex-animation.md` (user guide: bake, modes, presets, crowds, Lua, sockets, imports, troubleshooting, "Not in
this version"), README bullet (feature + "(Window > Animation > Vertex Animation Baker, the `bake-vat` CLI, or the
`bake_vertex_animation` MCP tool)"), glossary row ("none (new): Vertex Animation `.vat`, Vertex Animator"),
`lua-api.md`, `docs/mcp/checks.md`, the MCP guide and index, NOTICE.md entries for any third-party code or math
taken (tinyexr BSD-3; formulas credited to SideFX Labs BSD and the MIT projects in section 11).

## 10. Phases and work breakdown

Every task gets a brief with an explicit file list; shared wiring files (`editor/App.cpp`, root and engine
`CMakeLists.txt`, Xcode project) are edited only by T0 and the task named for a hook. A companion
`2026-10-27-vertex-animation-contract.md` (written by T0) lists every public header, hook and owner.

**Phase 0: foundations** (each independently useful, merged first)
1. T0 Contract: public headers with stubs (`VertexAnimation`, `VertexAnimationIO`, `VertexAnimationPool`,
   `VertexAnimationRuntime`, `CpuSkin`, `VertexAnimatorComponent` + registration, editor `vat/` headers), CMake/Xcode
   entries, gtest skeletons.
2. T1 Float textures (3.2.1) + EXR/16-bit readers (4.6).
3. T2 `CpuSkin` (3.2.2) with GPU-parity tests.
4. T3 Mesh attributes + bounds override + per-instance vat attributes (3.2.3-5).

**Phase 1: Vertices mode end to end**
5. T4 Asset IO, pool, runtime mirror (2, 3.1, 3.4).
6. T5 Shader variants, `vat.glsl`, every pass, collector (3.5) + GPU readback tests.
7. T6 Component, Lua, cloning/export (3.3, 3.8, 3.9).
8. T7 Baker core + session + command (4.1-4.5, 4.7) for Vertices.
9. T8 Window, Inspector, Project, Create Crowd (5).
10. T9 MCP tool, CLI, `check --vat`, guide (6, 7).
11. T10 Docs (9).

**Phase 2: Bones mode, sockets, events**
12. T11 Bones payload + shader path + Auto mode + rigid hierarchies.
13. T12 Sockets, attachments, events (single and instanced).

**Phase 3: imports and extras**
14. T13 Houdini VAT 3 importer (soft, rigid, fluid) with fixtures from the tutorial project.
15. T14 OpenVAT importer.
16. T15 LOD update interval, instanced attachments, crowd brush; reserved: Houdini sprites (instanced billboards
    exist: `Mesh.h:129-134`), motion vectors when the renderer gets them, play-mode recording of physics to Bones
    (destruction).

After each task: review (spec compliance, code quality, adversarial correctness) and fix rounds until clean, then
merge into `vertex-animation`; at the end a whole-branch review and the full MCP suite against the baseline.

## 11. Research credits (what came from where)

- One texture for all clips + clip table: Not-Lonely Vertex Animator, Khipu VAT Baker (ideas only, Asset Store EULA).
- Mesh-attribute lookup surviving reorder and shared by LODs; bounds over all frames: maxartz15/VertexAnimation (MIT).
- Frame timing (texel centres, loop closure): fuqunaga/VatBaker (MIT). Offsets from rest: sandwichpuissant/Unity-VAT
  (Unlicense). Random start per instance: sugi-cho/Animation-Texture-Baker (MIT).
- Bone texture mode, stale detection + Rebake from recorded settings, cost estimate before baking, validator with
  fixes: EQ VAT Lab (ideas). Packed quaternion tangent frame, sockets blending through crossfades, hardware-limit
  pre-check: VATMachine (ideas); quaternion frame also SideFX Labs (BSD).
- One asset with sources, preview, memory per clip and out-of-date flag; phase-synced blend: Simple VAT (ideas).
- GPU-timed A/B crossfade with no per-frame CPU writes: OpenVAT's Unity runtime (MIT decoders folder).
- Houdini VAT 3 format: SideFX Labs (BSD), G4ND44/Godot_VAT3 (MIT) as a decoding reference.
- Integer-addressed `Load`/`texelFetch` decoding, rigid-lerp shrink artifact to avoid, Houdini ROP settings in .hip
  text: keijiro/HdrpVatExample (Unlicense; Houdini VAT 2.0 only).
- VAT 3 decode reference (bound-encoded flags, smallest-three quaternions, precomputed-angle slerp, trajectories,
  lookup tables, sprite origin hiding): Bonjour-Interactive-Lab/Unity3D-VATUtils, read only: its repo LICENSE is
  GPL-3.0 while package.json says MIT, so none of its code is copied; the math is SideFX Labs' (BSD).
- Not used: isrooky (no license; merges meshes instead of instancing), OpenVAT Blender add-on code (GPL),
  manthrax/three-vat (no license).
