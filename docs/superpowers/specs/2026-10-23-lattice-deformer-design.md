# Lattice deformer (free-form deformation)

Status: approved scope (owner, 2026-10-23: "implement everything"). The declarations in §2.0
are committed in the contract commit on branch `lattice`; every work package builds against
them and must not rename them. Implementation notes added by work packages go in §12 and
override the sections they name.

Prior art studied (ideas only, clean-room; no code from these may be copied, none of them
is licensed for it): Harry Heath's "Lattice Modifier for Unity" (Asset Store 293850, docs at
harryheath.com/lattice), ValDesu/Knead (GitHub, unlicensed), Savasstion/LatticeModifier_Unity
(GitHub, unlicensed). The math is textbook (Sederberg & Parry 1986 FFD; uniform cubic
B-splines).

## 1. Goal

A lattice is a box-shaped cage of control points. Moving the points bends, squashes,
twists or wobbles every mesh bound to it, smoothly, without editing the mesh. JasperEngine
gets the best lattice tool available in any engine, for two kinds of users at once:

- **People in the editor**: create a lattice around the selection in one click, grab points
  in the Scene view with the normal W/E/R tools, box-select, proportional falloff, mirror,
  presets, poses, keyframes, and the cage tells them where the mesh is squashed or stretched.
- **AI agents over MCP and Lua**: one well-described tool (`edit_lattice`) with ordered
  operations, readable selectors (`"+y"` face, index ranges, world boxes), presets with
  physical meaning (twist 45 degrees around y), numeric read-back (what moved, by how much,
  is anything folded inside out, where does this world point end up) and screenshot
  verification, all undoable and identical to what a person would do by hand.

It must run everywhere the engine runs (desktop GL/Vulkan/Metal/D3D11, mobile, **WebGL2**),
on static meshes, skinned and morphed models, instanced meshes, sprites, polygons and
tilemaps, in every render pass (color, depth prepass, shadows, G-buffer, reflection probes)
and in the shader library, at no cost to objects that do not use it.

### 1.1 Non-goals

- Physics colliders do not follow the deformation (mesh/convex colliders keep the rest
  shape, as they already do for skinning and morph targets). Documented.
- Triangle-accurate picking of deformed meshes (picking stays AABB based; the AABB is
  grown to contain the deformation).
- Texture masks, UV-channel masks, static lightmap baking (the engine has no lightmapper),
  compute shaders.
- Deforming terrain, UI documents (HTML UI), points, lines, particles, text.

### 1.2 Anti-goals (a failure even if it "works")

- Any cost (variant, upload, CPU loop) for meshes without a lattice.
- A deformed mesh that is saved back deformed, so reloading deforms it twice.
- Lattice edits that bypass the command system (no undo, no prefab override, no live
  Hierarchy/Inspector refresh), or an MCP path that differs from the human path.
- Hierarchy clutter (one entity per control point).
- Normals recomputed from faces (destroys authored normals) or tangents left stale.
- Different results on CPU (followers, sampling, bake, bounds) and GPU.

## 2. Requirements

### 2.0 Committed contract (do not rename)

New engine files (all under `engine/core/`, listed in `engine/CMakeLists.txt`):

| File | Owner (§11) | Content |
| --- | --- | --- |
| `component/LatticeComponent.h` | contract | `LatticeComponent`, `LatticeInterpolation`, `LatticeOutside`, `LatticeJiggleAnchor`, `LatticePose` |
| `component/LatticeModifierComponent.h` | contract | `LatticeModifierComponent`, `LatticeBinding`, `LatticeStage`, `LatticeMaskChannel` |
| `component/LatticeFollowerComponent.h` | contract | `LatticeFollowerComponent` |
| `component/LatticeTracksComponent.h` | contract | `LatticeTracksComponent` |
| `lattice/LatticeTypes.h` | contract | constants, `LatticeMeshState` (render data on `MeshComponent`) |
| `lattice/LatticeSolver.h/.cpp` | WP-SOLVER | the math, §3 |
| `lattice/LatticePresets.h/.cpp` | WP-SOLVER | presets, resample, mirror, selection helpers, §4 |
| `lattice/LatticeRender.h/.cpp` | WP-RENDER | per-frame GPU packing, §5 |
| `subsystem/LatticeSystem.h/.cpp` | WP-ECS | poses/tweens, jiggle, followers, sprite tessellation flag, §6 |
| `object/Lattice.h/.cpp` | WP-ECS | `Lattice` GameObject wrapper, §8 |
| `action/keyframe/LatticeTracks.h/.cpp` | WP-ANIM | `LatticeTracks` action, §7 |
| `editor/lattice/LatticeEditorOps.h/.cpp` (editor) | WP-EDITOR-SCENE | the editor edit path, §9.3 |

`MeshComponent` gains `LatticeMeshState lattice;` (filled by `LatticeRender`, consumed by
`RenderSystem`). The four components are registered last in `EntityRegistry` (in the order
above), included from `JasperEngine.h`, and `LatticeSystem` is registered in `Scene::init`
between `MeshSystem` and `UISystem`.

The contract commit ships stub bodies for every declared function (solver, presets,
editor ops, `LatticeRender::prepare`) so every package links from the start; the owner
replaces them. A stub left in place at integration is a bug.

Field names, enum names and enum values in the contract headers are the serialized names
(YAML keys use the field names; enums serialize as the lowercase enumerator names, e.g.
`interpolation: cubic`, `stage: after_skinning`, `mask: red`).

### 2.1 Concepts and coordinate spaces

- A **lattice** is an entity with a `Transform` and a `LatticeComponent`. Its **local space**
  is the unit cube `[-0.5, 0.5]^3`; the cage's size, position and orientation in the world
  come from the Transform only (scale = size). There is no separate size field. Moving,
  rotating or scaling the lattice entity does not by itself deform anything that is fully
  inside it at rest (offsets are zero at rest).
- **Resolution** `(rx, ry, rz)`, each in `[2, 16]`, default `3,3,3`. Point `(i, j, k)` has
  flat index `i + rx * (j + ry * k)` (x fastest). Its **rest position** in local space is
  `(i/(rx-1) - 0.5, j/(ry-1) - 0.5, k/(rz-1) - 0.5)`.
- **Offsets** `offsets[index]`, local-space vectors, size `rx*ry*rz`, all zero at rest.
  A point's current local position is rest + offset. Offsets are what is stored, animated,
  overridden in prefabs, edited by tools and scripts.
- A **target** is any entity with a `MeshComponent` (meshes, models, sprites, polygons,
  tilemaps, instanced meshes). It opts in with a `LatticeModifierComponent` holding an
  ordered list of **bindings**; each names a lattice entity plus per-binding settings.
  `includeChildren` extends the bindings to every descendant entity with a `MeshComponent`
  (a whole car, a character with props).
- **Normalized coordinates** `u = local + 0.5` (the unit cube `[0,1]^3`). Offsets are in the
  same units in both.
- **Stacking**: a mesh is deformed by at most `LATTICE_MAX_PER_MESH = 4` bindings: its own
  bindings in list order, then inherited bindings from the nearest ancestor with
  `includeChildren` first (each ancestor's list in order). Extra bindings are ignored and
  reported (Console warning once per entity, inspector warning row, MCP warning).
- **Stage** (skinned meshes only): `before_skinning` bindings deform the bind pose in mesh
  space (a lattice around a T-posed character's head makes a big head in every animation);
  `after_skinning` bindings (default) deform the posed mesh (a lattice parented to a bone
  follows it; a lattice in the world squashes whatever walks through it). For non-skinned
  meshes the stage is ignored (all bindings apply after instancing).
- Vertex pipeline order (GPU and CPU): morph targets -> `before_skinning` lattices in order ->
  skinning -> instance matrix -> `after_skinning` lattices in order -> model matrix.

### 2.2 Engine runtime

- GPU deformation in the vertex shader (§5) for every pass that draws meshes. Variant bit:
  MESH property bit 5 (retired `Pcf`) becomes `LATTICE` ("Ltc"/"Lattice"); DEPTH and GBUFFER
  get their own free bit. Meshes without an active lattice keep their existing keys and
  pipelines unchanged.
- The deformation data is a per-mesh RGBA32F vertex texture plus a small uniform block, so
  WebGL2/GLES3 work (no storage buffers, no compute) and resolution up to 16^3 fits.
- Bounds: `MeshComponent::worldAABB` grows to contain the deformation (§5.5), so culling,
  light assignment and editor framing stay right.
- `LatticeSystem` (§6): pose tweens, jiggle springs, followers, the sprite tessellation
  request, nothing else. No CPU per-vertex deformation of meshes ever happens at runtime.
- Animation: `LatticeTracks` keyframe action (§7), crossfades like morph tracks.
- Lua and C++ scripting API (§8).

### 2.3 Editor

- Creation, inspectors, Scene-view editing, context menu, falloff, mirror, presets, poses,
  preferences, keying, bake (§9).

### 2.4 MCP

- Generic tools work on all four components through Catalog (§10.1).
- One dedicated tool `edit_lattice` (§10.2), a `lattice` channel in `edit_animation`, a
  `show_all_lattices` display flag, `create_entity` type `lattice`, a guide
  `jasper://docs/lattice`, tests (§10.4). The tool budget (`tools/mcp/tool_budget.py`) must
  pass after the additions: trim other descriptions to pay for them.

## 3. The math (`lattice/LatticeSolver.h`, WP-SOLVER)

The CPU solver and the GLSL in `includes/lattice.glsl` implement exactly this; a mismatch is
a bug. All formulas are per lattice in its normalized space `u`.

### 3.1 Padded grid

The padded grid `P` has `(rx+2)(ry+2)(rz+2)` entries; `P(a,b,c)` for `a in [-1, rx]` (same for
b, c), stored at `(a+1) + (rx+2)*((b+1) + (ry+2)*(c+1))`. Interior entries are the effective
offsets (authored `offsets` + runtime `dynamicOffsets` when present). Ghost entries are
linear extrapolations, applied axis by axis (x, then y on the x-padded result, then z):
`P(-1) = 2 P(0) - P(1)`, `P(r) = 2 P(r-1) - P(r-2)`. A linear offset field is therefore
reproduced exactly at the borders by every interpolation mode.

### 3.2 Offset field D(u) and its derivative

Per axis with resolution `r` and coordinate `x`:
`xc = clamp(x, 0, 1)`, `g = xc * (r-1)`, `c = min(floor(g), r-2)`, `t = g - c`.
Inside `[0,1]` on that axis the derivative factor is `(r-1)`; outside it is 0 (clamped).

- `sharp` (trilinear): nodes `c, c+1`; `w = (1-t, t)`, `dw = (-1, 1)`.
- `smooth`: `s = t*t*(3-2t)`; `w = (1-s, s)`, `dw = (-6t(1-t), 6t(1-t))`.
- `cubic` (uniform cubic B-spline, approximating: the mesh follows the cage smoothly but
  does not pass exactly through interior points; default): nodes `c-1 .. c+2`;
  `w0 = (1-t)^3/6`, `w1 = (3t^3 - 6t^2 + 4)/6`, `w2 = (-3t^3 + 3t^2 + 3t + 1)/6`, `w3 = t^3/6`;
  `dw0 = -(1-t)^2/2`, `dw1 = (3t^2 - 4t)/2`, `dw2 = (-3t^2 + 2t + 1)/2`, `dw3 = t^2/2`.

`D(u) = sum wx*wy*wz * P`, `dD/du_x = (rx-1)*sum dwx*wy*wz * P` (0 when clamped), same for y, z.

### 3.3 Outside the cage

`d = |u - clamp(u,0,1)|` (Euclidean distance outside the unit cube, 0 inside).

- `extend`: `f = 1` (the border deformation continues outward unchanged).
- `fade` (default): `f = 1 - smoothstep(0, fadeDistance, d)`; `fadeDistance` in local units,
  default `0.25`; `fadeDistance <= 0` means `f = (d > 0 ? 0 : 1)`.
  `grad f = -(6 q (1-q) / fadeDistance) * (u - uc) / d` with `q = d / fadeDistance`, for
  `0 < d < fadeDistance`, else 0.

### 3.4 Deformation, Jacobian, normals, tangents, stretch

With binding weight `w` (default 1, range `[0, 2]`) and vertex mask `m` (1 unless a mask
channel is set, then the vertex color channel, or `1 - channel` for the inverted ones):

- `delta(u) = w * m * f(u) * D(u)`; `u' = u + delta(u)`.
- `J = I + w * m * (f * dD/du + D (x) grad f)` (3x3, columns = derivatives along x, y, z).
- `M` = object space -> normalized space (§5.3), `A` = its linear 3x3 part, `Minv` its inverse.
  Position: `p' = Minv * (M p + delta)`.
- Normal: `n1 = transpose(mat3(Minv)) * n`; `n2 = cof(J) n1` with `cof(J)` columns
  `(j2 x j3, j3 x j1, j1 x j2)` (no inverse: safe when J is singular); `n' = normalize(transpose(A) * n2)`.
- Tangent: `t' = normalize(mat3(Minv) * (J * (A * t)))`; tangent `w` (handedness) unchanged.
- Stretch (volume ratio) `s = det(J)`; stacked lattices multiply. `s < 0` means the cell is
  folded inside out.

### 3.5 Solver API

Committed in `engine/core/lattice/LatticeSolver.h` (authoritative; namespace
`jasperengine::lattice`): `pointCount`, `pointIndex`, `restPosition`, `buildPaddedGrid`
(raw and from a component), `evaluate` (-> `Evaluation{delta, jacobian, stretch}`),
`deformWorldPoint`, `worldJacobian`, `objectToNormalized`, `ObjectSpaceLattice` +
`deformVertex`, `maxPaddedOffset`, `findFoldedCells`. Presets/selection API in
`LatticePresets.h` (§4).

## 4. Presets, resample, mirror, selection (`lattice/LatticePresets.h`, WP-SOLVER)

Shared by the editor (menus, inspector), MCP (`edit_lattice`) and Lua, so one name means one
result everywhere. All functions take the resolution and an offsets vector and an optional
point selection (`std::vector<int>` of flat indices; empty = all), and are pure.

- Presets (`LatticePreset` enum: `squash, stretch, taper, twist, bend, bulge, shear, noise`),
  parameters `axis` (x|y|z), `toward` (x|y|z, for bend/shear, default the next axis),
  `amount` (meaning per preset below), `seed` (noise), `mode` (`add` default / `replace`).
  Computed on rest positions `R` (local), result `R' - R` added to or replacing offsets.
  - `squash`: along axis scale `1 - amount` (amount 0..0.95), perpendicular scale
    `1/sqrt(1-amount)` (volume preserving). `stretch`: same with `1 + amount`.
  - `taper`: perpendicular scale `1 - amount * (a + 0.5)` where `a` is the axis coordinate
    (no change at the -0.5 end, `1-amount` at +0.5).
  - `twist`: rotate the perpendicular plane by `amount` degrees * `(a + 0.5)`.
  - `bend`: bend the axis toward `toward` by `amount` degrees total, circular arc around the
    centre of the -0.5 end, preserving length along the axis.
  - `bulge`: perpendicular scale `1 + amount * (1 - (2a)^2)` (max at the middle).
  - `shear`: offset along `toward` by `amount * (a + 0.5)`.
  - `noise`: deterministic random offset per point, uniform in a ball of radius `amount`
    (local units), from `seed` (same seed = same result on every platform: a fixed
    integer hash, not `rand()`).
- `resample(oldRes, offsets, newRes)`: evaluates the old offsets with **sharp** (trilinear)
  interpolation on the old grid at the new rest positions, so points where the grids
  coincide keep their exact offsets. Changing resolution keeps the deformation.
- `mirror(res, offsets, axis, fromPositive)`: copies offsets from one half to the mirrored
  points of the other half with the axis component negated; the middle plane (odd
  resolution) gets its axis component zeroed.
- Selection helpers (flat index lists): `selectAll`, `selectFace(axis, positive)`,
  `selectExterior`, `selectInterior`, `selectRange(i?, j?, k?)` (each an inclusive range or
  any), `expand(selection)` / `shrink(selection)` (6-neighbourhood on the grid),
  `invert(selection)`, `selectInWorldBox/Sphere(latticeModel, ...)` on current positions.
- `copyAsLua(entityVar, res, offsets, selection)`: text such as
  `lattice:setOffset(1, 2, 0, Vector3(0, 0.25, 0))` per non-zero selected point.

## 5. GPU deformation (WP-RENDER)

### 5.1 Data on the mesh (`lattice/LatticeTypes.h`, committed)

```cpp
#define LATTICE_MAX_PER_MESH 4
#define LATTICE_TEXTURE_WIDTH 64
#define LATTICE_RECORD_HEADER 12
struct LatticeMeshState {
    int count = 0;                          // active bindings for this mesh (0 = no lattice variant)
    std::vector<Vector4> texels;            // packed records, row-major, width LATTICE_TEXTURE_WIDTH
    float recordStart[LATTICE_MAX_PER_MESH] = {}; // first texel of each record
    int submesh[LATTICE_MAX_PER_MESH] = {-1, -1, -1, -1}; // binding submesh filter (-1 = all)
    float worldPadding = 0;                 // world-space AABB growth (§5.5)
    bool needUpload = false;                // texels changed since the last upload
    bool variantActive = false;             // count > 0 as the loaded shaders were built
    TextureRender texture;                  // RGBA32F, created on demand, destroyed with the mesh
};
```

### 5.2 Texture record layout (one per binding)

Texels (each RGBA32F), starting at `recordStart[b]`, laid out row-major in a texture of width
`LATTICE_TEXTURE_WIDTH` (texel `n` at `ivec2(n % 64, n / 64)`):
0-3 `M` columns (object -> normalized), 4-7 `Minv` columns, 8 `(rx, ry, rz, interpolation)`
(`0 sharp, 1 smooth, 2 cubic`), 9 `(stage, weight, outside, fadeDistance)` (`stage 0 before,
1 after`; `outside 0 extend, 1 fade`), 10 `(maskChannel, 0, 0, 0)` (`-1 none, 0..3 r,g,b,a,
4..7 inverted r,g,b,a`), 11 reserved 0, then the padded grid (§3.1) as `(x, y, z, 0)`.

### 5.3 Matrices

`latticeNorm = translate(0.5) * inverse(latticeTransform.modelMatrix)`;
`M = latticeNorm * meshTransform.modelMatrix` for both stages (before-skinning lattices are
placed around the mesh as it would stand unposed at its transform). Instanced meshes: the
deformation happens after the instance matrix, so every instance is deformed by where it
stands in the lattice. A lattice with a non-invertible transform (zero scale) is skipped and
reported.

### 5.4 Shaders

- `engine/shaderlib/includes/lattice.glsl`: `#ifdef HAS_LATTICE` declares
  `u_latticeTexture`/`u_lattice_smp` (unfilterable float, `texelFetch`) and
  `uniform u_vs_lattice { vec4 latticeParams; vec4 latticeStarts; }` (`x` = count, `y` =
  bitmask of the bindings active for this submesh, `z` = 1 when stretch output is wanted;
  `latticeStarts` = record starts), and `latticeDeform(int stage, inout vec3 pos, inout vec3
  normal, inout vec3 tangent, inout float stretch)` implementing §3. Without `HAS_LATTICE` it
  compiles to nothing.
- `TextureShaderType::LATTICE` (name `u_latticeTexture`), `UniformBlockType` entries for the
  color, depth and G-buffer vertex stages, slot fields on `Submesh` next to the morph ones.
- Call sites: `mesh.vert`, `depth.vert`, `gbuffer.vert`, `includes/surface_vs.glsl`,
  `includes/surface_depth_vs.glsl`, outline vertex include, and every other vertex path that
  draws a `MeshComponent` (the work package audits `shaderlib/` and `shaderlib/library/`
  and lists them in §12). Restructure so position, normal and tangent go through the same
  per-vertex Jacobian (compute once per lattice).
- Stretch: with `HAS_LATTICE`, `mesh.vert`/`surface_vs.glsl` output `v_latticeStretch`
  (`flat`-free float, 1 at rest) and the fragment side gets `float latticeStretch()` (1.0
  without a lattice) usable by library surface shaders; one library shader
  `library/lattice_stretch` (unlit heat map: blue squashed, white rest, red stretched, with
  `squashColor`, `stretchColor`, `range` params, base texture tint) demonstrates it.
- Forked shaders that copied `mesh.vert` before this change keep working (no lattice);
  the fork template and docs show the one-line call.
- `ShaderVariantCollector` (editor) and the export variant lists include the lattice bit.

### 5.5 Bounds

`worldPadding = sum over bindings of weight * maxMask(1) * maxPaddedOffset * max axis scale
of the lattice model matrix`; `RenderSystem` grows `worldAABB` by it (all axes) when
`lattice.count > 0`, and marks the AABB dirty when the padding changes.

### 5.6 Per-frame flow

`RenderSystem::update`, after the transform loop and bone matrices and before the mesh AABB
loop, calls `LatticeSystem::lateUpdate()` (followers, §6.3) and then
`LatticeRender::prepare(scene)`, which for every mesh entity affected by a binding (own or
inherited) rebuilds `LatticeMeshState` (skip identical texels: compare before marking
`needUpload`), and for meshes that lost all bindings resets the state. A change of
`count > 0` vs `variantActive` triggers the mesh variant reload path. `updateMeshBuffers`
uploads the texture when `needUpload`. Bindings to a deleted entity or an entity without a
`LatticeComponent` are skipped silently at runtime (the inspector shows them as missing).

## 6. `LatticeSystem` (WP-ECS)

### 6.1 Poses and tweens

`LatticeComponent::poses` (`LatticePose { std::string name; std::vector<Vector3> offsets; }`)
are authored in the editor/MCP/Lua. Runtime: `applyPose(name, weight)` sets `offsets = pose *
weight`; `tweenToPose(name, duration, EaseType)` / `tweenToRest(duration, EaseType)` animate
offsets from the current values (state in the component's runtime fields, not serialized;
`update(dt)` advances it; an ended tween leaves the exact target values). A pose whose size
does not match the resolution is resampled on use (§4 `resample`).

### 6.2 Jiggle

`LatticeComponent` jiggle fields (`jiggle`, `jiggleStiffness`, `jiggleDamping`,
`jiggleInertia`, `jiggleAnchor`, `jiggleMaxOffset`) drive a per-point spring in local space
into `dynamicOffsets` (not serialized): `x'' = -k x - c x' - inertia * a_local * anchorWeight`,
where `a_local` is the lattice's world acceleration (finite difference of its world
position, ignoring the first two frames after enable/teleport) rotated and scaled into local
space, and `anchorWeight` is 0 at the anchored face, 1 at the opposite face (all 1 for
`none`). Semi-implicit Euler with fixed substeps of at most 1/120 s, `|x|` clamped to
`jiggleMaxOffset`, sleeps when every point's speed and offset are under 1e-4 (then
`dynamicOffsets` is cleared so packing stops). `poke(i, j, k, impulse)` and
`pokeAt(worldPoint, worldImpulse, worldRadius)` add velocity (smooth radial falloff) and wake
it. Runs only while the scene is playing (not in the editor's edit mode).

### 6.3 Followers

`LatticeFollowerComponent { Entity lattice; Entity rest; bool applyPosition = true; bool
applyRotation = true; bool applyScale = false; float weight = 1; }`: each frame (in
`lateUpdate`, after transforms are current) the follower's world transform becomes the
`rest` entity's world transform pushed through the lattice: position by `deformWorldPoint`,
rotation by the rotation part of the world-space Jacobian (Gram-Schmidt of `J` applied to
the rest basis), scale by the column lengths of `J` along the rest axes. Converted to the
follower's parent space, written to its Transform, and its subtree's world matrices updated
that frame. Missing lattice/rest: the follower is left alone. A follower must not be its
own rest or an ancestor of the rest (inspector/MCP refuse; runtime ignores).

### 6.4 Sprite tessellation

A sprite (SpriteComponent) quad has four vertices, which a lattice can only warp bilinearly.
When a sprite entity is a lattice target (own or inherited binding),
`LatticeModifierComponent::subdivisions` (default 8, range 1..64; inherited targets use the
ancestor's value) makes `MeshSystem` generate an `n x n` grid instead of the quad (same UVs,
same texture rect, same pivot/size/flip semantics, indices and normals). Removing the binding
restores the quad. `LatticeRender::prepare` (which resolves bindings) sets
`LatticeMeshState::wantsTessellation`/`tessellation`; `MeshSystem` (WP-ECS) rebuilds the
sprite when they change (a frame later is fine).

## 7. Animation (`LatticeTracks`, WP-ANIM)

- `LatticeTracksComponent { std::vector<int> points; std::vector<std::vector<Vector3>> values;
  std::vector<std::vector<Vector3>> inTangents, outTangents; }`: sparse: `values[key][n]` is
  the offset of point `points[n]`. Points out of range for the target's current resolution
  are ignored. Keyed with `KeyframeTracksComponent` like the other tracks; Hermite tangents
  as morph tracks do.
- `ActionSystem` dispatch branch (target must have a `LatticeComponent`), `LatticeBlendAccum`
  per target and point for crossfades, flushed into `offsets` (points not animated by any
  running track keep their values).
- `LatticeTracks` action class (C++/Lua) mirroring `MorphTracks`.
- Editor `AnimationWindow`: lattice tracks show as blocks; keying (right-click key /
  Snapshot) on a lattice target records a `lattice` channel: the selected points when points
  are selected in the Scene view, else every point with a non-zero offset plus every point
  already in the track; creating a Lattice Tracks block from the menu. Selecting a lattice
  track's block in the Animation window selects its points in the Scene view.
- Serialization in scenes/prefabs; MCP `edit_animation` channel `"lattice"` with
  `points: {"<index>": [x,y,z]}` per key or `[[i,j,k],[x,y,z]]` pairs.

## 8. Scripting (WP-ECS; tracks by WP-ANIM)

Lua (and C++) `Lattice` GameObject (`Lattice(scene)` creates entity + Transform +
LatticeComponent): `setResolution(x,y,z)` (resamples), `getResolution()`, `getPointCount()`,
`getOffset(i,j,k)`, `setOffset(i,j,k, v)`, `getOffsetAt(index)`, `setOffsetAt(index, v)`,
`getPointPosition(i,j,k)` / `setPointPosition` (local), `getPointWorldPosition` /
`setPointWorldPosition`, `resetOffsets()`, `setInterpolation`, `setOutside`, `setFadeDistance`,
`applyPreset(name, axis, amount[, toward])`, `savePose(name)`, `applyPose(name[, weight])`,
`tweenToPose(name, duration[, ease])`, `tweenToRest(duration[, ease])`, `isTweening()`,
`setJiggle(enabled)` + jiggle setters, `poke(i,j,k, impulse)`, `pokeAt(worldPoint, impulse,
radius)`, `deformPoint(worldPoint)` (world in, world out), `bind(entityOrObject[, weight])`,
`unbind(entityOrObject)`. Index-based methods because `std::vector` properties are copied
into Lua tables (`comp.offsets[3].x = 1` would silently do nothing). Component bindings for
the four components in `ECSClassesLua.cpp`; `generate_lua_stubs.py` resolves every type
(`test_stubs.py` has no unresolved); `docs/mcp/guide/lua-api.md` gets a Lattice section.

## 9. Editor (WP-EDITOR-DATA, WP-EDITOR-SCENE)

### 9.1 Data plumbing (WP-EDITOR-DATA)

Catalog (component enum, names "Lattice", "Lattice Modifier", "Lattice Follower",
"Lattice Tracks", icons, properties incl. indexed `offsets[i]` so prefab overrides are per
point, `bindings[i].lattice/weight/stage/submesh/mask`), `Stream` encode/decode (scenes,
prefabs, copy/paste, `ComponentValuesCmd`), Factory codegen (C++ export), `McpValues`,
`ProjectUtils`, `isNonStructuralProperty`, `ComponentAddDialog` entries with
descriptions. Round trip must be byte-identical on re-save.

Creation (GameObject > Create > 3D Object > Lattice, Hierarchy "+" and context menu,
`CreateEntityType::LATTICE`):
- With entities selected: the lattice is fitted to their combined bounds (world AABB of all
  their meshes and descendants'; if exactly one entity is selected, aligned to its world
  rotation using its local bounds), padded by 2% (min 0.01 per axis; a flat axis gets
  `max(10% of the largest extent, 0.01)` and resolution 2 on that axis), and every selected
  entity that has a mesh or mesh descendants gets a `LatticeModifierComponent` (added, or
  the lattice appended to its bindings) with `includeChildren = true` when the entity itself
  has no mesh. One undo step. The lattice becomes the selection.
- Without a selection: a unit lattice at the scene view focus point.
- Also "Add Lattice to Selection" in the Hierarchy context menu (same behaviour).

Inspectors:
- Lattice: Resolution (ints, resamples, one undo step), Interpolation, Outside + Fade
  Distance, Points section (selected count; with one point selected its `(i, j, k)` and an
  editable Offset; Reset Selected, Reset All, Select All buttons), Presets row (preset,
  axis, toward, amount, Apply), Poses (list: name, Apply, Overwrite, Rename, Delete; Save
  Current As...), Jiggle foldout, "Deforming" list of targets (click selects), folded-cell
  warning row (from `findFoldedCells`), "Edit Points" hint.
- Lattice Modifier: Include Children, Subdivisions (shown when a sprite is involved),
  bindings list (+/-, reorder, lattice entity field with picker and drag-drop, Weight,
  Stage (skinned targets only), Submesh, Mask), warnings (missing lattice, more than 4),
  Bake button (§9.4).
- Lattice Follower: Lattice, Rest, Apply Position/Rotation/Scale, Weight, "Create Rest
  Here" button (creates an empty sibling at the follower's current transform and assigns it).
- Lattice Tracks: like Morph Tracks (point count, keys).

### 9.2 Scene view editing (WP-EDITOR-SCENE)

No separate mode to enter: selecting a lattice shows its cage; points are sub-objects.

- Cage drawing (`SceneRender3D`, tools scene so it shows through geometry, back-facing parts
  faded): edges between neighbouring points, points as screen-constant squares (white,
  selected orange `(1, 0.6, 0)` as other sub-selections), the rest cage faint when any
  offset is non-zero. Edge colour by stretch: edge length / rest length mapped through a
  blue (0.5) - white (1) - red (2) gradient (preference, default on). Shown for the
  selected lattice; all lattices with the Gizmos popup toggle "Lattices" / MCP
  `show_all_lattices` (unselected ones faint). Shown regardless of other gizmo toggles when
  the lattice is selected.
- Picking: click a point to select it (replaces), Shift/Ctrl toggles; drag a rectangle from
  empty space while a lattice is selected selects points in it (Shift adds); click empty
  space (no point, no entity) clears points; clicking another entity selects that entity as
  usual (points cleared). Esc clears the point selection, then the entity selection as today.
- Transform: with points selected, the W/E/R gizmo acts on them: pivot at the centroid
  (or the last selected point when the editor's pivot mode is "pivot", if the editor has
  that toggle), orientation lattice-local or world following the editor's local/global
  toggle; grid/rotation snapping as for objects. One drag = one undo step
  (`LatticePointsCmd`, merge while dragging, `setNoMerge` on release, `PrefabEditUndo`
  embedded so prefab instance overrides are recorded per point). Without points selected the
  gizmo moves the lattice entity as usual. F frames the selected points (or the lattice).
- Proportional falloff: toggle (context menu, inspector, shortcut `O`), radius in lattice
  local units shown as a circle around the pivot; mouse wheel during a drag changes it;
  curve `smooth` (default) / `linear` / `sharp` / `constant`; unselected points within the
  radius move by the gizmo delta times the curve weight (rotation/scale likewise around
  the same pivot).
- Context menu (right-click in the Scene view while a lattice is selected): Select All
  (Ctrl+A), Select None, Invert, Select Exterior, Select Face (+X..-Z), Expand, Shrink
  (shortcuts only where they do not collide with existing ones; record choices in §12), Mirror (X/Y/Z, + to - and - to +), Reset Selected, Reset All, Presets..., Fit to
  Targets, Fit to Selection..., Copy as Lua, Proportional Falloff (toggle), Lattice
  Preferences....
- Preferences (persisted in `AppSettings`, UI in the existing preferences/settings window,
  else a small dialog): line colour, thickness, behind-geometry opacity, point size, point
  colours, stretch colouring on/off and gradient end colours, falloff curve and default radius.
- Editing a lattice while playing follows the editor's existing play-mode rules for
  component edits.

### 9.3 Shared editor operations

`editor/lattice/LatticeEditorOps.h/.cpp` (WP-EDITOR-SCENE): the single path for edits made by
the Scene view, the inspector, the context menu and MCP: set/move/rotate/scale selected
points with falloff, reset, preset, mirror, resample, fit, bind/unbind, pose save/apply/
delete, each producing one undoable command (or a command group) through `CommandHandle`.
WP-MCP and the inspector call these; no other code writes lattice offsets in the editor.

### 9.4 Bake

"Bake Deformation" (Lattice Modifier inspector, MCP op `bake`): writes the current
deformation of the target's model into a new glTF binary next to the original
(`<name>_lattice.glb`, never overwriting: `_2`, `_3`...) using the CPU solver (§3, same math
as the GPU; before-skinning bindings bake into the bind pose of skinned models,
after-skinning ones are not bakeable and are kept with a note), switches the ModelComponent
to it, removes the baked bindings, one undo step (undo restores the model path and bindings;
the file stays). Generated meshes (sprites, primitives, polygons, tilemaps): the button is
disabled with the reason. If the engine cannot write glTF, record the replacement in §12.

## 10. MCP (WP-MCP)

### 10.1 Generic

Catalog properties make `get_entity`, `set_properties`, `add_component`, `reset_component`,
`list_component_types` work for all four components, with `offsets[i]` addressable.
`create_entity` gains `type: "lattice"` (same fitting and binding as the editor command
when `targets` is given). `set_scene_camera` `display` gains `show_all_lattices`.

### 10.2 `edit_lattice`

One call = one undo step (whole call rolled back on any error, errors indexed like
`ops[2].select.face`, listing valid choices). Uses `LatticeEditorOps` only. Refused while
playing (same message as terrain). Arguments:

- `lattice` (entity ref; optional with `create`, else falls back to the single selected
  lattice like terrain tools), `create: {targets?, name?, resolution?, fit_padding?}`.
- Top-level setters: `resolution [x,y,z]` (resamples), `interpolation`, `outside`,
  `fade_distance`.
- `ops` (ordered): `set` (`points: [{ijk:[i,j,k] | index, offset:[x,y,z] | position:[x,y,z],
  space: local|world}]`), `move` (`select`, `offset`, `rotate` (degrees xyz), `scale`,
  `pivot: center|lattice|[x,y,z]`, `space: local|world`, `falloff: {radius, curve}`),
  `reset` (`select`), `preset` (`name`, `axis`, `toward`, `amount`, `seed`, `mode`,
  `select`), `mirror` (`axis`, `from: "+"|"-"`), `fit` (`targets`, `padding`), `bind`
  (`target`, `weight`, `stage`, `submesh`, `mask`, `include_children`), `unbind`
  (`target`), `pose` (`save`|`apply`|`delete` name, `weight`), `bake` (`target`).
- `select`: `"all"`, `"exterior"`, `"interior"`, a face `"+x".."-z"` (aliases `top`,
  `bottom`, `left`, `right`, `front`, `back`), or an object combining (intersection)
  `i`, `j`, `k` (int or `[lo, hi]`), `points` (`[[i,j,k], ...]` or indices), `box`
  (`{min, max}` world), `sphere` (`{center, radius}` world).
- `sample: [[x,y,z], ...]` world points -> deformed world points (read-only).
- `detail: "summary" | "points"`.
- Returns: `lattice` (entity), `resolution`, `interpolation`, `outside`, `targets` (entity,
  name, stage, weight), `moved_points` (count non-zero), `max_offset` (local and world),
  `warnings` (folded cells with `(i,j,k)` and a fix hint, more than 4 lattices on a mesh,
  missing bindings, targets without mesh), `points` with `detail: points` (non-zero offsets:
  `ijk`, `offset`, `world`), `samples` when asked. With no ops it is a read-only query.

### 10.3 Docs

`docs/mcp/guide/lattice.md` (`jasper://docs/lattice`): concepts, coordinate spaces,
selectors, presets with pictures-in-words, recipes (squash-and-stretch bounce with
`LatticeTracks`, big-head character, car crumple, wind on a flag with Lua, jelly with
jiggle, rock variants with noise + bake), costs, limits. One line in `McpInstructions.cpp`
(guide stays <= 16 KB), the guide README table, `TOOLS.md` regenerated, `lua-api.md`
section, the top-level `README.md` features list.

### 10.4 Tests

`tools/mcp/tests/test_lattice.py` (real editor under Xvfb): create around a model and a
sprite, edit by every op and selector, undo/redo restores exactly, save/reload round trip
byte-identical, prefab instance per-point override, resolution resample keeps shape,
`sample` matches expected numbers, folded-cell warning, `show_all_lattices` screenshot
(pngdiff golden), Lua script driving offsets in play mode, `edit_animation` lattice channel
plays, bake writes a file and undo restores. Unit tests for the math (§3, §4) in a gtest
target `jasperengine-lattice-tests` (`engine/tests/lattice/`, googletest from the same place
the htmlui tests take it, Linux CI-runnable without a GPU): identity at rest for all modes
and outside settings, linear-field reproduction, analytic Jacobian vs finite differences,
normals for a known shear/rotation, fade continuity, preset volumes (squash), resample
identity, mirror, noise determinism, folded-cell detection.

## 11. Work packages

Each package works on its own branch `lat/<name>` in its own worktree, owns the files
listed, may touch other files only where listed, commits as it goes, and keeps the editor
(`jasperengine-editor`) building with no warnings in its files. Packages can merge sibling
branches to integrate (the integrator merges into `lattice`).

- **WP-SOLVER**: `lattice/LatticeSolver.*`, `lattice/LatticePresets.*`,
  `engine/tests/lattice/**`, its CMake option/target. §3, §4, unit tests.
- **WP-ECS**: `subsystem/LatticeSystem.*`, `object/Lattice.*`, `EntityCloner` copy + remap
  of the entity references (`bindings[].lattice`, follower `lattice`/`rest`),
  `MeshSystem` sprite tessellation, Lua bindings for the components and `Lattice` object
  (`GameObjectClassesLua.cpp`, `ECSClassesLua.cpp`), stubs fragments. §6, §8 (minus tracks).
- **WP-RENDER**: `lattice/LatticeRender.*`, `shaderlib/**`, `ShaderPool`,
  `ShaderVariantCompiler`, `ShaderData`, `Render.h` enums, `MeshComponent` `Submesh` slot
  fields, `RenderSystem`, editor `ShaderVariantCollector`, precompiled shader lists if any,
  the `lattice_stretch` library shader. §5.
- **WP-EDITOR-DATA**: Catalog, Stream, Factory, McpValues, ProjectUtils, ComponentAddDialog,
  Hierarchy create menu, `CreateEntityCmd`, Inspector drawing for the four components
  (calling `LatticeEditorOps` for actions). §9.1.
- **WP-EDITOR-SCENE**: `editor/lattice/**`, `command/type/LatticePointsCmd.*`, SceneRender/
  SceneRender3D/SceneWindow/ToolsLayer hooks, context menu, preferences, Gizmos popup
  toggle. §9.2, §9.3.
- **WP-MCP** (after the above merge): `edit_lattice`, `create_entity`/`set_scene_camera`
  additions, docs §10.3, tests §10.4, tool budget.
- **WP-ANIM** (after ECS + EDITOR-DATA merge): §7 everywhere it reaches.
- **WP-BAKE** (after RENDER + EDITOR-DATA merge): §9.4.
- **Integration** (integrator): Xcode project (`engine/workspaces/xcode/JasperEngine.xcodeproj`) file
  entries, merges, full build, full MCP test suite, review passes, fixes.

## 12. Implementation notes

(Work packages append here: decisions, deviations, file lists, shortcut choices.)

### 12.x WP-SOLVER (solver, presets, unit tests)

Files: `engine/core/lattice/LatticeSolver.{h,cpp}`, `engine/core/lattice/LatticePresets.{h,cpp}`
(stubs replaced; headers only gained comments), `engine/tests/lattice/**` (new),
`engine/CMakeLists.txt` (option `JASPERENGINE_LATTICE_TESTS`, default OFF, and its
`add_subdirectory` at the end, outside the standalone block). No declarations were added.

Behaviour the other packages rely on (all in the header comments too):

- Resolutions are clamped to [2, 16] everywhere (`pointCount`, `pointIndex`, `restPosition`,
  grids, presets, selection). Offsets/dynamic offsets shorter than `rx*ry*rz` count the missing
  entries as zero, longer ones are ignored; nothing reads out of bounds. `buildPaddedGrid`
  resizes its output (no reallocation when the size is unchanged).
- `evaluate` is allocation free and returns the rest evaluation (delta 0, I, stretch 1) for a
  **disabled** lattice, weight 0, a NaN `u`, or a padded grid of the wrong size. So followers,
  `deformVertex` and bake agree with a `LatticeRender` that skips disabled lattices (WP-RENDER:
  skip them). `f == 0` (beyond the fade) also returns the rest evaluation.
- Positions use `p' = p + mat3(Minv) * delta` (equal to `Minv * (M p + delta)` for affine M,
  exact at rest; the GPU may use either). A vertex the binding does not move (rest
  evaluation, mask 0) is left bit-identical, normal not renormalized. A normal or tangent whose
  transformed length is 0 keeps its old value (GPU: guard `normalize(0)` the same way).
- Mask: `m` = colour channel, or `1 - channel` inverted, **not clamped**; a null colour means
  `m = 1` for every channel including the inverted ones (WP-RENDER: a mesh without a colour
  attribute must behave as mask 1, e.g. pack `maskChannel -1` for it).
- `deformWorldPoint` returns `p + Lm * delta`; `worldJacobian` returns
  `I + Lm (J - I) Lm^-1` (= `Lm J Lm^-1`, exactly I at rest). A lattice model with a zero or
  non-finite linear determinant: point unchanged, Jacobian I, and `objectToNormalized`
  returns an all-NaN matrix (`isValid()` false) so callers can skip it.
- `findFoldedCells` evaluates det J (weight 1, offsets + dynamicOffsets, ignoring `enabled`)
  at each cell's 8 corners and centre **inside the cell** (cell index fixed, t = 0/1/0.5, so a
  sharp lattice uses the cell's own one-sided derivatives) and reports `(i, j, k)` (i in
  `[0, rx-2]`) when any value is `<= 1e-6` (or NaN). Ascending k, j, i order.
- Presets: computed in double on the rest positions. Axis `a`, perpendicular axes `a+1, a+2`
  (cyclic: x -> (y, z), y -> (z, x), z -> (x, y)). Twist is counterclockwise seen from +axis
  (around +y, +x goes to -z). Squash amount is capped at 0.95, stretch amount floored at
  -0.95 (scale >= 0.05). `toward == axis` means the next axis. Bend (comment in the code):
  with `s = a + 0.5`, `theta = amount` in radians, `R = 1/theta`: the line through the centre
  of the -0.5 end becomes the arc of length 1 starting there tangent to +axis, centred at
  `(a, b) = (-0.5, R)`; `a' = -0.5 + (R - b) sin(theta s)`, `b' = R - (R - b) cos(theta s)`
  (computed without the R - R cancellation), third axis unchanged; negative amounts bend
  toward -toward. Amount 0 (or non-finite) is the rest shape for every preset: add mode leaves
  offsets bit-identical, replace mode sets the selected points to zero. Invalid/repeated
  selection indices are ignored/counted once. `applyPreset` and `mirror` resize `offsets` to
  the point count first.
- Noise: `pcg_hash` (Jarzynski & Olano 2020) of `(seed, index, attempt)`, rejection sampling
  of a 2^24 integer grid over `[-1, 1)^3` (integer test), then one exact conversion and one
  multiply by `amount / 2^23` per component: bit-identical on every platform (no libm). The
  golden values in `TestLatticePresets.cpp` come from an independent Python implementation.
  The axis is irrelevant for noise.
- `resample` computes the old-grid cell and t of each new point in integers, so coincident
  points copy the old offsets bit for bit (same resolution = identical; 3 -> 5 -> 3 round trips
  exactly).
- Selection: `selectRange` with a negative lo is "any", hi is clamped, `hi < lo` is empty.
  `shrinkSelection` is the dual of expand (points with an unselected in-grid neighbour drop;
  the outside of the grid does not count, so shrinking everything keeps everything).
  `selectInWorldBox` accepts swapped min/max, box and sphere tests are inclusive, a negative
  radius selects nothing.
- `presetFromName` accepts the lowercase names only. `copyAsLua` writes one
  `var:setOffset(i, j, k, Vector3(x, y, z))\n` per non-zero selected point in index order,
  (i, j, k) 0-based (WP-ECS: the Lua `setOffset` indices must be 0-based to match), numbers
  as the shortest text that reads back as the same float, classic locale, -0 as `0`.

Unit tests (`jasperengine-lattice-tests`, 36 tests, Linux, no GPU/display): they compile only
`core/math/{Vector2,Vector3,Vector4,Matrix3,Matrix4,Quaternion,AABB,OBB,Plane}.cpp`,
`core/util/Angle.cpp` and the two lattice sources, plus `EngineStubs.cpp` (defines
`Engine::isUseDegrees()`, Angle's only engine dependency). googletest comes from
`libs/yaml-cpp/test/gtest-1.11.0` like the HTML UI tests (skipped if a `gtest` target already
exists). Coverage: the engine matrix conventions, indexing, padded grid and ghosts,
mismatched sizes, identity at rest (3 modes x extend/fade with fade 0.25, 0, -1, 2; evaluate,
world and vertex paths), linear-field reproduction (sharp/cubic everywhere, smooth at the
points), analytic J vs finite differences (inside, at cell borders, outside with fade and
extend), world Jacobian vs finite differences, normals for a known shear and rotation,
normals/tangents/stretch vs object-space finite differences, masks, stacked stretch, fade
continuity, folded cells, every preset (squash/stretch volumes), resample, mirror, noise
goldens and ball uniformity, selection helpers, copyAsLua. A mutation pass (26 single-line
bugs in the solver and presets, e.g. transposes, multiplication order, ghost passes, signs)
was each caught by at least one test.

Build and run, standalone (seconds):

    cmake -S engine/tests/lattice -B build/lattice-tests -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build/lattice-tests -j2
    build/lattice-tests/jasperengine-lattice-tests

or inside the editor build (the binary lands in the build root):

    cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_LATTICE_TESTS=ON
    cmake --build build/native --target jasperengine-lattice-tests -j2
    build/native/jasperengine-lattice-tests

(also from the standalone engine build `cmake -S engine ... -DJASPERENGINE_LATTICE_TESTS=ON`;
`ctest` runs it where testing is enabled).

### 12.x WP-EDITOR-DATA

§9.1 and the generic MCP surface of §10.1, as specified, with these decisions:

- **Component types.** `ComponentType::LatticeComponent`, `LatticeModifierComponent`,
  `LatticeFollowerComponent`, `LatticeTracksComponent` appended after `UIDocumentComponent`.
  Scene-file keys (`getComponentName(.., true)`): `lattice`, `latticemodifier`,
  `latticefollower`, `latticetracks`. Display names "Lattice", "Lattice Modifier",
  "Lattice Follower", "Lattice Tracks". Icons: `ICON_FA_BORDER_NONE` (lattice, also its
  Hierarchy row), `ICON_FA_WAND_MAGIC_SPARKLES`, `ICON_FA_LOCATION_ARROW`,
  `ICON_FA_CUBES_STACKED`. The first three have the shared enable checkbox.
  Added `LAST_COMPONENT_TYPE` (Catalog.h); the three MCP loops over every type
  (`McpRefs::componentTypeNames`, `resolveComponent`, `list_component_types`) use it instead
  of naming the last type.
- **Catalog properties.** Lattice: `resolutionX/Y/Z` (Int), `offsets` (Custom, the whole list),
  `offsets[N]` (Vector3, one per point: prefab overrides and PropertyCmd edits are per point),
  `interpolation`, `outside`, `jiggleAnchor` (Enum), `fadeDistance`, the jiggle floats,
  `jiggle` (Bool), `poses` (Custom). Modifier: `bindings` (Custom), `bindings[N].lattice`
  (Entity), `.weight` (Float), `.stage`, `.mask` (Enum), `.submesh` (Int), `includeChildren`,
  `subdivisions`. Follower: `lattice`, `rest` (Entity), the three `apply*` (Bool), `weight`.
  Tracks: `points`, `values`, `inTangents`, `outTangents` (Custom), `points[N]` (Int),
  `values[N]` (Custom, the offsets of key N). Runtime fields (`dynamicOffsets`, jiggle and
  tween state, `revision`, `warnedOverflow`) are neither properties nor serialized.
  Entity properties are generic, so prefab reference remapping, duplicate, paste, paste
  component values and MCP read/write cover `bindings[N].lattice` and the follower fields
  without lattice code.
- **`UpdateFlags_Lattice`** (bit 39), on every lattice and modifier property that changes the
  deformation (not `poses`, not the follower's, read every frame). `Catalog::updateEntity`:
  lattice: clamps the resolution to [2, 16] and bumps `revision`; modifier: clamps
  `subdivisions` to 1..64 and binding weights to [0, 2] (non-finite -> 1) and clears
  `warnedOverflow` (an edited modifier warns again). Every decode over a live component
  (undo of component values, paste, prefab refresh) bumps `revision` through
  `getChangedUpdateFlags` (Custom properties count as changed); `Catalog::copyComponent` bumps
  it too.
- **Resolution and offsets.** `updateEntity` never resizes `offsets`: a prefab instance
  re-applies its overrides one property at a time in name order (`offsets` before
  `resolutionX`), so a resize there would cut overridden points. A mismatched size is safe:
  the solver counts missing offsets as zero and ignores extra ones (§12 WP-SOLVER), the
  inspector reads within bounds. Two places keep the sizes in step: `Stream::decodeLatticeComponent`
  cuts/pads `offsets` to `rx*ry*rz` (a component encoding set through MCP, paste, a
  hand-edited file), and MCP refuses a lone `resolutionX/Y/Z` write in set_properties /
  add_component / create_entity values (`McpValues::buildPropertyCommand`: "the resolution
  changes with edit_lattice resolution: [x, y, z] (or the Inspector's Resolution), which
  resamples the offsets"). WP-MCP: `edit_lattice resolution` must go through
  `LatticeEditorOps::setResolution`, as the inspector does.
- **`Catalog::copyPropertyValue`** copies the lattice Custom properties (`offsets`, `poses`,
  `bindings`, the tracks' lists and `values[N]`). Without it a whole-list override (a
  resolution change, Add Binding, a key added) recorded on a prefab instance would be lost at
  the next prefab refresh or reload (existing gap of `ranges`, `pathPoints` and ShadowCaster2D
  `points`, untouched).
- **Scene format** (byte-identical on re-save; every key always written, so the clipboard's
  "missing key" filling never applies):

      lattice: {enabled, resolutionX, resolutionY, resolutionZ, offsets: [[x, y, z], ...],
                interpolation: sharp|smooth|cubic, outside: extend|fade, fadeDistance,
                poses: [{name, offsets: [...]}], jiggle, jiggleStiffness, jiggleDamping,
                jiggleInertia, jiggleAnchor: none|negative_x|positive_x|...|positive_z,
                jiggleMaxOffset}
      latticemodifier: {enabled, bindings: [{lattice: <entity>, weight,
                        stage: before_skinning|after_skinning, submesh,
                        mask: none|red|...|inverted_alpha}], includeChildren, subdivisions}
      latticefollower: {enabled, lattice, rest, applyPosition, applyRotation, applyScale, weight}
      latticetracks: {points: [..], values: [[[x, y, z], ..], ..], inTangents?, outTangents?}

  Floats are written as the shortest round-tripping text (`YamlNumbers`), non-finite ones are
  left out (decode keeps the default). Unknown enum names decode to the default. Decoding over a
  live lattice whose resolution changes clears its jiggle springs and tween (laid out for the
  old points); poses keep their size (resampled where applied, §6.1).
- **MCP enum names** = the scene-file names; `McpValues::enumTable` also gains
  `lattice_interpolation`, `lattice_outside`, `lattice_jiggle_anchor`, `lattice_stage`,
  `lattice_mask` for WP-MCP. A follower's `lattice` or `rest` set through MCP may not be the
  follower or a GameObject below it (refused with a hint), as the inspector's pickers exclude them.
- **Creation.** `EntityCreationType::LATTICE` (the spec's `CreateEntityType::LATTICE`):
  Transform + LatticeComponent, a unit lattice. The menus do not push it directly: the
  GameObject > 3D Object > Lattice entry (2D scenes: 2D Object > Lattice; both rootOnly), the
  Hierarchy "+" and the row context menu's Create items, and the new "Add Lattice to Selection"
  row item (3D and 2D scenes, enabled when a target or something below it has a mesh) all
  call `HierarchyWindow::createLatticeAround(targets)` -> `LatticeEditorOps::createLattice`
  with the authored, Transform-carrying targets (the selection, or the clicked row when it is
  not selected) and the Scene view camera's world target as the fallback position; a failure
  is a Console error. A lattice is never made a child of the row it was asked from.
  WP-EDITOR-SCENE may build its unit lattice with `CreateEntityCmd(LATTICE)`.
- **Inspectors.** Lattice: Resolution as three X/Y/Z fields applied when let go through
  `LatticeEditorOps::setResolution` (one undo step for a multi-selection, `CommandHistory`
  mark/groupSince), Interpolation, Outside, Fade Distance (only with Fade), Points (selected
  count, `(i, j, k)` and an Offset field for one selected point applied through `setOffsets`
  when let go, Reset Selected, Reset All, Select All, Select None, the Edit Points hint),
  folded-cell warning (`findFoldedCells`, cached on a hash of resolution, settings, offsets and
  dynamic offsets), Presets (preset, axis, toward for bend/shear, amount, seed for noise,
  replace; Apply to the selected points or all), Poses (Save Current As..., Apply,
  Overwrite, Rename, Delete), Jiggle foldout (plain properties), Deforming list
  (`findTargets`, click selects). Points, presets and poses need a single selected lattice.
  Modifier: Include Children, Subdivisions (only when a sprite is among the deformed meshes),
  Add Binding / reorder / remove (whole-list PropertyCmds), per binding Lattice (entity field,
  picker filtered to lattices, drag-drop, itself excluded), Weight, Stage (only when a
  deformed mesh is skinned), Submesh, Mask; warnings for a binding without a lattice, a
  deleted one or one without a LatticeComponent, a mesh reached by more than
  `LATTICE_MAX_PER_MESH` lattices (own + inherited, as §2.1 counts them), and nothing to deform.
  The **Bake** button (§9.4) is left to WP-BAKE. Follower: Lattice, Rest (pickers exclude the
  follower and its subtree), apply flags, Weight, warnings, "Create Rest Here"
  (`CreateEntityCmd(GAMEOBJECT)` under the follower's parent, quiet, its local
  position/rotation/scale copied, and `rest` set: one undo step). Lattice Tracks: points (count
  and list), keys (count, per key "N offsets, M moved", Add Key / remove with Hermite tangents
  kept aligned as Morph Tracks), a warning for a key whose size differs from `points`.
  Edits that go through `LatticeEditorOps` report failures in the Console.
- **`isNonStructuralProperty`**: every property of the four components.
- **ProjectUtils**: add/remove for the four; `getEntityClassInfo` maps a lattice entity to the
  `Lattice` class (object base), so script templates type it as `Lattice` (WP-ECS's Lua class).
- **Factory** (C++ export): all four; only the moved points are written
  (`offsets.assign(n, 0)` then `offsets[i] = ...`), entity fields through `formatEntity`.
  Verified by compiling the generated code of a scene with every component (`-fsyntax-only`
  with the engine's flags).

Contract additions: none in the engine headers. Editor: `UpdateFlags_Lattice`,
`LAST_COMPONENT_TYPE`, the four `ComponentType`s, `EntityCreationType::LATTICE`,
`HierarchyWindow::createLatticeAround`, the Stream/Factory/inspector functions above.

Files: `editor/Catalog.{h,cpp}`, `editor/Stream.{h,cpp}`, `editor/Factory.{h,cpp}`,
`editor/mcp/McpValues.cpp`, `editor/mcp/McpRefs.cpp` and `editor/mcp/tools/ComponentTools.cpp`
(the `LAST_COMPONENT_TYPE` loops only), `editor/util/ProjectUtils.cpp`,
`editor/window/dialog/ComponentAddDialog.cpp`, `editor/window/HierarchyWindow.{h,cpp}`,
`editor/window/InspectorWindow.{h,cpp}`, `editor/command/type/CreateEntityCmd.{h,cpp}`,
`editor/command/type/PropertyCmd.h`, `tools/mcp/tests/test_lattice_data.py` (new).

Tests: `tools/mcp/tests/test_lattice_data.py` (7 tests, new empty project): component types and
property lists (no runtime fields), every property type through add_component / set_properties
/ get_entity with undo/redo and reset_component, the resolution refusal, clamping, follower
self/descendant refusal, the scene file's names and the absence of runtime state, a second
editor reloading every value and re-saving byte-identically (the file is emptied first, so the
comparison is real), a prefab instance overriding `offsets[2]` only while the prefab's change
to `offsets[5]` reaches it (also after a reload; revert restores the prefab value), duplicate
and copy/paste of lattice + target + follower + rest with every reference remapped, a target
duplicated alone keeping its lattice, paste component onto another mesh.

    JASPER_FIXTURE=<empty project> JASPER_EDITOR=build/native/jasperengine-editor \
        tools/mcp/run-tests.sh tools/mcp/tests/test_lattice_data.py

Integrator notes:
- LatticeRender (WP-RENDER) must not trust `revision` alone across entity reuse: a new lattice
  (created, pasted, loaded) starts at 0 whatever entity id it gets.
- The inspector's lattice actions are only as good as `LatticeEditorOps`; with the contract
  stubs they report "lattice editing is not implemented yet" in the Console.
- Also run on a temporary merge of `lat/editor-scene` (with `lat/ecs`, `lat/render` and the
  solver): `test_lattice_data.py` passes, the inspector shows the folded-cell warning and the
  Deforming list. That build's Console showed `includes/lattice.glsl:135: 'active' : Reserved
  word` for the mesh and depth `Ltc` variants (glsl410): WP-RENDER must rename it.

### 12.x WP-RENDER

GPU deformation as §5, with these decisions and deviations:

- **Variant bits.** MESH bit 5 `Ltc` (`HAS_LATTICE`, the retired `Pcf` slot), DEPTH bit 14
  `Ltc`, GBUFFER bit 10 `Ltc` plus bits 11/12 `Vc3`/`Vc4` (the vertex colour a mask channel
  reads; set only with `Ltc`). `getMeshProperties`, `getDepthMeshProperties` and
  `getGBufferMeshProperties` take a trailing `lattice` argument (default false), so every
  mesh without a lattice keeps its exact key. Counts: DEPTH 15, GBUFFER 13. A key with the
  terrain bit compiles `Ltc` inert (the compiler leaves `HAS_LATTICE` out). Consequence of
  reusing bit 5: a persisted key from an editor older than the PCF retirement that still has
  bit 5 set now normalizes to a lattice variant instead of dropping the bit (one extra
  exported variant at worst).
- **Vertex colour in every pass of a lattice variant.** `SubmeshShaderInputs::lattice`
  (added) forces the mesh's vertex colour bits into the colour, depth and G-buffer variants of
  a lattice mesh, whatever the shader's `@require`, so changing a binding's mask never
  reloads anything and shadows/depth follow the mask. The built-in depth and G-buffer loads
  bind `COLOR` (a no-op for shaders that do not declare `a_color`). `water_depth`,
  `spin_bob_depth` and `dissolve_depth` keep the colour defines under `HAS_LATTICE`. A mesh
  without vertex colours is not masked (mask 1 for every channel, inverted ones too), as the
  CPU solver does with a null colour.
- **Variant switch.** `LatticeRender::prepare` sets `MeshComponent::needVariantReload` when
  `count > 0` differs from `variantActive` (set by `loadMesh`): the loaded variant keeps
  drawing (with count 0 in its block when the bindings went) until the new one is built, no
  invisible frames while the editor compiles it. A mesh whose lattice variant is missing
  from a build without runtime compilation behaves as any other missing variant (it does not
  draw); the variant collector adds the lattice variants of authored targets (below), so
  only bindings made by scripts at runtime rely on runtime compilation.
- **GLSL** (`includes/lattice.glsl`): `latticeDeform(stage, pos, normal, tangent, stretch)`
  as specified, plus `latticeDeformSmooth` (also moves the smoothed normal of
  `includes/surface_vs.glsl`, so outlines follow) and `latticeWriteStretch`. One Jacobian per
  lattice and vertex, taken to object space (`A^-1 J A`): position `p + A^-1 delta`
  (`= Minv (M p + delta)`), normal by the cofactor of that Jacobian (`= A^T cof(J) A^-T`, the
  spec's), tangent by it, stretch its determinant. A binding whose field is zero at the
  vertex leaves it bit-identical (as the solver's `deformVertex`), and bindings at rest or
  with weight 0 are left out of the draw by the CPU (`LatticeMeshState::movingBindings`), so
  a lattice at rest draws exactly what no lattice draws (tested pixel for pixel). Loops
  have constant bounds with `break`; data through `texelFetch` on an unfilterable RGBA32F
  texture with a non-filtering sampler (the bones texture's annotations) on every backend,
  the block is floats only. The record constants come from `LatticeTypes.h` as compiler
  defines (`LATTICE_MAX_PER_MESH`, `LATTICE_TEXTURE_WIDTH`, `LATTICE_RECORD_HEADER`, in
  the library hash). `u_vs_lattice.z` is 1 in the colour passes (stretch computed), 0 in
  depth and G-buffer.
- **Pipeline order** in every vertex path: morph targets, stage 0 bindings (only under
  `HAS_SKINNING`; without skinning every binding is stage 1), skinning, instance matrix,
  instance fade, stage 1 bindings, then (surface shaders) the `SURFACE_DISPLACE` hooks,
  which see the deformed position and normals, then the model matrix.
- **Stretch** (deviation): the `v_latticeStretch` varying exists only when both stages
  define `LATTICE_STRETCH` (an output the fragment stage does not read fails the
  cross-compile, so it cannot be written by every `mesh.vert`/`surface_vs.glsl` variant).
  The fragment side defines `LATTICE_FS` (and `LATTICE_STRETCH`) and includes
  `includes/lattice.glsl` for `latticeStretch()` (1.0 without a lattice or without the
  define). `library/lattice_stretch` (.vert + .frag, category Effects): log-scale heat map,
  `squashColor`, `stretchColor`, `range` (volume ratio of the full colour, 1.05..16, default
  2), `baseColor` and `u_baseTexture` tint; folded cells show the squash colour.
- **Vertex paths audited** (all call `latticeDeform`/`latticeDeformSmooth`): `mesh.vert`,
  `depth.vert`, `gbuffer.vert`, `includes/surface_vs.glsl` (every library mesh shader's
  colour pass: `surface`, `unlit`, `simple_lit`, `vertex_lit`, `toon`, `matcap`,
  `triplanar`, `panner`, `decal`, `projected_decal`, `foliage`, `flipbook`, `dissolve`,
  `spin_bob`, `water`, `fake_lit`, `sprite`, `outline` and `outline_stencil` through
  `library/outline_vs.glsl`, `lattice_stretch`), `includes/surface_depth_vs.glsl` (every
  depth program: `surface_depth`, `unlit_depth`, `foliage_depth`, `flipbook_depth`,
  `dissolve_depth`, `spin_bob_depth`, `water_depth`). Not mesh paths (no lattice):
  `sky_vs`, `ui_vs`, `points_vs` (`particle`), `htmlui*`, `lines`, `shadow2d`, `fullscreen`,
  `bloom`, `uber`, `effects/*`. Forks made before this keep compiling and draw undeformed
  (their block and texture slots are -1); `mesh.vert`'s header and
  `docs/mcp/guide/shaders.md` (Forking) show the call.
- **Packing** (`LatticeRender::prepare`): one walk down the Transform array from the first
  modifier (skipping the parts no `includeChildren` branch covers), the path of ancestors
  giving the inherited bindings nearest first after the own ones. A lattice is validated and
  padded once a frame; skipped when deleted, without `LatticeComponent`, disabled, inactive
  in the hierarchy, with a singular or non-finite transform, or when the mesh's transform is
  singular. Resolutions are clamped and short offset lists read as zero, as the solver does
  (texel 8 holds the clamped resolution); a NaN fade distance is packed as 0. More than
  `LATTICE_MAX_PER_MESH` valid bindings: the rest are ignored, `Log::warn` once per modifier
  (`warnedOverflow`, cleared when it fits again). Texels are built in a scratch vector,
  compared with the mesh's (`memcmp`) and swapped in only when they differ (`needUpload`).
  Terrains are never targets. `wantsTessellation` is true for every target with `count > 0`
  (MeshSystem only reads it for sprites), `tessellation` the first binding's modifier's
  `subdivisions` clamped to 1..64. Meshes filled last frame and not this one are reset.
  With no `LatticeModifierComponent` and nothing filled last frame it returns at once; in a
  steady frame it allocates nothing (scratch kept in `LatticeRenderState`).
- **Bounds**: `worldPadding = sum |weight| * maxPaddedOffset * s`, `s` the lattice model's
  longest column (or the root sum of squares of the columns for a skewed matrix, a bound of
  the spectral norm); a before-skinning binding of a skinned mesh is also scaled by its
  largest bone scale. Added to `worldAABB`, to tilemap chunk bounds and to the per-instance
  culling radius (`buildInstanceView`). A padding change marks the AABB dirty, which for an
  instanced mesh also rebuilds its instance list (a jiggling lattice on a large instanced
  mesh does that every frame).
- **Texture**: `TextureRender::createDynamicTexture` (RGBA32F stream), `LATTICE_TEXTURE_WIDTH`
  wide, as many rows as the texels need (made again when that changes), uploaded by
  `updateMeshBuffers` once a frame at most and bound at each draw (so a remade texture needs
  no reload). Destroyed by `destroyMesh` and by a load without the lattice variant.

Contract additions: `LatticeMeshState::movingBindings`; `LatticeRenderState` and
`LatticeRender::prepare(Scene*, LatticeRenderState&)`, `getUniformBlock`,
`isLatticeTarget` (LatticeRender.h); `RenderSystem::getLatticeState()` (RenderSystem owns
the state); `SubmeshShaderInputs::lattice`; `UniformBlockType::VS_LATTICE`,
`DEPTH_VS_LATTICE`, `GBUFFER_VS_LATTICE`, `TextureShaderType::LATTICE`; `Submesh`
`slotVSLattice`/`slotVSLatticeTexture`, `slotVSDepthLattice`/`...Texture`,
`slotVSGBufferLattice`/`...Texture`, the same pair on `SubmeshExtraPass`.

Variant collection: `ShaderVariantCollector` adds, for every mesh that
`LatticeRender::isLatticeTarget` (own or inherited binding to a lattice entity, enabled or
not), the lattice variants next to the unbound ones; `Project.cpp`'s loaded-mesh list derives
the depth key with the lattice bit; `ShaderLibraryMatrix` gains four lattice cases (plain;
skinning + morph + vertex colour; instancing + fade + vec3 colour + shadows; no normals/UV
with a sprite rect).

Files: `engine/core/lattice/LatticeRender.{h,cpp}`, `lattice/LatticeTypes.h` (one field),
`component/MeshComponent.h` (slots), `render/Render.h`, `shader/ShaderData.cpp`,
`shader/ShaderVariantCompiler.cpp`, `pool/ShaderPool.{h,cpp}`,
`subsystem/RenderSystem.{h,cpp}`, `registry/EntityCloner.cpp` (one line: a clone's
`LatticeMeshState` is reset, the texture handle is the original's; WP-ECS owns the file),
`engine/shaderlib/{mesh,depth,gbuffer}.vert`, `includes/{lattice,surface_vs,surface_depth_vs}.glsl`,
`library/lattice_stretch.{vert,frag}`, `library/{water,spin_bob,dissolve}_depth.vert`,
`editor/shader/{ShaderVariantCollector.{h,cpp},ShaderLibraryMatrix.cpp}`, `editor/Project.cpp`,
`docs/mcp/guide/shaders.md`, `docs/shaders/library.md` (regenerated),
`engine/tests/htmlui/TestLatticeRender.cpp` (+ goldens `golden/lattice_*.png`,
`fixtures/lattice/skinned_column.gltf`, a copy of the shader-library fixture's).

Limits: a lattice variant of a manifest shader that did not read the vertex colour now
declares it, so a mesh already at the 16 vertex attributes (skinning + 8 morph attributes +
instancing + colour) can exceed the limit in its lattice variant only; the non-lattice
variant of such a mesh already fails today with morph normals. Physics colliders and
triangle picking keep the rest shape (§1.1).

Verification (all on this host, GL 4.1 core through Mesa llvmpipe under Xvfb):
- `jasperengine-editor shaders --out ... --backend all` with 14 built-in lattice specs
  (`mesh`, `depth`, `gbuffer` with skinning, morphs, instancing and fade, vertex colours,
  shadows, vertex lights, sprite rect, 2D lights, terrain): 84 compiles, 0 failed (GLSL 4.10,
  GLSL ES 3.00, HLSL 5, MSL macOS/iOS, SPIR-V). This caught `active` (a GLSL reserved word)
  and a dynamic index into the block GL flattens (`latticeStarts[b]`): both fixed.
- `shaders --library all --backend all`: 41 library shaders, 16230 compiles, OK (the matrix
  includes the four lattice cases). `docs/shaders/library.md` regenerated.
- `jasperengine-htmlui-tests --gtest_filter=LatticeRender*` (standalone engine build,
  `cmake -S engine -B build/htmlui-tests -DJASPERENGINE_HTMLUI_TESTS=ON`): 6 tests: a lattice
  at rest draws pixel-identical to no lattice and unbinding restores the variant and image;
  sheared silhouettes match `lattice::deformWorldPoint` (sharp, smooth, cubic) within 1.5 px;
  the tilted front face's shading equals a box rotated to the cofactor normal; the shadow on
  a wall shifts by the deformation, a RED mask (white vertices) equals no mask and
  INVERTED_RED equals no lattice in colour and shadow, and the depth pre-pass image is
  identical to the one without it; a skinned column deformed before skinning (lattice at
  the bind pose) and after (at the posed column), and not when the after-skinning lattice
  stands at the bind pose; two instances, only the one inside the lattice deformed. A
  mutation (no lattice call in `depth.vert`) fails the shadow and pre-pass checks. The whole
  htmlui suite without the perf tests: 3232 passed.
- MCP (merged with lat/editor-data): `test_lattice_data.py` 7/7; shader library tests as in
  the baseline; a throwaway script binding a cube and a sphere (with `library:lattice_stretch`)
  through `add_component`/`set_properties` showed the deformation and the heat map, and
  `get_build_info include_shaders` listed `Mesh (..., Ltc, ...)` and `Depth (Vc4, Ltc)` for a
  cube inheriting a binding from its parent.

Integration notes: `RenderSystem::update` calls `scene->getSystem<LatticeSystem>()->lateUpdate()`
then `LatticeRender::prepare(scene, latticeState)` right after the skin-binding bone loop.
WP-ECS's MeshSystem reads `LatticeMeshState::wantsTessellation`/`tessellation` (set for every
target, read for sprites). `EntityCloner.cpp` gets one line from this package. Followers,
bake and sampling use the solver directly; nothing else needs to call LatticeRender.

### 12.x WP-ECS (LatticeSystem, Lattice object, EntityCloner, sprite grid, Lua)

Files: `engine/core/subsystem/LatticeSystem.{h,cpp}`, `engine/core/object/Lattice.{h,cpp}`,
`engine/core/registry/EntityCloner.cpp` (lattice parts), `engine/core/subsystem/MeshSystem.{h,cpp}`
(sprite grid), `engine/core/script/binding/{GameObjectClassesLua,ECSClassesLua}.cpp`,
`generate_api_suggestions.py` (MANUAL_SYMBOLS for the lambda/overload bindings),
`docs/mcp/guide/lua-api.md` (§4.19 Lattice, the enums table, the class tree),
`editor/Project.cpp` (one line, below), `tools/mcp/tests/test_lattice_lua.py`. No stub fragment
was needed: `generate_lua_stubs.py` resolves every new binding (0 unresolved).

Frame flow and play mode:
- `update(dt)` advances pose tweens and accumulates the play time; `lateUpdate()` (called by
  RenderSystem::update, WP-RENDER) steps the jiggle with that time, then runs followers. The
  jiggle is in `lateUpdate` (not `update`) so it reads the lattice's world position of this
  frame (no lag, no stale identity matrix on the first frame) and followers ride on the
  jiggle in the same frame. Without WP-RENDER's call neither jiggle nor followers run.
- Edit mode: `editor::Project::pauseEngineScene` now also pauses `LatticeSystem` (one line,
  next to Physics/Action/Audio). Paused = no tweens, no jiggle; followers still run (they
  only place transforms) and offsets/poses written by the API show at once. The game's Pause
  freezes tweens and jiggle where they are.
- Disabled (`enabled = false`) or inactive lattices: tweens and the jiggle state are kept
  but not advanced; a disabled jiggle (or lattice) clears `dynamicOffsets`.

Tweens: any direct write of offsets (setOffset..., setPointPosition..., resetOffsets,
applyPose, applyPreset) stops a running tween. `EaseType::CUSTOM` tweens linearly (the
component cannot hold a function). A tween whose from/to size no longer matches the
resolution (changed without `LatticeSystem::setResolution`) is stopped.

Poses and resolution: `LatticeSystem::setResolution(component, rx, ry, rz)` resamples the
offsets, every pose and a running tween with `lattice::resample`, and resets the jiggle.
**WP-EDITOR-SCENE/WP-MCP: `LatticeEditorOps::setResolution` should apply this helper to a copy
of the component (then commit it as the command's new value), so poses stay in step.** A
pose whose size does not match the resolution (data from a path that did not resample it)
is resampled on use from the resolution its size factors into, the factorization closest to
the current resolution (ties: x before y before z); a size no resolution has reads as zeros.

Jiggle (spec §6.2) details: acceleration from finite differences of the lattice's world
position in `lateUpdate`; frames 1 and 2 after enabling, `resetJiggle()`, frames the lattice
spent inactive or the system paused (the springs are kept, only the history restarts), a frame longer
than 0.25 s or a jump of more than 2 lattice sizes (largest world axis length) in one frame
only rebuild the history (teleport). Substeps of at most `min(1/120 s, 1/sqrt(k))`, at most
64 per frame; a frame longer than 0.25 s is integrated as 0.25 s. Clamp keeps
`|x| <= max(0, jiggleMaxOffset)` and removes the outward velocity at the limit. While asleep
it wakes when `inertia * |a_local| > 1e-4 * max(k, 1)` or on a poke; `poke`/`pokeAt` do
nothing while `jiggle` is off. Anchor weight is linear across the lattice (0 at the anchored
face, 1 at the opposite). `revision` is bumped every simulated frame and when it sleeps.

Followers (spec §6.3) details: processed in Transform array order (a follower inside another
follower's subtree sees it moved). Rotation: the rest's world axes pushed through the world
Jacobian, Gram-Schmidt x then y, z = x cross y (a degenerate basis keeps the rest rotation).
Scale: rest world scale times the lengths of the deformed rest axes. Channels whose apply
flag is off are not written (the follower keeps its own value). The follower and its subtree
get `needUpdate` and `RenderSystem::updateTransform` (already public) in the same frame; when
nothing changed nothing is written. Left alone: follower disabled/inactive, missing lattice
or rest, a zero-scale lattice transform, the follower being its rest or an ancestor of it,
and (addition) the lattice inside the follower's subtree, which would feed back every frame.
A disabled or inactive lattice puts the follower at its (undeformed) rest. Followers write
Transform values in edit mode too: a saved scene stores the placed values, recomputed
identically on load; moving the follower by hand is undone the next frame (move the rest).
Integrator: check that prefab override detection does not record these writes as overrides
(they bypass the command system the same way a script would).

Sprite grid (spec §6.4): `MeshSystem::createSprite` builds `n x n` cells
(`n = clamp(lattice.tessellation, 1, 64)` while `lattice.wantsTessellation`), vertices
row-major from the bottom row, corners computed as `(1 - t) * a + t * b` so they equal the
quad's values exactly (same AABB), same UV cut ratios, flipY, pivot, normals, tangents,
colours, winding. Without the flag the quad code path is unchanged (verified byte-identical
vertex and index data, and pixel-identical rendering of an undeformed 8x8 grid vs the quad).
`createOrUpdateSprite` compares the grid the mesh was built with (from its vertex count:
4 = quad, `(n+1)^2` = grid) with the request and rebuilds; a loaded mesh whose grid size
changes (quad <-> grid, n -> m) gets `needReload` (its GPU buffers keep their size), the same
size only `needUpdateBuffer`. Indices stay 16-bit (65 x 65 vertices at most).

EntityCloner: copies the four components. `LatticeModifierComponent::bindings[].lattice` and
`LatticeFollowerComponent::lattice/rest` are remapped (a copied lattice -> its copy; one not
copied stays in the same registry and becomes `NULL_ENTITY` in another; the binding is kept,
skipped at runtime like a deleted lattice). The copy's jiggle state is cleared (asleep,
warm-up again where it is placed); a running tween goes on. `prepare<MeshComponent>` resets
`MeshComponent::lattice` (the copy packs and uploads its own texture; sharing the original's
`TextureRender` would double-destroy it) but keeps `wantsTessellation`/`tessellation`, which
the copied vertices were built for.

Contract additions (all in WP-ECS files):
- `LatticeSystem`: static component helpers `getResolution`, `setResolution`, `findPose`,
  `getPoseOffsets`, `savePose`, `applyPose`, `startTween`, `stopTween`, `resetJiggle`, `poke`,
  `pokeAt`; members `deformWorldPoint(Entity, point, weight)`, `isValidFollower(follower,
  lattice, rest)` (the inspector/MCP refusal rule, shared).
- `Lattice`: the §8 API plus `getLatticeComponent`, `setEnabled/isEnabled`,
  `getResolutionX/Y/Z`, `getPointIndex`, `getRestPosition`, getters for every setter,
  `hasPose`, `removePose`, `stopTween`, `isJiggleSleeping`, `getJiggleOffset`, `resetJiggle`,
  `addFollower(follower, rest[, applyPosition, applyRotation, applyScale, weight])`,
  `removeFollower`, `getModifierComponent(entity)`, `getFollowerComponent(entity)`;
  `bind`/`unbind`/`addFollower`/`removeFollower` take an entity or any GameObject wrapper.
  `getResolution()` returns a Vector3 of whole numbers. Indices are 0-based (as
  `lattice::copyAsLua`). `bind` adds the component with `includeChildren = true` when the
  target has no MeshComponent of its own, clamps the weight to 0..2, updates the weight of
  an existing binding to the same lattice, and refuses the lattice itself.
- `MeshSystem::createSpriteGrid` (private).
- Lua: enums `LatticeInterpolation`, `LatticeOutside`, `LatticeJiggleAnchor`, `LatticeStage`,
  `LatticeMaskChannel`; classes `LatticePose`, `LatticeComponent` (resolution and the vectors
  read-only, setting enabled/interpolation/outside/fadeDistance bumps `revision`),
  `LatticeBinding`, `LatticeModifierComponent` (bindings: read a copy, assign it back),
  `LatticeFollowerComponent`, `LatticeTracksComponent` (WP-ANIM may add its action class).

Verification: `jasperengine-editor` builds with no new warnings in these files (also checked
with `-Wall -Wextra`). `tools/mcp/tests/test_lattice_lua.py` (3 tests, real editor, needs
WP-RENDER's `lateUpdate` call): a script creates and binds a lattice, offsets by index/flat
index, local/world point positions, poses, resampling, follower placement equal to
`deformPoint`, a tween ending exactly on the pose, a poked jiggle that settles and sleeps
with no dynamic offsets left, an acceleration that wakes it, back at rest; edit mode
(applyPose immediate, tween waits, follower follows); `GameObject.instantiate` remaps. The
offline stub check (every Lua symbol of `engine_api_suggestions.h` has a stub) passes; the
editor-side `test_stubs.py`/`test_docs.py` failures on this host match the baseline.
Timing note (test_lattice_lua mid-tween check): a script's `onUpdate` (Engine event) runs
before the scene's systems in the same frame, and the first frame after Play starts has its
delta clamped to 0.25 s. The first version of the test (06be36e9e, still in `lat/anim`)
sampled the offset on the first `onUpdate` with 0.2 s elapsed and saw 0, since the tween had
not run that frame. Fixed in ecc5bc286: it records the first value strictly between the rest
and the pose while tweening (a 1 s tween spans at least four updates). LatticeSystem was not
at fault: a frame trace shows the offset advancing every update (0, 0.109, 0.181, ... 0.25).
Branch `lat/ecs` merged `lattice` (WP-SOLVER) and `lat/render` (for the `lateUpdate` call
and the tessellation request); with them the three tests pass and a 2D sprite bound to a
lattice renders as a smoothly warped 8 x 8 grid.

### 12.x WP-ANIM (LatticeTracks, §7)

Files: `engine/core/action/keyframe/LatticeTracks.{h,cpp}` (stub replaced),
`engine/core/subsystem/ActionSystem.{h,cpp}`, `engine/core/script/binding/ActionClassesLua.cpp`,
`engine/core/script/LuaBinding.cpp` (`pushEntityHandleByType` key `latticetracks`),
`engine/core/JasperEngine.h` (include), `engine/tests/htmlui/TestWrapperOwnership.cpp` (LatticeTracks
in the wrapper list), `editor/util/LatticeTrackKeys.{h,cpp}` (new, root `CMakeLists.txt`),
`editor/window/AnimationWindow.{h,cpp}`, `editor/command/type/CreateTrackKeyCmd.{h,cpp}`,
`editor/command/type/CreateEntityCmd.{h,cpp}`, `editor/window/HierarchyWindow.cpp` (Create >
Animation > Lattice Track), `editor/util/ProjectUtils.cpp` (script references to a lattice track
are `LatticeTracks`), `editor/mcp/tools/WorldTools.cpp` (`edit_animation` only),
`docs/mcp/guide/lua-api.md`, `tools/mcp/tests/test_lattice_anim.py`.

Added declarations (nothing in a contract header renamed): the `LatticeTracks` API (the contract
header was empty): constructors `(Scene*)`, `(Scene*, Entity)`, `(Scene*, times, points, values)`,
`setTimes`, `setPoints`, `setValues` (keeps tangents mirrored like MorphTracks), `setEasings`,
`setEasing`; private `ActionSystem::LatticeBlendAccum`, `latticeBlend`, `latticeTracksUpdate`;
`EntityCreationType::LATTICE_TRACKS` (after `MORPH_TRACKS`); a lattice constructor of
`CreateTrackKeyCmd`; private `AnimationWindow::selectLatticeTrackPoints`; `editor::LatticeTrackData`
and `editor::LatticeTrackKeys` (the keying rules, shared by the Animation window and MCP).

Runtime:
- Dispatch: an action with KeyframeTracks + LatticeTracks whose target has a LatticeComponent.
  Per point column: lerp with the segment easing, Hermite when `inTangents`/`outTangents` have one
  row per key and the segment's two rows are sized like its value rows (else linear), as morph
  tracks. A segment animates the columns both of its keys have (`min(points, row sizes)`): a
  hand-edited short row animates fewer points instead of failing the track (morph logs an error and
  skips it). Points `< 0` or `>= rx*ry*rz` (clamped resolution) are skipped.
- `LatticeBlendAccum` per target: weighted sums and weights per point, sized to the point count
  when first touched in a frame, kept between frames (`active` flag, no reallocation; entries of
  destroyed entities are dropped in `clearPoseBlend`). Flush: points with weight > 0 get
  `sum / weight` (normalized like morph weights: one clip fading in or out alone is not mixed with
  the authored offsets); other points keep their values. `offsets` is grown to the point count
  when shorter; `revision` is bumped only when a value changed. The editor preview uses the same
  path (`updateAnimationPreview`).
- A LatticeSystem tween/pose writing the same lattice while a LatticeTracks plays: the later
  writer of the frame wins (the systems are not blended).
- Engine fix found by the play test (all keyframe tracks, not only lattice): a non-looping
  Animation that ended in an update stopped its tracks before the all-actions pass sampled them
  at the end, so the last keys were missed by up to a frame (0.385 instead of 0.4 at the ~8 fps of
  the headless test). `ActionSystem::animationApplyEnd` (private, added) samples every running
  keyframe child once at the clip's end (at the clip time, as every other update samples it, the
  animation duration when that cut it) before the clip stops; the pose blend of that update writes
  the final values.
- Integration fix: `pushEntityHandleByType` had no `Lattice` entry (ProjectUtils names a lattice
  entity's class `Lattice`, so `eval_lua object(ref, "Lattice")` and typed script references fell
  back to EntityHandle); added `DISPATCH_TYPE(Lattice, "lattice")` in `LuaBinding.cpp`.

Editor keying (`LatticeTrackKeys`):
- Recorded points: the Scene-view selection (`LatticeEditorOps::getSelectedPoints`) when points
  are selected, else every non-zero point plus every point already in the track, at the current
  resolution. Extension: a lattice at rest whose track has no point (or no track) keys **every**
  point, so the rest pose can be keyed (the spec rule would record nothing and a later key would
  then have no rest to come from).
- A key within 1 ms of an existing one updates the recorded columns only. A new key is inserted in
  time order; the columns it does not record get the track's own value at that time (sampled as
  ActionSystem plays it: easing, tangents), so the rest of the animation is unchanged; the split
  segment keeps its easing on both halves; tangent tracks get zero tangents for it.
- Points new to the track become columns; the keys that exist get the lattice's **current offset**
  of those points (the only value known for them), tangents zero. Hand-edited rows shorter than the
  point list are completed the same way.
- Animation window: lattice blocks are labelled "Lattice Tracks" (rose); right-click a lattice
  block: Insert Lattice Key; right-click the empty area with a lattice target: Key Lattice (creates
  the track with CreateTrackKeyCmd when none spans the time); Snapshot keys lattice tracks too; Add
  Property > Lattice Tracks; the preview snapshot excludes LatticeTracksComponent (the target's
  LatticeComponent is snapshotted and restored, so a preview never leaves animated offsets behind).
  Selecting a lattice block (or one of its keys) selects its lattice as the GameObject and the
  track's points (valid at the current resolution) in the Scene view.

MCP `edit_animation` channel `lattice`: per key `points` (or `value`) as `{"<index>": [x,y,z]}`,
`[[[i,j,k],[x,y,z]], ...]` / `[[index, [x,y,z]], ...]`, or `"current"` (the lattice's offsets
chosen by the keying rules above, the MCP path of editor keying). Indices are validated against
the target's resolution (error names the key, the point and the resolution). Replace mode: one
column per point any key names (ascending); a key that leaves a point out gets it linearly
interpolated in time between the nearest keys naming it (held before the first / after the last).
Merge mode: each key goes through `LatticeTrackKeys::setKey` (same as editor keying), its `ease`
set for its segment. MCP writes linear keys (tangents cleared), as the other channels.
`get_animation`/results read lattice keys back as `points: {"<index>": [x,y,z]}`. The keys
schema now requires only `time` (lattice keys have `points`). Description trimmed to keep the tool
total within the budget.

Tests: `tools/mcp/tests/test_lattice_anim.py` (7 tests, new project): the MCP channel (both point
forms, read back, one undo step, redo), fill/merge/errors, `"current"` keying (rest pose, moved
points, new point at its current offset in earlier keys), the Animation window preview
(`preview_animation` seek: offsets within one 1/60 s step of the key line, the unnamed point kept,
the Scene view screenshot differs from the rest frame because the GPU path deforms the bound box,
stop restores the authored offsets and leaves the keys), play mode (offsets on the key line at most one
frame behind the action time at every sample, end exactly on the last key, untouched points kept,
stop reverts), Lua (`LatticeTracks` by constructor and by setters, out-of-range point ignored, two
clips on different points plus a shared point crossfaded with fadeOut/fadeIn: the shared point is
the fade-weighted mean, the others keep the value of the only clip naming them, also after it
stopped), a second editor reloading the scene (tracks, easings) and re-saving byte-identical.
`test_html_ui_tools.py -k budget`: 100628 of 100637 characters after the trims.

    JASPER_FIXTURE=<empty project> JASPER_EDITOR=build/native/jasperengine-editor \
        tools/mcp/run-tests.sh tools/mcp/tests/test_lattice_anim.py

Not verified headless (no MCP path for them): clicking a lattice block in the Animation window
(point selection in the Scene view), the right-click/Snapshot menu items, keying with a Scene-view
point selection (`LatticeEditorOps::getSelectedPoints` was still the contract stub in this branch;
WP-EDITOR-SCENE's implementation filters by lattice and works with the calls made here).

Not done here: `create_entity` type `lattice_tracks` (EntityTools, WP-MCP), `docs/mcp/TOOLS.md`
regeneration (WP-MCP), the Xcode project entry for `editor/util/LatticeTrackKeys.cpp` (integration).

### 12.x WP-EDITOR-SCENE (Scene-view editing, LatticeEditorOps)

Files: `editor/lattice/LatticeEditorOps.cpp` (stubs replaced), `editor/lattice/LatticeSceneRender.cpp`
(new: `SceneRender`'s lattice members, cages, picking, point gizmo drag),
`editor/lattice/LatticeSceneTools.{h,cpp}` (new: right-click menu, shortcuts),
`editor/command/type/LatticePointsCmd.{h,cpp}` and `LatticeBindingsCmd.{h,cpp}` (new),
`editor/render/SceneRender.{h,cpp}` (hooks in `update`, `mouseClickEvent`, `mouseDragEvent`,
`mouseReleaseEvent`, `hideAllGizmos`, destructor; `SceneDisplaySettings::showAllLattices`),
`editor/window/SceneWindow.{h,cpp}` (picking, rectangle, empty click, Esc/O/... shortcuts, F,
right-click menu, wheel, Gizmos toggle, `focusOnBounds`), `editor/AppSettings.{h,cpp}`
(`LatticeViewSettings`, YAML `lattice:`), `editor/window/dialog/EditorSettingsWindow.{h,cpp}`
(Preferences > Lattice page, `open(project, category)`), `editor/App.{h,cpp}` (`openPreferences`,
Ctrl+A in the Scene view selects points), `editor/Stream.cpp` (`showAllLattices` in the
workspace's per-scene view settings, written only when on), `CMakeLists.txt`,
`tools/mcp/tests/test_lattice_scene.py` (new).

Contract additions (all in `LatticeEditorOps.h`, under "additions to the contract"):
- `getEditedLattice(project, sceneId)`: the lattice the Scene view edits (the only selected
  entity, with LatticeComponent and Transform), else `NULL_ENTITY`.
- `validateSelection(sceneId, lattice, pointCount)`: SceneRender calls it every update; the
  stored point selection is dropped when another entity is edited (another selection, a
  deleted lattice) and indices past the point count are dropped (undo of a resolution change).
- `setOffsetsContinuous(...)` + `endContinuousEdit(sceneId)`: an edit made of many steps (the
  Scene-view drag; an inspector widget held, if WP-EDITOR-DATA wants live feedback) merges
  into the previous continuous edit of the same lattice until `endContinuousEdit` marks the
  history (`CommandHistory::mark`). Every other op is a discrete step that never merges
  (`setNoMerge` before `addCommand`), so two MCP/inspector edits in a row are two undo steps.
- `transformedOffsets(...)` (pure; what `transformPoints` and the drag both use) and
  `falloffWeight(falloff, distance)`.
- `getPointLocalPosition(lattice, index, position)`, `getSelectedOrAllPoints(...)`.
WP-RENDER (not me) added `bake`/`canBake` to the same header for WP-BAKE.

Commands:
- `LatticePointsCmd`: the only writer of lattice offsets in the editor. Offsets-only form
  (size must match the resolution when it runs) or whole-state form (resolution, offsets,
  poses: resample, pose save/rename/delete, creation's flat-axis resolution). It bumps
  `revision`, marks the scene modified, embeds `PrefabEditUndo`, and records prefab instance
  overrides: per changed point `offsets[i]` for offsets edits; for whole-state edits also
  `recordPrefabComponentEdit` with the component as it was. A whole-state apply clears
  `dynamicOffsets`, `jiggleVelocities` and `tweening` (laid out for the old points).
  `affectsStructure()` is false (no Hierarchy rebuild per drag frame).
- `LatticeBindingsCmd`: sets a target's `bindings` + `includeChildren`, adding the
  LatticeModifierComponent when missing (`ProjectUtils::addEntityComponent`,
  `recordPrefabComponentAdded`; undo removes it), else `recordPrefabComponentEdit`.

Ops behaviour (one undo step each; nothing changes on failure; an edit that changes nothing
pushes no step and returns true):
- Validation errors name the fix: point index range, offsets/points count, non-finite values,
  resolution 2..16, squash amount 0..0.95, bend/shear `toward` != `axis`, binding weight 0..2,
  submesh >= -1, enum ranges, "not a lattice (it has no Lattice component)", a broken layout
  (offsets size != rx*ry*rz) is refused with "set its resolution again".
- `transformPoints`/drag: `p' = pivot + T (p - pivot)` in lattice local space; falloff weight from
  the distance (local units, current positions) to the nearest moved point; weighted points
  get translation * w, slerp(identity, rotation, w), scale lerp(1, s, w) when `T` decomposes
  into TRS (checked by recomposing), else their moved position blended by w (a world rotation
  of a non-uniformly scaled lattice is a shear in local space). Curves: smooth
  `t*t*(3-2t)`, linear `t`, sharp `t*t`, constant 1, with `t = 1 - d/r`, 0 at and past r.
- `setResolution` resamples through `LatticeSystem::setResolution` on a copy (offsets and
  every pose, the runtime's rule for poses of another size), so the editor and
  `Lattice:setResolution` give identical results; it clears the point selection.
- `applyPose` uses `LatticeSystem::getPoseOffsets` (a pose of another size is resampled as at
  runtime), times the weight. `savePose` overwrites a pose of the same name; rename refuses
  an existing name.
- `computeFit`: meshes = targets and their descendants with a non-null local AABB
  (`SceneRender::getMeshLocalAABB`: skinnedAABB when set, the rest shape, never the
  lattice-grown worldAABB). One target: measured in its frame (world position/rotation,
  |world scale|; a zero scale falls back to world axes). Padding: size + max(size * padding,
  0.01) per axis. Flat axis (<= max(1e-3 * largest, 1e-6)): max(10% of largest, 0.01) and
  resolution 2.
- `fitTo` moves/rotates/scales the lattice (ObjectTransformCmd, parent space) and keeps its
  resolution and offsets (an edited cage keeps its points; only creation picks resolution 2
  for flat axes).
- `bind` replaces the settings of a binding already listing the lattice (no duplicates),
  `includeChildren` is set to the argument; refuses the lattice itself and entities without
  Transform. `unbind` removes the lattice's entries and keeps the (possibly empty) component.
- `createLattice`: `CreateEntityCmd(GAMEOBJECT)` with the fitted Transform +
  `AddComponentCmd(LatticeComponent)` + (flat axes) the resolution + one `LatticeBindingsCmd`
  per target with a mesh (own or below; `includeChildren` when the target has no mesh of its
  own, or already had it), grouped by `mark`/`groupSince`; the lattice becomes the selection.
  Targets without any mesh are not bound; when none has one the result is the no-selection
  cage at `fallbackPosition`. **Deviation**: that cage is 1 unit in 3D scenes and 100 units in
  2D/UI scenes (which measure in pixels: a 1-unit cage would be invisible).

Scene view (§9.2):
- Cages: each lattice drawn with two LineRenderers, one in the tools scene (always on top,
  `behindOpacity` in perspective views, full colour in 2D/orthographic views) and one
  depth-tested in the scene (`overlay`, full colour, perspective views only): parts behind
  geometry show faded. Edges stop at the point squares; squares are filled camera-facing
  squares (`pointSize` px, outlines only for unselected points of lattices above 512 points),
  nudged towards the camera so their own edges never cut them. Thickness = parallel 1-px
  copies across the edge on screen (engine lines are 1 px). Stretch colour: log2(length /
  rest length) clamped to [-1, 1] blends the line colour (rest) to the squash colour (half) or
  the stretch colour (double). Rest cage at 22% alpha when any offset is non-zero. Unselected
  lattices (Gizmos > "Show all lattices") at 35% alpha without points; the selected one shows
  whatever the other toggles say. Hidden with the other gizmos (camera preview).
- Gizmo on selected points: centroid (the editor has no pivot/center toggle, so the last
  selected point is stored but unused), lattice world rotation in Local mode, identity in
  Global (MainToolbar's X toggle); W/E/R and the 2D gizmo's move; same snapping as objects
  (grid on translation deltas, rotation step, Ctrl inverts rotation snap). The point gizmo is
  disabled while an edit layer (inline child scene / game layer) holds the tools, while
  terrain editing, and for a lattice with a non-invertible Transform.
- Picking: a click on a point selects it (Shift or Ctrl toggles); the points drawn on the same
  spot (a flat 2D cage seen face on has two layers) are taken together. The gizmo then sits
  on it, so the next press on its centre drags. With no point selected a point under the
  cursor wins over the lattice entity's own gizmo (its arrows cross the cage); with points
  selected their gizmo's handles win, as for the other sub-selections. Verified by hand: the
  wheel during a falloff drag shrinks the ring and the moved neighbours without zooming, F
  frames the selected point, E on all points rotates them about the centroid (y offsets stay
  0, radii kept, the axis column stays), R scales them. The uniform scale handle inherits the
  object tool's ratio (distance from the gizmo centre now / at the press), so a press right on
  the centre gives a very large factor, as it does for objects. A rectangle while a lattice is edited selects
  points (Shift adds) and keeps the lattice selected; a click on empty space with points
  selected clears the points and keeps the lattice (the next one deselects as usual);
  clicking another entity selects it (points dropped by `validateSelection`).
- Shortcuts (Scene view focused, a lattice edited; none collides): Ctrl+A select all (in
  `App::selectAll`, else the entities as before), Alt+A none, Ctrl+I invert, Ctrl+= /
  Ctrl+keypad+ expand, Ctrl+- / Ctrl+keypad- shrink, O proportional falloff, Esc clears the
  points (the editor has no Esc deselect for entities, so a second Esc does nothing, as
  before), F frames the selected points (else the cage: `focusOnEntities` frames a lattice by
  its cage bounds, also from the Hierarchy and MCP `frame_entity`).
- Mouse wheel during a point drag with the falloff on scales the radius by 1.1 per notch
  (0.01..10 local units) instead of zooming; the ring (a local-space circle facing the
  camera, so an ellipse for a non-uniform cage) is drawn around the selection's centroid.
- Right-click menu: a right click that turns the camera less than 4 px opens it where the
  click was (3D right-drag still flies). For the edited lattice, or the only lattice of a
  multi-selection (then point items are disabled): Select All/None/Invert/Exterior/Face
  (+X..-Z)/Expand/Shrink, Mirror (X/Y/Z, "+ to -" / "- to +"), Reset Selected, Reset All,
  Presets... (submenu: preset, axis, toward, amount, seed, replace, Apply to the selected
  points or all), Fit to Targets, Fit to Selection... (submenu: fits to the *other* selected
  entities, padding in %), Copy as Lua (`lattice:setOffset(...)` lines of the selected or all
  points to the clipboard), Proportional Falloff (O), Falloff (curve, radius), Lattice
  Preferences... (opens Preferences on the Lattice page).
- Preferences > Lattice (`settings.yaml` `lattice:`): line colour, thickness 1..4 px,
  behind-geometry opacity, point size 3..20 px, point colour, selected point colour, stretch
  colouring + squash/stretch colours, falloff curve, default radius (1.0 local units: with
  0.5 a 3x3x3 cage's neighbours, 0.5 apart, would not move). Applying new falloff defaults
  sets the live falloff.
- Play mode: the same rules as other component edits (commands go to the play session's
  history; the tools skip what the running scene created).

Verification: `tools/mcp/tests/test_lattice_scene.py` (8 tests, new empty project,
`xvfb="harness"`, real X input with xdotool, points found by their on-screen colour): Ctrl+A /
Alt+A / Esc, click + gizmo drag moves exactly one point, undo/redo restore it exactly, a
rectangle and Shift+rectangle, the falloff drag (neighbours move less, one undo step), the
right-click menu opens, a prefab instance's dragged point is the `offsets[i]` override and undo
drops it, a 2D view's click takes the three points stacked on one spot. Run with
`JASPER_FIXTURE=<empty project> JASPER_EDITOR=build/native/jasperengine-editor tools/mcp/run-tests.sh tools/mcp/tests/test_lattice_scene.py`.
Also driven by hand on Xvfb (screenshots): GameObject > 3D Object > Lattice around a box (fit
1.02 x 2.04 x 0.51, bound, one undo step, redo), corner drag bending the mesh (WP-RENDER
merged), falloff drag with the ring, every menu (Mirror, Presets, Reset All), the preferences
page, a 2D scene (sprite: 3x3x2 cage, the corner drag moves both layers). `test_lattice_data.py`
passes on the merge; `test_regressions`, `test_post_processing_views`/`_two_views`,
`test_scene_color_scale`, `test_scene`, `test_ui_anchors`, `test_project` show no new failure
against the baseline run on the empty fixture. Build: no warnings in the files above.

Integrator notes:
- WP-MCP's `edit_lattice` should call the ops and group a call with
  `CommandHistory::mark()`/`groupSince()`; `move` maps to `transformPoints` (its `pivot` in
  lattice local space); selection helpers + `setSelectedPoints` for nothing (MCP selectors are
  per call). `create` = `createLattice`; `fit` = `fitTo` (keeps the resolution).
- The inspector's Offset field could use `setOffsetsContinuous` while held and
  `endContinuousEdit` on release for live feedback; today it commits once on release, which is
  also correct.
- `LatticeEditorOps::falloff()` is initialised from the preferences on first use.
- SceneRender finds its project scene id by searching the project's scenes and prefab stages
  for itself (cached), so no Project.cpp change was needed.

### 12.x WP-BAKE

§9.4 as specified, with these decisions:

- **What a bake takes.** `bake(target)` / `canBake(target)`: the meshes of the target (its glTF
  Model's meshes: the Model itself on the flat path, every mesh part entity on the node path; a
  mesh part alone when the target is one), plus, when the target's Lattice Modifier has Include
  Children, every mesh below it. Each glTF Model among them gets its own file (`path` returns
  them joined by ", "). Meshes that are not glTF models (sprites, primitives, polygons,
  tilemaps, terrains, meshes made by tools, OBJ models, Models without a file or still loading)
  cannot be baked; when nothing else is left the reason names what the target is ("'Box' is a
  generated mesh: only glTF models (.gltf, .glb) can be baked", "an OBJ model ... export it as
  glTF first", "a sprite", ...). Refused while the scene plays.
- **Same bindings as the GPU.** The bindings are resolved per mesh exactly as
  `LatticeRender::prepare` packs them (lat/render exposes no per-entity resolver, so
  `LatticeBake.cpp` has a `Resolver` mirroring it): own bindings when the modifier is enabled,
  then every enabled ancestor modifier with Include Children nearest first, lattices skipped
  when deleted / without a component / disabled / inactive in the hierarchy / with a singular or
  non-finite transform / a padded grid that does not fit / non-finite offsets, non-finite weights
  skipped, a mesh with a singular transform skipped, at most `LATTICE_MAX_PER_MESH`, terrains
  never. `M = objectToNormalized(lattice, mesh)` and `Minv = M^-1` as packed. Per submesh: the
  Submesh filter (engine submesh index; the loader stores a node's primitives reversed, the
  bake maps them back), the mask only when the submesh has vertex colours (COLOR_0 VEC4, or VEC3
  FLOAT/UBYTE/USHORT, as the loader binds them; otherwise mask 1), the weight; bindings at rest
  or with weight 0 (`movingBindings` off) are not baked. Vertices go through
  `lattice::deformVertex` in order; a vertex no binding moves keeps its file values bit for bit.
- **Which bindings bake.** A binding is baked (and removed) only when everything it deforms is
  baked with it, in the order the GPU applies it: (1) every mesh it deforms is in the bake (a
  binding that also deforms another mesh stays: "it also deforms 'Prop', ..."); (2) on a
  skinned submesh (JOINTS_0 or WEIGHTS_0 loaded) only Before Skinning bindings bake, into the
  bind pose; After Skinning ones stay ("set its Stage to Before Skinning..."); (3) without
  skinning every binding deforms after the instance matrix, so nothing bakes into an instanced
  mesh (an `InstancedMeshComponent` on it); (4) lattices do not commute, so a submesh bakes the
  bindings before the first one that stays (skinned: in the Before Skinning sequence, which runs
  before the rest); this is iterated to a fixed point over all submeshes. Bindings that stay are
  listed in `note` ("Kept binding 2 of 'Hero' (lattice 'Feet'): ..."), also those at rest,
  invalid or past the limit; `note` also says when a mesh had more than 4 lattices (the ignored
  ones deform it once the baked ones are gone) and when morph weights are not 0.
- **Morph targets** (applied before the lattices) become displacements of the baked shape:
  `deform(base + delta) - deform(base)` per target (position, normal, tangent), exact at weight
  0 and 1 of a single target, close in between. A delta whose vertex no binding moves keeps its
  file value; a sparse target without a base stays sparse with the same indices.
- **The file** (`LatticeBakeGltf.{h,cpp}`, the glTF is read as JSON + buffers, not through
  tinygltf, so everything not touched is kept as written: key order, extensions, extras,
  materials, textures, samplers, skins, animations, morph targets, scenes). `<stem>_lattice.glb`
  next to the original, never an existing file (`_2`, `_3`...; baking `x_lattice[_n].glb` counts
  on from `x`). One binary buffer: the old buffers one after the other (16-byte aligned, external
  `.bin` files and data URIs embedded), new data after them; bufferViews moved with them. Images
  keep their URIs (valid: same directory) or bufferViews. New vertex data per node: a mesh used
  by several nodes that get different data is copied for the nodes with new data (nodes that keep
  their data keep the mesh index); an accessor is written in place when it is FLOAT, dense, every
  primitive using it gets the same data, nothing else uses it (indices, skins, animations,
  EXT_mesh_gpu_instancing) and no other accessor, sparse part or image shares its bytes
  (interleaved attributes in one bufferView do not count); otherwise the primitives get new FLOAT
  accessors (one per distinct result), the old one stays for its other users. POSITION
  accessors get exact `min`/`max`, also in place. Normalized/quantized (KHR_mesh_quantization)
  and sparse inputs are read as glTF defines them and written as FLOAT, which is exact; refused:
  KHR_draco_mesh_compression / EXT_meshopt_compression / KHR_meshopt_compression ("export the
  model uncompressed"), a file that changed on disk since the Model loaded it ("load the model
  again"), Merge Static Meshes models whose vertex data is not FLOAT (the merge only transforms
  FLOAT data), merged nodes scaled to zero. Merge Static Meshes models are baked in the space the
  merge puts the vertices in (the node transforms, computed as `MeshSystem` does) and written
  back in node space (the load applies the transforms again); their morph deltas are not
  transformed by the merge, nor by the bake.
- **The scene** (`command/type/LatticeBakeCmd.{h,cpp}`, one undo step): the baked bindings leave
  their modifiers (`LatticeBindingsCmd` each, the component stays) and every baked Model loads
  the new file in place with `MeshSystem::loadGLTF(..., skipEntities)` as a reopened scene does:
  the node, mesh part, bone and animation entities are kept with their ids and components
  (Lattice Modifiers on parts, submesh edits), because the file has the same nodes. A Prefab
  instance records a `filename` override of its Model. Undo loads the original file back and
  restores the bindings; the baked file stays (redo loads it again). A failure to load the new
  file rolls everything back and deletes the files that bake wrote. The Project window refreshes
  its directory. `path` is the file as the Model stores it (relative to the assets directory,
  `ModelComponent::filename`'s convention).
- **Inspector**: a "Bake" section at the end of the Lattice Modifier (single selection): "Bake
  Deformation", disabled with `canBake`'s reason (or "Stop the scene to bake") as tooltip; the
  result goes to the Console (the path, each note as a warning, or the error).
- Known engine limitations met (not WP-BAKE's files): the loader ignores the sparse part of a
  base attribute (POSITION/NORMAL/...; only morph targets are materialized), so the live render
  of such a model shows the base values while the bake writes the glTF-correct ones; sokol has no
  3-component SHORT/BYTE vertex format, so a KHR_mesh_quantization model with VEC3 positions does
  not render before the bake (it does after: FLOAT).

Contract additions: none in the headers. New: `editor/lattice/LatticeBakeGltf.{h,cpp}`,
`editor/command/type/LatticeBakeCmd.{h,cpp}` (CMake editor list), the editor's include path gains
`engine/libs/tinygltf` (the bake reads the loaded `tinygltf::Model` for the layout the meshes were
built from). Xcode project entries for the new files are the integrator's.

Files: `editor/lattice/LatticeBake.cpp` (stub replaced), `editor/lattice/LatticeBakeGltf.{h,cpp}`,
`editor/command/type/LatticeBakeCmd.{h,cpp}`, `editor/window/InspectorWindow.cpp` (the Bake row:
`drawLatticeBakeRow`, called at the end of `drawLatticeModifierComponent`), `CMakeLists.txt`,
`tools/mcp/tests/test_lattice_bake.py` (new).

Tests: `tools/mcp/tests/test_lattice_bake.py` (4 tests, new empty project, harness Xvfb, the button
clicked with xdotool: it is found in an editor screenshot as the lowest full-width button-coloured
row once the inspector is scrolled to its end; the test skips with a message if the layout moves).
Ball (.gltf + .bin + PNG, NORMAL, TANGENT with w = -1 on half, COLOR_0, a sparse morph target, two
primitives sharing their accessors; two lattices, one on submesh 1 with the red mask): positions,
normals, tangents (w kept) and the morph displacement of chosen vertices against the spec formulas
computed in Python, texture URI, one embedded buffer, split accessors; the baked render matches the
live GPU render (pngdiff, tolerance 16, at most 0.3% of the pixels; the unbaked view is checked to
differ), undo/redo, a second bake writes `_2` and leaves the first file alone, the saved scene names
the file. Column (skinned): Before Skinning bakes into the bind pose (every vertex checked, written
in place, skin and joints untouched, `max` updated), the After Skinning binding stays with its
Console note, render matches, undo/redo. Pair (two nodes, one mesh, one bound): the bound node gets
a mesh copy, the other keeps its data, part entities keep their ids, undo. Box primitive: the
button is disabled and clicking it writes nothing. With a temporary debug MCP tool (not committed)
the reasons above, a group with Include Children baking two models into two files, a quantized
model (exact to 1e-7) and a Merge Static Meshes model (exact to 1e-8, the unbound node unchanged)
were also checked. WP-MCP: `edit_lattice` `bake` should call `LatticeEditorOps::bake` and return
`path` and `note`; adding its variant of these checks to the test drops the xdotool dependency.

    JASPER_FIXTURE=<empty project> JASPER_EDITOR=build/native/jasperengine-editor \
        tools/mcp/run-tests.sh tools/mcp/tests/test_lattice_bake.py

### 12.x Engine runtime review

Adversarial review of the merged engine side (solver, presets, LatticeSystem, Lattice, LatticeTracks
and the ActionSystem changes, EntityCloner, the sprite grid, the Lua bindings). Fixed:

- `EntityCloner` `prepare<MeshComponent>`: WP-RENDER's `mesh.lattice = LatticeMeshState()` ran
  before WP-ECS read `wantsTessellation`/`tessellation`, so a copied lattice sprite was rebuilt
  as a quad, drawn bilinearly bent for a frame, then rebuilt and reloaded as its grid. The
  request is kept now (`TestLatticeRuntime.cpp`, htmlui tests).
- `ActionSystem::animationApplyEnd` clamped each track to its frame's explicit duration, while
  every other update samples it at the clip time (an explicit frame duration does not cut a track
  at runtime): a track whose frame is shorter than its keys jumped back to its value at the
  frame's end on the clip's last update (all track types). It samples at the clip time now.
- Jiggle: the position history now restarts after frames without a step (inactive lattice,
  paused system); a lattice moved meanwhile was kicked by a stale finite difference.
- Lua `Lattice:addFollower` had only the 2- and 6-argument forms (LuaBridge matches the exact
  arity): the documented optional `applyPosition, applyRotation, applyScale` failed with 3 to 5
  arguments.
- `LatticeSystem` steady frames allocate nothing (jiggle anchor weights, follower order, padded
  grids and the subtree walk use kept scratch).

Tests: `tools/mcp/tests/test_lattice_review.py` (3 tests), `engine/tests/htmlui/TestLatticeRuntime.cpp`.
