# LOD cross-fading for JasperEngine: research and design

Date: 2026-11-04. Research only: nothing in the repository was edited or built.

**Labels used in this report**
- **[verified]**: I, or one of the three research passes I ran, read the source or doc and cite it.
  The passes' full notes, with exact snippets, are in `/tmp/xfade-research/notes-unity-unreal.md`,
  `/tmp/xfade-research/notes-godot-bevy.md` and `/tmp/xfade-research/notes-others.md`. Repo paths
  `X:line` are JasperEngine at `e8142a81e`.
- **[verified by script]**: I checked it with my own script, `/tmp/xfade-scripts/check_bayer.py`.
- **[inferred]**: my reasoning from what was read. Nothing here was built or run on a GPU.
- **[unverified]**: widely stated, but I found no primary source.

---

## 0. Decision in one screen

- **Technique.** Use a dithered, screen-door cross-fade.
  - During a fade, both levels of the mesh are drawn opaque, depth-writing, from the same vertex
    and index buffers.
  - The two draws use one screen-space 8x8 Bayer pattern and opposite signs of one quantised fade
    value `±k` (k = 1..63, in 64ths).
  - Every pixel is kept by exactly one level. There is no blending, no sorting and no hole.
  - Geomorphing is rejected: meshoptimizer 1.3 does not expose the collapse map, so a geomorph would
    need a patched simplifier, chained levels and a per-vertex morph stream about as big as the
    vertex buffer.
- **Cost control.**
  - The discard lives only in a new shader variant, MESH bit 36 `Lfd` ("Lod Fade"). Only draws that
    are fading use it. Every other draw keeps today's pipeline and early-Z/HSR.
  - The fading draws go after the non-fading opaque queue, as Apple, PowerVR and Arm recommend.
  - The depth and G-buffer passes get no new variant:
    - the in-target depth pre-pass skips fading draws;
    - the SSAO, scene-depth and G-buffer pre-passes draw only the dominant level;
    - shadows keep their own light-relative selection and snap.
- **Defaults (Unity names).**
  - Scene settings: **Cross Fade** on, **Cross-fade Duration** 0.5 s.
  - Per mesh:
    - **Fade Mode** = Cross Fade;
    - **Animate Cross-fading** = on (time-based);
    - **Fade Transition Width** = 0.1, used when not animated and always by instanced meshes, which
      fade by distance with no per-instance state.
  - Hysteresis of 10 % on switching back to a finer level.
  - A fade snaps when:
    - the mesh was not drawn last frame;
    - its projected error budget jumped 2x in one frame (a camera cut);
    - it is under 8 px tall;
    - the variant is not ready yet;
    - it uses a custom fork.
- **Zero cost when off.** Cross Fade off, or Mesh Detail off, means no `Lfd` variant is collected,
  compiled, shipped or created, no state is updated and no extra draw is made. It costs one branch per
  mesh draw.

---

## 1. What JasperEngine has today (verified in code)

### Levels
- `MeshLodPool::simplify` (`engine/core/pool/MeshLodPool.cpp:140-205`) cuts levels 1..3 **each from
  the source** with `meshopt_simplifyWithAttributes`/`meshopt_simplify`:
  - options `meshopt_SimplifyErrorAbsolute | meshopt_SimplifyPrune`;
  - targets 1, 0.5, 0.25, 0.125 (`:18`);
  - a level that does not shrink by at least 10 % ends the chain (`:170`).
- Each level is a range of `MeshLodData::indices`. It is uploaded as `Submesh::lodIndices` and
  indexes the **source vertex buffer** (`MeshLodPool.h:19-31`).
- `MAX_MESH_LODS` = 4.

### Selection
- `RenderSystem::selectMeshLod` (`engine/core/subsystem/RenderSystem.cpp:8222-8234`) is stateless:
  ```
  allowed = threshold * (perUnit * distance + constant) / lodBias
  level   = last l whose lodError[l] * worldScale <= allowed
  ```
  There is no hysteresis.
- `computeLodView` (`:8192-8208`) makes the main view's LodView (`perUnit` = world units per pixel at
  distance 1).
- `selectMeshDraw` (`:8382-8400`) re-selects in every pass that has a LodView.
- Only the main camera has detail levels: `lodView = isMainCamera ? &rv().lodView : nullptr`
  (`:16030-16031`).
- `MeshComponent` holds `lodEnabled`, `lodBias`, `lodError[4]` and `numLods` (`MeshComponent.h:517-521`).
  Scene settings `meshLodEnabled = false` and `meshLodThreshold = 1.0` px (`SceneSettings.h:354-355`).
- Terrain and tilemaps never get levels (`RenderSystem.cpp:7218`). Picking always uses LOD0 (S26 R5,
  `docs/specs/S26-scene-view.md:144`).

### Drawing a level
- `drawSubmeshGeometry` (`RenderSystem.cpp:8409-8447`):
  - level 0 draws the submesh's own range;
  - level > 0 swaps the index buffer to `lodIndices` and draws `levels[l].indexBase/indexCount`;
  - a culled instance view draws one bucket per level (`range.lodCount[l]`).

### Instanced meshes
- `buildInstanceView` (`:8293-8379`) culls the instances, selects a level for each and sorts by
  (level, key).
- The main view and each shadow slot get their own slice. Views are rebuilt only when
  `instanceViewsDirty` is set (camera or list change; `:8239-8243`).
- `InstanceRenderData` is a `Matrix4`, a `Vector4` colour and a `Rect` (`InstancedMeshComponent.h:87-91`).
- A VAT batch already has a second per-instance stream parallel to the instance data
  (`RenderSystem.h:817-827`), which is the precedent for a per-view side stream.

### Shadows
- `shadowLodView` (`RenderSystemShadows.cpp:1032-1046`) chooses levels by shadow texels:
  `SHADOW_LOD_TEXELS`, from the light, not from the camera.
- Static caching keys on the casters (`lodBias` is in the caster revision, `:580`).
- So shadow levels already switch at different moments from the camera's levels.

### Passes
- Colour pass, main camera: opaque meshes are sorted by `sortPriority`, then distance
  (`RenderSystem.h:638-646`, `RenderSystem.cpp:16039-16081`).
  - `PIP_DEFAULT` takes the material's depth compare and write (`SokolObject.cpp:788-800`).
- The optional in-target depth pre-pass:
  - `depthPrepassEnabled`, default false (`SceneSettings.h:358`);
  - pipelines `PIP_ZPREPASS*` use LESS_EQUAL with depth write on (`SokolObject.cpp:725-748`);
  - it is drawn from the same opaque list (`RenderSystem.cpp:16086-16103`).
- SSAO depth (`drawMeshDepth`, `:9413`) and the G-buffer (`drawMeshGBuffer`, `:9999`) draw the main
  view's selection.

### Shader variants
- A submesh holds one `ObjectRender` per pass, with one shader each (`MeshComponent.h:186-201`).
- Variant bits map to defines in `ShaderVariantCompiler.cpp:365-411` (MESH), `:413-433` (DEPTH) and
  `:435-454` (GBUFFER).
- MESH has 36 bits (0..35, `ShaderPool.cpp:652`), so the next free bit is **36**. Bit 26 'Ifd'
  "Instance Fade" is a geometric shrink, not a dither (`mesh.vert:220-230`).
- The export collects every variant content can reach (`editor/shader/ShaderVariantCollector.h`).
  S32 builds shaders and pipelines asynchronously and interns pipelines; `ObjectRender::prewarmWith`
  makes another variant's pipelines on the same layout "only when that variant takes the same inputs"
  (`ObjectRender.h:96-101`, `SokolObject.h:119-133`).
- Uniform blocks may hold only float or int members (`engine/libs/shadercompiler/src/spirvcross.cc:193`).
  On GL a block is uploaded "typed after its first member" (`mesh.frag:3-7`), so the new block is all
  float.

### Existing dither
- The only discard in a mesh shader is ALPHA_MASK (`mesh.frag:271-275`, `depth.frag:61-65`).
- Interleaved gradient noise is used for the world-locked shadow cascade dither
  (`shadow_sampling.glsl:65-69`) and the uber/post dither (`uber.frag:165-167`).
- S10 accepted dither grain without TAA (`S10-shadows.md:630`). The engine has no TAA.
- A 64x64 blue-noise table exists (`engine/core/render/BlueNoise64.h`), used by the atmosphere only.

### Precision
- GLES output is made by SPIRV-Cross with fragment `default_float_precision = Mediump` and
  `default_int_precision = Highp` (`spirv_glsl.hpp:190-191`).
- `uber.frag` declares its IGN `highp` for that reason.

### Clocks
- `Engine::getUnscaledDeltatime()` (`Engine.h:696-700`) keeps running at time scale 0 and is 0 while
  paused. `Engine::setFixedDeltatime(seconds)` (`Engine.h:661`) makes it deterministic.

### UI, data, Lua, MCP
| What | Where |
|---|---|
| Inspector "Detail Levels" / "Detail Bias" | `InspectorWindow.cpp:7445-7453` |
| Scene section "Mesh Detail (LOD)" (Enabled, Threshold) | `InspectorWindow.cpp:12673-12686` |
| Reflection | `RenderReflect.cpp:191` (known keys `:501`) |
| YAML | `StreamComponents.cpp:105,200`; scene `Stream.cpp:4328,4449`; schema `StreamSchema.cpp:62` |
| Generated code | `Factory.cpp:1167` |
| Lua | `GameObjectClassesLua.cpp:1843` (mesh), `CoreClassesLua.cpp:3198` (scene) |
| MCP scene settings | `editor/mcp/tools/SceneTools.cpp:816-817` (`mesh_lod_enabled`, `mesh_lod_threshold`) |
| Render counters | the `get_render_stats` tool |

---

## 2. Survey

### 2.1 Unity

**LOD Group settings [verified]** (sources: UnityCsReference `LOD.bindings.cs`, the Manual's LOD Group
page, the ScriptReference; notes-unity-unreal §1)
- `LODFadeMode {None, CrossFade, SpeedTree}`.
- "By default, the Animate Cross-fading option is enabled, meaning that Unity performs a time-based
  transition" (Manual, class-LODGroup, 2022.3 and 6).
- `LODGroup.crossFadeAnimationDuration` is one **static, global** value. Zero or negative throws.
  The docs give no default; the commonly cited 0.5 s is **[unverified]** (it is native code).
- `LOD.fadeTransitionWidth`: "proportion to the current LOD's whole length [0-1]. Only used if it's
  not animated". The C# constructor sets 0.
- The distance-mode zone ends at the next level's threshold.

**`unity_LODFade` [verified]** (Core SRP `Common.hlsl:1672-1687`)
- `x` is **signed**: "LOD0 must use this function with ditherFactor 1..0, LOD1 ... -1..0", and
  "if LOD 0 has f = x, LOD 1 has f = -x".
- URP `LODCrossFade.hlsl`: `d = unity_LODFade.x - CopySign(p, unity_LODFade.x); clip(d);`, where `p`
  is a point-sampled screen-tiled texture.
- Built-in (2019.3+) does the same with a 4x4 mask (`UnityCG.cginc:1104-1116`).
- Instancing carries a per-instance `unity_LODFadeArray`. DOTS packs an snorm8 in the instance index
  (`UnityDOTSInstancing.hlsl:711-717`), so 8 bits of fade are enough.

**URP dithering and keyword [verified]**
- "LOD Cross Fade" is on by default and "LOD Cross Fade Dithering Type" defaults to **Blue Noise**
  (`UniversalRenderPipelineAsset.cs:495-500`).
- Dithering types (`:374-386`):
  - Bayer: 4x4, 16 levels.
  - Blue Noise: 64x64, 256 levels.
  - "2x2 Stencil": 3 full-screen seed triangles write stencil bits 4 and 8, and the colour pass
    stencil-tests instead of clipping. That is early-Z friendly, but gives only 4 levels.
- `#pragma multi_compile _ LOD_FADE_CROSSFADE` is in every pass: ForwardLit, ShadowCaster, GBuffer,
  DepthOnly, DepthNormals, MotionVectors (`Lit.shader:184,238,323,372,420,527`).
- It is all-stage because of SpeedTree's vertex morph and DOTS index packing (`UniversalTarget.cs:2975-2990`).
- Turning the setting off strips every cross-fade variant (`ShaderScriptableStripper.cs:974-989`).
- **[inferred]** Only renderers that are mid-fade get the keyword, so steady objects keep the no-clip
  variant.

**Clip placement, shadows and depth priming [verified]**
- The ForwardLit clip sits after the surface data and before lighting (`LitForwardPass.hlsl:276-279`).
- **Shadows fade** in URP and the built-in pipeline: the ShadowCaster clips
  (`ShadowCasterPass.hlsl:62-72`, `UnityStandardShadow.cginc:181-202`).
- With depth priming, the opaque pass uses depth Equal and still clips (`DrawObjectsPass.cs:198-237`).

**HDRP [verified]**
- Uses a hash of the view direction instead of a texture, "stable under camera rotation"
  (`ShaderVariablesFunctions.hlsl:219-244`).
- "varying the pattern with time confuses the TAA".

**SpeedTree mode [verified]**
- The vertex stores the next level's position, and the shader lerps by `unity_LODFade.x`
  (`SpeedTree8Passes.hlsl:112-117`). This is authored geomorph data, not something simplification
  produces.

### 2.2 Unreal

**Material and fade timing**
- The material property "Dithered LOD Transition" opts a material in [verified, UE docs].
- `FLODMask` "by default the two LODs are the ones used in a dithered LOD transition", so both levels
  are drawn [verified, API docs].
- Fade timing is "temporal LOD": two camera samples `lod.TemporalLag` = **0.5 s** apart, with the
  factor interpolated between them [verified via a third-party CVar wiki quoting 5.4.4 source].
- `foliage.DitheredLOD` = 1 and `r.HLOD.DitherPauseTime` = 0.5 s [same source].

**Early-Z**
- `r.StencilForLODDither` (default 0): "If disabled, LOD dithering will be done through clip()
  instructions in the prepass and base pass, which disables EarlyZ ... Forces a full prepass when
  enabled" [verified via wiki].

**Mobile**
- `r.Mobile.AllowDitheredLODTransition` defaults to **0** and is read-only.
- Project Settings: "Enabling this may degrade performance because rendering will not benefit from
  Early Z optimization" [verified].

**Not confirmed**
- The UE dither function (a hash of `floor(SvPosition)` with a signed factor) is **[unverified]**,
  quoted from memory; no public copy could be fetched. Whether UE shadows fade is also unconfirmed.

### 2.3 Godot 4 (master `fefec82d`; notes-godot-bevy)

**Visibility-range fade [verified]**
- It is **alpha blending**, not dithering. The fade value is distance-based: the CPU computes a
  smoothstep over the margin and stores 8 bits in the per-instance flags, in a storage buffer.
- A fading instance (< 0.999) **moves to the transparent list** (`render_forward_clustered.cpp:999-1013,1163-1173`).
- Self mode leaves both levels see-through in the band [inferred]. In Dependencies mode the parent
  stays opaque and the children blend in.
- It exists in Forward+ only. Mobile and Compatibility (GLES3/WebGL2) have none
  (`GeometryInstance3D.xml:138,142`).
- Shadows are not faded.
- The margins act as hysteresis only in Disabled mode (`renderer_scene_cull.cpp:2861-2899`).
- A screen-space IGN dither exists only as the material's "Distance Fade". Proposal
  godot-proposals#5240 asks for dithered visibility ranges; a commenter notes that "you have to invert
  the dither pattern on the two" levels.

**Automatic mesh LOD (meshoptimizer) [verified]**
- Selection is stateless by screen-space error: 1 px default, no hysteresis, no fade; each level is
  an index buffer over the shared vertex buffer (`mesh_storage.h:486-513`, the same in GLES3).
- Godot accumulates `current_error*1.5` per level "to limit the switching (pop) distance"
  (`importer_mesh.cpp:812-815`).
- **Why it does not fade:** no stated reason was found.
  - [inferred] The 1 px threshold makes the geometric pop sub-pixel.
  - [inferred] The pick is stateless at draw time.
  - [inferred] A fade would need two draws and a discard pipeline everywhere.
  - Proposal #692 notes the shadow cannot be cross-faded.

### 2.4 Bevy (main `4c344352`; notes-godot-bevy)

**How the fade value is computed [verified]**
- `VisibilityRange { start_margin, end_margin, use_aabb }` is distance-based.
- Ranges are deduplicated into a vec4 table. That table is a storage buffer when available, else a
  **64-entry uniform array** (the WebGL2 path, `mesh_view_bindings.wesl:40-44`).
- The **vertex shader** computes a level from -16 to 16 from the model origin and passes it as a
  `flat i32` (`mesh_functions.wesl:125-149`).

**The dither [verified]** (`pbr_functions.wesl:57-96`)
- A screen-space **4x4 Bayer** pattern on `frag_coord % 4`.
- The outgoing level uses `+L` and the incoming `L-16`. The sign selects the complementary half
  [complementarity inferred from the code].

**Variant and passes**
- `VISIBILITY_RANGE_DITHER` is a pipeline key set for every entity with a non-abrupt range, **for its
  whole life**, not only while fading [verified key, inferred consequence].
- Shadows are **not** dithered [verified].
- The dither runs in the prepasses that have a fragment stage. A depth-only prepass has none, so a
  fading mesh writes full depth there [inferred possible bug].

### 2.5 Others

**bgfx `examples/12-lod` [verified]**
- An 8x4 R8 stipple texture with knight's-tour thresholds and complementary uniforms: the old level
  keeps `ii < 32-f`, the new one `ii >= 32-f`.
- Simulated: no overlap and no gap.
- Timed in **frames**, not seconds. It uses the interpolated NDC position, not `gl_FragCoord`, which
  can skew the pattern [inferred]. A new target is taken only after the current fade ends. No
  hysteresis.

**Babylon.js [verified]**
- New `DitheredTileFadeMaterialPlugin` (tree at 2026-11-03): an 8x8 Bayer threshold on
  `gl_FragCoord`, with a `[lower, upper)` interval per mesh, so complementary intervals cross-fade.
- It keeps depth writes and also dithers the depth pre-pass. It explicitly does **not** handle
  shadow, standalone depth or picking passes.

**three.js, Filament, O3DE [verified]**
- three.js `LOD`: hysteresis "as a fraction of distance" (`LOD.js:113,267-272`), no fade.
- Filament: no mesh LOD system.
- O3DE Atom: hard switch by screen coverage (`Culling.cpp:966-985`).

**CryEngine 2 "dissolve"** [unverified, search excerpts of Mittring 2007]
- Per-pixel in the early-Z pass, then Z-equal, with a screen-space dissolve texture and an edge blur.

**Hoppe 1996 geomorphs [verified]**
- They render the **finer** connectivity, with each vertex lerping to its coarse ancestor through a
  surjective ancestor map.

**CDLOD [verified]**
- The morph target is implicit to a regular grid (`CDLODTerrain.vsh:59,133`). That is why it is free
  for terrain and not for arbitrary meshes.

**Tile-based GPU cost of discard [verified, official docs]**
- **Arm Mali** best practices (Issue 10, p.22, p.104): "Do not use discard ... forces a late ZS test",
  the same for alpha-to-coverage, and "render alpha-tested geometry front-to-back".
- **PowerVR**: "Do Not Use Discard", and order "Opaque, Alpha-tested, Blended".
- **Apple**:
  - "The GPU cannot perform hidden surface removal when ... a fragment shader uses the discard
    instruction".
  - Draw opaque first, then discard, then blended, and "Use the discard instruction as early as
    possible".
  - WWDC20 calls these "feedback fragments ... fragment functions containing a discard statement".
  - [inferred] The cost attaches to the **shader** containing discard, not to whether it executes.
    So a uniform branch around a discard still taxes every draw of that shader.

**Alpha-to-coverage [verified, GLES 3.0.6 §4.1.3]**
- A no-op without MSAA.
- "No specific algorithm is required", so the masks for a and 1-a are not guaranteed complementary.
- `gl_SampleMask` is not in ES 3.0.

**GLSL ES 3.00 [verified, spec text]**
- `& ^ | << >>` exist on int and uint, with matching signedness.
- The fragment default int precision is mediump (16 bits minimum); SPIRV-Cross output overrides it to
  highp.
- `gl_FragCoord` is `highp`. Flat integer varyings and `texelFetch` exist.
- **§6.4: after a non-uniform `discard`, "subsequent implicit or explicit derivatives are undefined"**
  (`/tmp/xfade-research/web/glsl_es_300.txt:6181-6187`, pdf p.87).

### 2.6 meshoptimizer and geomorph (1.3, commit `f0a684f`) [verified]

**What the API gives**
- There is **no** public output mapping a removed vertex to its collapse target. The internal
  `collapse_remap` exists per pass, is reset to identity each pass (`simplifier.cpp:2481,2513-2516`)
  and is never returned.
- Upstream: zeux, 2026-09-24, in #1095, says an API to "record the collapses that have been made" is
  "on my list". Nothing has shipped.

**Which vertices a level keeps**
- Every level's indices are a subset of the source vertices; the plain simplifiers never write
  positions.
- JasperEngine cuts each level **from the source** (`MeshLodPool.cpp:157`), so level k+1's vertices
  are not a subset of level k's [inferred]. The README says chaining "produces smoother visual
  transitions" (`README.md:674`).
- `Prune` deletes whole components, which leaves no target at all.

**So a geomorph here needs [inferred]**
1. A vendored patch that composes an ancestor map, about 10 lines.
2. Chained generation, which changes how errors accumulate and which goldens and caches apply.
3. A fallback for pruned components.
4. A morph stream of 12-24 B per vertex per transition. With halving levels the total is about the
   size of the source vertex buffer. The alternative is a 2-4 B target index plus a position texture
   read with `texelFetch` in the vertex shader. Both are legal on GLES3.
5. Draws of the finer level, for every transition, by every mesh that uses LOD. Skinned meshes would
   need the morph before skinning.

**Verdict:** the data, build time and memory cost are too high for a feature that only turns a pop into
motion. Dithering needs no vertex data at all. Revisit for hero assets if upstream ships the collapse
record.

---

## 3. Options weighed

| Option | Look | Cost | Fits JasperEngine? |
|---|---|---|---|
| **Complementary screen-space dither, discard variant for fading draws only** | Opaque, correct depth, stipple visible for the fade duration | 2nd draw per fading submesh; late-Z/HSR loss only on fading draws; 1 extra shader per base variant | **Yes, primary** |
| Uniform branch around a discard in the normal shader (Bevy-like, always-on variant) | Same | Every LOD mesh pays the discard tax forever (Apple/PowerVR/Mali decide statically) | No: breaks "disabled costs nothing" on tilers |
| URP 2x2 stencil | 4 levels, steppy over 0.5 s | 3 full-screen seed draws per frame, 2 stencil bits, clashes with stencil materials (`Submesh::stencilTested`) | No |
| UE stencil prepass + depth-equal | Good | Forces a full prepass | No (mobile) |
| Alpha-blend both levels (Godot) | See-through mid-fade (2x50 % = 75 % coverage), no self-occlusion | Transparent queue, sorting, HSR off | No |
| Alpha-to-coverage | Smooth with MSAA, nothing without | Masks not complementary, late-Z on Mali, MSAA only | No |
| Geomorph | Best (no stipple) | See §2.6 | Not now |

---

## 4. Design

### 4.1 Pattern and complementarity

**Pattern: an 8x8 Bayer rank computed in ALU from `ivec2(gl_FragCoord.xy)`**
- No texture: a lit variant already binds about 16 textures (base, MR, occlusion, emissive, normal,
  3 extension maps, SSAO, 3 IBL, shadow atlases, LUT), the GLES3 minimum.
- No float precision issue: all values are integers below 64.
- 64 levels, enough for a 0.5 s fade at 60 fps (30 frames).
- **[verified by script]** The rank is a permutation of 0..63, the standard Bayer matrix.

**Fade value**
- The CPU sends a signed integer `k` in 64ths:
  - the level faded **from** gets `+k` and keeps ranks `< k`;
  - the level faded **to** gets `-k` and keeps ranks `>= k`.
- **[verified by script]**: for every k = 1..63:
  - the two draws partition each 8x8 tile exactly;
  - `+k` keeps exactly k of the 64 pixels;
  - the kept set grows monotonically with k, so a pixel switches level only once per fade (no
    shimmer).
- Exactness needs only that both draws compute the same rank at the same pixel. They do: the same
  `gl_FragCoord` and integer maths, so this does not depend on float rounding [inferred].
- Quantising on the CPU (k = 0 or 64 means a single draw) keeps both draws on the same integer and
  never sends a fade that would draw nothing.

**Why screen space and not world or object space [inferred]**
- Complementarity is needed **per pixel**. A world- or object-space key differs between the two
  levels' surfaces (different triangles reach a pixel at different points), so the partition breaks.
- Under camera motion the stipple stays fixed to the screen while the object slides under it. That
  is acceptable for 0.5 s and does not swim within the object.
- HDRP's view-direction seed is a rotation-stable refinement, not needed without TAA.

**Bayer versus noise**
- URP defaults to blue noise for looks.
- Bayer reads as an even "screen door" at mid-fade, is exact and is the cheapest. Bevy and Babylon
  use it.
- If users object, swap `lodFadeRank` for an IGN threshold (`rank = int(ign * 64)`, `highp`); the
  sign rule and the cost stay the same.
- **Not** a per-frame animated pattern: without TAA it would sparkle.

**Backend difference**
- GL's `gl_FragCoord` origin is bottom-left; the other backends' is top-left. The pattern's phase
  differs, but its look and complementarity do not.
- Goldens are already per backend (glcore; Vulkan checked separately), so expect different mid-fade
  images on Vulkan and do not try to match them.

### 4.2 Shader changes (MESH type only)

**The new bit**
- `ShaderPool.cpp`: MESH count 36 → 37, with `case 36: "Lfd" / "Lod Fade"`.
- `ShaderVariantCompiler.cpp` `addMeshPropertyDefinitions`: `if (prop & shaderPropertyBit(36)) defs.push_back({"USE_LOD_FADE", "1"});`.
- DEPTH and GBUFFER get **no** bit (§4.4).

**`engine/shaderlib/includes/lod_fade.glsl`** (new; the GLSL is written for the engine's `#version 450`
source and is legal GLSL ES 3.00 after cross-compilation: integer and bitwise operators, `flat`
varyings):

```glsl
// (c) Eduardo Doria and contributors
// SPDX-License-Identifier: MIT

// LOD cross-fade (USE_LOD_FADE). The two detail levels of a fading mesh draw with one screen-space
// 8x8 Bayer rank and opposite signs of the same fade: +k keeps the pixels ranked below k, -k the
// others, so each pixel keeps exactly one level, opaque and depth-writing (RenderSystem lodFade).

#ifdef USE_LOD_FADE

#ifdef HAS_INSTANCING
    // a fading copy's fade, from its instance matrix (surface_vs.glsl, mesh.vert)
    flat in float v_lodFade;
    float lodFadeValue(){ return v_lodFade; }
#else
    // its own block, declared last so the variant keeps its base's slots (checked at load)
    uniform u_fs_lodFade {
        vec4 lodFade; // x: signed 64ths this draw keeps (1..63 or -1..-63); yzw unused
    } lodFadeParams;
    float lodFadeValue(){ return lodFadeParams.lodFade.x; }
#endif

// the rank 0..63 of a pixel in the 8x8 Bayer matrix: the bits of x^y and y, interleaved and reversed
int lodFadeRank(ivec2 pixel){
    ivec2 p = pixel & ivec2(7);
    int v = p.x ^ p.y;
    return ((v & 1) << 5) | ((p.y & 1) << 4) | ((v & 2) << 2) |
           ((p.y & 2) << 1) | ((v & 4) >> 1) | ((p.y & 4) >> 2);
}

bool lodFadeKeeps(){
    float fade = lodFadeValue();
    int k = int(abs(fade) + 0.5);
    int rank = lodFadeRank(ivec2(gl_FragCoord.xy));
    return (fade > 0.0) ? (rank < k) : (rank >= k);
}

#endif
```

**Where the discard goes in `mesh.frag`**
- GLSL ES 3.00 §6.4 makes derivatives undefined after a non-uniform discard, and Bayer neighbours
  differ within a 2x2 quad. `mesh.frag` still samples with implicit LOD late in `main()`:
  - occlusion `:402`;
  - SSAO `:421`;
  - emissive `:501`;
  - the IBL lambertian cube `ibl.glsl:37`.
- So the spec-safe place is **after the last implicit-derivative sample, just before the final colour
  write**, and before the early `return` of the unlit path (`mesh.frag:281-293`):

  ```glsl
  #ifdef USE_LOD_FADE
      // last: the dither is not uniform within a quad, and derivatives after a non-uniform
      // discard are undefined (GLSL ES 3.00 6.4)
      if (!lodFadeKeeps())
          discard;
  #endif
  ```

- Cost: during a fade, the pixels the dither drops are still fully shaded on the fading objects' own
  coverage.
- An optimisation, only if profiling on a Mali or Apple device shows fading draws fragment-bound:
  - make the rank quad-uniform (`lodFadeRank(ivec2(gl_FragCoord.xy) >> 1)`, a 16x16 pixel period);
  - then the discard can move right after the derivative work, before lighting, as URP does
    (`LitForwardPass.hlsl:276-279`).
  - [inferred] This is safe on real hardware, but not by the letter of the spec, which reasons per
    primitive.

**Instanced vertex shader**
- Fading copies carry `±k` in the w of the instance matrix's first column, which is 0 in every affine
  matrix. So there is no extra stream, no layout change and the vertex inputs are the same as the
  base variant's (§4.5).
- Every instanced matrix build must ignore that w:
  - `surface_vs.glsl:386`;
  - `mesh.vert:208,214`;
  - `depth.vert:104,108`. The depth passes draw dominant copies, which carry k (§4.4).

  ```glsl
  #ifdef HAS_INSTANCING
      // affine: col1.w is 0, except in a fading copy, where it carries the copy's fade
      mat4 instanceMatrix = mat4(vec4(i_matrix_col1.xyz, 0.0), i_matrix_col2, i_matrix_col3, i_matrix_col4);
      #ifdef USE_LOD_FADE
          v_lodFade = i_matrix_col1.w;   // flat out float v_lodFade;
      #endif
  #endif
  ```

  [inferred] This is free: one constant lane. It assumes instance matrices are always affine, which
  they are by construction (InstanceData transforms); the implementer must check every reader of
  `i_matrix_col*`.
- Fallback if that trick is refused: a per-view `lodFade` side stream, like the VAT one
  (`RenderSystem.h:821-825`). It adds an attribute, so the fade variant then needs its own pipeline
  layout.

### 4.3 CPU side

**Runtime state per `MeshComponent`** (never saved; about 16 bytes, no allocation):

```cpp
// detail-level fade of the main view (RenderSystem::updateLodFade); runtime only
struct MeshLodFade{
    uint8_t level = 0;        // the level drawn alone, or faded from
    uint8_t target = 0;       // == level when not fading
    float progress = 1.0f;    // 0..1 from level to target (time mode)
    float lastAllowed = 0.0f; // the error budget at the last update (camera-cut test)
    uint64_t frame = 0;       // the last main-view frame it was updated in (0: never)
};
struct LodDraw{ uint8_t from = 0, to = 0; int8_t k = 0; }; // k == 0: one draw of 'to'
```

**Selection with hysteresis** (main view, non-instanced):

```cpp
// a coarser level stays until its error passes the budget by LOD_HYSTERESIS (0.1): a mesh at a
// threshold does not swap (and fade) back and forth with every small camera move
unsigned int selectMeshLodStable(const MeshComponent& m, float allowed, float scale, unsigned int current){
    unsigned int level = selectFromAllowed(m, allowed, scale);     // today's loop
    if (level < current && current < m.numLods && m.lodError[current] * scale <= allowed * (1.0f + LOD_HYSTERESIS))
        level = current;
    return level;
}
```

**Time mode** (Animate Cross-fading on; non-instanced). It runs once per mesh per frame, in the first
main-view pass that selects it (guarded by `frame`), so the pre-pass, colour and G-buffer passes see
one plan.

```cpp
LodDraw updateLodFade(MeshComponent& m, float allowed, float scale, float screenPx, uint64_t frame, float dt){
    MeshLodFade& f = m.lodFade;
    unsigned int sel = selectMeshLodStable(m, allowed, scale, f.target);

    bool snap = !fadeAllowed(m)                         // §4.6: off, None, fork, variant not ready...
             || f.frame == 0 || f.frame + 1 != frame    // first frame, or not drawn last frame
             || screenPx < LOD_FADE_MIN_PIXELS          // 8 px tall: the pop does not show
             || (f.lastAllowed > 0.0f && (allowed > 2.0f * f.lastAllowed || allowed < 0.5f * f.lastAllowed)); // cut/teleport/zoom
    f.frame = frame;
    f.lastAllowed = allowed;
    if (snap){
        f.level = f.target = (uint8_t)sel;
        f.progress = 1.0f;
        return {(uint8_t)sel, (uint8_t)sel, 0};
    }

    if (f.level == f.target){
        if (sel != f.level){ f.target = (uint8_t)sel; f.progress = 0.0f; }   // start (any distance of levels)
    }else if (sel == f.level){
        std::swap(f.level, f.target); f.progress = 1.0f - f.progress;      // reverse smoothly, no pop
    }                                                                       // a third level waits for the end

    if (f.level != f.target){
        f.progress += dt / scene->getMeshLodCrossFadeDuration();          // unscaled dt: ends at time scale 0
        if (f.progress >= 1.0f){ f.level = f.target; f.progress = 1.0f; }
    }
    if (f.level == f.target)
        return {f.level, f.level, 0};
    int k = (int)std::lround(64.0f * (1.0f - f.progress));                 // 64ths 'level' still keeps
    if (k <= 0)  return {f.target, f.target, 0};
    if (k >= 64) return {f.level, f.level, 0};
    return {f.level, f.target, (int8_t)k};
}
```

- `dt` is `Engine::getUnscaledDeltatime()`. It is 0 while paused, so the state holds; with
  `setFixedDeltatime` it is deterministic.
- **[assumed, to verify]**: the editor's Edit mode may not advance it. If it does not, use the render
  frame's own delta there, or snap in Edit mode.

**Distance mode** (Animate Cross-fading off, and always for instanced). It is stateless and matches
Unity's "zone ends at the next threshold":

```cpp
// level l and the next one fade over the last w of l's band of the error budget
// (e_l = lodError[l] * scale, e_0 = 0); allowed is linear in distance in perspective
LodDraw distanceFade(const MeshComponent& m, float allowed, float scale, float w){
    unsigned int l = selectFromAllowed(m, allowed, scale);
    if (l + 1 >= m.numLods || w <= 0.0f) return {l, l, 0};
    float el = (l == 0) ? 0.0f : m.lodError[l] * scale, en = m.lodError[l + 1] * scale;
    float start = en - w * (en - el);
    if (en <= el || allowed <= start) return {l, l, 0};
    int k = (int)std::lround(64.0f * (en - allowed) / (en - start));      // share of l, 1 -> 0
    if (k <= 0)  return {l + 1, l + 1, 0};
    if (k >= 64) return {l, l, 0};
    return {l, l + 1, (int8_t)k};
}
```

- It needs no hysteresis, because the fade is continuous.
- Its weakness: a camera that stops inside a band leaves those objects double-drawn indefinitely.
  That is why time mode is the default for single meshes.

**Instanced meshes** (`buildInstanceView`, NEAREST order, **main view only**)
- For every candidate, compute `distanceFade(...)` with the instance's own `allowed` and
  `bounds.scale * entityScale`; the code already computes both (`RenderSystem.cpp:8355-8360`).
- An instance with `k != 0` is appended **twice**:
  - into a new fade bucket of level `from`, with `instanceMatrix[0][3] = +k`;
  - into a fade bucket of level `to`, with `-k`.
  The other instances stay in the plain buckets.
- `InstanceViewRange` gains `fadeCount[MAX_MESH_LODS]` after `lodCount`.
- Views are still rebuilt only on camera or list changes, so a still camera costs nothing per frame.
- [inferred] Per-instance timers would need per-instance state and a rebuild every frame of any fade
  over lists of 100k foliage instances, so instanced meshes fade by distance only. Animate
  Cross-fading is documented as not applying to instanced meshes.
- Shadow slots, KEEP and BACK_TO_FRONT orders (blended batches, particles, 2D) never get fade
  buckets.

### 4.4 Each pass

| Pass | Behaviour | Why |
|---|---|---|
| Main colour pass (opaque) | Fading draws use the `Lfd` pipeline for both levels: one `beginDraw`, then the usual uniforms, `u_fs_lodFade=+k`, the `from` range, `u_fs_lodFade=-k`, the `to` range. The opaque comparator sorts non-fading first (after `sortPriority`, which stays first for stencil writers), fading after them, each nearest first. Instanced meshes get a second queue entry that draws only their fade buckets | Apple, PowerVR and Arm order: opaque, then discard; front to back |
| Main colour pass (transparent) | The same two draws, consecutive in the sorted queue | Each pixel blends one level: correct |
| In-target depth pre-pass (`PIP_ZPREPASS*`, opt-in) | **Skip** fading draws and fade buckets, as `skipsDepthPrepass` does (`RenderSystem.cpp:16096`). The colour pass draws them with LESS_EQUAL and depth write | A non-dithered pre-pass of both levels would write min(depthA, depthB) and punch holes where the pattern assigns the farther level. Skipping needs no DEPTH variant and only costs overdraw behind fading objects for 0.5 s |
| SSAO depth, scene-depth input, G-buffer (SSR) | Draw the **dominant level only** (`k >= 32 ? from : to`) with today's shaders; for instanced, the dominant copy (its col1.w is ignored, §4.2) | No new DEPTH/GBUFFER variants. The levels differ by about the LOD threshold (1 px default), so AO and SSR inputs differ by sub-pixel geometry for half the fade [inferred] |
| Shadow maps (atlas, points, cached static layer) | **Unchanged: snap**, with the shadow selection's own levels | Shadow levels are chosen per light texel (`shadowLodView`), not per camera, so a camera-time fade has no meaning there. Dithering cached static slots would redraw them every frame of a fade, defeating S10 caching. Bevy and Godot do not fade shadows either. Unity does |
| Picking, Scene-view tools | LOD0 (S26 R5) | Unchanged |
| Other cameras, probes, mirrors | LOD0 today (no LodView), no fade | Unchanged |
| Terrain, tilemaps | No levels (CDLOD morphs terrain itself) | Unchanged |

**Skinned, morph, lattice and VAT meshes**
- The fade is fragment-only, so it is orthogonal to these vertex bits; both draws run the same
  vertex work.
- GPU skinning pays the vertex shader twice for the fading frames. CPU skinning (`CpuSkin`) is shared.

**Alpha-mask materials**
- These already discard, so a fade costs them nothing extra in early-Z.

**Mirrored transforms**
- They take the existing mirrored pipeline variant of the `Lfd` shader, made on first use.

### 4.5 Variants, pipelines, warm-up, export

**One sibling shader per base variant**
- For every MESH variant a cross-fading submesh draws with, the variant with bit 36 set is its fade
  sibling.
- It has the same vertex inputs and one extra fragment block, declared last; instanced siblings
  have no extra block.

**How it attaches to the submesh**
- Do **not** add a second `ObjectRender`. That would duplicate every uniform and texture apply in
  `drawMesh`.
- Extend the S32 variant path instead:
  - at load, the submesh records its fade sibling `ShaderRender`;
  - `ObjectRender` gets a fade pipeline variant made from it (`prewarmWith`-style, same bindings,
    interned in `SokolPipelineCache`);
  - `beginDraw(pipType, …, lodFade = true)` applies it.
- **Check at load** that the sibling's reflected uniform and texture slots equal the base's, plus
  `u_fs_lodFade`. If they do not, the submesh never fades (log once). This guards against slot
  renumbering [inferred risk: I did not check how slots are numbered].

**When it is created**
- Asynchronously, at load, for submeshes that may fade (§4.6), so the first fade does not stall.
- Until the sibling is ready (`isReady`), a fade **snaps**. S32's stand-in rule ("never garbage")
  holds trivially.

**Pipeline count**
- One extra shader, and one extra pipeline per colour pipeline type in use, per **distinct** base
  variant that fades. Interning shares them across objects.
- In practice that is the number of distinct (material, vertex layout, light tier) combinations of
  LOD meshes: typically 5 to 30 [inferred], plus their instanced counterparts.
- On WebGL2 the cost is a program link each (`KHR_parallel_shader_compile` path). On Vulkan and
  Metal, pipeline compiles go to the worker (S32).

**Export**
- `ShaderVariantCollector::collectScene` adds the `Lfd` sibling of every MESH variant of a submesh
  that can fade, under these conditions:
  - scene `meshLodEnabled` and `meshLodCrossFade`;
  - `mesh.lodEnabled`;
  - `lodFadeMode != None`;
  - triangles;
  - not terrain or tilemap;
  - no custom colour fork.
- The S32 warm-up list records the siblings in Play like any variant.
- With either setting off, nothing is collected, so the build size is unchanged.

**Why a variant beats a uniform branch, on tilers and WebGL2 [inferred from the docs in §2.5]**
- A uniform branch costs nothing to create but taxes every draw of every LOD mesh, every frame:
  - HSR off on Apple and PowerVR;
  - late ZS on Mali before G725.
- The variant costs one program and pipeline creation per distinct base variant, once, off the main
  thread, and nothing per non-fading draw.

**Custom forks** (`customShader`)
- They snap: the engine cannot inject a discard into user GLSL safely.
- A library shader can opt in later through a manifest flag, once it includes `lod_fade.glsl`.

### 4.6 When a fade is skipped (snap)

- Scene Cross Fade off, Mesh Detail off, the mesh's Fade Mode None, `lodEnabled` off, or levels not
  built yet (`isMeshLodPending`).
- The mesh was not drawn in the main view last frame: first frame, just loaded, culled before, or
  hidden.
- Its error budget changed by more than 2x in one frame. That covers a camera cut, a teleport, an FOV
  jump or a main-camera switch. It is per object, so no global cut API is needed. A Lua call
  `scene:resetLodFades()` is optional.
- Its projected height is under 8 px. That is far and tiny: the pop does not show, and the draw call
  is what costs.
- The fade sibling is not ready or failed, or the submesh uses a custom fork.
- Optional safety valve, internal and shown in stats: at most 512 objects start a fade per frame;
  beyond that they snap. It guards worst-case frames (a cut the 2x test misses) on mobile.

### 4.7 Cost model

**Nothing fading (steady state, feature on)**
- The same draws, pipelines and passes as today.
- Per LOD mesh per frame: one hysteresis compare and a state update, a few nanoseconds.
- Resident memory: the fade siblings' shaders and pipelines (interned), plus about 16 B per
  MeshComponent.

**Per fading non-instanced submesh, for the fade's duration**
- +1 draw call and +1 16-byte uniform apply.
- Triangles: the coarse level is 12.5-50 % of LOD0, and it runs alongside the finer one.
- Both draws are on the discard pipeline:
  - late-Z on Mali;
  - no HSR on Apple and PowerVR for those draws;
  - the fading object's own pixels are shaded by both levels, because the discard comes last (§4.2).
- Bounded by duration × switch rate. At 0.5 s, a mesh that changes level every 5 s of camera motion
  is double-drawn 10 % of the time.

**Instanced**
- Per fading instance: one extra copy of about 96 B in the view buffer, uploaded only on view
  rebuilds, and drawn twice.
- Per batch: up to 2 extra draws per transition (fade buckets).
- Instances in the band are a share of about `w` of each level's ring (10 % at the default width),
  persistent while the camera is still in mode B. That is why width, not time, is their knob.

**Disabled**
- No variant collected, compiled, shipped or created; no allocation; no pass; no extra draw.
- One `if` in `selectMeshDraw` and the draw loop.

### 4.8 What users and agents see

**Scene settings, "Mesh Detail (LOD)" section** (`InspectorWindow.cpp:12673`; `SceneSettings`;
YAML; Lua `scene.meshLodCrossFade`, `scene.meshLodCrossFadeDuration`; MCP `set_scene_settings` /
`get_scene_settings` keys `mesh_lod_cross_fade`, `mesh_lod_cross_fade_duration` in
`SceneTools.cpp:816`):
- **Cross Fade** (bool, default **on**; shown when Enabled): "Blend between detail levels with a short
  dithered fade instead of switching at once. Off: no fade shaders are built or shipped."
  This is URP's "LOD Cross Fade".
- **Cross-fade Duration** (seconds, 0.05..2, default **0.5**): Unity's
  `LODGroup.crossFadeAnimationDuration`, which Unity exposes only to scripts. The 0.5 matches UE
  `lod.TemporalLag` (verified) and Unity's commonly cited value (unverified).

**Mesh inspector, under Detail Levels** (`InspectorWindow.cpp:7445`; `RenderReflect.cpp:191`; Lua
`mesh.lodFadeMode`, `mesh.lodAnimateCrossFading`, `mesh.lodFadeTransitionWidth`; undo through the
existing `propertyRow` commands):
- **Fade Mode**: `None`, `Cross Fade` (default Cross Fade). There is no SpeedTree mode: there is no
  authored morph data (§2.6).
- **Animate Cross-fading** (default on; shown with Cross Fade): "Fade over the scene's Cross-fade
  Duration when the level changes. Off: fade over a band of distance (Fade Transition Width)."
- **Fade Transition Width** (0..1, default **0.1**; shown when not animated, and always on an
  Instanced Mesh, with "Instanced meshes always fade by distance").

**Runtime readout for agents**
- `get_render_stats` gains `lod: {fading_objects, fade_draws, fade_instances, snapped_cuts,
  fade_variants_ready, fade_variants_pending}`.
- The entity inspection tools show read-only `lod_level`, `lod_fade_target` and `lod_fade_progress`
  through reflection runtime fields.
- Guide text goes in `docs/mcp/guide/rendering.md`.
- No new tool is needed: this extends existing ones, per AGENTS.md "Adding an MCP tool" step 1.

### 4.9 Data and compatibility

- New fields get defaults (MeshComponent 3, SceneSettings 2). Add them to:
  - `StreamComponents.cpp:105/200`;
  - `Stream.cpp:4328/4449`;
  - `StreamSchema.cpp:62`;
  - `kMeshKnownKeys` (`RenderReflect.cpp:501`);
  - `Factory.cpp:1167`.
- There is no format change, so no version bump.
- **Decision to confirm:** an old scene with Mesh Detail on starts fading after the upgrade, because
  the default is on. If that change in look is unwanted, migrate missing `meshLodCrossFade` to false
  for scenes saved before the bump. I recommend leaving it on: Mesh Detail is recent and opt-in.

---

## 5. How to test it

All of these follow `docs/testing.md`. Each lists its red control.

### Core unit tests (gtest, no GPU), e.g. `engine/tests/core/render/LodFadeTest.cpp` on `updateLodFade` / `distanceFade` as free functions

1. **Fade completes on time.** Fixed dt 1/60 and duration 0.5: a switch at frame N fades for 30
   frames, k goes 64→0 monotonically, and the state is stable at N+30 (±1).
2. **Reversal.** A reverse at progress 0.3 continues from 0.7 toward the old level, with no frame at
   full k.
3. **Third level.** A third-level target during a fade waits for the end.
4. **Snaps.** Not drawn last frame; allowed ×2.5 in one frame; under 8 px; sibling not ready; Fade
   Mode None.
5. **Hysteresis.** Allowed oscillating ±5 % around a threshold for 120 frames gives exactly one fade.
   - Red control: `LOD_HYSTERESIS = 0` (today's stateless selection) gives about 60 fades.
6. **Distance band.** `distanceFade` is continuous at both band ends, k is 1..63 inside, and there is
   one draw outside.
7. **Instanced partition.** `buildInstanceView` with 1000 instances along a line: every instance
   outside the bands appears once, every one inside appears twice with `+k`/`-k` in `[0][3]`, and the
   counts add up.
8. **Bayer partition.** A C++ replica of `lodFadeRank`/`lodFadeKeeps`, exhaustively over 8x8 and
   k = 1..63: exact partition and exact k/64.
   - Red control: make the `-k` test `rank > k`; the pixel with rank k is then lost and the test
     fails. This matches `/tmp/xfade-scripts/check_bayer.py`.

### GPU suites (`tools/build.sh unit-tests`, golden images; glcore, then `GRAPHIC_BACKEND=vulkan`)

9. **Complementarity on the GPU.** Force a fade at k = 32 on a mesh whose two levels are tinted
   (test-only material colours: red for `from`, green for `to`), unlit, against a blue clear. Read
   back:
   - no blue pixel inside the silhouette;
   - per 8x8 tile, exactly 32 red;
   - a blended overlap check: a second run with additive blend gives no yellow.
   - Red control: both draws `+k` gives holes (blue) and the test fails.
10. **Golden mid-fade.** `lod_crossfade_mid.png`: a Lit mesh with levels, fixed dt, duration 0.5,
    the switch triggered by a `lodBias` step at frame N, captured at N+15. Also
    `lod_crossfade_done.png` at N+31, which must equal the single-level image of the target level
    (pixel-exact).
11. **Non-fading draws keep the base pipeline.**
    - Over a still scene, assert per submesh that the pipeline id applied in the colour pass is the
      base one (`ObjectRender::getPipelineId`, `ObjectRender.h:103`). During a fade it is the fade
      variant's; after the fade, the base one again.
    - Assert `get_render_stats` `fade_draws == 0` in steady state.
    - Red control: force `lodFade = true` on every draw and the test fails.
12. **Zero cost when off.** With Cross Fade off:
    - `aliveShaders`/`alivePipelines` (`RenderStats.h:174-178`) equal a build without the feature;
    - no `Lfd` key is collected;
    - draw counts are unchanged while the camera crosses thresholds.
    - Red control: turn the setting on and the counts grow by the number of siblings.
13. **Double-draw count.** 10 meshes, 3 crossing a threshold:
    - `lod.fading_objects == 3`;
    - `drawCalls == base + Σ submeshes of the 3` during the fade;
    - `== base` after it.
14. **Pre-pass.** With `depthPrepassEnabled` and a fading mesh in front of a wall: no hole pixels of
    the wall colour inside the fading silhouette.
    - Red control: draw the fading mesh in the pre-pass and the holes appear.

### MCP and export

15. **MCP test** (`tools/mcp/tests/test_lod_crossfade.py`):
    - set and get the new scene keys;
    - edit mesh fields through the component tools, with undo;
    - read the `lod` stats during a forced fade.
16. **Export.**
    - `ShaderVariantCollector`: the `Lfd` siblings are collected only for meshes that can fade (with
      and without Instancing), and none with the setting off.
    - `tools/build.sh web` and `tools/build.sh android` compile `lod_fade.glsl` for GLSL ES 3.00.
    - `tools/smoke-test.sh`.

**Reporting:** say which run was red: the controls above, run before the fix.

---

## 6. Integration points (files)

| Area | File | Change |
|---|---|---|
| Shader | `engine/shaderlib/includes/lod_fade.glsl` (new) | §4.2 |
| Shader | `engine/shaderlib/mesh.frag` | include it; discard before the final write and before the unlit return |
| Shader | `engine/shaderlib/mesh.vert:208-214`, `includes/surface_vs.glsl:386`, `depth.vert:104-108` | affine col1, `v_lodFade` |
| Variants | `engine/core/pool/ShaderPool.cpp:652,~738` | MESH count 37, bit 36 `Lfd` |
| Variants | `engine/core/shader/ShaderVariantCompiler.cpp:~411` | `USE_LOD_FADE` |
| Pipelines | `engine/core/render/ObjectRender.h`, `engine/renders/sokol/SokolObject.*`, `SokolPipelineCache` | fade pipeline variant on the same layout, `beginDraw(…, lodFade)`, slot check |
| Load | `RenderSystem::loadMesh` / submesh variant code | the fade sibling per submesh that may fade; async prewarm |
| Selection | `RenderSystem.cpp:8222` `selectMeshLod`, `:8382` `selectMeshDraw`; `RenderSystem.h:1304` `MeshDraw` (+`from,to,k`) | hysteresis, `updateLodFade`, `distanceFade` |
| Draw | `RenderSystem.cpp:8409` `drawSubmeshGeometry`, `:8604` `drawMesh` | two ranges with ±k; fade buckets |
| Queue | `RenderSystem.h:603-646`, `RenderSystem.cpp:16039-16081` | fading key after `sortPriority`; second entry for instanced fade buckets |
| Pre-passes | `RenderSystem.cpp:16086-16103` (skip), `:8995/9413` (SSAO), `:9833/9999` (G-buffer) | skip, or dominant level |
| Instances | `RenderSystem.cpp:8293-8379`; `InstancedMeshComponent.h:108` | fade copies, `fadeCount[]` |
| Shadows | `RenderSystemShadows.cpp` | none (snap; documented) |
| Components | `MeshComponent.h:517` | `lodFadeMode`, `lodAnimateCrossFading`, `lodFadeTransitionWidth`, runtime `MeshLodFade` |
| Settings | `SceneSettings.h:354`, `Scene.h:233` | `meshLodCrossFade`, `meshLodCrossFadeDuration` |
| Editor | `InspectorWindow.cpp:7445-7453, 12673-12686`; `RenderReflect.cpp:191,501` | UI rows, reflection |
| Data | `StreamComponents.cpp`, `Stream.cpp`, `StreamSchema.cpp:62`, `Factory.cpp:1167` | fields |
| Lua | `GameObjectClassesLua.cpp:1843`, `CoreClassesLua.cpp:3198`, `tools/lua_api` | properties |
| MCP | `editor/mcp/tools/SceneTools.cpp:816`, `get_render_stats`, `docs/mcp/guide/rendering.md`, regenerate `TOOLS.md` | keys, stats |
| Export | `editor/shader/ShaderVariantCollector.cpp` | the `Lfd` siblings |
| Stats | `engine/core/render/RenderStats.h` or a RenderSystem counter | `lod` counters |

---

## 7. Risks and open questions (with defaults)

- **Slot renumbering** (§4.5). If the fade sibling's reflection does not keep its base's slots, the
  same-layout pipeline variant cannot share bindings.
  - Default: validate at load and snap on a mismatch.
  - Verify early with one Lit variant on all five backends.
- **Instance matrix col1.w trick** (§4.2). It relies on affine instance matrices and on every reader
  using the sanitised build.
  - Default: use the trick.
  - Fallback: the side stream, which needs its own layout.
- **Late discard cost** (§4.2). Fading objects shade their dropped pixels.
  - Default: accept it; the fade is short.
  - If a device trace shows a problem, use the quad-uniform early discard.
- **Edit-mode clock** (§4.3). Check whether `getUnscaledDeltatime` advances outside Play.
- **Shadow pop.** Shadows still pop at their own thresholds. Default: accepted, as in Bevy and Godot.
  Shadow-selection hysteresis would be a separate small change.
- **Pattern look.** The Bayer crosshatch is visible for 0.5 s on large objects.
  - Default: Bayer.
  - If users object, swap to IGN (one function); no data or pipeline change.
- **Value at a 1 px threshold.** At the default 1 px the geometric pop is about 1 px, so the fade
  mostly hides shading (normal and UV) changes. At mobile thresholds (2-4 px) it matters more.
  - Default: on. Its steady-state cost is zero.

---

## 8. Sources

**JasperEngine repository**
- Paths with line numbers are cited inline.

**Unity**
- Unity-Technologies/Graphics @`0efc28ea`: `LODCrossFade.hlsl`, `Common.hlsl:1672-1687`,
  `UniversalRenderPipelineAsset.cs:374-386,495-500`, `Lit.shader`, `ShaderScriptableStripper.cs:974-989`,
  `StencilDitherMaskSeed.shader`, `DrawObjectsPass.cs`, `LitData.hlsl:207-212`.
- UnityCsReference `LOD.bindings.cs` and `LODGroupEditor.cs`.
- https://docs.unity3d.com/2022.3/Documentation/Manual/class-LODGroup.html
- https://docs.unity3d.com/Packages/com.unity.render-pipelines.universal@17.0/manual/universalrp-asset.html
- https://docs.unity.com/en-us/engine/current/script-reference/unityengine/lodgroup/crossfadeanimationduration

**Unreal**
- https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-material-properties
- https://dev.epicgames.com/documentation/en-us/unreal-engine/API/Runtime/Engine/FLODMask
- https://dev.epicgames.com/documentation/en-us/unreal-engine/rendering-settings-in-the-unreal-engine-project-settings
- https://indxzero.github.io/ue544cvarwiki/articles/lod.temporallag
- https://indxzero.github.io/ue544cvarwiki/articles/r.stencilforloddither
- https://indxzero.github.io/ue544cvarwiki/articles/r.mobile.allowditheredlodtransition

**Godot**
- godotengine/godot @`fefec82d`: `render_forward_clustered.cpp:999-1013,1163-1173`,
  `scene_forward_clustered.glsl:1216`, `renderer_scene_cull.cpp:2861-2899`, `mesh_storage.h:486-513`,
  `importer_mesh.cpp:812-839`.
- https://github.com/godotengine/godot-proposals/issues/5240
- https://github.com/godotengine/godot-proposals/issues/692

**Bevy**
- bevyengine/bevy @`4c344352`: `bevy_camera/src/visibility/range.rs`, `mesh_view_bindings.wesl:40-44`,
  `mesh_functions.wesl:125-149`, `pbr_functions.wesl:57-96`, `mesh.rs:3120,3715`,
  `prepass/mod.rs:623-631`, `light.rs:2585-2640`.

**Other engines**
- bkaradzic/bgfx @`00a1927c`, `examples/12-lod/{lod.cpp,fs_tree.sc}`.
- BabylonJS @`fff529b`, `Materials/ditheredTileFadeMaterialPlugin.ts`, `bayerDitherFunctions.fx`.
- three.js `src/objects/LOD.js:113,267-272`.
- O3DE `Culling.cpp:966-985`.
- Filament `RenderableManager.h:580`.

**meshoptimizer**
- zeux/meshoptimizer @`f0a684f`: `meshoptimizer.h:464-615`, `simplifier.cpp:1527-1657,2481-2622`,
  README.md:550-674.
- https://github.com/zeux/meshoptimizer/issues/1095

**Papers and GPU documentation**
- Hoppe 1996, §3.2: https://www.cs.princeton.edu/courses/archive/fall10/cos526/papers/hoppe96.pdf
- CDLOD: https://github.com/fstrugar/CDLOD
- Arm GPU Best Practices, Issue 10 (pp. 21-22, 104): https://documentation-service.arm.com/static/67a62b17091bfc3e0a947695
- PowerVR: https://docs.imgtec.com/starter-guides/powervr-architecture/html/topics/rules/do-not-use-discard.html
- Apple: https://developer.apple.com/library/archive/documentation/3DDrawing/Conceptual/OpenGLES_ProgrammingGuide/Performance/Performance.html
  and WWDC20 10632.
- GLSL ES 3.00 rev 6: §4.1.3, §4.5, §5.9, §6.4, §7.2, §8.8.
- OpenGL ES 3.0.6: §4.1.3.
- IGN: http://www.iryoku.com/next-generation-post-processing-in-call-of-duty-advanced-warfare
  (formula via https://blog.demofox.org/2022/01/01/interleaved-gradient-noise-a-different-kind-of-low-discrepancy-sequence/).
