# T3 — Terrain materials and holes

Wave 5 (terrain programme, `TERRAIN-LEDGER.md`, D1-D21 binding). Owns D7: the hole representation and the terrain
material shader changes. Reports: `/tmp/terrain-research/reports/terrain3d-shaders-instancer.md`,
`our-terrain-runtime.md`, `engine-constraints.md`.

## 1. Goal

A painted terrain today fetches every non-zero layer at up to four reads per map and leaves no
texture slot free, so it costs too much on phones and cannot grow. T3 gives it a per-pixel cap on
the strongest layers, and three Quality tiers (High, Medium, Low) with a per-platform default and
a hard fetch budget for Low. It adds macro variation, distance resampling, cliff projection with no
extra fetch, a macro normal map, compressed layer arrays and up to 12 layers, and it makes terrain
outside the view cast shadows. It adds **holes**: one crisp mask that rendering, collision, navmesh,
picking, foliage, particles and splines all read the same way, which T6 uses for cave entrances.
A terrain that uses none of this draws what it drew, for what it cost.

## 2. Scope

**In scope**
- Quality tiers, top-N layer selection from the existing blend maps, blend-map trimming.
- Macro Variation (noise tint or base map), Distance Resampling, per-layer Cliff Projection, height
  blend sharpness (Terrain3D's formula, MIT), Macro Normal Map.
- Layers 10-12 in the blend maps' alpha channels.
- Compressed layer arrays on export (S14's array exclusion lifted, §6 D8).
- The shadow cut: off-screen terrain casts into the shadow maps.
- The hole map, its behaviour in every consumer, `cut_holes` / `fill_holes` semantics for T1's
  engine, a hole overlay, the Hole brush.
- Terrain Layers panel, Inspector and Project Settings Quality UI with a live fetch readout, Scene view
  per-tier preview; `sample_terrain` and `visualize_terrain` extensions; Lua.

**Out of scope**
- T1's operation engine, surface query, undo store and `edit_terrain` / `visualize_terrain` tools:
  T3 specifies what the hole operations and overlays mean, T1 owns the plumbing (D4, D5, D7).
- Tiles, streaming, far overview, per-tile formats (T4); voxel surfaces under a hole (T6).
- A control-map splat, painted per-texel angle and scale, per-layer UV rotation: they need a new map
  or an integer format (D13), and hex tiling already breaks repetition (S11).
- Per-pixel normals from the height map: rejected in §6 D6.
- Displacement. A hole cut in the vertex stage. Skirts or walls around a hole (T6's voxels supply
  the walls).
- Runtime automatic navmesh rebake after a hole edit: a game calls S19's bake with T1's dirty rectangle.
- **Do-not-copy** (`docs/specs/README.md:23-27`, D13): no CBT terrain, no GPU-driven culling, no GPU
  grass. From Terrain3D: no `//INSERT:` shader splicing, no world noise, no float-bits control map,
  no region uniform arrays, no clipmap (D11).

## 3. Current state

Read from the code at `b4f7816ea`. **Nothing here was built or run: every behaviour below is
inferred from reading unless it says otherwise.** Paths relative to the repository; RS is
`engine/core/subsystem/RenderSystem.cpp`, TF `engine/shaderlib/includes/terrain_fs.glsl`, TV
`engine/shaderlib/includes/terrain_vs.glsl`, TC `engine/core/component/TerrainComponent.h`, MS
`engine/core/subsystem/MeshSystem.cpp`.

**Data.** One height map, up to 3 RGB blend maps, up to 9 layers (TC:23-24). `blendMaps[m]` channel `c`
weights `surfaceLayers[m * 3 + c]` (TC:168). The alpha of a blend map is deliberately ignored because
saved maps are opaque (TC:21-22).

**Two fragment paths.** `getTerrainColor` (TF:167, `HAS_TERRAIN`, no uniform block) and
`getTerrainSurface` (TF:497, `HAS_TERRAIN_PBR`, block `u_fs_terrainLayers`, TF:42-51). The wider one is
chosen by `hasTerrainSurfaceLayers` (TC:242-250); the reload hook is RS:14811-14826.

**Fetch cost, counted from the code** (terrain material reads per pixel, no base maps and no lighting):
- Three blend maps, always (TF:171-173, TF:501-503).
- Every layer with a non-zero weight is read (TF:181-190, TF:516-531).
- One map: near **and** far tiling on the flat projection, even where the far share is 0 (TF:139-142,
  TF:92), plus up to two side planes (TF:146-155). So 2 on flat ground and 4 on a blended slope.
- A PBR layer has up to three maps. Worst plain pixel: 3 + 9 x 3 x 4 = **111**. With hex tiling
  (three flat taps and two sides per map, TF:415-434): 3 + 9 x 3 x 5 = **138**. With Stochastic Tiling
  (three taps per projection, `TerrainComponent.h:95-99`): 3 + 9 x 3 x 9 = **246**.

**Texture slots.** The portable limit is 16 per stage (`engine/libs/sokol/sokol_gfx.h:2051`). Counted for
a painted terrain with every lighting feature:

| Variant | Base maps | SSAO, IBL, spot, shadow atlases, LUT | Terrain | Total |
|---|---|---|---|---|
| Painted, post colour (emissive dropped, `mesh.frag:70-78`) | 4 | 1 + 3 + 1 + 2 + 1 | 4 | **16** |
| Painted, no post colour | 5 | 1 + 3 + 1 + 2 | 4 | **16** |

S11:78-87 records the first row. The second is also full, so a new map has no slot in either.

**Uniform blocks.** Sokol allows 8 per stage (`sokol_gfx.h:6281`). A lit painted terrain already has
`u_fs_pbrParams`, `u_fs_litFrame`, `u_fs_objectLights`, `u_fs_shadow_data`, `u_fs_point_shadow_data`,
`u_fs_reflectionProbe`, `u_fs_terrainLayers`, and with height fog `u_fs_heightFog`. A new fragment block
has no room, which is why S11 appended its lobes to an existing block (`mesh.frag:122-140`).

**Variants.** Key = ShaderType + 64-bit properties + customId. MESH bits run to 35, DEPTH to 17, GBUFFER
to 15 (`engine/core/pool/ShaderPool.cpp:1711-1800`). Terrain Lit features travel as `builtin:lit#<defines>`
(RS:3908-3916, the table at `engine/core/texture/MaterialPbr.cpp:89-99`, 18 of 32 bits used). A build
without a compiler that lacks a variant drops only optional features and never a uniform-block bit
(`engine/core/pool/ShaderPool.h:75-78`, `ShaderPool.cpp:1078-1098`, `:1120-1132`). The same customId is
kept, so a missing `#define` variant has no fallback and is reported missing.

**Detail array.** One RGBA8 `TEXTURE_ARRAY` built on the fly from source files, at most 2048 px, one
sampler, colour slices first (RS:5481-5726; RGBA8 at RS:5713). Up to 27 slices: 576 MiB at 2048 px with
mips. S14 excludes array assets (S14:34) and ships terrain layer sources uncooked (S14:549-554,
`editor/texture/TextureCookQueue.h:49`). `Ktx2.cpp:68` rejects layered files.
`SokolTexture::createTextureLevels` already accepts any `GpuTextureFormat` with contiguous slices per
level (`engine/renders/sokol/SokolTexture.cpp:520-540`).

**Normals.** Per vertex, four height fetches (TV:96-113), delta tied to the node's cell, so far nodes
shade smooth and change with morph.

**Shadows.** Selection (RS:12820) culls nodes against the camera frustum (RS:12887). Depth passes draw that
cut (RS:9023, comment RS:9020; `mainTerrainView` RS:13928; shadow draw at
`engine/core/subsystem/RenderSystemShadows.cpp:1651`). Other casters are tested against the shadow camera.
The cascade fit already extends its near plane over off-screen casters using the terrain's bounds
(`engine/core/render/shadow/CascadeFit.cpp:344-349`). So a ridge behind the camera has its depth range
reserved and is not drawn: **terrain outside the view casts nothing (inferred; AC24 confirms it red first).**

**Holes.** No hole code anywhere (grep). Consumers of the surface, all ignoring holes:

| Consumer | Where |
|---|---|
| Colour, extra pass | `mesh.frag:264-276`, `:311-318` |
| Depth (shadow, SSAO, pre-pass, atmosphere depth) | `depth.frag`, `depth.vert:80-81`, RS:9021-9024 |
| G-buffer | `gbuffer.frag:93-101` |
| Library shaders | `includes/output.glsl` (35 frags), `includes/surface_depth_fs.glsl` |
| Jolt heightfield | `engine/core/physics/JoltShapeBuilder.cpp:257-308`; `PlacementWorld.cpp:1429` |
| Navmesh | `engine/core/navigation/NavBakeCollector.cpp:615-672`, `NavMeshBuilder.cpp:217-243`, tile hash `NavBakeInput.cpp:301-325` |
| Nav grid | `engine/core/subsystem/NavigationSystem.cpp:1433` |
| Picking | MS `raycastTerrainSurface`, `engine/core/picking/MeshRaycast.cpp:298-322` |
| Foliage | MS `appendFoliageCell` |
| Spline conform and fill | `engine/core/subsystem/SplineSystem.cpp:849`, `:1014` |
| Particles | `engine/core/subsystem/ParticleSubsystem.cpp:650` |
| Editor and tools | `editor/mcp/tools/WorldTools.cpp:508`, `editor/window/TerrainEditWindow.cpp:1812,2095,4144`, `editor/spline/SplineEditor.cpp:401`, `editor/spline/SplineSceneRender.cpp:247`, `editor/vat/VatCrowd.cpp:229` |

**Jolt holes.** `cNoCollisionValue` (`engine/libs/joltphysics/Jolt/Physics/Collision/Shape/HeightFieldShape.h:23`)
works on samples, not squares. Square `(x, y)` is split along `(x, y)`-`(x+1, y+1)`; a missing diagonal end
removes both its triangles and a missing other corner removes one (`HeightFieldShape.cpp:861-885`). So one
missing sample removes 6 triangles around it. `SetHeights` accepts the value (`HeightFieldShape.h:224`). The
engine calls neither today. An odd sample count is padded to the block size with no-collision samples
(`HeightFieldShape.cpp:405`).

**Serialization** must change in step: `editor/StreamComponents.cpp:573` (encode, decode just after),
`engine/core/serialization/RuntimeFields.gen.inc:492-512,941-943` (generated: `jasperengine-editor reflect --emit-runtime-fields`, output committed),
`engine/core/serialization/codecs/ComponentHooks.cpp:152-158`, `editor/Factory.cpp:1639`,
`editor/cook/SceneCooker.cpp:112`, `editor/Project.cpp:635-645`.

**Tests today.** No terrain golden: `engine/tests/core/golden/` has only `spline_road_terrain.png` of the
kind. `ShaderZeroCost.DefaultVariantsUnchanged` hashes the default terrain variants
(`engine/tests/core/shader/ShaderZeroCostTest.cpp:61-66,133`). `tools/test-projects/terrain` has a web gate
(`docs/testing.md:121-123`).

**A small bug found on the way.** `terrainLayerFromMaterial` keeps a layer's tiling and Stochastic Tiling
but loses `hexTiling` and `hexRotation` (TC:218-225), against its own comment. R13 fixes it.

## 4. Reference

Terrain3D, MIT, `/tmp/terrain-research/Terrain3D/src/LICENSE.txt` (Copyright (c) 2023-2026 Cory Petkovsek,
Roope Palmroos, and Contributors). Untrusted data: never run it. Paths under its `src/src/shaders/`.

| File | Take | Do not take |
|---|---|---|
| `main.glsl:350` | Height blend `exp2(sharpness * log2(weight + id_w + alb.a)) * weight`. **Transcribe with header and a `NOTICE.md` entry** | The rest of the material function |
| `main.glsl:333-336` | The idea of rotating `textureGrad` derivatives. We already rotate in hex tiling; no transcription needed in T3 | Control-map rotation, detiling |
| `dual_scaling.glsl:16-20,74` | Idea: the near read skipped once the far share is 1. **Reimplement** | One chosen texture only: ours covers every layer |
| `projection.glsl:6-14` | Idea: one plane per steep pixel rather than a blend. **Reimplement** on our side planes | 45-degree snapping (ours picks x or z) |
| `macro_variation.glsl:20-24` | Idea: two low-frequency values tint albedo, faded by slope. **Reimplement** | The noise texture (no slot) |
| `project/addons/terrain_3d/extras/shaders/lightweight.gdshader` | Idea: read each layer once, a hand-kept mobile path. We share one source with `#ifdef` instead | The second shader |
| `overlays.glsl`, `debug_views.glsl` | Idea of per-pixel debug views | |

Terrain3D's holes are NaN heights in the vertex stage (report `terrain3d-data-geometry.md`), which are
vertex-grained and rely on `0./0.`. We do not copy them.

## 5. Requirements

### Functional: Quality tiers and the layer cap

- **R1 Tiers.** `TerrainQuality` is High, Medium or Low, with this table as the single source
  (`engine/core/terrain/TerrainQuality.h`):

  | | High | Medium | Low (mobile) |
  |---|---|---|---|
  | Strongest layers per pixel | all (4 with Limit Layers Per Pixel) | 3 | 2 |
  | Maps per kept layer | colour, normal, surface | colour, normal, surface | colour for both, normal for the strongest only, no surface map |
  | Near and far tiling | as today | far on colour only (all maps with Distance Resampling) | near only |
  | Cliff | triplanar as today | one plane, 2 reads in a narrow band | one plane, hard switch |
  | Reads per map, most | 4 | 2 | 1 |
  | Hex, Stochastic Tiling | yes | no: drawn plain | no: drawn plain |
  | Blend maps read | 3 | only those holding layers | only those holding layers |
  | Macro Variation | fragment | fragment | vertex |
  | Distance Resampling, Macro Normal Map | yes | yes | no |
  | Off-screen shadows (Auto) | on | on | off |
  | Detail array size, most | 2048 | 2048 | 1024 |
  | Worst fetches (R8) | as today | 21 | 6 |

- **R2 Resolution order.** Effective tier = editor preview if set, else runtime global override, else the
  component's `quality` if not Auto, else the project's tier of the running platform. A tier the build did not
  ship clamps to the nearest shipped one, higher first, with one warning (R34).
- **R3 Selection.** Medium, Low and Limit Layers Per Pixel select from the blend weights already read, with no
  new map. With `K` kept and `w` the `(K+1)`-th strongest weight, each kept weight becomes `max(w_i - w, 0)`,
  then is scaled so the kept weights sum to the original sum of **all** layer weights. The base weight stays
  `1 - sum(all)`. When the scaled sum is 0 (a tie of more than K), the kept weights are used as read. This makes
  a layer entering or leaving the cap continuous: its share is 0 at the swap. Height biasing applies after it.
- **R4 High is unchanged.** At High with no feature on, the output is the same as before T3 within AC1's
  tolerance, on both paths. The only source change is the skipped far read where its share is exactly 0, which is
  exact (`mix(a, b, 0.0) == a`) and precedented (`terrain_fs.glsl:131`, S11:761 perf2).
- **R5 Low's strongest-layer normal.** The strongest layer's normal strength is scaled by
  `(w1 - w2) / w1`, so it fades to none where the top two swap and no seam shows. A kept layer without a
  normal map contributes the geometric normal.
- **R6 Height on Low without a surface read.** Each PBR layer's colour slice stores its height in alpha, with
  separately built alpha mips. The colour bytes and their mips stay as built today, and High ignores that alpha
  (`terrain_fs.glsl:579-580`), so High's output does not move. Low reads height from the colour fetch.
- **R7 Blend-map trimming.** At Medium and Low, a terrain with at most 3 layers reads one blend map and with at
  most 6 reads two (`TERRAIN_BLENDMAPS_1`, `_2`), and the unused slots are not declared. High keeps three, so old
  variant keys do not change. Output equals three maps since the others bind black today (RS:5819-5834).
- **R8 Fetch budgets** (terrain material reads per fragment; base maps and lighting not counted):

  | | Flat, near, 2 layers | Worst, plain layers | + holes | + Macro Normal |
  |---|---|---|---|---|
  | High | 3 + 2x3 = 9 (today 15) | 111 (138 hex, 246 stochastic) | +1 | +1 |
  | High, Limit Layers Per Pixel | 9 | 3 + 4x12 = 51 | +1 | +1 |
  | Medium | 9 | 3 + 3x3x2 = **21** | **22** | **23** |
  | Low | 3 + 2 + 1 = 6 | **6** | **7** | n/a |

  Low's budget is a hard limit in every pixel: **at most 7 reads with holes, at most 6 without.** With trimming
  (R7) a terrain of up to 6 layers has 6 and 5, and one of up to 3 layers has 5 and 4.

### Functional: material features

Each works on the wide path: turning one on makes `hasTerrainSurfaceLayers` true, as `hexTiling` does (colour
layers stay raw there, TF:577-579). Each costs nothing when off.

- **R9 Macro Variation.** `macroMode`: Off, Noise, Base Map.
  - Noise: two octaves of value noise over terrain-local XZ at `macroScale` metres, with an integer hash as in
    `includes/hex_tiling.glsl`, tinting albedo by `macroTintA` and `macroTintB` at `macroStrength`, faded on steep
    ground by `macroSlopeFade`. No texture read. Fragment stage at High and Medium, vertex stage at Low (one
    varying, no fragment cost).
  - Base Map: the base material's colour texture, already read for the base weight (`mesh.frag:252`), multiplies
    the layers as `mix(1, base * 2, macroStrength)`. No new slot or read. It gives authors a macro
    colour map (T2 bakes no colour map; a graph can only make the mask, T2 §8). With no base texture the Inspector says so and it has no effect.
- **R10 Distance Resampling.** `resampleEnabled`, `resampleNear`, `resampleFar` (metres), `resampleReduction`
  (default 0.25). Replaces the fixed tile break (TF:19-22) on this terrain: near only below `resampleNear`, a
  crossfade of near and far between, far only past `resampleFar` with the near read skipped. Far terrain then costs
  1 read per map instead of 2. Never more than the tier's reads per map: at Medium the far read yields to a side
  plane in the band.
- **R11 Cliff Projection** per layer: Auto (the tier's: triplanar at High, one plane at Medium and Low) or Flat
  Only (never a side plane: for grass or sand that never sits on a cliff; saves up to 3 reads per map on slopes at
  High). One-plane mode replaces the triplanar weights of `setupTerrainDetail` (TF:96-118): flat where
  `abs(n.y) >= 0.7071`, otherwise the larger of x and z. It reuses the side UVs, gradients and whiteout normals,
  so it adds no fetch: it removes them.
- **R12 Height blend sharpness.** `heightBlendSharpness`, default 0: the classic `pow(height, 6)` (TF:187, TF:630).
  Above 0: the Terrain3D formula, in `includes/terrain_height_blend.glsl` with its copyright header and a `NOTICE.md`
  entry. `TERRAIN_HEIGHT_BLEND` compiles it in only when above 0.
- **R13 Layer from material.** `terrainLayerFromMaterial` also keeps `hexTiling`, `hexRotation` and
  `cliffProjection` (bug in §3).
- **R14 Macro Normal Map.** `macroNormal`, default off. A terrain-wide normal map is baked on the CPU from the
  retained height data (`engine/core/terrain/TerrainMacroNormal.h`), at the height map's resolution capped at 2048,
  RG8 with renormalized mips, on a Background job; on the single-thread web build by rows within S06's 4 ms slice. It is
  bound to `u_normalTexture`, so it adds no slot (a base material normal map is replaced, with an Inspector warning),
  and replaces the vertex normal as the shading base and in whiteout (not in the triplanar weights). Until the bake is
  ready, its strength in the block is 0 and vertex normals draw. T1's dirty rectangles re-bake a region, uploaded at
  most 4 times a second during a stroke. Not at Low.
- **R15 Layers 10-12.** A terrain with more than 9 layers weights layer `9 + m` by `1 - blendMaps[m].a`, so an
  opaque map means weight 0 and every existing map stays valid (an absent map binds `emptyBlack`, opaque too, RS:1152). No new slot or fetch (the maps are already read).
  `TERRAIN_EXTENDED_LAYERS` selects a 12-entry block; a terrain with up to 9 keeps the 9-entry block. Blend map slots
  for layers 10-12 force all three maps at Medium and Low. `MAX_TERRAIN_LAYERS` becomes 12.
  `terrainLayerChannel(index)` in `engine/core/terrain/TerrainLayers.h` is the one mapping; T1's paint operation calls it.
- **R16 Compressed layer arrays.** With Compress Terrain Layers on, an export cooks each array slice as an ordinary
  2D KTX2 through S14's encoders, and the player loads the slices of a terrain and concatenates each level
  (`createTextureLevels`, `TEXTURE_ARRAY`):
  - Slice settings are fixed so all slices of an array share a format: desktop BC7, mobile ASTC 6x6, Android ETC2
    fallback ETC2_RGBA8, web UASTC (transcoded to one target for all slices). Size at most the tier table's largest
    for that family (mobile 1024, others 2048).
  - A device that lacks the format uses S14's CPU-decode fallback on every slice, ending at RGBA8.
  - Slices are keyed by the existing slice key (RS:5486-5511) in `je-cooked/<family>/index.json`, and shared maps
    deduplicate. Sources of a cooked layer no longer ship unless another reference reads them (S14 `readsSource`).
  - The editor stays on the raw path, except the Game view when it simulates a family (S14's provider).
  - Runtime array shrink for Low skips the top levels of the cooked or raw chain, and changing tier rebuilds the array on
    a Background job with the old one drawing meanwhile.
- **R17 Shadow cut.** A terrain that casts shadows gets a second CDLOD cut per camera that owns shadows: the same
  selection, same ranges, same eye and morph origin, with the frustum test replaced by "intersects any active shadow
  camera of a light with a shadow slot". The LOD of a node depends only on the range spheres
  (RS:12882, RS:12901), so nodes in both cuts are identical and existing shadows do not move. The depth pass draws
  it for `PIP_SHADOW_DEPTH`, and SSAO and pre-passes keep the view cut. A terrain stays a camera-dependent caster for S10's cache
  (`RenderSystemShadows.cpp:607`). `offscreenShadows`: Auto (tier), On, Off (Off is
  today's behaviour, drawing the view cut). Buffers are allocated on first use and freed when off.

### Functional: holes

- **R18 Representation.** One optional **R8** `holeMap` per terrain, 0 = hole, 255 = solid (threshold 0.5), read with
  `texelFetch` and no filtering or mips. It lives beside the other maps as an 8-bit PNG in `getTerrainMapsDir()`.
  Absent means no holes, and no cost anywhere. Created at the height map's resolution by the first cut, deleted when the
  last hole is filled (R29). Why not a blend map's alpha: §6 D3.
- **R19 The query.** `isHole(map, u, v)` in `engine/core/terrain/TerrainHoles.h` is the only definition: texel
  `min(floor(u * W), W - 1)`, same for v, with `u = (localX + half) / size`. T1's surface query calls it. A point on a
  texel edge belongs to the higher texel. Outside the terrain is not a hole (it is off the terrain, as today).
- **R20 Rendering, every pass.** Fragments in a hole are discarded, at the top of the shader, with the same texel test
  and the same interpolated local XZ, in: colour (`mesh.frag`), the extra pass, depth (shadow atlas, SSAO, depth
  pre-pass, scene depth, atmosphere depth), G-buffer, and the library's `finalizeColor` and
  `includes/surface_depth_fs.glsl`. Variant bit named `Thl`, define `TERRAIN_HOLES`, on MESH, DEPTH and GBUFFER (appended at landing, D2). The UV
  is one varying set in `getTerrainPosition` (TV:69), the function every path shares. Morphing moves the
  vertices, not the hole. A custom or forked shader that does not read it draws over holes: the Inspector warns.
- **R21 Slot.** The hole map takes the slot of the base material's occlusion map: `TERRAIN_HOLES` leaves
  `u_occlusionTexture` out on every tier, on the precedent of the emissive map (`mesh.frag:70-74`). The base occlusion then
  reads 1, the layers' own occlusion still applies (`mesh.frag:408`), and the Inspector warns if the base material has one.
  Depth and G-buffer have free slots, but a `Thl` G-buffer leaves the occlusion map out too, so its AO matches the colour pass (R57).
- **R22 Jolt.** A lattice square is **open** when every hole texel its local rectangle overlaps is a hole. A sample
  becomes `cNoCollisionValue` when all its existing adjacent squares (4, 2 at an edge, 1 at a corner) are open
  (`JoltShapeBuilder`, with the mirror of JSB:276-281 applied). No hole map: not one extra instruction in the loop. So the
  collider's gap never includes drawn ground, and the collider stays under a rim of at most one collision cell inside
  the drawn hole. A hole with no interior sample opens nothing: `cut_holes` and the brush report it (R27, R41).
- **R23 Navmesh.** A lattice cell overlapping any hole texel is left out of the bake (both triangles, in
  `NavMeshBuilder.cpp:217-243`), carried as `NavTerrainInput::holeCells` (empty without holes) and hashed into the tile
  key (`NavBakeInput.cpp:301-325`). The nav grid (`NavigationSystem.cpp:1433`) marks a cell over a hole Not Walkable
  unless overridden.
- **R24 The nesting rule.** Everywhere: **navmesh walkable area ⊆ drawn ground ⊆ collider surface.** Nothing is routed
  across a hole, nothing falls through drawn ground, and the collider is open everywhere deeper than the rim of R22.
- **R25 Picking.** `raycastTerrainSurface` and MeshRaycast skip a crossing or closest point inside a hole and go on along
  the ray, so a ray down a hole misses this terrain and returns what lies beyond it. An optional `ignoreHoles` is set by every
  terrain brush, so a brush can paint and fill inside a hole (it passes `skipHoles = false` to T1's `raycastSurface`, T1 R16). S26's 1e-4 rule (S26:467) holds outside holes.
- **R26 Other consumers.**

  | Consumer | Over a hole |
  |---|---|
  | Foliage scatter | No instance (origin test, after the density test, before height). Hole change sets `needUpdateFoliage` |
  | Spline conform, Extrude | Reads the height map as usual: a road bridges a hole. `SplineWarningKind::OverHole` with a count |
  | Spline Instantiate | An item whose origin is over a hole is dropped, deterministically, like dropout |
  | Spline fill, terrain bake | Height map only; a bake neither cuts nor fills |
  | Particles (`sampleTerrain`) | No surface: the particle passes |
  | PlacementWorld | The heightfield of R22 |
  | `sampleTerrainSurface` | Height and normal as today: the height map stays defined under a hole (T1 reports `hole` beside them, R43) |
  | Lua height query (T1) | Same as above |

- **R27 `cut_holes` / `fill_holes` semantics** (T1's engine runs them; §6 D5):
  - A region is a rectangle, circle, polygon or R8 mask (threshold 0.5); a texel is in it when its centre is. No falloff, no
    strength: the operation is absolute and idempotent (a second identical call changes 0 texels). `fill_holes` with no
    region fills all.
  - Validation is all-or-nothing; `dry_run` changes nothing; one undo step per call.
  - The result has `changed_texels`, the rectangle in T1's `changed` (T1 R9), `hole_texels`, `hole_fraction`, `map_created`, `map_removed`,
    `collision_samples_open` and a `warnings` list (`hole_no_collision`, a region that opens no collision sample, T1 Table 2).
  - Height, blend and density data are untouched, so filling restores the surface exactly.
- **R28 Refresh on change.** Rendering: the R8 map re-uploads at most once a frame (`sg_update_image`, D12). Collider:
  T1's refresh re-evaluates R22 over the dirty rectangle grown by one cell, since a sample depends on four squares
  (`SetHeights`, or a rebuild, T1's choice). Foliage: the dirty cells. Navmesh: its hash notices at the next bake.
- **R29 Lifecycle.** First cut creates the map and reloads the variant; last fill removes it from the component, and the
  file at save, and reloads the variant. Undo restores either state. While the non-saved `holeMapHolds` count on the component
  is above 0 (T6 R40: a terrain with an attached Automatic Voxel Volume, in Play), a last fill keeps the map, all solid, and
  reloads nothing, so a first in-game dig costs no variant reload. A hole map whose size differs from the height map
  stays valid (the query is in UV). A hole map that fails to load warns once and draws no holes.
- **R30 Overlay.** `Holes` overlay (editor only): hole texels hatched in place on a ghost of the surface, so a hole can be
  seen and filled; the collider's rim in a second colour; navmesh exclusions outlined. No runtime cost.

### Platform & backends

- **R31** Every feature works on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan, and all export targets. No compute,
  SSBO, bindless, indirect, integer textures or float-bits packing (D13). `texelFetch` is already used in the vertex stage
  (`includes/vat.glsl:42`, `includes/skinning.glsl`); T3 is its first fragment-stage use on terrain, on `highp` coordinates, and AC23
  runs it on WebGL2.
- **R32 Slots.** No terrain variant exceeds 16 fragment textures (§6 D4 table). `ShaderLibraryMatrix` gains the worst case
  of every tier with holes, post colour and all lighting, and fails above 16.
- **R33 Blocks.** No new fragment uniform block. Feature parameters are appended to `u_fs_terrainLayers` under
  `#ifdef`, located by reflection as `pbrExtOffset` is (RS:6539-6552). A variant without them has a smaller block and the
  upload uses its reflected size.
- **R34 No runtime compiler.** The collector reaches every variant a player can ask for:
  - Tiers of an export = the project's tiers of its target platforms, plus any component `quality`, plus all three if a
    script mentions `setTerrainQuality` or `terrainQuality`, or **Ship All Terrain Tiers** is on.
  - A terrain ships `Thl` when it has a hole map or a Voxel Volume attached to it with `terrainCut = Automatic` (T6 R40). The rule is T3's; if T3
    lands before T6, T6 adds the second clause and AC10's case in this form.
  - `MeshShaderEnvironment` gains `terrainTier`, included in `environmentBits` (`ShaderVariantCollector.cpp:214-218`).
  - `PlayerConfig` carries the shipped-tier mask and R2 clamps to it, so a lite player never requests a variant it lacks and
    never loses the draw. Nothing in T3 relies on a fallback, and no `Thl` or define bit joins `getFallbackProperties`,
    because `ShaderPool.h:75-78` allows only features whose loss keeps blocks, and a dropped `Thl` would draw over holes.

### Performance

"Host" is this 8-core machine with llvmpipe; ratios are the terrain pass by S15's timers at 1280x720 in one run. "Campaign"
is the RTX machine of `docs/perf/gpu-campaign.md`, not available here. "Mobile" is S18's mid-range 2022 Android, manual.

- **R35 Tier cost**, against High on the same scene, fixtures `Terrain Baseline` (4 painted layers,
  `tools/fixtures/pbr-extended/generate.py:236`) and new `Terrain Nine` (9 overlapping PBR layers):

  | Fixture | Medium | Low |
  |---|---|---|
  | Terrain Baseline | ≤ 0.9x | ≤ 0.6x |
  | Terrain Nine | ≤ 0.5x | ≤ 0.3x |

  Low on mobile at 1080p: terrain pass ≤ 4 ms (manual).
- **R36 Feature cost.** No feature on: within 1 % of pre-T3 (host), expected faster from R4. On High, each alone:
  Holes colour pass ≤ +3 %, depth pass ≤ 1.3x (discard defeats early Z); Noise macro ≤ +3 %; Macro Normal ≤ +3 %; Resampling
  ≤ 1.0x; Cliff Flat Only ≤ 1.0x.
- **R37 CPU.** `isHole` ≤ 20 ns on the host. Heightfield build with holes ≤ +20 %. Shadow cut selection ≤ 1.5x the view
  selection on the same terrain. Macro normal bake of 2048² ≤ 150 ms on one host core. Fetch readout ≤ 50 µs per update.
- **R38 Memory.** Detail array, 9 PBR layers (27 slices, with mips):

  | | Raw RGBA8 2048 | Raw RGBA8 1024 (Low) | BC7 2048 | ASTC 6x6 1024 |
  |---|---|---|---|---|
  | MiB | 576 | 144 | 144 | 16 |

  Hole map: 1 byte per texel on CPU and GPU. Macro normal 2048²: 11 MiB. Shadow cut: the same buffers as one view
  (RS:6232-6236).

### Editor UX (human)

- **R39 Terrain Layers panel** (Terrain window). A cost strip on top: `Fetches per pixel: Flat 9 · Slope 15 · Worst 21 ·
  Textures 16/16` for the effective tier, updated live as layers change, with a tier selector that shows the same numbers
  for another tier without changing anything. Red above a tier's budget, with a tooltip naming what costs most. Each layer row:
  thumbnail, painted coverage in percent, Cliff Projection, and a badge when its maps are not read at the shown tier. Slots
  10-12 are labelled as using the alpha of Blend Maps 1-3.
- **R40 Inspector and Project Settings.** Terrain gets a **Quality** section (one shared widget,
  `editor/window/TerrainQualityPanel.{h,cpp}`, ledger D2): Quality (`Auto (Project: Medium)`, High, Medium, Low), Limit Layers Per
  Pixel, Off-screen Shadows, Macro Variation group, Distance Resampling group, Height Blend Sharpness, Macro Normal Map, and
  the cost strip. A feature a tier does not draw is greyed with the reason. Warnings: base occlusion (R21), base normal (R14),
  custom shader with holes (R20). Project Settings › Graphics › Terrain: **Quality** per platform (`graphics.terrainQuality.<platform>`,
  as `SettingsRows.cpp:1648`), **Ship All Terrain Tiers**, **Compress Terrain Layers**, and the cost strip for the selected
  platform on each terrain of the open scene. All undoable.
- **R41 Hole brush.** `CutHole`, `FillHole` in `TerrainBrushMode`, sharing the Size and Shape controls; no falloff or strength.
  Ctrl inverts (D16), and the decal is a distinct hole colour and shows the texels it will change when close. The Holes
  overlay turns on with it. The info panel shows hole or solid under the cursor, the texel size and the collision cell, and warns
  while a stroke's hole opens no collision sample. One undo step per stroke.
- **R42 Scene view preview.** A **Terrain** dropdown in the Scene view toolbar: Preview Quality (As Project, High, Medium,
  Low) and View (Shaded, Layer Count, Fetch Cost, Holes). A badge names an active preview. The preview applies to all edit-mode
  views, since a variant belongs to the submesh, ends at Play, and is not saved in the project.

### Agent UX (MCP, CLI, Lua)

No new tool (D15, `docs/mcp/conventions.md`). Group `terrain` (D17).

- **R43 `sample_terrain`.**
  - `info.material`: `quality`, `quality_source`, `limit_layers_per_pixel`, `layers`, `blend_maps_read`,
    `fetches` (`flat`, `slope`, `worst`, for the effective tier and `by_tier`), `texture_slots` (`used`, `limit`),
    `detail_array` (`format`, `slices`, `bytes`), `features`.
  - `info.holes`: `map`, `resolution`, `hole_texels`, `hole_fraction`, `collision_cell`; null without a map.
  - Per point: `hole`, `collision_gap`, `nav_excluded`, `layers_painted`, `layers_drawn` (`[{layer, weight}]` after the cap),
    `fetches`.
  - `stats: ["coverage"]` with T1's `region` (T1 R70) returns the hole fraction and layer-count histogram of that rectangle as
    `stats.coverage`.
- **R44 `visualize_terrain`** (T1's tool) gains overlays `layer_count` (1-12 on a fixed ramp), `fetch_cost` (reads per pixel on
  a fixed ramp) and `holes` (R30), and a `quality` argument that renders one frame at that tier without changing anything. Its
  result includes the per-pixel maximum and a histogram read back from the frame.
- **R45 Setting tiers.** `set_properties` on `terrain.quality` and the other new fields (`terrain.surfaceLayers[i].cliffProjection`,
  `terrain.holeMap` with a path or null); `set_project_settings` on `graphics.terrainQuality.<platform>`. All validated and undoable through
  the existing paths. `set_terrain_maps` does not change for T3: reflection reaches these fields, and its exception does not rise.
- **R46 `edit_terrain`** (T1's tool) exposes R27 with the same arguments and result. `get_project_settings` returns the shipped
  tiers an export would have.
- **R47 Guide.** `docs/mcp/guide/terrain.md` (T1 or `agent-authoring` creates it; T3 adds its section, and creates it if neither has landed) with how to
  check a budget and a hole end to end. No agent guide line. `docs/mcp/TOOLS.md` regenerated. Stay within `tools/mcp/budget.json`: the `terrain` group's
  limits are T1 R74's, and no `exceptions` entry is added or raised: `sample_terrain` and `visualize_terrain` stay within 1,500 with T3's keys (T1 §6).
- **R48 Lua.** On `Terrain`: `terrainQuality`, `effectiveQuality` (read only), `limitLayersPerPixel`, `offscreenShadows`,
  `macroMode`, `macroScale`, `macroStrength`, `resampleEnabled`, `resampleNear`, `resampleFar`, `heightBlendSharpness`, `macroNormal`,
  `setHoleMap(path)`, `getHoleMapPath()`. On `TerrainSurfaceLayer`: `cliffProjection`. Globally
  `Engine.setTerrainQuality(q)` and `Engine.clearTerrainQuality()`. The query is T1's `isHoleAt(x, z)` and runtime hole edits are
  T1's Lua operations.

### Data & compatibility

- **R49 Fields.** `TerrainComponent` gains, after the `u_vs_terrainParams` run (TC:186-195, its layout is uploaded as is):
  `Texture holeMap` (added by T1's foundation, T1 R81), `quality` (Auto), `limitLayersPerPixel` (false), `offscreenShadows` (Auto), `macroMode` (Off), `macroScale` (64),
  `macroStrength` (0.35), `macroTintA` (0.80, 0.84, 0.76), `macroTintB` (1.04, 1.0, 0.92), `macroSlopeFade` (0.33),
  `resampleEnabled` (false), `resampleNear` (30), `resampleFar` (120), `resampleReduction` (0.25), `heightBlendSharpness` (0),
  `macroNormal` (false), and the non-saved, not reflected `holeMapHolds` (0, R29; reset on clone). `TerrainSurfaceLayer` gains `cliffProjection` (Auto), appended and in `operator==`.
- **R50 No format bump.** All additive (`AGENTS.md`: an optional new field gets a default and no version bump). The blend alpha of R15 is no change of meaning: it is read only on a terrain with more than 9 layers, which no old scene has, and S03's unknown-data preservation covers an older editor. The encoder omits every field at its default, so an old scene re-saves byte-identical.
  `StreamComponents`, `RuntimeFields.gen.inc`, `ComponentHooks`, `Factory`, `SceneCooker`, the catalog snapshot and
  `Project.cpp:635-645` change together (S30:174-178).
- **R51 Project defaults.** `graphics.terrainQuality` absent means **High on every platform**: an old project keeps its look on
  every target. New projects from the templates write Windows, Linux, macOS High, Web Medium, Android and iOS Low, and turn
  Compress Terrain Layers on. Project Settings shows one line on an old project saying what the recommended mobile defaults are.
- **R52 Textures.** `holeMap` joins S14's CPU-readable slots (S14:123): never block-compressed, R8, NEAREST, clamped, no mips,
  data retained on load as the height map's is. S05's self-write suppression covers it, through T1's map writer.
- **R53 New terrains.** The Create Terrain dialog and `create_entity type terrain` (T1) set Limit Layers Per Pixel on, in T1's
  `applyNewTerrainDefaults` (T1 R91). The struct default stays off so old terrains are untouched.
- **R54 Lit define bits are 64 wide.** D2's table takes `defineCodes` to 30 entries and T6 R49 adds 4, past the 32 of today's
  `uint32_t`. T3 widens `MaterialPbr::getDefineBits`, `getDefineBit` and `getDefinesFromBits` (`engine/core/texture/MaterialPbr.cpp:538-563`)
  to `uint64_t` with `1ull << i`, and every consumer with them: `MeshComponent::litDefineBits` (`MeshComponent.h:211`), `environmentBits`
  and the tests at `RenderSystem.cpp:3270-3272,14816-14819`, `editor/Project.cpp:3788`, `editor/mcp/tools/MaterialsLightingTools.cpp:45-46,82`,
  `editor/window/InspectorMaterialPbr.cpp:197`. A `static_assert` ties the table's size to the width. The field is runtime only
  and variant names use the codes, not the bits, so no file or basename changes.

### Upstream revision: no CI, lean MCP, Doriax

- **R55 Where each AC runs.** There is no CI (`AGENTS.md`, Test): the gate per commit is `tools/check-changed`, and the full suite runs before the push when it
  needs it. Each AC runs on this host or is labelled not verifiable here with the command a person runs (§8's table). A row not run is reported unverified.
- **R56 Shaders.** Every commit that changes a shader bumps the shader cache version in `editor/App.cpp` (`App::getUserShaderCacheDir`, `v44` at this writing) by one over its
  `main`, with a comment line as `v40`-`v44` have; on a merge with another programme, re-bump so each change has its own version (Doriax ledger).
- **R57 Doriax overlap (D21; none landed: whichever lands second merges onto the first).**

  | Doriax spec | What T3 touches in common | Rule |
  |---|---|---|
  | `draw-perf` | `Dlo`, MESH's next free bit, guards point and spot code in `lights.glsl`, `mesh.frag` and shadow includes, and frees two texture units in sun-only scenes. `updateAllTerrainRenderTextures` walks `TerrainComponent` | Bits by name (D2). D4's worst case stays the general variant, so T3 relies on no freed unit. Terrain tiers and `Thl` combine with `Dlo`, and the collector ships each `Dlo` key with its twin, tiers included (AC49). T3's hooks in that function follow whichever loop calls it |
  | `ssr-probes` | G-buffer terrain occlusion: `g_occlusion` is set when the colour variant samples the map, plus `terrainSurface.occlusion` under `HAS_TERRAIN_PBR`; GBUFFER bits `Occ`, `Uv2`; `color[2].a` | `Thl` drops `u_occlusionTexture` from colour (R21), so `g_occlusion` is off with `Thl` and the swap's AO matches. Low has no surface map, so its G-buffer occlusion reads 1 as colour does. Bits by name (AC50) |
  | `mesh-memory` | `MeshComponent::litDefineBits` and `Submesh` move into `StableArray` and `GrowArray`; capacities go | R54's widening applies to the field wherever it lives. `sg_` limits on slots are unaffected |
  | `render-correctness` | G-buffer variants and the variant match at `RenderSystem.cpp:3270`, shader loops rule ("lod-safe" comments, `terrain_fs.glsl:565`), Foliage Distance | `terrain_select.glsl` and `terrain_macro.glsl` follow that rule. `pbrVariantMatches` learns T3's defines through the one helper of D2, on whichever form lands |
  | `agent-authoring` | `set_terrain_maps` factors and blend slots 0-2, `terrain.md` | T3 adds no factor and no blend parameter. A terrain using layers 10-12 still paints through `terrainLayerChannel` |

  **Pixel art.** S41's `Pxs` and S52's stream bits are appended to MESH and DEPTH too, in landing order: bits by name. S49's Analytic particles do not collide, so R26's particle row applies to S20's path only. No name clash.
- **R58 Eval cases.** T3 adds no tool, but its new keys and its `terrain.md` section need cases that fail without them (AC51).
- **R59 Vendored text.** `terrain_height_blend.glsl` holds a transcribed formula under `engine/shaderlib`, so it gets a `NOTICE.md` entry and no `engine/notices.json` entry (that file covers `engine/libs`). `python3 tools/ci/check-notices.py` covers it.

## 6. Design constraints & decisions

**D1 Files** (ledger D1, D2).

| New file | Holds |
|---|---|
| `engine/core/terrain/TerrainQuality.{h,cpp}` | Tier table, platform tiers, resolution, shipped mask (as `ShadowTiers`) |
| `engine/core/terrain/TerrainLayers.h` | `terrainLayerChannel`, `selectLayers` (C++ mirror of R3, used by `sample_terrain`, tested against the shader) |
| `engine/core/terrain/TerrainMaterialCost.{h,cpp}` | The fetch formula for the readouts |
| `engine/core/terrain/TerrainHoles.{h,cpp}` | `isHole`, lattice open-square and sample rules, coverage, region rasterization |
| `engine/core/terrain/TerrainMacroNormal.{h,cpp}` | The bake |
| `engine/core/terrain/TerrainDetailBuilder.{h,cpp}` | CPU half of RS:5481-5726, moved, with the height-in-alpha of R6 and the cooked-slice load |
| `engine/core/subsystem/RenderSystemTerrain.cpp` | The GPU half of the array, variant inputs, hole binding, block fill, shadow cut (precedent `RenderSystemShadows.cpp`) |
| `engine/shaderlib/includes/terrain_select.glsl`, `terrain_macro.glsl`, `terrain_holes_fs.glsl`, `terrain_height_blend.glsl` | Shader parts |
| `editor/window/TerrainQualityPanel.{h,cpp}` | The Quality widget and cost strip, shared by Inspector, Terrain window and Project Settings |

`RenderSystem.cpp`, `MeshSystem.cpp`, `TerrainEditWindow.cpp`, `InspectorWindow.cpp`, `WorldTools.cpp` take one-line hooks only (§8).

**D2 Variants.** One shader source, `#ifdef`, offline (no splicing). New Lit defines, appended to `defineCodes` so existing
basenames do not change; 30 entries of 64 once R54 widens them, more if another programme appended first:

| Define | Code | When |
|---|---|---|
| `TERRAIN_QUALITY_MEDIUM`, `TERRAIN_QUALITY_LOW` | `Qm`, `Ql` | Effective tier |
| `TERRAIN_LAYER_LIMIT` | `Ll` | High with the limit on |
| `TERRAIN_BLENDMAPS_1`, `TERRAIN_BLENDMAPS_2` | `B1`, `B2` | R7 |
| `TERRAIN_EXTENDED_LAYERS` | `Ex` | More than 9 layers |
| `TERRAIN_MACRO`, `TERRAIN_MACRO_NORMAL` | `Mv`, `Mn` | R9, R14 |
| `TERRAIN_RESAMPLE`, `TERRAIN_CLIFF`, `TERRAIN_HEIGHT_BLEND` | `Rs`, `Cl`, `Hb` | R10, R11 (any layer not Auto), R12 |
| `TERRAIN_DEBUG_VIEW` | `Dv` | Editor overlays; never collected for an export |

`pbrVariantMatches` (RS:3270) and the reload hook (RS:14811-14826) learn them through one helper. Medium and Low never define the hex
or stochastic paths. Holes are a **property bit** instead of a Lit define because every shader type and library shader needs them, and Lit
defines reach only non-manifest colour shaders (RS:3907). G-buffer takes `Thl` and two new tier bits, `Tqm` and `Tql`, so SSR on a Low terrain
stays within budget; it ignores Macro Variation and Macro Normal, as it ignores hex today.

The programme's property bits are names plus an order among our streams, not reservations (D19), the one table T4, T5 and T6 cite. Each bit
takes the next free one of its type when it lands, in landing order; pixel art (`Pxs`), Doriax (`Dlo`, `Occ`, `Uv2`) and others are appended the same way. Masks
are `uint64_t` (`ShaderBuildTypes.h:17`). The spec that lands a bit raises `getShaderPropertyCount` to cover it, adds a case to `getShaderPropertyName` and to
`ShaderVariantCompiler.cpp`'s definitions, using `shaderPropertyBit`, adds one test pinning the number it got (the form of draw-perf's `ShaderKey.DirectionalOnlyIsMeshBit36`), and bumps
the shader cache by one (R56).

| Order | Bit | Spec | Define | MESH | DEPTH | GBUFFER |
|---|---|---|---|---|---|---|
| 1 | `Thl` | T3 R20 | `TERRAIN_HOLES` | yes | yes | yes |
| 2 | `Tqm`, `Tql` | T3 | the Medium and Low Lit defines above | no: Lit defines | no | yes |
| 3 | `Cin` | T5 F3 | `HAS_COMPACT_INSTANCE` | yes | yes | yes |
| 4 | `Wnd` | T5 F8 | its wind define | yes | yes | yes |
| 5 | `Dth` | T5 F6 | its dither define | yes | yes | yes |
| 6 | `Vox` | T6 R49 | `HAS_VOXEL` (G-buffer only: Lit defines do not reach it) | no | no | yes |
| 7 | `Twd` | T4 D4 | `HAS_TERRAIN_WORLD` | yes | yes | yes |

**D3 Hole map: its own R8, not a blend map's alpha.** A blend alpha is free (no slot, no fetch) and was the serious option. Rejected:
- blend maps may be absent or bound black (RS:5819-5834: a map not covered by the array binds `emptyBlack`), and that binding would need
  splitting;
- its resolution is the paint's, not the terrain's;
- blend maps may be render targets or be replaced by `setBlendMap`, which would lose every hole silently, and third-party RGBA splat
  maps carry a fourth weight there;
- S14 block-compresses blend maps while holes need exact CPU-readable texels, so all of blend 0 would go uncompressed and retained;
- every paint writer would have to preserve it.

R8 costs a slot only in a terrain that has holes, and gets it from a map rarely set on a terrain (R21). It also lets T4 keep one mask per
tile with no edge filtering. Alpha goes to layers 10-12 instead (R15), inverted, which has none of these problems because a wrong value only
tints.

**D4 Slot table, after T3** (worst case: all lighting):

| Variant | Base | Lighting | Terrain | Total |
|---|---|---|---|---|
| High, painted, post colour, holes | baseColor, MR, normal = 3 | 1 + 3 + 1 + 2 + 1 = 8 | 3 + 1 + hole = 5 | 16 |
| High, painted, no post colour, holes | 4 (with emissive) | 7 | 5 | 16 |
| Medium, up to 6 layers, holes | 3 or 4 | 8 or 7 | 4 | 15 |
| Low, up to 3 layers, holes | 3 or 4 | 8 or 7 | 3 | 14 |
| Macro Normal | uses the normal slot | | | unchanged |

**D5 Seam with T1.** T1 owns `TerrainMapStore`, the operation engine, undo and tools. `TerrainMapStore` reads and writes hole rectangles in texels,
with 0 and 255 only. `TerrainOps` calls `rasterizeRegion` (`TerrainHoles.h`) and reports R27's result. T1's surface query returns
`isHole`; T3 replaces the listed direct calls of §3 only where T1 has not already moved them. If T1's tool names differ when
T3 starts, follow T1 and record it in Deviations.

**D6 Normals.** Per-pixel normals from the height map in the fragment stage would take a new slot (`u_heightMap` is vertex only) and 4 reads per
pixel, on a full budget. Rejected. A baked map costs 1 read, uses a slot that already exists, and removes the LOD-dependent shading. Vertex normals stay
the default and the only option at Low.

**D7 Sampling the hole.** `texelFetch` with no filter gives a texel-exact edge, identical on CPU and GPU, testable bit for bit. The cost is a
stair-stepped edge at texel scale; T6's voxel surface covers entrances, and the default resolution equals the height map's. A smoothed edge would
disagree with collision and has no exact CPU form.

**D8 S14's array exclusion.** Lifted without touching S14's format support: a slice is an ordinary 2D texture to the cook, so `Ktx2.cpp:68` keeps
rejecting layered files and `createTextureLevels` takes the concatenation. All slices are forced to one format per family; S14's per-type tables
do not apply. Normals stay RGB, as S14 Deviation 18 says.

**D9 Selection is computed per pixel from the blend reads.** No control map, per the brief and D13. A small insertion over the weights in
`terrain_select.glsl`; layers are then read in a loop over the kept indices. Uniform arrays are indexed dynamically, which GLES3 allows. The note at
TF:520-522 about local-memory arrays applies to the picked weights, which stay out of a local array where registers show it.

**D10 Sketches.**

```cpp
namespace jasperengine::terrain {
    enum class TerrainQuality : int { HIGH, MEDIUM, LOW };   // stored as ints: append only
    struct HoleMapView { const uint8_t* texels = nullptr; int width = 0, height = 0; };
    bool isHole(const HoleMapView& map, float u, float v);
    // squares and samples of an N x N lattice over the terrain, mirrored as the collider's
    void openSquares(const HoleMapView&, int samples, float mirrorX, float mirrorZ, std::vector<uint8_t>& open);
    void noCollisionSamples(const std::vector<uint8_t>& open, int samples, std::vector<uint8_t>& out);
    void navHoleCells(const HoleMapView&, int samples, std::vector<uint8_t>& cells);
    // R27's texel-centre rule: writes 0 (cut) or 255 (fill) through T1's store, returns the changed count
    int rasterizeRegion(TerrainMapStore& store, const Shape& region, uint8_t value, TexelRect* changed);
    struct LayerChannel { int map; int channel; bool inverted; };
    LayerChannel terrainLayerChannel(int layer);
    struct MaterialCost { int flat, slope, worst, textureSlots; };
    MaterialCost materialCost(const TerrainMaterialInputs&, TerrainQuality);
}
```

```glsl
// includes/terrain_holes_fs.glsl (texture slot TextureShaderType::TERRAINHOLES, sampler nearest, clamped)
void terrainDiscardHole(){
    ivec2 size = textureSize(sampler2D(u_terrainHoles, u_terrainHoles_smp), 0);
    ivec2 texel = min(ivec2(floor(v_terrainHoleUV * vec2(size))), size - 1);
    if (texelFetch(sampler2D(u_terrainHoles, u_terrainHoles_smp), texel, 0).r < 0.5) discard;
}
```

**D11 Implementation order** for one implementer, each landing separately: (1) move the array code, golden first (AC1), far-read skip;
(2) tiers, cap, budgets, readouts; (3) holes in shaders, then each consumer; (4) shadow cut; (5) material features;
(6) layers 10-12; (7) compressed arrays; (8) UX and agent surface. T1's foundation lands before any of them (T1 §8), then T3 runs in
parallel with T1's rest, T2, T5 and T6, and T4 lands last. Steps 1, 2, 4, 5, 6 and 7 need only that foundation; hole authoring in step 3 and
the overlays of step 8 need T1's rest (`edit_terrain`, `visualize_terrain`). Until it lands, `TerrainHoles` is driven directly by tests and `set_properties` on `holeMap`.

## 7. Acceptance criteria

A GPU test is listed with its backends: **G** is glcore on llvmpipe, **V** is Vulkan on lavapipe, both on this host; goldens are written on G and must
match on V (`docs/testing.md:643-644`). Every test named below is new unless it names an existing file. **Red** says what proves the test can fail: for
a mutation, disable the named line and show the failure, and record the run.

| AC | R | Test | Backends | Criterion | Red |
|---|---|---|---|---|---|
| AC1 | R4, R6 | `TerrainMaterialsGpu.LegacyHighMatchesPreS55` | G, V | Four scenes (3 colour layers; 4 PBR layers with a cliff; hex and stochastic; post colour with shadows), 640x360. **Golden drawn at the commit before any render change.** G: 0 pixels differ by more than 1/255, at most 0.01 % by 1/255. V: within the Vulkan-to-glcore difference measured on that same pre-change commit, recorded in the test | Change `TERRAIN_HEIGHT_CONTRAST` to 5: fails |
| AC2 | R4, R7 | `ShaderZeroCost.DefaultVariantsUnchanged` (existing) | CPU | Only the 4 terrain rows of the list are regenerated, once, for the far-read skip; all other rows unchanged; each T3 define and bit off gives the same hash | Add a line to `mesh.frag` outside a guard: fails |
| AC3 | R3 | `TerrainLayerSelect.Continuity` | CPU | 10,000 weight pairs across each swap: the kept shares change by at most 2x the input step; the sum and the base weight are conserved | Hard top-K without the subtraction: fails |
| AC4 | R3 | `TerrainMaterialsGpu.LayerCapSeamless` | G, V | A ramp through 5 layers at each tier: no neighbour pixel step above 6/255 beyond the High render's own largest step; golden | Same mutation in `terrain_select.glsl` |
| AC5 | R3 | `TerrainMaterialsGpu.ShaderMatchesCpuSelect` | G, V | `TERRAIN_DEBUG_VIEW` readback of drawn layer count matches `selectLayers` at 64 points | Off-by-one K in C++ |
| AC6 | R1, R8 | `TerrainMaterialsGpu.FetchBudgets` | G, V | `fetch_cost` readback on `Terrain Nine` with cliffs and holes: maximum over every pixel ≤ 23 at Medium with holes and Macro Normal, ≤ 7 at Low, ≤ 6 at Low without holes, ≤ 5 at Low with 3 layers and holes | High on the same scene exceeds each (the test asserts it) |
| AC7 | R8 | `TerrainMaterialCost.MatchesShader` | G, V | `materialCost` and the readback agree at flat, slope and worst points at each tier | Mutate a count in the formula |
| AC8 | R5, R6 | `TerrainMaterialsGpu.LowHeightAndNormal`, `TerrainDetailBuilder.PbrColourRgbUnchanged` | G, V; CPU | Low golden shows height contact without a surface read and no seam at the swap; RGB bytes of every level identical to the old builder | Clear the alpha packing: golden fails |
| AC9 | R2, R34 | `TerrainQuality.Resolution`, `TerrainQuality.ClampsToShipped` | CPU | Each order case; an unshipped request clamps and warns once | Remove the clamp |
| AC10 | R34 | `ShaderVariantCollectorTerrain.TiersReachable`, `.AttachedVolumeShipsHoles` | CPU | For a project at Low on Android: the Low keys are present and High absent; after a script mention, all three; `TERRAIN_DEBUG_VIEW` never; a terrain with no map and an attached Automatic volume has `Thl` keys | Remove the environment bit |
| AC11 | R32, R31 | `ShaderLibraryMatrix` (existing, new cases) | transpile, 6 backends | Worst case of every tier compiles; fails above 16 fragment textures | Add a texture to the case: fails |
| AC12 | R33 | `TerrainMaterialsGpu.BlockSizes` | G, V | Each define set uploads at its reflected size with no sokol validation error; no new fragment block in reflection | Upload `sizeof` for the extended block |
| AC13 | R9 | `TerrainMaterialsGpu.MacroVariation` | G, V | Golden per mode; Low's vertex path within 3/255 of fragment at the fixture's scale; Off identical to AC1; a fixed 0 fetch delta in the readback | Set strength 0: differs from golden |
| AC14 | R10 | `TerrainMaterialsGpu.DistanceResampling` | G, V | Golden; readback: past `resampleFar` exactly 1 read per map; crossfade continuous | Skip the near read early: band shows |
| AC15 | R11 | `TerrainMaterialsGpu.CliffProjection` | G, V | Golden at Medium and Low on a cliff; readback ≤ tier's reads per map; Flat Only reads no side plane | Keep the side read in Flat Only: count fails |
| AC16 | R12 | `TerrainMaterialsGpu.HeightBlendSharpness`; `TerrainShaderSources.HeightBlendNotice` (reads the include and `NOTICE.md`) | G, V; CPU | Golden; 0 gives AC1's output; copyright header present and `NOTICE.md` names Terrain3D | Remove the entry |
| AC17 | R13 | `TerrainLayers.FromMaterialKeepsSampling` | CPU | Hex and cliff fields survive | Current code (TC:218-225) fails: **red before the fix** |
| AC18 | R14 | `TerrainMacroNormal.MatchesCentralDifferences`, `TerrainMaterialsGpu.MacroNormal` | CPU; G, V | Baked normal within 1/127 of the height's central difference; golden; no change of shading between two LOD distances where vertex normals change | Bake with a wrong delta |
| AC19 | R15 | `TerrainLayers.ChannelMapping`, `TerrainMaterialsGpu.TwelveLayers` | CPU; G, V | Mapping table; 12 flat layers each draw their colour; an opaque map with 12 layers draws layers 10-12 at 0 | Non-inverted alpha: every terrain goes wrong |
| AC20 | R16 | `TerrainDetailBuilder.CookedSlicesConcatenate`, `TerrainMaterialsGpu.CompressedArray` | CPU; G (BC7), V (BC7) | Levels equal expected sizes; PSNR of the array against raw ≥ 38 dB on colour; all slices one format | Mix formats: refused with an error |
| AC21 | R16, R38 | `test_terrain_materials.py::test_export_ships_cooked_slices` | Linux export, G | Index has the slices, sources are absent, the player draws within AC20's PSNR, `detail_array.bytes` matches R38 ±5 % | Setting off: sources ship |
| AC22 | R16 | `ANDROID_PROJECT=terrain-materials tools/build.sh android` | build only | Builds with ASTC slices, no emulator here (see §9) | Not run here; emulator command in §8 |
| AC23 | R31, R20, R1 | `GATE=terrain-materials tools/build.sh web-gate` with `tools/test-projects/terrain-materials`, `tools/web/gates/terrain-materials.json` | WebGL2, headless Chrome | Low tier, hole, compressed slices: centre pixel is the sky seen through a hole, a second is painted ground; no console error | Fill the hole: centre pixel fails |
| AC24 | R17 | `TerrainShadowsGpu.OffscreenRidgeCastsShadow` | G, V | Ridge behind the camera, sun behind the ridge: the visible ground is in shadow at the expected pixel. **Run first on unchanged code: it must fail there, which confirms §3.** | That run |
| AC25 | R17 | `TerrainShadowsGpu.VisibleShadowsDoNotMove`, `.OffRestoresOldBehaviour`, `.NoAllocationWhenOff` | G, V | A scene with no off-screen caster matches AC1 with the cut on; Off matches the red run; no buffer created at Off or with shadows off | Select the cut with a different LOD: first test fails |
| AC26 | R18, R19 | `TerrainHoles.TexelRule` | CPU | Edges, corners, out of range, 1x1 and non-square maps, and agreement at 10,000 random points between `isHole` and a GPU readback dumped by `TerrainHolesGpu.CpuMatchesGpu` | Round instead of floor |
| AC27 | R20 | `TerrainHolesGpu.EveryPassCuts` | G, V | Same hole in colour, shadow map, SSAO depth, depth pre-pass, G-buffer, library shader: each readback's hole texels are exactly those of the map; golden | Remove `Thl` from depth: shadow stays solid |
| AC28 | R20, R21 | `TerrainHolesGpu.NoHoleNoCost` | G, V | No map: key without `Thl`, no binding, no discard in the source, base occlusion still drawn; with a map: occlusion gone, 16 slots | Bind unconditionally |
| AC29 | R22 | `JoltShapeBuilder.HoleLatticeRule` in `editor/tests/engine/physics/JoltShapeBuilderTest.cpp` | CPU | Exact samples for 1x1, 2x2, 3x3, edge, corner and mirrored holes against Jolt's `IsNoCollision` | Any-adjacent rule |
| AC30 | R22, R24 | `JoltShapeBuilder.HolesNestInsideDrawn` | CPU | 10,000 down rays: no drawn-ground point lacks a collider hit; every drawn-hole point more than one collision cell from the edge has none | Remove the rule |
| AC31 | R23, R24 | `NavBake.TerrainHoleExcluded`, `NavBake.HoleChangesTileHash` in `engine/tests/core/navigation/NavBakeTest.cpp` | CPU | No path crosses; every navmesh point is on drawn ground; only touched tiles rebake | Skip the hash |
| AC32 | R25 | `MeshRaycast.TerrainHoleMisses` in `engine/tests/core/picking/MeshRaycastTest.cpp` | CPU, scalar and SIMD runs | A ray down a hole misses and a mesh below it is hit; `ignoreHoles` hits; S26's 1e-4 holds outside | Remove the check |
| AC33 | R26 | `TerrainHolesConsumers.Foliage`, `SplineSystem.ConformOverHole`, `SplineSystem.InstantiateDropsOverHole`, `ParticleCollision.TerrainHole`, `NavigationSystem.GridHole` | CPU | Each row of R26 | Mutate `isHole` to false: each fails |
| AC34 | R27, R29 | `TerrainHoles.RasterizeRegion`, `TerrainHoles.HoldKeepsTheMap`, `test_terrain_holes.py::test_ops_semantics` | CPU; MCP, G | Texel-centre rule; idempotence; `dry_run`; one undo; fill restores byte-identical heights; first cut and last fill reload the variant; undo restores; with `holeMapHolds` above 0 a last fill keeps the map and reloads nothing | Skip removal: map stays |
| AC35 | R28 | `test_terrain_holes.py::test_refresh_after_cut` | MCP, G | After a cut: `sample_terrain` `collision_gap`, foliage count and `pick_at` all change without a reload | Skip the collider refresh |
| AC36 | R30, R41 | `test_terrain_holes.py::test_brush_and_overlay` (xdotool on the harness's X, screenshots to evidence) | MCP, G | Stroke cuts, Ctrl fills, overlay shows it, warning on a small hole, one undo per stroke | Visual check of the screenshots recorded in the report |
| AC37 | R39, R40 | `test_terrain_materials.py::test_ui_readout` | MCP, G | `get_inspector_layout` lists the rows; the strip's numbers equal `sample_terrain`; changing a layer updates them; undo works | Stale readout |
| AC38 | R42, R44 | `test_terrain_materials.py::test_preview_and_overlays` | MCP, G | Preview Low draws without saving (project file unchanged); `visualize_terrain quality=low` leaves state unchanged; overlays' maxima match AC6 | Leave preview on |
| AC39 | R43, R45, R46 | `test_terrain_materials.py::test_agent_fields`, group check | MCP, G | Every field of R43; tiers set by `set_properties` and `set_project_settings`; invalid value refused atomically; the `terrain` group hidden until `tool_groups` enables it | Invalid value accepted |
| AC40 | R47 | `tools/mcp/gen_tool_docs.py --check`, `tools/mcp/tool_budget.py --check tools/mcp/budget.json`, doc test | CPU | Pass | Stale docs |
| AC41 | R48 | `LuaTerrainMaterials` in `engine/tests/core/script/`, doc test of Lua blocks | CPU | Every member reads and writes; T1's `isHoleAt` matches `TerrainHoles`; `setTerrainQuality` takes effect | Binding removed |
| AC42 | R49, R50 | `StreamComponent.TerrainMaterialsRoundTrip`, `StreamComponent.OldTerrainResavesIdentical`, `GeneratedScenes` (existing), `Compat.CorpusRoundTrip` (existing), reflection-parity fixture | CPU | Every field round-trips by Stream, Factory and runtime codec; an old scene re-saves byte-identical; `jasperengine-editor reflect --emit-runtime-fields` leaves no diff | Encode a default: fails |
| AC43 | R51, R53 | `test_terrain_materials.py::test_project_defaults` | MCP, G | Old project: High everywhere and no key written; new template: table of R51; new terrain has the limit on | Change the default |
| AC44 | R52 | `TextureTypeInference.HoleMapStaysReadable` in `editor/tests/texture/TextureTypeInferenceTest.cpp` | CPU | Cooks to uncompressed R8; CPU pixels present in a cooked build | Remove from the list |
| AC45 | R35, R36, R37 | `tools/perf/terrain_tiers.py` (new; uses S15's timers), `TerrainHolesBench` | Host, llvmpipe | Each ratio and time, 5 runs, median; results in `terrain-evidence` | The High run is the control |
| AC46 | R35 | `docs/specs/checklists/T3.md` | Campaign GPU and Android: **manual, not on this host** | Same ratios on the RTX at 4K; Low ≤ 4 ms on the phone; recorded when run | n/a |
| AC47 | all | `tools/check-changed` per commit; before the push, the full suite (R55), then `tools/build.sh web`, `android`, `tools/smoke-test.sh`, `GRAPHIC_BACKEND=vulkan` unit tests | Host | Green | n/a |
| AC48 | R54 | `MaterialPbrDefines.BitsAreSixtyFourWide` in `engine/tests/core/texture/` | CPU | The three functions and `litDefineBits` are `uint64_t`; every table entry round-trips through `getDefineBit`, `getDefineBits` and `getDefinesFromBits`; T6's defines take this past bit 31 | Leave `litDefineBits` at `uint32_t`: fails |
| AC49 | R57 | `ShaderVariantCollectorTerrain.DloTwinsCoverTiers`, `TerrainMaterialsGpu.TiersUnderDlo` | CPU; G, V | Every T3 tier and `Thl` key with `Dlo` has its general twin; a sun-only terrain draws the same as its twin within the strict tolerance. Runs once `draw-perf` is on the tree, by whichever lands second | Remove the twin for a tier key |
| AC50 | R20, D2 | `ShaderKey.TerrainHolesBitIsPinned`, `.TerrainTierBitsArePinned`, and for G-buffer `TerrainHolesGpu.GBufferAoMatchesColourWithHoles` | CPU; G, V | Each bit's name maps to the number it got at landing; a `Thl` terrain's G-buffer has no occlusion map, once `ssr-probes` is on the tree, by whichever lands second | Set `g_occlusion` with `Thl`: AO differs |
| AC51 | R58, R47 | Cases in `tools/mcp/eval/cases.py`, run as T1 AC74 says (`-k terrain_`; lent accounts, else unverified): `terrain_cut_hole_check` (guard; "cut a cave mouth and check a ball falls in": `edit_terrain cut_holes`, then `sample_terrain` `collision_gap`), `terrain_mobile_budget` (bad; "why is this slow on phones": `sample_terrain` `info.material`, `visualize_terrain fetch_cost`, must not only list materials), `terrain_set_quality_tier` (guard; "use Low on Android": `set_project_settings graphics.terrainQuality.android`) | eval | Each fails on a surface from before T3's text | That surface |
| AC52 | R59, R12 | `python3 tools/ci/check-notices.py`; `python3 tools/check_raw_writes.py` (no new raw write: the hole map goes through T1's writer) | CPU | Pass | `--self-test` |

## 8. Dependencies & conflicts

**Depends on**
- **T1**: the operation engine, `TerrainMapStore`, surface query, `edit_terrain`, `visualize_terrain`, collision refresh, undo. See §6 D5. Landing order is T1 §8's: T1's foundation first, then T1's rest, T2, T3, T5 and T6 in parallel, then T4 last.
- S11 (Lit defines, hex), S14 (cook, encoders, CPU-readable slots), S15 (timers), S19 (bake), S26 (picking), S30 (runtime codec), S34 (lite), S10 (cascade fit).

**Blocks and seams**
- **T1** must: store hole rectangles; call `rasterizeRegion`; return R27's result; include `isHole` in the surface query; set `ignoreHoles` on brush rays; call
  `terrainLayerChannel` for paint; grow a collider refresh by one cell and apply R22; re-bake Macro Normal from dirty rectangles; give Create Terrain the limit on (R53);
  host R44's overlays.
- **T4**: a hole map is per tile, with no filtering, so tile edges agree. `TerrainHoles` takes a view, and tiled stores supply one per tile. Each tile's collider and nav lattice apply
  R22 and R23; the rim rule crosses tile edges only if the lattice does. The tier is global, so one variant across tiles, and `Thl` is per World (T4 D14: a tile with no holes binds a shared solid texel). `TerrainDetailBuilder` keys by slice, so tiles
  can share one array. The far overview should draw at Low. Shadow cut per tile.
- **T5**: the foliage origin test comes through T1's query. `HoleMapView` is a plain view over texels, so a job's copied texels use the same `isHole`; a hole edit reaches T5 as T1's dirty event. R26's foliage row is what T5's `exclude.holes` default must keep. No spec of this programme bumps the scene version (R50, T1 R81, T4 R54, T5 R60, T6 R80).
- **T6**: cuts a footprint with `cut_holes` and an R8 mask, and fills it with the same mask (no ownership tag in the map). T3 owns T6's two requests: `holeMapHolds` (R29, R49) and the collector rule (R34). The rim of R22 means an entrance should be at least 3 collision cells wide
  and the voxel surface should overlap the hole's edge by one texel. `sample_terrain` reports `collision_gap` for checking. Holes never make walls.

**Hooks in hotspots** (D2): `RenderSystem.cpp`: 3623-3626 inputs, 3908-3916 defines, 5481-5726 moved out, 5752-5785 block fill call, 5787-5853 hole bind call, 6539-6552 block offset, 9023 and
`terrainViewData` call for the shadow cut, 14811-14826 reload. `RenderSystemShadows.cpp`: `prepareCameraShadows` calls cut selection. `MeshSystem.cpp`: raycast crossing and closest point, foliage origin. `InspectorWindow.cpp`
9801 region: one panel call. `TerrainEditWindow.cpp`: brush modes via T1. `WorldTools.cpp`: `terrainSample`, `terrainInfo` call a new `TerrainMaterialInfo` helper in `editor/mcp/`. Shared hotspots with other
streams: `terrain_fs.glsl`, `terrain_vs.glsl`, `TerrainComponent.h`, `MaterialPbr.cpp` table, `Render.h` texture enum (append only), `tools/mcp/budget.json`, `NOTICE.md`.

**Verifiable on this host (8 cores, software rendering, no KVM, no Windows or macOS).** There is no CI, so a row marked No is unverified until a person runs its command.

| Check | Here? | Elsewhere: command a person runs |
|---|---|---|
| glcore on llvmpipe, Vulkan on lavapipe: all GPU tests and goldens | Yes | |
| GLES3 path actually executing: WebGL2 in headless Chrome (AC23) | Yes (software) | |
| Transpile for Metal, D3D11, GLES3 (`ShaderLibraryMatrix`) | Yes, compile only, never drawn | |
| Android APK build | Yes, build only; no emulator (no KVM), so ASTC draw is **not** verified here | On a host with `/dev/kvm`: `ANDROID_PROJECT=terrain-materials ANDROID_ABIS=x86_64 tools/build.sh android`, then `tools/android-emulator-test.sh` |
| Windows build and CPU suites | **No** | With `vcvars64`: `cmake -S engine -B build-engine-gl -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl -DJASPERENGINE_TESTS=ON && cmake --build build-engine-gl --target jasperengine-core-tests && build-engine-gl/tests/core/jasperengine-core-tests.exe --gtest_filter=Terrain*-*Gpu*` |
| macOS build; real Metal draws | **No** | `cmake -S engine -B build-engine -G Ninja -DCMAKE_BUILD_TYPE=Release -DJASPERENGINE_TESTS=ON && cmake --build build-engine --target jasperengine-core-tests && build-engine/tests/core/jasperengine-core-tests --gtest_filter=TerrainMaterialsGpu*:TerrainHolesGpu*:TerrainShadowsGpu*` |
| iOS build; real D3D11 draws | **No**: no automated pixel test exists | iOS: `python3 tools/xcode_sync_engine.py --check`, then `terrain-materials` from the Xcode workspace. D3D11: `docs/specs/checklists/T3.md` on Windows |
| Real GPU timing, mobile timing | **No**: AC46, manual | `docs/specs/checklists/T3.md` |
| `fixture-unity` MCP tests | No: they skip; T3's tests use their own project (the default MCP suite) | |

## 9. Risks & defaults

| Risk | Default |
|---|---|
| **Software rendering misleads on cost.** llvmpipe's fetch and ALU balance differs from a phone | Fetch budgets are structural and exact (AC6). Ratios (AC45) are a guard, the absolute target is manual (AC46). If a ratio misses by up to 10 % on the first measure, the implementer reports the measure and the reason, and does not change the number |
| Rim mismatch feels wrong in a game | Rule R24 is conservative by design and cell size is a project choice. If playtests dislike it, a later `collisionSamples` doubling shrinks it; R22 does not change |
| Discard in the depth pass on tile-based GPUs | Only in `Thl` variants, budgeted by R36. A terrain with holes pays; nothing else does |
| Base occlusion silently gone with holes | Warning in the Inspector and `sample_terrain.features`. Layers keep theirs |
| The per-variant cost of new Lit defines | S11 measured a variant with extra defines at +0.3 % once registers were controlled (S11 AC11 perf3). Medium and Low remove the hex and stochastic paths for the same reason |
| Selection loop register pressure on some drivers (TF:520-522, S11 perf3) | Pick weights, avoid a local array where measured worse; measure on the campaign machine when available |
| WebGL2 `textureSize` plus `texelFetch` on R8 | Gate AC23 runs them; R8 sampling is core in ES3 |
| Two variants may already exceed 16 slots (inferred, untested): a painted terrain with the 2D shadow atlas (`includes/lighting2d.glsl:79`), and the colour path with post colour and a base normal map (emissive is dropped only on the PBR path, `mesh.frag:73`) | `ShaderLibraryMatrix` adds both. If one overflows, that variant leaves the base emissive out too, and the implementer records it in Deviations |
| Tier change is a hitch (variant reload, array rebuild) | Stand-in draws meanwhile (RS `needVariantReload`). Documented as a settings-menu operation, not per-frame |
| Web raw path of a big array still takes heap | New projects compress by default. Old projects keep raw for fidelity (R51) and see the setting's hint |
| Height in the alpha of a PBR colour slice breaks a future reader of that alpha | The shader ignores it today (TF:579-580); a comment and AC8 pin it |
| T1 names differ at implementation | D5: follow T1, record in Deviations |
| Off-screen shadows change the look of old scenes that had the bug | Accepted as a bug fix, only where such a caster exists; Off restores it. Listed in release notes |

**Defaults taken.** Holes cut in the fragment stage only. Low on Android and iOS, Medium on web, High on desktop, only for new projects. Limit Layers Per Pixel on for new terrains. Macro strength 0.35 when on. Layer count 12 at most, no 16:
a fourth blend map has no slot. Base Map macro multiplies with a neutral at 0.5 grey.

## Deviations

- Upstream revision, 2026-11-04: `set_terrain_maps` keeps its schema (R45); bits are names plus order (D2); guide text goes in `docs/mcp/guide/terrain.md`, not the agent guide, with eval cases (R47, R58).
- **R20: `mesh.frag` discards at the end of `main`, not the top.** A discard at the top of the lit shader crashed llvmpipe (Mesa 23.2, LLVM 15) in its fragment JIT: a `vpgatherdd` whose index register held the bits of 1.0. Measured: the crash needs llvmpipe's native 256-bit width (`LP_NATIVE_VECTOR_WIDTH=128` draws correctly) and the discard ahead of the lit code (at the end it draws correctly); `textureLod` in place of `texelFetch`, and dropping `textureSize`, did not change it. The cause is inferred to be the JIT, not proven. A hole pixel so shades before it goes, which is R36's colour cost on hole pixels only. `depth.frag`, `gbuffer.frag`, `surface_depth_fs.glsl` and the library's finalizers are as specified; only the finalizers run at the end of a shader, as the spec has them.
- **R53: no `TERRAIN_LAYER_LIMIT` when a terrain has no more layers than the cap keeps** (High's 4). A cap
  that keeps every layer draws the same image without the selection, so the variant skips it, which is a
  saving in itself; and it works around llvmpipe's GL path (Mesa 23.2.1, LLVM 15), which dies on an illegal
  instruction in its fragment JIT drawing the colour-only High variant with the cap and High Lit Quality
  (`mesh_lit_LqLl_PucShwNorTerShq`), the variant every new terrain (no layers, the limit on) would draw. The
  shader is valid: glslangValidator and spirv-val accept it, lavapipe draws it, and the same crash happens at
  `LP_NATIVE_VECTOR_WIDTH=128`. `TerrainQuality.LayerLimitOnlyWhenItCuts` pins the rule,
  `TerrainMaterialsGpu.LimitThatCutsNothingDrawsTheSame` the image, and `EveryVariant/TerrainVariantGpu.Draws/*`
  draws the capped variant with nine layers on glcore at both vector widths and on lavapipe.
- **A terrain with no layer draws its base material alone:** no tier, cap, feature or Terrain view define, and
  the narrow path (`hasTerrainSurfaceLayers` is false without a layer); its holes still cut. The tiers, the
  cap, the features and the views act on layers, so the image is the same, but for Macro Variation, which
  waits for a layer, and the Holes view, which shows such a terrain's holes as gaps rather than hatched: the Inspector, `sample_terrain`'s issue `macro_without_layers` and `docs/terrain.md` say so. It
  is the cheapest path, and a workaround for llvmpipe's GL path (Mesa 23.2.1, LLVM 15), which faults in its
  fragment JIT (SIGILL, and SIGSEGV with holes) on every tier and feature variant of a terrain without layers,
  e.g. `NoLayers_medium_LitStandard` and `NoLayers_high_LitStandard_Limited_MacroNoise` of
  `EveryVariant/TerrainVariantGpu.Draws`: 44 of 180 at the default width, none with layers. sokol's validation
  reports nothing for them, and lavapipe draws all of them. `TerrainQuality.NoLayersDrawTheBaseAlone` pins it.
  At `LP_NATIVE_VECTOR_WIDTH=128` llvmpipe aborts on most variants, layers or not (not chased).
- **R20: no extra pass on a terrain.** `RenderSystem::hasExtraPass` returns false for a terrain, so the extra pass has nothing to cut; `mesh.frag` covers it if that ever changes.
- **R26: `TerrainComponent::holeMapRevision`** (not saved, not reflected) is new: `SplineSystem` rebuilds on a hole change by it, which the spec's "Spline conform" and "Instantiate" rows need and no existing flag said without also firing on a density stroke.
- **AC31:** the tests are `NavBake.*` in `engine/tests/core/navigation/NavBakeHolesTest.cpp`: `NavBakeTest.cpp`'s fixtures have a function named `terrain`, which the `jasperengine::terrain` namespace makes ambiguous. **AC33:** `NavigationSystem.GridHole` is `NavGridTest.GridHole`; `ParticleCollision.TerrainHole` is `TerrainHolesConsumers.ParticlesPassThroughAHole`; `SplineSystem.InstantiateDropsOverHole` and `.ConformOverHole` are one test, `SplineGpu.InstantiateDropsOverHole`, because pool models load on the device.
- **AC1 on Vulkan:** `legacyOptions` holds Vulkan to the cross-backend bound of the other suites (threshold 16, 3 %, mean 3), not to a difference measured on the pre-change commit: that needed a second full build of the base. The difference each run measures is printed.
- **AC29, AC30:** `JoltShapeBuilder.*` is `HeightfieldHoles.*` in `engine/tests/core/physics/HeightfieldHolesTest.cpp`: it tests engine code and needs no editor, which keeps it in the fast suite. It asks the shape itself (`IsNoCollision`, `CastRay`).
- **R17 rides on upstream's per-slot terrain cut.** The merge of origin/main brought Doriax's `RenderSystem::shadowTerrainCut` (S55 R5): each shadow slot selects its own terrain nodes, from the slot's eye for a Static Shadow Caster, else as the camera's cut filtered by the slot's frustum. T3's second per-camera cut and its buffers are gone; with Off-screen Shadows on, the non-static branch runs the CDLOD selection from the camera's eye and ranges with the slot's frustum in place of the camera's, which is R17's rule per slot. Its nodes stream through upstream's per-draw binding, so no buffer is allocated at all and `NoAllocationWhenOff` was dropped. Red control for AC24: the same test on the merged tree before this change (`terrain-evidence/T3/merge-gpu-2.log`: lit 139, shadowed 139); with it, 139 against 60.
- **R15, R33: `u_fs_terrainLayers` is packed** to three vectors and a component a layer, so 12 layers with
  every feature fit GLES3 / WebGL2's 224 fragment uniform vectors beside the heaviest lighting
  (`TerrainUniformBudgetSlow.TerrainProgrammeVariantsFitGles3`: 241 before). Roughness, metallic, occlusion
  strength and Stochastic Tiling's sharpness, contrast and rotation go in 12-bit pairs, clamped to 0-1 (the
  Inspector's range, and the shader's for the stochastic ones); the slices, PBR and Flat Only in one exact
  integer, so `flatOnly[3]` left the cliff block; the colour slice is the layer's index; a hex layer's
  rotation shares the stochastic cell size's slot, as a layer with both takes Stochastic Tiling. The other
  values stay exact. A data texture would have taken a seventeenth texture from the normal-mapped variants.
- Implementers record further deviations here.
