# S94 — Rendering, materials, animation gaps from the JasperArcade port

**Status: implemented** on `feature/s94-rendering-anim-gaps` (P1-P8 and the integration). What the
code does where it differs from this text is in "Deviations" at the end.

Arcade engine-gaps program (S92-S98). Branch `feature/s94-rendering-anim-gaps`.

The JasperArcade port (`JasperArcade-Ported/docs/port/ENGINE_LIMITATIONS.md`, section "Rendering,
materials, animation") listed 68 gaps against engine `7f48d7e8d` (2026-10-30). This spec triages
66 of them on current `main` (`82ccd7c6a`) and specifies the fixes. R57 (HTML UI under an ortho
camera) and R66 (Lua overload dispatch) belong to S92.

**Two id spaces.** `R1`-`R68` are always the port ledger's ids. This spec's own requirements are
`F1`-`F50` and its acceptance criteria are `AC1`-`AC52`. Paths are relative to the repository
root. `ARCADE` means `/var/lib/orquester/workspaces/jaspersito/JasperArcade-Ported` (read-only).
Every claim about the code was checked by reading the cited lines on `82ccd7c6a`. "Inferred" means
read in the code but not run. "Unverified" means reading could not settle it, so an AC settles it.

## 1. Goal

A Unity user porting a game should find the renderer, materials, animation and model import doing
what Unity does, without per-game workarounds. Since the port, S14, S20, S21, S52, S53 and others
have closed 21 of these items. S94 closes the rest with real features. These are outlines, a
MaterialPropertyBlock equivalent, custom blend factors, gradient ambient, TMP-style text alignment,
an orthographic size, Unity `.anim` and `.controller` import, Mecanim root transform options, PSD
layer import and 2D sprite skinning. Each comes with its editor UI, Lua API, MCP surface and tests.

## 2. Scope

### In scope

- Every item the Triage (§3.1) marks MISSING or PARTIAL. That is 37 items, grouped in eight work
  packages (§10).
- Regressions found during triage that the port's projects would hit. These are the stale
  `gameTime` reservation (F14), legacy particle scripts on migrated scenes (F2), and a ragdoll that
  does not pause the Animator (F37).
- Docs for every surface: `docs/mcp/guide/*.md`, `docs/mcp/guide/lua-api.md`,
  `docs/unity-glossary.md`, and the regenerated `docs/mcp/TOOLS.md`.

### Out of scope

- **GAME-SIDE items** (R10, R12, R16, R21, R26, R35, R51). They are fixes to the port's own
  `tools/unity2je` and `tools/gen`. The one engine part (sky rotation conventions, R11/R35) is F21.
- **FIXED and NOT-A-BUG items.** S94 only adds a pinning test where none exists (F4).
- **Unity humanoid muscle-space clips** (`.anim` with `m_MuscleClip` humanoid curves). F35 refuses
  them with a clear error. Muscle space is a separate project.
- **A gamma colour-space rendering mode** (R10). Unity's Linear mode is the modern default. Ports of
  Gamma projects keep their converter's raw-gamma shader ports.
- **Per-instance property overrides on Instanced Mesh**, and **IBL from a panoramic sky**. They are
  recorded as follow-ups in §9.
- Nothing on the do-not-copy list (`docs/specs/README.md`) is involved. No OpenEngine code is taken.

## 3. Current state

### 3.1 Triage

Totals: **21 FIXED, 15 PARTIAL, 22 MISSING, 7 GAME-SIDE, 1 NOT-A-BUG** (66 items). Every commit
cited as a fix is after `7f48d7e8d` (checked with `git merge-base --is-ancestor`).

| Ledger | Verdict | Evidence on `82ccd7c6a` | S94 |
|---|---|---|---|
| R1 outline component | PARTIAL | No Outline component in `engine/core/component/`. `library:outline_stencil` / hull presets are editor-only (`editor/material/MaterialPresets.cpp:17,124`). The Lua stencil API predates the port (`CoreClassesLua.cpp:3076-3100`), so the ledger's "Lua can't set stencil" is wrong | F22 |
| R2 sorting layers | FIXED | Sorting Layers, Order in Layer and Sorting Group (`4d5f2544f`, `664e26cfa`). Keys first in `RenderSystem.h:863-878` | — |
| R3 custom depth/shadow program | PARTIAL | `@depth` is honoured only for `library:` shaders (`RenderSystem.cpp:3539-3543`, `:4014`). A project shader on a material needs a per-mesh `customDepthShader` (`:3545-3547`) | F10 |
| R4 keep node hierarchy | MISSING | A lone mesh node is baked into the root unless there are clips, more than one skin or an Animation Player (`MeshSystem.cpp:5341-5353`). No setting | F44 |
| R5 PSD/TIF/TGA/EXR | PARTIAL | EXR is a texture (`2e844999d`, cooked through tinyexr: `editor/texture/TextureEncoders.cpp:126`). TGA and flat PSD always loaded (stb_image). No TIFF. `Util::isImageFile` lacks `.exr`/`.tif` (`editor/util/Util.h:34`) | F50 |
| R6 Animator state machines | FIXED | S21 Animator (`05c1f3be6`, `e31fdc776`): `AnimatorInstance.cpp`, `AnimatorWindow.cpp`, MCP `edit_animator_controller` | F36 imports Unity's |
| R7 avatar masks / layers | FIXED | Per-layer `.mask` weights (`ControllerDef.h:161,193`, `AnimatorInstance.cpp:345-381`). MCP `edit_avatar_mask` | — |
| R8 particle modules, world space | FIXED | S20 noise, collision, sub-emitters, trails, rate over distance, world space (`f0f4f919c`; `ParticleEffect.h:96,200,334,360,381`) | — |
| R9 flipX, sorting layers | PARTIAL | Sorting fixed (R2). `SpriteComponent.h:94` has only `flipY` | F29 |
| R10 gamma colour space | GAME-SIDE | Converter's choice: raw-gamma shader ports round-trip exactly | — |
| R11 sky rotation convention | PARTIAL | The engine convention is in `panoramic_sky.frag:12-13` and `docs/shaders/library.md:1360`. Nothing maps Unity `_Rotation` | F21 |
| R12 vertex point lights | GAME-SIDE | Per-vertex lighting exists (`Material.h:74-80`, `LightComponent.h:22-26`). Unity's attenuation mapping is converter work | — |
| R13 ragdoll on model bones | PARTIAL | `RagdollBuilder::guessBones` and wizard/MCP `create_ragdoll` (`29edb33c8`, `30e63d02a`). The ragdoll pauses only the Animation Player and the like (`PhysicsSystemRagdoll.cpp:39-60,145-158`), never the Animator, which writes `bonesMatrix` itself (`AnimatorSystem.cpp:576-579`); inferred: an Animator character ignores its ragdoll. No Lua build | F37, F38 |
| R14 global shader uniforms | FIXED | `Shader.setGlobal*` (`5130e5b84`, `CoreClassesLua.cpp:488-517`), Project Settings > Graphics > Shader Globals | — |
| R15 MaterialPropertyBlock | MISSING | `writeSubmeshCustomUniforms`: material params first, mesh uniforms only as fallback (`RenderSystem.cpp:500-504`) | F7, F8 |
| R16 unpacked FBX in bind pose | GAME-SIDE | The converter rebuilt the prefab from skinless meshes; a placed FBX model skins correctly | — |
| R17 / R33 submesh order | MISSING | Primitives follow ufbx `material_parts`, empty parts skipped (`GlbBuilder.cpp:869-871`). `GlbBuilder.cpp` unchanged since `7f48d7e8d` | F42 |
| R18 leftover animation objects | PARTIAL | Animation Entities: Auto (`cfe74ec02`, `ModelLoadCmd.cpp:768-778`). MCP `create_entity` loads the model before applying values (`EntityTools.cpp:709-724`); a later `animationEntities` write does not reload | F47 |
| R19 LineRenderer width/texture | FIXED | Polyline mode, width curve, texture mode (`92e8641b7`, `LineRendererComponent.h:91-113`) | — |
| R20 fade-in + fade-out | FIXED | Color over Lifetime gradient alpha keys (`f0f4f919c`). The deprecated `setAlphaModifier` shim keeps its old one-ramp meaning (`LegacyParticleConverter.cpp:237-239`) | — |
| R21 converter blend | GAME-SIDE | The engine's Unity importer maps blends (`editor/unity/UnityParticleImport.cpp:724-727`) | — |
| R22 skinned FBX root offset | MISSING | Load options unchanged (`FbxImporter.cpp:360-375`); static `local_transform` (`GlbBuilder.cpp:1077`). Root cause unverified | F40, F41 |
| R23 `.mtl` Windows paths | MISSING | Raw `baseDir + diffuse_texname` (`MeshSystem.cpp:6707-6713`) | F46 |
| R24 curves, start-size ramps | FIXED | `MinMaxCurve` everywhere; Size over Lifetime multiplies start size (`EmitterRuntime.cpp:1670`). The shim stays absolute by design | — |
| R25 particle depth write, blend | FIXED | `depthWrite = false` default (`ParticleEffect.h:498`). Blend in `create_particles.modules.renderer.blend`. Migration keeps an old Points' depth write on purpose (`LegacyParticleConverter.cpp:376-377`) | — |
| R26 flipbook frame API | GAME-SIDE | Sprites have `setFrame`, `sheet`, `frame`, `playTag` (`c3c73e186`, `GameObjectClassesLua.cpp:3122-3145`). The converter emitted plain meshes | — |
| R27 burst at t=0 after restart | FIXED | `burstCheckedTo = -1` on play (`EmitterRuntime.cpp:326`, `54f852877`); no test pins it | F4 |
| R28 multiplier mode | FIXED | As R24 | — |
| R29 RGB/grey on additive | PARTIAL | Alpha Source "From Gray Scale" (`TexturePrepare.h:29`, MCP `alpha_source`). Grey decodes with alpha 255 (`TextureData.cpp:1362`); unverified whether additive draws now | F3 |
| R30 custom blend factors | MISSING | `@state blend=` takes five names (`ShaderManifest.cpp:495-501`; `Material.h:36-43`). The backend has `BlendFactor`/`BlendState` (`Render.h:529-548`) | F9 |
| R31 TrailRenderer | FIXED | `TrailRendererComponent.h`, `TrailSystem.cpp` (`92e8641b7`) | — |
| R32 outline on skinned root | MISSING (engine part) | `apply_material_preset` fails on an entity without its own Mesh (`AssetTools.cpp:2804-2809`). The nested drone mesh was a converter mapping bug (game-side) | F23 |
| R34 particle blend from Lua | FIXED | `ps.renderer.blend` (`d3c0c95bc`, stub `ParticleSystemModules.lua:418-422`) | — |
| R35 converter sky/particles | GAME-SIDE | unity2je bugs; engine part is F21 | F21 |
| R36 scaled game time | FIXED | Engine-written `sceneTime` (`d33b22638`, `Scene.h:281-292`, `RenderSystem.cpp:498`). A stale `gameTime` reservation rejects the port's shaders | F14 |
| R37 first frame as rest pose | MISSING | No setting in `ModelSettingValues` (`editor/model/ModelImportSettings.h:65-80`) | F40 |
| R38 per-scene material instances | NOT-A-BUG | `.material` is a project asset like Unity's `.mat`. Unlinked scene-embedded materials exist (`UnlinkMaterialCmd.h`) | — |
| R39 restart on re-enable | FIXED | Play On Awake replays on reactivation (`8a6ca47bb`, `ParticleSubsystem.cpp:1059-1082`). Migrated action-driven systems keep the old behaviour (`StreamMigrations.cpp:276`) by design | — |
| R40 / R65 ASTC/ETC2 | FIXED | S14 KTX2/Basis runtime and per-platform cook (`456126926`, `de52a3fd8`) | — |
| R41 FBX normals options | PARTIAL | S53 Normals Import/Calculate, Smoothing Angle, Weld (`c1debf32f`, `74add701e`; `ModelCooker.cpp:582-627`). No None, weighting, smoothing groups or tangents (`ModelImportSettings.h:40`) | F43 |
| R42 Text alignment, size | MISSING | Only `pivotBaseline`/`pivotCentered` (`TextComponent.h:42-43`). `setText` only sets a dirty flag (`Text.cpp:121-128`) | F24, F25 |
| R43 em font size | MISSING | `scaleForPixelHeight` (`STBText.cpp:61`). `scaleForEmSize` exists (`FontFace.h:70`) but Text doesn't use it | F26 |
| R44 ortho transparent sort | PARTIAL | Lua sorting fields exist. Distance is still Euclidean (`RenderSystem.cpp:17456`, `:18453`; group unit `RenderSystemSorting.cpp:269`). Modes are only Default/Custom Axis (`SortingLayers.h:29-32`) | F5 |
| R45 PSD/PSB layers, 2D bones | PARTIAL | A full PSD/PSB reader exists for Title Art only (`408ba7e72`, `editor/titleart/interchange/psd/PsdReader.*`). No `.psb` asset type. No SpriteSkin | F48, F49 |
| R46 ortho autoResize | MISSING | `updateCameraSizeForCanvas` overwrites clips (`RenderSystem.cpp:16098-16128`). Lua setters clear `autoResize` (`Camera.cpp:154,173,192,211`); MCP doesn't. No orthographic size | F27, F28 |
| R47 clip-only FBX | MISSING | "the file has no triangle meshes" (`GlbBuilder.cpp:207-209`) | F39 |
| R48 per-renderer order | FIXED | Order in Layer on Mesh/Points/particles, MCP `sortingOrder` (`RenderReflect.cpp:231-232`) | — |
| R49 vignette radii | MISSING | Start/width come from smoothness (`RenderSystem.cpp:5177-5181`), unchanged since the port | F19 |
| R50 LUT green flip | MISSING | "green 0 is the bottom row" (`PostColorLut.cpp:382`) | F20 |
| R51 Standard approximation | GAME-SIDE | Converter material mapping (repack metallic/gloss to glTF layout) | — |
| R52 one hierarchy rule | MISSING | As R4 | F44, F45 |
| R53 Mesh:setShaderTexture | MISSING | `Mesh::setTexture` writes `baseColorTexture` only (`Mesh.cpp:161-168`). Points, Line, Trail, Polygon, Text and Skybox bind `setShaderTexture` (`GameObjectClassesLua.cpp:1483,2896,2980,3076,3401,3469`); Mesh doesn't | F11 |
| R54 Points frame units | MISSING | `Points::addSpriteFrame` takes UVs without a check (`Points.cpp:290-300`) | F1, F2 |
| R55 trilight/gradient ambient | MISSING | One colour (`Scene.h:351-357`, `SceneSettings.h:450-451`). Global uniforms (the other half) are fixed | F15 |
| R56 stop(clear), emit(n) | FIXED | `stop(clear)`, `StopBehavior`, `emit(n)` (`8a6ca47bb`, `ParticleSubsystem.cpp:413-418`) | — |
| R58 Points sortPriority | FIXED | Sorting Layer and Order in Layer on Points (`PointsComponent.h:96-98`) and the particle `renderer.sortPriority` (`RenderSystemParticles.cpp:544`) | — |
| R59 float sprite size, setScale(x,y,z) | PARTIAL | `pixelsPerUnit` (`SpriteComponent.h:106`). `width`/`height` are `unsigned` (`:90-91`). Lua `setScale` is uniform only (`GameObjectClassesLua.cpp:1537`) | F30, F31 |
| R60 humanoid retargeting | FIXED | One retarget path, auto mixamo map (`80630b891`, `6aa6f2c5e`; `AnimatorRetarget.h:116,191`, `ModelRig.cpp:419`) | — |
| R61 Unity `.anim` | MISSING | Detected and refused (`AnimationClipIO.cpp:155-170`). An editor Unity YAML reader exists (`editor/unity/UnityYaml.*`) | F35 |
| R62 texture preload / fence | PARTIAL | `Texture:load()` exists (`CoreClassesLua.cpp:2991`). No ready query. Old texture shown while loading (`RenderSystem.cpp:4761`) | F12 |
| R63 per-submesh queue | MISSING | One opaque priority per mesh, the minimum over submeshes (`RenderSystem.cpp:7263-7270`) | F6 |
| R64 root transform | PARTIAL | `applyRootMotion`, bake rotation/Y/XZ (`AnimatorAssetIO.h:201-207`). Yaw is relative to the clip's start (`AnimatorInstance.cpp:1278`). Bakes run only with Apply Root Motion on (`:1573`). The Animation Player has none | F32-F34 |
| R67 sky off switch, cheap panorama | PARTIAL | `visible` is reflected and in the Inspector (`LightingReflect.cpp:340`). No Lua. The draw ignores entity activity (`RenderSystem.cpp:19697-19728`, probe `:3192`) and runs first with no depth rejection. `panoramic_sky.frag:34-58` is unchanged | F16-F18 |
| R68 `@require vertexnormals` | MISSING | `surface_vs.glsl:229` declares `v_normal` for `@require normals`. No attribute-only flag | F13 |

### 3.2 Findings beyond the ledger

- **Stale `gameTime`.** `51062a893` made `gameTime` engine-written for Palette Cycling. Palette
  Cycling now reads `sceneTime` (`engine/shaderlib/library/sprite.frag`), and nothing calls a
  `writeGameTime` (grep: none on `main`). Yet `ShaderManifest.cpp:624` still rejects
  `@property gameTime` with "is written by the engine", and `ShaderUniforms.cpp:138-139` still
  parses the member. 47 ARCADE shaders declare `@property gameTime` (e.g.
  `ARCADE/assets/shaders/u2j/u2j_water.frag:24`).
- **Legacy particle scripts.** The S20 migration moves an old system onto the entity it targeted
  and removes that entity's empty Points (`StreamMigrations.cpp:217-300`). ARCADE's
  `scripts/k/KFx.lua:37-58` builds `Points(scene, e)` on the old target and
  `ParticleSystem(scene, pe)` on the old system entity, then calls `addSpriteFrame` and
  `setSpriteIntializer`. Inferred: both wrappers now address entities without the component they
  need.
- **Ragdoll vs Animator.** See R13.

## 4. Reference

- **Unity behaviour is the reference for names and semantics.** That covers SpriteRenderer (Flip,
  Size), Camera (Size), Transparency Sort Mode, MaterialPropertyBlock, ShaderLab `Blend`,
  Environment Lighting (Source: Color/Gradient), the Model Importer (Preserve Hierarchy, Normals,
  Smoothness Source, Tangents), the Animation clip Root Transform options, the 2D PSD Importer and
  2D Animation's Sprite Skin, and TextMesh Pro alignment and font size.
- **QuickOutline** (Chris Nolet, MIT), for ideas only: smoothed normals for the hull, a mask pass
  and a fill pass, and its five modes. No code is taken.
- **The port's workarounds show the API shape games want.** They are read-only reference:
  - `ARCADE/scripts/shared/GameTime.lua` and `Uniform.lua` (R36, R15);
  - `scripts/k/KJasper.lua:setTexture` (R53) and `scripts/k/KFx.lua` (R54, R62);
  - `scripts/l/LSpr.lua` (R59) and `scripts/l/LFx.lua` (R58);
  - `scripts/d/DPieces.lua` (R42, R43) and `scripts/m/MPlayer.lua` `BEACH_*` (R64);
  - `tools/gen/M_chars.py` (R60) and `tools/gen/M.py shark_anim` (R61);
  - `assets/shaders/k_prop.frag` (R55) and `assets/shaders/m/m_sky.frag` (R67);
  - `tools/unity2je/normals.py` (R41).
- **Engine code to reuse:**
  - `editor/unity/UnityYaml.*` and `UnityProject.*` (F35, F36);
  - `editor/titleart/interchange/psd/PsdReader.*` and its writer (F48);
  - the S55 `submeshMask` split (F6);
  - the `library:outline_stencil` programs and `@require smoothnormals` (F22);
  - the skinned-mesh vertex path (`bonesMatrix`) (F49).

## 5. Requirements

Each requirement names the ledger items it closes.

### Functional — particles and Points (P1)

- **F1 Points frame units (R54).** `Points:addSpriteFrame` keeps normalised UVs.
  - A rect with any component outside [-0.001, 1.001] logs one warning per Points:
    `addSpriteFrame rect is in UV units (0..1), got (x, y, w, h)`.
  - `docs/mcp/guide/lua-api.md` states the unit next to `Sprite:addFrame`'s pixels.
- **F2 Legacy particle scripts keep working on migrated scenes.** After the S20 migration, both of
  the K pattern's calls drive the migrated system:
  - `ParticleSystem(scene, oldSystemEntity)`;
  - `Points(scene, oldTargetEntity):addSpriteFrame/setSpriteIntializer`.

  The migration writes `legacyFrom` (the old system entity id) on the migrated
  `ParticleSystemComponent`. The Lua `ParticleSystem` constructor on an entity without a system
  resolves through it. `Points` on an entity with a ParticleSystem and no PointsComponent forwards
  frame calls to the effect's Texture Sheet Animation sprites. Each forward logs one deprecation
  note naming `ps.textureSheetAnimation`.
- **F3 Textures without alpha on additive particles (R29).** An RGB PNG with no alpha channel and a
  greyscale PNG both draw their colour on an additive particle. If AC3 shows they don't, the fix
  is in decode: a grey image expands to RGB with alpha 1.
- **F4 Burst at zero after restart (R27).** A pinning test only.

### Functional — draw order (P2)

- **F5 Transparency Sort Mode by projection (R44).** `TransparencySortMode` gains `PERSPECTIVE`
  and `ORTHOGRAPHIC` (Unity names), beside `DEFAULT` and `CUSTOM_AXIS`.
  - The modes:
    - Perspective: the current Euclidean distance.
    - Orthographic: depth along the camera's forward axis, `dot(objPos - camPos, camForward)`.
    - Default: Orthographic for an orthographic camera, Perspective otherwise (Unity's rule).
  - It applies in:
    - `updateMVP` (`RenderSystem.cpp:17456`);
    - the lazy batch path (`:18453`);
    - Sorting Group `unitDistance` (`RenderSystemSorting.cpp:269`);
    - point and particle sort distances.
  - Project migration v1→v2: an existing project with no `transparencySortMode` key becomes
    `perspective`, so its look does not change. New projects get Default.
- **F6 Per-submesh queue priority (R63).** Each submesh keeps its own priority, `material.sortPriority`
  plus `@state priority` (`RenderSystem.cpp:7266`).
  - When the submeshes of one mesh differ in priority or queue, the mesh is queued as one entry per
    (queue, priority) group, through the S55 `submeshMask` split.
  - Equal priorities keep one entry, as today.
  - The hull of a "Transparent-1, ZWrite Off" submesh then draws after every opaque.

### Functional — materials and shaders (P2)

- **F7 Material Property Overrides (R15).** This is the MaterialPropertyBlock equivalent.
  - A renderer (Mesh, and so Sprite, Model children and Tilemap) has optional per-renderer
    overrides, with an optional per-submesh layer. Each override is a name → float, vector or
    colour value, or a name → texture.
  - Precedence: submesh override > renderer override > `material.shaderParams` > `mesh.shaderUniforms`
    > the scene's default > the manifest's default. The existing chain is untouched below the
    overrides.
  - Overrides apply to custom and library shader members and textures. They also apply to the
    built-in Lit's `baseColorFactor`, `emissiveFactor` and `baseColorTexture`.
  - Overrides are saved with the renderer. Unlike Unity's runtime-only block, generators and agents
    need them persisted.
- **F8 Shadowed mesh uniform warning (R15).** A `Mesh:setShaderUniform` / `set_shader_uniform` value
  that a material parameter of the same name hides logs one warning per (entity, name) naming
  Property Overrides. This is the silent failure M hit.
- **F9 Custom blend factors (R30).**
  - `MaterialBlendMode::CUSTOM` with `blendSrc`, `blendDst`, `blendSrcAlpha`, `blendDstAlpha`
    (Unity's factor names: One, Zero, SrcColor, SrcAlpha, DstColor, DstAlpha and the four
    OneMinus forms) and `blendOp` (Add, Subtract, ReverseSubtract, Min, Max).
  - Manifest syntax: `@state blend=DstColor,SrcColor` (colour and alpha alike) or
    `blend=SrcAlpha,OneMinusSrcAlpha,One,One`, plus an optional `blendop=`.
  - Mapped to the backend's `BlendState` (`Render.h:529-548`).
- **F10 Project shaders' own depth program (R3).** The `@depth` of any shader a submesh draws with
  (library or project) drives its depth, shadow and SSAO passes. The `isLibrary` gates at
  `RenderSystem.cpp:3539` and `:4014` go. A mesh's `customDepthShader` still wins when set.
- **F11 `Mesh:setShaderTexture` and the main texture (R53).**
  - `Mesh:setShaderTexture(name, path [, submesh])` writes `material.shaderTextures`. The default
    is every target submesh 0, as `setTexture` does.
  - `Mesh:setTexture(path)` on a material with a custom or library shader writes that shader's main
    texture: the `@texture` marked `main`, else its first `@texture`. A shader with no `@texture`
    falls back to `baseColorTexture`.
  - `@texture <uniform> main` is new manifest syntax.
- **F12 Texture readiness (R62).** Lua `Texture.ready` (a property) is true once the GPU texture
  exists. The guide documents the preload path: `Texture(path):load()` held for the scene, then
  swap. The "old texture until the new one is ready" rule stays and is documented.
- **F13 `@require vertexnormals` (R68).** The vertex stage gets `a_normal` and `p_hasNormal` with no
  `v_normal` varying. `@require normals` keeps its meaning.
- **F14 `gameTime` is an ordinary name again (R36 follow-up).** Remove the reservation
  (`ShaderManifest.cpp:624`) and the dead `gameTimeMember` (`ShaderUniforms.cpp:138-139,166`,
  `ShaderUniforms.h:80`). The guide names `sceneTime` as Unity's `_Time.y`.

### Functional — lighting, sky and post (P3)

- **F15 Environment Lighting Source (R55).**
  - `SceneSettings::ambientMode` is Color (default) or Gradient.
  - Gradient adds `ambientSkyColor`, `ambientEquatorColor` and `ambientGroundColor`, stored linear;
    the intensity stays `globalIllumIntensity`.
  - Gradient compiles the Lit with `AMBIENT_GRADIENT` and gives `sky·max(n.y,0) +
    equator·(1−|n.y|) + ground·max(−n.y,0)`.
  - Custom shaders get the same through `ambientLight(normal)` in the surface includes.
- **F16 Sky on/off (R67).** Lua `Skybox.visible` (a property) plus `Skybox:setVisible`. An inactive
  sky entity draws no sky and feeds no probe capture (`RenderSystem.cpp:3192`, `:19722`).
- **F17 Sky after opaques (R67).**
  - The sky draws after the opaque queue, depth-tested at the far plane, so covered pixels are not
    shaded.
  - The sky keeps drawing first in two cases: a camera with `depthTest = false`, and a draw mode
    that needs it (Overdraw).
- **F18 Per-vertex panorama (R67).** `library:panoramic_sky` gains `@feature VERTEX_MAPPING`
  ("Per-Vertex Mapping"):
  - longitude and latitude are computed on a 64×32 sphere grid with a duplicated seam column and
    pole fans;
  - one `textureLod` read per fragment, with no atan, acos or derivatives.

  Off by default.
- **F19 Vignette radii (R49).**
  - `vignetteFalloff` is Smoothness (default, today's maths) or Radii.
  - Radii adds `vignetteStart` and `vignetteEnd`, in the roundness-scaled radius where 1 is the
    edge midpoint, and gives `smoothstep(start, end, length(p))`.
- **F20 LUT green flip (R50).** `lutFlipGreen` (default false) flips the strip's rows on the CPU when
  the LUT is built (`PostColorLut.cpp:382`). The GPU cost is zero.
- **F21 Sky orientation conventions (R11, R35, R67).** GPU tests pin three things: the panorama's
  rotation, the cubemap faces (the z flip in `Skybox.cpp setCubeMap`) and their Unity
  equivalents. `docs/unity-glossary.md` and `docs/shaders/library.md` give the formula from
  Unity's `_Rotation` for Skybox/Panoramic and Skybox/Cubemap. A wrong z flip, if AC21 finds one,
  is fixed only with a scene migration that keeps old skies looking the same.

### Functional — Outline (P4)

- **F22 Outline component (R1).** `OutlineComponent` ("Outline", Add Component > Rendering):
  - Outline Mode: Outline All, Outline Visible, Outline Hidden, Outline And Silhouette, Silhouette
    Only.
  - Outline Color (with alpha).
  - Outline Width, in Width Units: Pixels (default) or World.
  - Include Children (default on).
  - Drawn as a mask pass (stencil write, colour off) and a fill pass (a hull extruded along smoothed
    normals, stencil not-equal, depth test by mode). It uses the `library:outline_stencil`
    programs, after the opaque queue. Skinned and instanced meshes are covered.
  - No component, or a disabled one, adds no pass, draw or allocation.
- **F23 Outlines on model roots (R32).**
  - With Include Children, an Outline on a model root outlines every descendant Mesh. That covers
    skinned children and meshes nested under `RootNode/<fbx>/`.
  - `apply_material_preset` on an entity without a Mesh applies to its descendant meshes. The
    result's `next` says so and suggests the Outline component for outline presets.

### Functional — Text, Camera, Sprite (P5)

- **F24 Text anchor (R42).**
  - `TextComponent` gains `anchorHorizontal` (Left, Center, Right) and `anchorVertical` (Top,
    Middle, Bottom, Baseline, Capline, Midline).
  - Today's default anchor (the line box's bottom) is Bottom.
  - The anchor is relative to the text's box: its fixed width/height when set, else its content.
    Capline and Midline use the font's cap height and x-height (OS/2), else 0.7 / 0.5 em.
  - `pivotBaseline` and `pivotCentered` migrate (scene v10→v11, prefab v10→v11):
    - `pivotBaseline: true` → `anchorVertical: baseline`;
    - `pivotCentered: true` → `anchorHorizontal: center`.

    Lua keeps both as deprecated aliases.
- **F25 Text size before the first draw (R42).**
  - `Text.width` and `Text.height` (Lua and C++) and a new `Text:updateLayout()` build the layout
    on the calling (main) thread when it is dirty.
  - Reading them right after `setText` returns the final size.
- **F26 Em font size (R43).** `fontSizeUnit` is Pixel Height (the default and today's behaviour:
  ascender − descender) or Em (`FontFace::scaleForEmSize`, as CSS and TMP measure).
- **F27 Camera clip writes clear Auto Resize (R46).** `set_properties`, `create_entity` values and
  the Inspector clear `autoResize` when they write `leftClip`, `rightClip`, `bottomClip` or
  `topClip`. The clear goes in the same undo step, as the Lua setters do (`Camera.cpp:154`).
- **F28 Orthographic Size (R46).** `CameraComponent::orthographicSize` (float, default 0 = off).
  - When it is > 0 on an orthographic camera, the bounds are centred: ±size vertically and
    ±size·aspect horizontally.
  - The bounds follow the canvas or render target aspect in `updateCameraSizeForCanvas`, ahead of
    the pixel `autoResize`.
  - A Pixel Perfect Camera still wins (S41 R3).
- **F29 Sprite Flip X (R9).** `SpriteComponent::flipX` mirrors the quad about the pivot. It works the
  same through Sprite Batch, Dynamic Batching and Sliced draw mode. Flip Y keeps its meaning.
- **F30 Sprite Size (R59).** `SpriteComponent::size` (Vector2, world units, default (0,0) = from
  width/height and Pixels Per Unit) sets the quad's world size in every draw mode. In Sliced mode
  the corners stay unscaled (Unity's `SpriteRenderer.size`).
- **F31 `GameObject:setScale(x, y, z)` (R59).** It is an arity overload beside `setScale(s)`.
  Distinct arities avoid the R66 miss cost.

### Functional — animation (P6)

- **F32 Root Transform "Based Upon" (R64).** `ClipRootMotion` gains:
  - `rotationBasedUpon`: Original or Body Orientation;
  - `rotationOffset` (degrees);
  - `positionYBasedUpon`: Original, Center Of Mass or Feet;
  - `positionYOffset`;
  - `positionXZBasedUpon`: Original or Center Of Mass.

  Body Orientation and Center Of Mass need a humanoid rig (`resolveHumanoid`). Otherwise they warn
  once and act as Original. They are settable on `.anim` files and on model clips (`.clips`).
- **F33 Bake settings without Apply Root Motion (R64).** With Apply Root Motion off, the unbaked
  root motion is removed from the pose (the character stays in place), as in Unity. Today nothing
  is removed (`AnimatorInstance.cpp:1573`). A clip without a `root_motion` block is unchanged.
- **F34 Animation Player root motion (R64).** `AnimationPlayerComponent::applyRootMotion` and the
  same clip settings, through the shared `removeRootMotion` path.
- **F35 Unity `.anim` import (R61).** Editor-side, in `editor/unity/UnityAnimImport.*` on `UnityYaml`.
  It writes a JSON `.anim` beside or at a chosen path.
  - Imported:
    - `m_PositionCurves`, `m_RotationCurves`, `m_EulerCurves` and `m_ScaleCurves`;
    - `m_FloatCurves` on Transform, GameObject `m_IsActive` and renderer material properties;
    - keys with their tangents and Unity tangent modes (`ClipTangentMode`);
    - `m_AnimationClipSettings` (loop time, start and stop);
    - events.
  - Unity's left-handed X mirror is an option, `mirrorX`, default on.
  - Humanoid muscle curves, and bindings it cannot map, are listed in the report. A muscle-only
    clip fails with an error naming the limit.
  - The YAML refusal message (`AnimationClipIO.cpp:167`) names this import route.
- **F36 Unity `.controller` and `.mask` import (R6, R7).** These go through the same importer into
  the S21 JSON assets:
  - the states, sub-state machines, transitions with their conditions, and parameters;
  - 1D and 2D blend trees;
  - layers with weight, blending and mask;
  - an AvatarMask's humanoid body parts and transform paths.

  Motions resolve through `UnityProject` GUIDs, imported with F35 when needed. What cannot map is
  reported.
- **F37 A ragdoll pauses the Animator (R13).** When a ragdoll takes over, it pauses an Animator on
  the model, as it does the Animation Player (`PhysicsSystemRagdoll.cpp:145-158`). It resumes on
  release. The ragdoll's bone transforms then drive `bonesMatrix`.
- **F38 Lua ragdoll build (R13).** `Ragdoll.build(scene, modelEntity [, options])` wraps
  `RagdollBuilder` (bone guessing, the mass total, the collider scale) and returns the Ragdoll
  handle. Bone entities must exist; when they don't, it returns nil and logs the reason.

### Functional — model import (P7)

- **F39 Clip-only FBX (R47).** An FBX with nodes and animation stacks but no geometry imports as a
  glb with nodes, skins (if any) and animations. Unity's `Model@Clip.fbx` convention then works:
  `AnimationPlayer:addClip("Model@Run.fbx#<take>")`, and Animator motions. A file with neither
  geometry nor animation still fails.
- **F40 Rest Pose: First Frame (R37, R22).**
  - The model import setting `restPose` is Node Values (default) or First Frame. First Frame writes
    the node TRS of the chosen take's first frame (`restPoseTake`, default the first) into the
    glb's nodes. Inverse bind matrices are kept.
  - `get_model_info` reports `first_frame_differs` for any root or hips translation that differs
    from its node value by more than 1 cm. Its `next` names the setting.
- **F41 Skinned geometric transforms (R22).** Find the cause of CLEIVER's −2.8 offset with an ASCII
  FBX fixture: a skinned mesh with a GeometricTranslation, a pre-rotation and a non-unit scale on
  its parents. If the import differs from ufbx's evaluated skinned positions, fix it in
  `FbxImporter`/`GlbBuilder`. If it doesn't, record that F40 alone covers R22.
- **F42 Submesh order (R17, R33).**
  - Primitive *i* is the node's material slot *i*, which is Unity's submesh order, not ufbx's
    `material_parts` order.
  - An empty slot no longer shifts later indices: it is kept as an empty primitive, or its index is
    reserved.
  - Each primitive records `extras.fbxMaterialSlot` and the FBX material name.
- **F43 Normals and tangents (R41).** These go in `ModelSettingValues`:
  - `normals`: Import, Calculate or None. None writes no normals; the Lit then shades with face
    normals from derivatives.
  - `normalsWeighting`: Area And Angle (the default for Calculate, as Unity) or Area.
  - `smoothnessSource`: From Angle (default), From Smoothing Groups (FBX only, computed during the
    FBX→glb step) or Prefer Smoothing Groups.
  - `tangents`: Import, Calculate (MikkTSpace) or None. Absent keys keep today's behaviour.
- **F44 Preserve Hierarchy (R4, R52).** `ModelComponent::preserveHierarchy` (default false, today's
  rule). When it is true, every glTF node gets its own entity and a mesh is never baked into the
  root. Project Settings > Mesh "Preserve Hierarchy for New Models" is on for new projects and off
  for existing ones, as Animation Entities: Auto did (`cfe74ec02`).
- **F45 One addressing rule (R52).** `get_model_info` lists, for each mesh node, `drawn_by` (the
  entity that draws it). A material or mesh property sent to a model-node entity that has no Mesh
  but whose mesh the root draws fails with the root's path and `preserveHierarchy` in its text.
- **F46 Tolerant `.mtl` paths (R23).** Texture paths are resolved in this order:
  1. backslashes become `/`;
  2. drive letters and absolute prefixes are stripped;
  3. the path is tried relative to the `.mtl`;
  4. its basename is tried in the `.mtl`'s folder.

  One warning gives the resolved path. If tinyobj's `mtllib` escape handling
  (`tiny_obj_loader.h:2904`) has to change, the change follows that library's patch ledger.
- **F47 Animation objects decided before the load (R18).**
  - `create_entity` applies the load-affecting Model values (`animationEntities`,
    `preserveHierarchy`) before `ModelLoadCmd`.
  - A later write of either through `set_properties` reloads the model in the same undo step.
  - In Auto, a model that has (or has under an ancestor) an Animator or an Animation Player gets no
    per-clip animation objects.

### Functional — PSD layers, 2D skinning, TIFF (P8)

- **F48 PSD/PSB layer import (R45).** `.psb` becomes a texture type (`Util.h:34`, `AssetTypes.cpp:29`).
  The texture importer gains Import Mode: Flattened (default, today) or Layers (Unity's PSD
  Importer). Layers mode:
  - reads with `PsdReader`;
  - packs every visible pixel layer into a sprite sheet (S45 format) with one sprite per layer,
    named after the layer;
  - writes a prefab `<file>.prefab`, with one Sprite GameObject per layer at its document position,
    layer groups as parents and stack order as Order in Layer.

  Reimport keeps sprite ids by layer id.
- **F49 Sprite Skin (R45).** This is the `SpriteSkinComponent` of Unity's 2D Animation.
  - A sprite gains optional geometry: vertices, triangles and up to 4 bone weights per vertex,
    stored in its sheet's metadata.
  - A Sprite Skin binds a Root Bone and a bone list (entity Transforms). Deformation is GPU skinning
    through the existing skinned-mesh vertex path, with no compute.
  - The editor gets a Skinning Editor window: Create Bone, Auto Geometry (alpha outline,
    triangulation, density), Auto Weights (distance-based, normalised), a Weight Brush, and
    Preview Pose.
  - With PSB import, a layer can be bound to a bone at 100%, as MonsterTruckHunter's rig is.
- **F50 TIFF and image extensions (R5).**
  - An editor-only baseline TIFF reader, cooked like EXR:
    - first IFD only;
    - strips and tiles;
    - None, LZW, Deflate and PackBits compression;
    - 8 and 16 bit;
    - grey, RGB and RGBA.
  - `Util::isImageFile` adds `.exr`, `.tif`, `.tiff` and `.psb`, so hot reload and auto-sync see
    them.

### Platform & backends

- **F-PB1** Every runtime feature works on GL 4.1, GLES3/WebGL2, Metal, D3D11 and Vulkan, and on
  all six export targets. That covers F5-F7, F9-F13, F15-F20, F22, F24-F31, F32-F34, F37-F38, F44
  and F49.
- **F-PB2 Stencil.** The outline needs a depth-stencil target on every camera pass that draws one.
  Where the WebGL default framebuffer has no stencil, outlines go through the offscreen target, as
  `outline_stencil` already does. AC48 checks it.
- **F-PB3** Custom blend factors use only the fixed-function factors listed in F9, with no dual-source
  blending. Min and Max ignore the factors, as on every API.
- **F-PB4** The editor-only pieces (F35, F36, F39-F43, F46 import, F48, F50) run in the editor. Their
  outputs (JSON assets, glb, sprite sheets, cooked textures) are what ships.

### Performance (each budget measured on the dev host and reported in the PR)

- **F-PF1 Disabled is free.** Each of these adds no pass, no draw and no per-frame allocation:
  - no Outline component;
  - no overrides;
  - Ambient Color;
  - vignette Smoothness;
  - Flip X off;
  - no Sprite Skin;
  - Per-Vertex Mapping off.

  Each AC on these features compares draw and pass counts with the feature absent.
- **F-PF2 Outline.** Two draws per outlined submesh. Smoothed normals are computed once per mesh at
  load, on a job.
- **F-PF3 Property Overrides.** Renderers with identical override sets batch together: the override
  hash joins the Dynamic Batching key (`RenderSystemBatch.cpp`). Different sets draw apart.
- **F-PF4 Per-submesh priority** splits only meshes whose submesh priorities differ.
- **F-PF5 Gradient ambient** adds at most 6 ALU per fragment, only in the `AMBIENT_GRADIENT` variant.
- **F-PF6 Per-Vertex Mapping:** one texture read and no transcendental per fragment. Sky after
  opaques: on a scene with 50% of the screen covered, GPU time of the sky pass drops (S15 pass
  timings, reported).
- **F-PF7 Text layout on demand** runs only when a getter reads a dirty text.
- **F-PF8 Orthographic sort** costs one dot product (cheaper than today's length).

### Editor UX (human)

All edits are undoable commands. The Unity names are:

- **Sorting.** Project Settings > Tags and Layers > Transparency Sort Mode (Default, Perspective,
  Orthographic, Custom Axis).
- **Mesh Inspector:**
  - a "Property Overrides" foldout that lists the shader's properties with an override toggle each;
  - Material > Blend Mode "Custom" with Src/Dst factor and Blend Op rows.
- **Lighting > Environment.**
  - Source is Color or Gradient: Sky Color, Equator Color, Ground Color.
  - Post Processing > Vignette > Falloff (Smoothness / Radii), Start, End.
  - Color Lookup > Flip Green.
- **Rendering components:**
  - Skybox: Per-Vertex Mapping, on the panoramic material.
  - Add Component > Rendering > Outline (Outline Mode, Outline Color, Outline Width, Width Units,
    Include Children).
  - Text: Anchor (horizontal and vertical button rows), Font Size Unit.
  - Camera: Size, shown for orthographic cameras.
  - Sprite: Flip (X, Y) and Size.
- **Animation:**
  - Clip and model-clip Inspector > Root Transform Rotation / Position (Y) / Position (XZ): Bake
    Into Pose, Based Upon, Offset.
  - Animation Player: Apply Root Motion.
  - Assets > Import Unity Animation… for `.anim`, `.controller` and `.mask`. Dropping one into the
    Project window offers it.
- **Model Importer:**
  - Normals (Import, Calculate, None), Normals Weighting, Smoothness Source, Tangents;
  - Rest Pose (Node Values, First Frame) with Take;
  - Model: Preserve Hierarchy.
- **Texture Importer:**
  - Import Mode (Flattened, Layers) for PSD/PSB;
  - Window > 2D > Skinning Editor.

### Agent UX (MCP, Lua, CLI)

Extend before adding (AGENTS.md "Adding an MCP tool"). Reflection-driven fields reach
`set_properties`, `get_entity` and `create_entity` values with no new tool.

- **MCP.**
  - **Reflection only:** F5 through `set_project_settings tags_and_layers.transparency_sort_mode`;
    F9, F15, F19, F20 through `set_scene_settings` and the material values; F22, F24, F26, F28-F30,
    F34 and F44 through reflection.
  - **`set_shader_uniform`** gets `override: true` (F7). That writes the renderer's Property Override
    and pairs with `submesh`. `get_entity` lists overrides.
  - **`apply_material_preset`** descends (F23).
  - **`edit_animation`:** `create: {unity: "<abs .anim>", mirror_x}` (F35), and `root_motion` gets
    the F32 fields.
  - **`edit_animator_controller` `create: {unity}`** and **`edit_avatar_mask` `create: {unity}`**
    (F36).
  - **`set_model_import_settings`:** F40 and F43 keys.
  - **`get_model_info`:** `drawn_by` and `first_frame_differs` (F40, F45).
  - **`set_texture_import_settings`:** `import_mode: layers` (F48).
  - **One new tool, `edit_sprite_skin`** (F49): bones create/remove, `auto_geometry`,
    `auto_weights` and `set_weights`, with one undo step per call. No existing tool edits sprite
    geometry.
  - **The rest:** each new text gets an eval case in `tools/mcp/eval/cases.py` that fails without
    it, and `tools/mcp/budget.json` is updated.
- **Lua.** Bound getters are properties (`.claude/rules/scripting.md`). New classes go in new binding
  files, not in `GameObjectClassesLua.cpp`. No new overload set mixes userdata types of the same
  arity (S92 R66).

  | Feature | Lua |
  |---|---|
  | F7 | `MaterialPropertyBlock()` with `setFloat`, `setVector`, `setColor`, `setTexture`, `clear` and `isEmpty`; `Mesh:setPropertyBlock(block [, submesh])` and `Mesh:getPropertyBlock([submesh])` (a copy) |
  | F11 | `Mesh:setShaderTexture` |
  | F12 | `Texture.ready` |
  | F15 | `scene.ambientMode`, `scene:setAmbientGradient(sky, equator, ground)` |
  | F16 | `Skybox.visible` |
  | F22 | `Outline` component handle |
  | F24 | `Text.anchorHorizontal` / `anchorVertical` (`TextAnchor` enum tables) |
  | F25 | `Text:updateLayout()` |
  | F26 | `Text.fontSizeUnit` |
  | F28 | `Camera.orthographicSize` |
  | F29 | `Sprite.flipX` |
  | F30 | `Sprite.size` |
  | F31 | `setScale(x, y, z)` |
  | F34 | `AnimationPlayer.applyRootMotion` |
  | F38 | `Ragdoll.build` |
  | F49 | `SpriteSkin` handle (`enabled`, `rootBone`, `bones`) |

  Stubs go in `engine/core/script/binding/stubs/`.
- **CLI.** Everything above is reachable through `jasperengine-editor call`. No new subcommand.

### Data & compatibility

- **New optional fields, with defaults and no version bump:**
  - `legacyFrom`, `propertyOverrides`, the blend factor fields and `ambientMode` with its colours;
  - `vignetteFalloff` with its radii, `lutFlipGreen` and `fontSizeUnit`;
  - `orthographicSize`, `flipX`, `size`, the root-motion fields and `applyRootMotion` on the Player;
  - `preserveHierarchy`, the model import keys and the texture `importMode`;
  - Outline and Sprite Skin (new components), and the sprite geometry in sheet metadata.
- **Migrations (each with a fixture in `editor/tests/fixtures/migrations/` and a test):**
  - **Text:** `pivotBaseline` / `pivotCentered` → alignment (scene 8→9, prefab 9→10; P5 owns it).
  - **Project:** an absent `transparencySortMode` → `perspective` (project 1→2; P2 owns it).
  - **Scene (only if AC21 finds the cubemap z flip wrong):** existing skies are compensated (P3, the
    next scene version after P5's).
- **Persistent writes** (imported `.anim`, controllers, masks, sprite sheets, prefabs, glb) go
  through `AtomicFile` helpers.
- **`gameTime` (F14):** the 47 ARCADE shaders load with no manifest error.

## 6. Design constraints & decisions

Decisions made here (the owner delegated product calls), each final for the implementers:

1. **Overrides are persisted, named Property Overrides in the editor and `MaterialPropertyBlock` in
   Lua.** Unity's block is runtime-only, but the port's need was authored per-object values, and
   agents need to read them back. `setShaderUniform` keeps its fallback meaning, so old scripts
   behave the same. F8 turns the silent case into a warning.
2. **Transparency Sort Mode Default follows the projection, as Unity's does.** Old projects are
   migrated to Perspective, so they don't re-sort. New projects get Unity's rule.
3. **Sky after opaques.** The cost win applies everywhere. A golden equality gate (AC17) guards it.
4. **Outline is an engine component, not a material preset.** Presets stay for hand-made looks.
   Width in pixels is the default (QuickOutline's look). World width serves VR and 3D games.
5. **Text alignment replaces the two pivot booleans through a migration**, not a third overlapping
   field. Lua keeps the old names as aliases.
6. **Em font size is opt-in.** Changing the default would resize every old text and every
   `Text(scene)` an old script makes.
7. **Orthographic Size is opt-in (0 = off).** The engine's pixel-unit 2D default (UI, S41) stays.
   The Unity converter writes a Size instead of `autoResize: false`.
8. **Sprite Size is a new world-size field.** `width`/`height` stay integers in pixels, so there is
   no re-type migration.
9. **Unity asset import is editor-side and writes engine JSON.** The runtime still has no YAML
   (S21 §0). It lives in `editor/unity/` beside the particle importer and reuses its GUID table.
10. **Root motion follows Unity when Apply Root Motion is off.** This is a behaviour change, only
    for clips with a `root_motion` block. It is listed in `docs/animator.md` "Changes".
11. **Preserve Hierarchy is per model, with a project default for new placements.** Making it
    universal would change entity ids and paths in every old scene.
12. **Submesh order is a fix, not an option.** Glb materials keep FBX names, so materials remapped
    by name still land. A model whose order changes reimports with a Console note listing the
    moved slots.
13. **Sprite Skin uses the existing GPU skinning path.** That means no compute and the same code on
    all five backends. The weights live in sprite metadata, so one rig is shared by every instance.
14. **R13 is split.** S94 owns the Animator pause (it is animation state) and the Lua build. S96
    owns the physics behaviour of ragdolls.
15. **Migrated particle systems keep old semantics** (depth write, action-driven play). F2 adds
    forwarding, not new behaviour.

API sketches (header shape, not final):

```cpp
// engine/core/registry/SortingLayers.h
enum class TransparencySortMode : uint8_t { DEFAULT, CUSTOM_AXIS, PERSPECTIVE, ORTHOGRAPHIC };

// engine/core/texture/Material.h
enum class MaterialBlendMode { NONE, ALPHA, PREMULTIPLIED, ADDITIVE, MULTIPLY, CUSTOM };
struct MaterialBlendFactors { BlendFactor src, dst, srcAlpha, dstAlpha; BlendOp op; };

// engine/core/component/MeshComponent.h
ShaderPropertyOverrides propertyOverrides;   // values + textures; per-submesh in Submesh

// engine/core/component/OutlineComponent.h
enum class OutlineMode : uint8_t { OUTLINE_ALL, OUTLINE_VISIBLE, OUTLINE_HIDDEN, OUTLINE_AND_SILHOUETTE, SILHOUETTE_ONLY };
struct OutlineComponent { OutlineMode mode; Vector4 color; float width; OutlineWidthUnits units; bool includeChildren = true; bool enabled = true; };
```

```lua
local block = MaterialPropertyBlock()
block:setColor("u_tint", Vector4(1, 0.5, 0, 1))
mesh:setPropertyBlock(block)            -- all submeshes; mesh:setPropertyBlock(block, 1) for one
text.anchorVertical = TextAnchor.Middle
camera.orthographicSize = 11            -- Unity Size
sprite.flipX = true; sprite.size = Vector2(1, 1)
```

## 7. Acceptance criteria

Tests go where `docs/testing.md` "Where a test goes" says. GPU suites end in `Gpu` and write
goldens on glcore; Vulkan must match them (`GRAPHIC_BACKEND=vulkan`). "Red" names the run that
fails without the change. Where a red control cannot be built, the PR says so.

| AC | Verifies | Check (red control) |
|---|---|---|
| AC1 | F1 | `PointsTest.SpriteFrameOutsideUnitWarnsOnce`: two pixel rects log one warning; UV rects log none. Red: no warning |
| AC2 | F2 | Fixture `scene-v7-legacy-particles-k.scene` (K's layout: Points target plus a separate system entity). `test_particles_legacy.py::test_k_pattern_script` plays it with KFx's lines. The Console has no errors, `control_particles` emits, and the effect has 8 sprites. Red (expected, confirmed by the run): an error on `addSpriteFrame` |
| AC3 | F3 | `ParticleRenderGpu.AdditiveRgbAndGreyTexturesDraw`: golden of three additive quads (RGBA, RGB and grey PNG). Each centre pixel > 0.5. If it is green before the change, F3 is closed as a test and the PR says so |
| AC4 | F4 | `EmitterRuntimeTest.BurstAtZeroAfterRestart`: reset, play, one tick, then count = the burst. Red: reverting `EmitterRuntime.cpp:326` locally |
| AC5 | F5 | `RenderSortingTest.OrthographicSortsByViewDepth`: an ortho camera, A at (50, 0, −1) and B at (0, 0, −2), camera x swept −100..100. A always draws after B in Default and Orthographic, and Perspective reproduces today. `StreamMigrationsTest.ProjectV1KeepsPerspectiveSort` (fixture `project-v1-sort.yaml`). Red: the order flips during the sweep |
| AC6 | F6 | `RenderQueueGpu.SubmeshPriorityHullDrawsAfterOpaques`: a body (priority 0) and a hull submesh (priority 1, ZWrite off), then a later opaque box beside them. Golden shows the hull. A mesh with equal priorities has an unchanged entry count. Red: the hull is erased |
| AC7 | F7 | `MaterialPropertyBlockGpu.OverrideWinsOverMaterial`: two meshes share one material with two override colours (golden), and a third with a submesh override. `test_materials.py::test_property_overrides`: `set_shader_uniform override:true`, undo, save and reload, `get_entity`. Lua play test: `setPropertyBlock` and the Console read-back. Red: both draw the material colour |
| AC8 | F8 | `ShaderUniformsTest.ShadowedMeshUniformWarnsOnce`. Red: no warning |
| AC9 | F9 | `ShaderManifestTest.BlendCustomFactorsParse` (2- and 4-factor forms, errors name the valid factors). `RenderBlendGpu.CustomDstColorSrcColor` (multiply ×2) golden on glcore, matched on Vulkan. Red: a manifest error "blend" |
| AC10 | F10 | `RenderShadowGpu.ProjectShaderDepthProgramClipsShadow`: a project shader with `@depth` discarding the right half casts a half shadow. Red: a full square shadow |
| AC11 | F11 | `test_lua_mesh.py::test_set_shader_texture`: in Play, `Mesh:setShaderTexture` changes `get_entity` submesh `shaderTextures` and the screenshot. `setTexture` on a custom shader writes its `main` texture. Red: a nil method error |
| AC12 | F12 | `TextureTest.ReadyAfterLoad` (false before the GPU texture, true after `load()` and an upload). The stub is in `test_lua_api.py`. Red: a missing property |
| AC13 | F13 | `ShaderManifestTest.RequireVertexNormals` and `ShaderVariantGpu.VertexNormalsWithoutVarying`: it compiles on glcore and Vulkan, and reflection shows `a_normal` with no `v_normal`. Red: an unknown require |
| AC14 | F14 | `ShaderManifestTest.GameTimeIsAnOrdinaryProperty`: `@property gameTime float 0` parses with no error and a per-mesh value is written. Red: the error at `:624` |
| AC15 | F15 | Dropped: S99 (#149) delivers the gradient ambient and its tests (Deviations, P3 F15) |
| AC16 | F16 | `test_sky.py::test_skybox_visible_and_inactive`: in Play, Lua `sky.visible = false` and then an inactive sky entity each give a screenshot pixel equal to the clear colour, and the probe capture skips them. Red: a Lua error, and the inactive sky still drawn |
| AC17 | F17 | The existing sky goldens (`SkyGpu.*`, `test_post_fx_golden.py` defaults) are unchanged. The S15 frame pass list shows the sky after the opaque queue with a depth test. Red: the pass order before |
| AC18 | F18 | `SkyGpu.PanoramaVertexMapping`: max channel error ≤ 3/255 against per-pixel mapping outside ±10° of the poles (a documented band). The `check_shader` source of the variant has no `atan`, `acos` or `dFdx`. Red: the feature is unknown |
| AC19 | F19 | `PostColorGpu.VignetteRadii`: pixels at r < start are untouched, at r > end fully darkened, and at mid within 1/255 of smoothstep. Smoothness mode matches `test_post_fx_golden` `defaults.png`. Red: the field is unknown |
| AC20 | F20 | `PostColorLutTest.FlipGreenReadsTopRowFirst`: a strip with green 0 at the top gives the identity after the flip. Red: colours shift |
| AC21 | F21 | `SkyGpu.PanoramaRotationMatchesUnity` and `SkyGpu.CubemapFacesMatchUnity`: a marker texel's screen position for rotations 0, 90 and 200 against Unity's direction formula through the documented mapping. The docs are updated. If the cubemap is mirrored, the migration test in AC51 covers it |
| AC22 | F22 | `OutlineGpu.Modes`: goldens of 5 modes × 2 width units on an occluded cube. `OutlineGpu.DisabledAddsNothing`: the draw and pass counts of a disabled or absent outline are equal. `test_outline.py`: add, set, undo, screenshot with outline-colour pixels, `get_entity`. Lua play test. Red: no component |
| AC23 | F23 | `OutlineGpu.ModelRootOutlinesSkinnedChild` (`rigged_cube.fbx` root). `test_materials.py::test_preset_on_model_root_descends` with `next` present. Red: "has no Mesh component" |
| AC24 | F24 | `StreamMigrationsTest.SceneV8TextPivotsBecomeAnchor` with prefab twin fixtures. `TextLayoutTest.AnchorVerticals`: the bounding box of each mode against font metrics, within 0.5 px. Red: the fields are absent |
| AC25 | F25 | `TextLayoutTest.SizeKnownBeforeFirstDraw`, and a Lua play test (`t.width > 0` right after `setText`). Red: 0 |
| AC26 | F26 | `TextLayoutTest.EmSizeMatchesCss`: LiberationSans 100 Em cap height = capHeight × 100 / unitsPerEm ± 0.5 px, and Pixel Height ÷ Em = 2048 / 2288 ± 1%. Red: the field is absent |
| AC27 | F27 | `test_camera.py::test_clip_write_clears_auto_resize`: `set_properties camera.topClip` sets `autoResize` false, the bounds survive Play and a canvas resize, and one undo restores both. Red: the bounds are overwritten (R46) |
| AC28 | F28 | `CameraTest.OrthographicSizeFollowsAspect` (1920×1080 then 1080×1920 gives ±size·aspect), Pixel Perfect precedence, Lua and MCP. Red: the field is absent |
| AC29 | F29 | `SpriteGpu.FlipX` golden. `SpriteBatchGpu.FlipXBatchedMatchesUnbatched`. Red: the field is absent |
| AC30 | F30, F31 | `SpriteTest.SizeSetsWorldQuad` (an AABB of 1.5 × 0.75 in all draw modes; Sliced corners unscaled). Lua `go:setScale(1, 2, 3)` gives scale (1, 2, 3). Red: an unknown field, a Lua arity error |
| AC31 | F32, F33 | `AnimatorRootMotionTest.BodyOrientationRemovesStartYaw`: a humanoid clip with hips yawed 30° at frame 0 faces the root within 1° under Body Orientation; Offset 10° gives 10°. `AnimatorRootMotionTest.ApplyOffDiscardsUnbakedMotion`: XZ drift < 1 mm. Red: 30° remains, and the drift is kept |
| AC32 | F34 | `AnimationPlayerRootMotionTest.MovesTheRoot`: a walk clip on the Player moves the entity by the clip's displacement ± 1 cm. Red: the field is absent |
| AC33 | F35 | `UnityAnimImportTest.TransformAndFloatCurves`: a hand-written YAML fixture `editor/tests/fixtures/unity/Idle.anim` (position, euler, quaternion, scale, float and active curves, tangent modes, events, loop) samples equal to the reference values at 7 times. `UnityAnimImportTest.MuscleClipRefused`. `test_animation.py::test_import_unity_anim` through `edit_animation create.unity`, and the refusal text names the route. Red: the refusal |
| AC34 | F36 | `UnityControllerImportTest.StatesTransitionsBlendTrees` and `UnityMaskImportTest.BodyPartsAndPaths` (YAML fixtures with GUIDs resolved through a fixture `Assets/` tree). Red: no importer |
| AC35 | F37 | `editor/tests/engine/physics/RagdollAnimatorTest.RagdollPausesAnimator`: a skinned model with an Animator; on simulate, `bonesMatrix` follows the bodies (a bone moves > 0.1 m as it falls); on release the Animator resumes. Red: the matrices stay on the clip |
| AC36 | F38 | Lua play test: `Ragdoll.build` on `rigged_cube` returns a handle with bodies. Red: nil method |
| AC37 | F39 | `FbxImportTest.ClipOnlyFile`: an ASCII FBX written by the test (a skeleton and one take, no geometry) gives a glb with nodes and an animation. An Animation Player on `rigged_cube` plays `clip.fbx#take`. Red: "no triangle meshes" |
| AC38 | F40 | `FbxImportTest.RestPoseFirstFrame`: node TRS equals frame 0 in First Frame and the node values by default. `get_model_info` gives `first_frame_differs` and `next`. Red: the setting is absent |
| AC39 | F41 | `FbxImportTest.SkinnedGeometricTransform`: the bind-pose skinned positions match ufbx's evaluation within 1e-4. The PR records whether it was red |
| AC40 | F42 | `FbxImportTest.SubmeshOrderFollowsMaterialSlots`: an ASCII FBX whose first-used material is slot 1, plus an empty slot 0. The primitive order and `extras.fbxMaterialSlot` follow the slots. Red: the order swapped (B's eyes/body case) |
| AC41 | F43 | `ModelCookTest.NormalsNone`, `.AreaAndAngleWeighting` (a vertex shared by a large and a small face against reference normals), `.SmoothingGroupsFromFbx`, `.TangentsCalculateMikkt`. `ModelImportSettingsTest` round-trips. Red: the keys are unknown |
| AC42 | F44, F45 | `editor/tests/engine/model/ModelHierarchyTest.PreserveHierarchySingleMesh` (a child entity draws; the root has no Mesh). `test_models.py::test_node_material_error_names_root` and `drawn_by`. Red: the mesh is baked into the root, and the generic error |
| AC43 | F46 | `MeshSystemObjTest.MtlWindowsPaths`: a fixture `.mtl` with `C:\art\tex\a.png` and `tex\b.png` resolves both, with one warning each. Red: missing textures |
| AC44 | F47 | `test_models.py::test_create_entity_animation_entities_false`: the entity count shows no per-clip objects. A `set_properties` write reloads in one undo step. In Auto, an Animator suppresses them. Red: the objects are created (R18) |
| AC45 | F48 | `PsdLayerImportTest.LayersBecomeSprites`: a PSD and a PSB written by the Title Art PSD writer (3 layers and a group) give 3 sprites, a prefab with the group parent, positions within 0.5 px and order. Reimport keeps the ids. Red: no import mode |
| AC46 | F49 | `SpriteSkinGpu.TwoBoneBend` golden. `SpriteSkinTest.AutoWeightsNormalised`. `SpriteSkinGpu.AbsentAddsNothing`. `test_sprite_skin.py` (create bones, auto geometry and weights, undo, Play pose). Red: no component |
| AC47 | F50 | `TiffReaderTest.Baseline`: in-test fixtures for each compression, bit depth and channel count decode equal to the source pixels. `UtilTest.ImageExtensions`. Red: the decode fails |
| AC48 | F-PB1, F-PB2 | Every new `*Gpu` suite passes on glcore and Vulkan. `tools/build.sh web` and `tools/build.sh android` build. The web gate runs an outline scene and a custom blend scene (`tools/build.sh web-gate`) |
| AC49 | Lua | `tools/lua_api/tests/test_lua_api.py` passes with every new stub and its doc blocks |
| AC50 | MCP text | `tool_budget.py --check tools/mcp/budget.json` passes, `docs/mcp/TOOLS.md` is regenerated, and every new eval case fails without its text |
| AC51 | Data | All migration tests pass. `tools/test-projects/upgrade-from-main` loads with no new warnings. A copy of ARCADE's scenes M and K loads on a scratch editor with no manifest errors (F14) |
| AC52 | Evidence | Screenshots from a scratch editor of: outline modes, gradient ambient, text alignment, Flip X, Sprite Skin and PSD import |

## 8. Dependencies & conflicts

- **Depends on (all landed):**
  - S14, S20 (with BF23), S21, S41, S45, S52, S53 and S55;
  - S87's PSD reader;
  - S15 pass timings (for F-PF6).
- **S92 (priority).**
  - R66 changes LuaBridge overload dispatch (`engine/libs/luabridge3/LuaBridge.h`) and may reorder
    overloads in `GameObjectClassesLua.cpp`. S94 adds no same-arity userdata overload set.
  - R57 changes HTML UI composition under ortho cameras (`UIDocumentSystemRender.cpp:94-115`). F28
    must keep UI on screen with an Orthographic Size camera: AC28 adds an HTML UI screenshot after
    S92 lands. If S92 lands later, S92 owns that check.
- **S95 (Lua runtime).**
  - It shares the binding files `GameObjectClassesLua.cpp`, `CoreClassesLua.cpp` and their stubs.
  - S7 (cameras from Lua start in look-at mode) touches `Camera.cpp` next to F27/F28.
  - S31 (particles frozen on scene re-entry) touches `ParticleSubsystem.cpp` next to F2.
  - Coordinate: P1 and P5 rebase on S95's binding changes, or the reverse, whichever lands second.
- **S96 (physics).** `PhysicsSystemRagdoll.cpp` and `RagdollComponent.h` (F37, F38). S96 owns
  ragdoll physics behaviour, and S94 touches only the driver pause and the Lua build.
- **S98 (editor/tooling).**
  - E35 (`.psb` and Unity PSD sprites) is closed by F48; S98 should mark it so.
  - E39/E40 (FBX import warnings) touch `GlbBuilder.cpp` with P7.
  - E37 (`create_particles` speed) touches `WorldTools.cpp`.
  - E41 (`sortPriority` key) is fixed by S52 Order in Layer.
  - E3 (prefabs embed materials) relates to R38.
- **Merge hotspots:**
  - `RenderSystem.cpp` and `.h`: P2, P3 and P4, in disjoint regions (§10);
  - `GameObjectClassesLua.cpp`: P1, P2, P3, P5 and P6, each in its own class block only;
  - `InspectorWindow.cpp`, `editor/reflect/components/RenderReflect.cpp` (P2 Mesh, P5 Sprite);
  - `editor/mcp/tools/AssetTools.cpp` (P2 `set_shader_uniform`, P4 `apply_material_preset`, P8
    texture import);
  - `WorldTools.cpp` (P6 `edit_animation`/`create_ragdoll`, P7 `get_model_info`);
  - `StreamMigrations.cpp` (P2 project, P5 scene and prefab);
  - `tools/mcp/budget.json`, `docs/mcp/TOOLS.md`, `tools/mcp/eval/cases.py`,
    `editor/tests/reflect/catalog_snapshot.json`, `editor/tests/reflect/parity_golden/` (everyone).

## 9. Risks & defaults

| Risk / question | Default taken |
|---|---|
| Unity's submesh rule may not be "node material slot order" for every FBX | AC40 pins the slot rule. A scratch reimport of ARCADE's B `Walk.fbx` (Unity: body, eyes) confirms it. If it disagrees, the slot index in extras lets the converter and the importer remap, and Deviations records it |
| Sky after opaques may change a scene whose shader reads scene colour before the sky | AC17's goldens. A scene-input shader keeps the sky first (`sceneInputs`), recorded |
| Outline stencil on WebGL default framebuffers | Offscreen target (F-PB2). AC48's web gate |
| Default sort mode differs between old (Perspective) and new projects | Intended (migration). `docs/rendering-2d` and the guide say which to choose |
| F33 changes old Animators that relied on drift with Apply Root Motion off | Limited to clips with `root_motion`; noted in `docs/animator.md` "Changes" |
| Smoothing groups exist only in FBX | `smoothnessSource` falls back to From Angle for glTF, with one note |
| TIFF variants beyond baseline (JPEG-in-TIFF, CMYK, layers) | Refused with a message naming the supported set |
| Sprite Skin authoring UI is large | Shipped in phases inside P8 (geometry and weights first, brush last). The MCP tool and Auto Weights are enough for agents in phase 1 |
| Per-instance overrides on Instanced Mesh and IBL from a panoramic sky | Follow-ups, not in S94 |
| F2 forwarding may hide that old scripts should move to the new API | Each forward logs one deprecation note per entity |

## 10. Work packages

Eight packages, each in its own worktree `.worktrees/s94-<pkg>` on
`feature/s94-rendering-anim-gaps--<pkg>`, merged into `feature/s94-rendering-anim-gaps`. Rules for
shared files:

- **`GameObjectClassesLua.cpp`:** edit only inside your class's `beginClass/deriveClass` block. New
  classes go in new `*Lua.cpp` files registered beside `SortingClassesLua.cpp`.
- **`RenderSystem.cpp`:**
  - P2 owns `updateMVP` and the queue build (≈7250-7300, 17440-17460, 18440-18460, 19600-19650) and
    `writeSubmeshCustomUniforms` (≈495-520).
  - P3 owns the sky draw and probe gate (3185-3200, 19695-19730), the vignette (5150-5185) and the
    ambient uniforms.
  - P4 adds `RenderSystemOutline.cpp` and one call after the opaque queue.
  - Anyone else rebases.
- **Reflection, budget, TOOLS.md, eval cases and catalog/parity snapshots:** regenerate at merge;
  `cases.py` is append-only.
- **Migrations:** P5 takes scene v9 and prefab v10. P2 takes project v2. A later need takes the
  next number at merge.

| Pkg | Name | Items (F / ledger) | Owns | Shared hotspots | Size | Depends on |
|---|---|---|---|---|---|---|
| P1 | `particles` | F1-F4 / R54, R29, R27 (+ migration compat) | `engine/core/object/Points.*`, `engine/core/object/ParticleSystem.*`, `engine/core/particles/LegacyParticleConverter.*`, `engine/core/component/ParticleSystemComponent.h` (`legacyFrom`), `engine/tests/core/particles/`, `tools/mcp/tests/test_particles_legacy.py`, `docs/mcp/guide/particles.md` | `StreamMigrations.cpp` (the particle migration function only, no version bump), `GameObjectClassesLua.cpp` (Points and ParticleSystem blocks), `TextureData.cpp` (only if AC3 is red) | S | — |
| P2 | `draw-materials` | F5-F14 / R44, R63, R15, R30, R3, R53, R62, R68, R36 | `engine/core/registry/SortingLayers.*`, `RenderSystemSorting.cpp`, `RenderSystemBatch.cpp` (override hash), `engine/core/shader/ShaderManifest.*`, `ShaderUniforms.*`, `engine/core/texture/Material.h` and its codec, `engine/core/object/Mesh.*`, `engine/core/texture/Texture.*`, `engine/shaderlib/includes/surface_vs.glsl`, new `engine/core/script/binding/MaterialPropertyBlockLua.cpp`, `editor/window/InspectorRenderers.cpp`, `InspectorMaterialPbr.cpp`, `editor/reflect/components/MaterialReflect.*`, `docs/mcp/guide/materials.md`, `shaders.md`, `rendering-2d.md` | `RenderSystem.cpp` (its regions), `RenderReflect.cpp` (Mesh), `AssetTools.cpp` (`set_shader_uniform`), `editor/Stream.cpp` and `StreamMigrations.cpp` (project v2), `GameObjectClassesLua.cpp` (Mesh block), `CoreClassesLua.cpp` (Texture) | L | — |
| P3 | `lighting-sky-post` | F15-F21 / R55, R67, R49, R50, R11, R35 | `engine/core/SceneSettings.*`, `engine/core/serialization/SceneSettingsCodec.cpp`, `engine/core/Scene.*` (ambient), `engine/core/object/environment/Skybox.*`, `engine/core/util/PostColorLut.cpp`, `engine/shaderlib/library/panoramic_sky.*`, `engine/shaderlib/includes/` (ambient, post_color), `editor/window/InspectorPostProcessing.cpp`, the Lighting rows of `InspectorWindow.cpp`, `LightingReflect.cpp`, `editor/mcp/tools/SceneTools.cpp`, `docs/shaders/library.md`, `docs/unity-glossary.md`, `docs/mcp/guide/lighting.md`, `post-processing.md` | `RenderSystem.cpp` (its regions), `GameObjectClassesLua.cpp` (Skybox block), `CoreClassesLua.cpp` (Scene) | M | — |
| P4 | `outline` | F22-F23 / R1, R32 | new `engine/core/component/OutlineComponent.h`, new `RenderSystemOutline.cpp`, new `engine/core/script/binding/OutlineLua.cpp`, `engine/shaderlib/library/outline_stencil.*` (if extended), `editor/material/MaterialPresets.cpp`, the Outline Inspector (new `editor/window/InspectorOutline.cpp`), its reflection table entry, `tools/mcp/tests/test_outline.py`, `docs/mcp/guide/materials.md` §Outline | `RenderSystem.cpp`/`.h` (one call, a pass member), `AssetTools.cpp` (`apply_material_preset`), the component registry and factory | L | — |
| P5 | `text-camera-sprite` | F24-F31 / R42, R43, R46, R9, R59 | `engine/core/component/TextComponent.h`, `engine/core/object/ui/Text.*`, `engine/core/subsystem/UISystem.cpp` (text layout), `engine/core/util/STBText.cpp`, `engine/core/component/CameraComponent.h`, `engine/core/object/Camera.*`, `engine/core/component/SpriteComponent.h`, `engine/core/object/Sprite.*`, `UIReflect.cpp`, `CameraAudioReflect.cpp`, `PixelArtReflect.cpp`, `editor/window/InspectorWindowCamera.cpp`, the scene v9 / prefab v10 migration and its fixtures, `docs/mcp/guide/ui.md`, `camera.md`, `sprites.md` | `RenderSystem.cpp` (`updateCameraSizeForCanvas` only), `MeshSystem.cpp` (sprite quad build only), `RenderSystemSpriteBatch.cpp`, `RenderReflect.cpp` (Sprite), `GameObjectClassesLua.cpp` (Text, Camera, Sprite, GameObject `setScale`), `StreamMigrations.cpp` | M | S92 R57 for the AC28 UI check |
| P6 | `animation` | F32-F38 / R64, R61, R6, R7, R13 | `engine/core/animation/AnimatorInstance.*`, `AnimatorAssetIO.*`, `AnimationPlayerRuntime.*`, `ModelClipSettings.*`, `AnimationClipIO.cpp` (refusal text), `engine/core/component/AnimationPlayerComponent.h`, new `editor/unity/UnityAnimImport.*` and `UnityControllerImport.*`, `editor/window/InspectorAnimation.cpp`, `ModelClipSettingsView.cpp`, `editor/mcp/tools/AnimationClipTools.cpp`, `AnimatorTools.cpp`, `docs/animator.md`, `docs/mcp/guide/animation.md`, `animator.md` | `PhysicsSystemRagdoll.cpp` and `RagdollComponent.h` (S96), `WorldTools.cpp` (`edit_animation`, `create_ragdoll`), `GameObjectClassesLua.cpp` (AnimationPlayer, Ragdoll blocks) | L | — |
| P7 | `model-import` | F39-F47 / R47, R37, R22, R17, R33, R41, R4, R52, R23, R18 | `editor/util/FbxImporter.*`, `editor/util/GlbBuilder.*`, `editor/model/ModelImportSettings.*`, `ModelCooker.cpp`, `ModelSettingsEdit.*`, `editor/window/ModelImporterView.cpp`, `editor/command/type/ModelLoadCmd.cpp`, `editor/mcp/tools/EntityTools.cpp` (create order), `MeshTools.cpp`, `engine/core/component/ModelComponent.h`, `editor/tests/util/FbxImportTest.cpp`, `editor/tests/model/`, `docs/mcp/guide/meshes.md` | `MeshSystem.cpp` (`loadGLTF` hierarchy ≈5330-5420, `loadOBJ` ≈6700), `WorldTools.cpp` (`get_model_info`), `McpValues.cpp` (Model reload), `ProjectSettingsWindow.cpp` (Mesh tab), `engine/libs/tinyobjloader` (patch ledger, only if needed) | L | — |
| P8 | `psd-sprite-skin` | F48-F50 / R45, R5 | new `editor/texture/PsdLayerImport.*`, new `editor/texture/TiffReader.*`, `editor/texture/TextureCookQueue.cpp`, `TextureEncoders.cpp`, `editor/util/Util.h`, `editor/asset/AssetTypes.cpp`, `editor/window/TextureImporterView.cpp`, new `engine/core/component/SpriteSkinComponent.h` and its system, new `engine/core/script/binding/SpriteSkinLua.cpp`, new `editor/window/SkinningEditorWindow.*`, new `edit_sprite_skin` in `editor/mcp/tools/SpriteTools.cpp`, `docs/mcp/guide/sprites.md` §Skinning | `AssetTools.cpp` (texture import settings), the sprite sheet metadata codec (S45) shared with P5's Sprite fields (different keys), `MeshSystem.cpp` (skinned sprite mesh build) | L | — |

Order inside each package: the failing tests first (red), then the feature, then the editor UI,
Lua, MCP and docs, then regeneration (AGENTS.md "Land"). Every package runs `tools/check-changed`
and its filtered suites only (owner's rule, BRIEF "Testing rules").

## Deviations

- **Coordinator decisions.**
  - R57 (HTML UI under an orthographic camera) moved to S92 with U27 and U34: it lives in the HTML UI
    composite code S92 rewrites. AC28's HTML UI screenshot under an Orthographic Size camera therefore
    waits for S92 (PR #10); the rest of AC28 is verified here.
  - R13 is split: S96 (WP-D) owns the runtime ragdoll builder, its Lua build (F38) and the ragdoll
    physics; S94 P6 only the Animator pause (F37).
  - S98's E35 (`.psb` and Unity PSD sprites) is closed here by F48 (P8); E41 is R48 (FIXED).
  - Version numbers: scene 9 / prefab 10 (P5), project format 2 (P2), cooked scene format minor 7
    (P3; 6 was S92's) and built-in shader cache v69 (P3) are all above `main`'s. S98 also takes
    scene 9 / prefab 10, S92/S95/S96 cooked minor 6 (S95 also 7) and S92 the shader cache v69: the
    PR the owner merges second renumbers and chains its migration after the first one's.
- **Integration.**
  - `transparencySortMode`, `transparencySortAxis`, `sortingLayers` and `sortingLayerNextId` are now
    known project.yaml root keys. S52 wrote them without listing them, so a project using them logged
    "unknown key" on every load; P2's project migration writes `transparencySortMode` into every older
    project, which would have made that warning universal (AC51).
  - `set_texture_import_settings`'s `import_mode` text says "a frame per layer" instead of "layers
    sprites": the word made an unrelated API search ("Sprite setTexture") offer that tool
    (`test_docs.py::test_search_names_unlisted_tools`).
  - The Mesh Inspector's "Edit the parts" tooltip asked `IsWindowHovered` for an item-only flag, an ImGui
    assert shown over the editor whenever a Model drawn by its parts is selected (a main bug that
    Preserve Hierarchy, F44, makes common): fixed.
  - Android x86_64 did not build on `origin/main` (`97d450661`, red control on the same image): CMake
    3.22's FindThreads falls back to `-pthread` on NDK 28 (its libc test calls `pthread_cancel`), and since
    main stopped passing `-w` to NASM (`31a5646d1`) that interface option reaches dav1d's NASM sources as
    `-p thread`. dav1d's CMake no longer links Threads on Android (bionic's libc holds the pthreads).
  - AC48: no web gate scene for Blend Mode Custom was added (P2); the custom blend is covered by
    `RenderBlendGpu.CustomDstColorSrcColor` on glcore and Vulkan, and the Outline gate runs on WebGL2.
  - AC28's HTML UI check under an Orthographic Size camera: on this tree (without S92) a Screen Space UI
    Document draws under the camera at Size 0 and disappears at Size 5 (R57: the UI goes through the
    camera's projection). S92's composite stage owns the fix; the world part of AC28 is verified here.
- **P1, F3 (R29).** `ParticleRenderGpu.AdditiveRgbAndGreyTexturesDraw` was green before any change:
  RGB and grey PNGs on additive particles draw white with the Texture Importer's defaults, so F3 is
  closed as a pinning test (as AC3 allows). A grey source with no import settings at all (R8's case)
  still draws red; the editor and exports always import with settings, so it is left as is.
- **P1, AC2 fixture.** The K layout is the fixture project `tools/fixtures/particles-legacy`
  (`Legacy K.scene`), not a migration fixture `scene-v7-legacy-particles-k.scene`: the test plays it
  in an editor, which needs a project.
- **P2, F7 storage.** Property Overrides are stored out of line on the Mesh and on each Submesh, so
  meshes without overrides keep their size (the MeshMemory ceilings). Blend Mode Custom also has
  ShaderLab's separate alpha `BlendOp` (`blendOpAlpha`).
- **P2, test files.** AC7's MCP checks and AC11 (`Mesh:setShaderTexture`) are in
  `tools/mcp/tests/test_draw_materials.py` (`test_property_overrides`,
  `test_lua_property_block_and_textures`), not in `test_materials.py` / `test_lua_mesh.py`. AC23's
  preset check is `test_outline.py::test_preset_on_model_root_descends`.
- **P5, F24 vertical anchor on a UI camera.** The modes are measured along the Text's local +y.
  In a y-up world view that is the screen's up, as specified. A UI camera draws y down, so there
  Top and Bottom swap on screen; Baseline, Capline, Midline and Middle do not. The other choice
  (screen-up everywhere) would have moved every existing UI-camera Text, whose stored default is
  Bottom.
- **P5, F24 Capline and Midline** put the middle of the first line's capitals (cap height / 2) and
  of its lowercase letters (x-height / 2) on the origin, as TextMesh Pro's Capline centres the
  capitals. That is what centring a number on a point needs (R42's door numbers).
- **P5, F24 metrics fallbacks.** Cap height: OS/2 `sCapHeight`, else the `H` glyph's box, else
  0.7 em. x-height keeps `FontFace::getXHeight`'s fallback (`x` box, then ascender / 2), which the
  HTML UI's `ex` unit shares. Bitmap fonts use 0.7 / 0.5 of their native size.
- **P5, F24 is Unity TextMesh's Anchor, not an alignment** (user decision, with S93's Text
  Alignment): the fields are `anchorHorizontal` / `anchorVertical` (enums `TextAnchorHorizontal`,
  `TextAnchorVertical`, Lua `TextAnchor`, Inspector "Anchor"), and they only place the box on the
  origin. How the lines sit inside the box is S93's Alignment (left/center/right/justified,
  top/middle/bottom/baseline, plus Auto Size), a separate row, as in Unity. So Center and Right no
  longer align each line: a migrated multi-line `pivotCentered` text draws as before
  (`pixel_fonts/vector_text.png` back to main's). The scene 10 -> 11 and prefab 10 -> 11 migrations
  write the anchor keys.
- **P5, F26 AC26** measures the built-in Roboto (its own unitsPerEm and ascender - descender)
  instead of LiberationSans, which is not in the repository; the formulas checked are the same.
- **P5, F27** also turns the Orthographic Size off on a bound written by hand (Lua setters, MCP,
  Inspector), for the same reason as Auto Resize. The MCP names of the bounds are the reflected
  `camera.left`, `right`, `bottom`, `top` (`leftClip`... are their YAML keys).
- **P5, F28** applies to Orthographic cameras only; a UI camera keeps its pixel bounds.
- **P5, F30 Size** is per axis: an axis at 0 keeps width or height over Pixels Per Unit.
- **P5, AC28** HTML UI under an Orthographic Size camera: pending S92's R57 (not on this base).
- **P4 (Outline).**
  - The outlined submesh's render is loaded and drawn by `RenderSystem::loadOutlineRender` /
    `drawOutlineRender` in `RenderSystem.cpp`, beside the extra pass they follow, because they need
    that file's local binding helpers (skinning, lattice, VAT, custom blocks). The pass itself
    (gathering, mode states, sweep, `findOutline`) is `RenderSystemOutline.cpp`. Hooks besides the
    call after the opaque queue: one after a 2D pass's hierarchy (2D passes have no opaque queue),
    one line in `destroyMesh` and `destroy`, and the stencil-less swapchain redirect (F-PB2).
  - Renders use the dynamic pipeline path (`ObjectRender::endLoadDynamic`), so the mask, the fill
    and every mode share one render per submesh; only a change of Width Units makes it again.
  - Smoothed normals come from the existing per-geometry cache (`bindSmoothNormals`) when an
    outline first draws a mesh, on the main thread, not on a job (F-PF2): the extra-pass outlines
    already work that way, and a job would need a copy of the positions anyway. A reload of a mesh
    whose other shaders do not read them makes them again.
  - The Inspector is the reflected default drawer (Unity names, undo) plus a note when nothing is
    outlined, so no `InspectorOutline.cpp`.
  - Not outlined: Terrains, Tilemaps, Sprite Batch instances and non-triangle primitives. A 2D
    camera draws outlines after its whole hierarchy, so over that camera's component UI.
  - Exports ship the outline variants of every mesh an Outline covers, and of every mesh when the
    project's scripts mention `Outline` (`ShaderVariantCollector`, `scriptsAddOutlines`).
- **P8 (F48) prefab path.** "`<file>.prefab`" is read literally: `art/hero.psd` writes
  `art/hero.psd.prefab`, so a `hero.png` and a `hero.psd` side by side never share one.
- **P8 (F49) skin storage.** The skin is kept per frame name in the meta of the sheet's source
  (`import.spriteSkin`, frame pixels) and the builders attach it to the record's frames (`skins`, an
  optional list of the sheet record, no version bump). Textures (every Sprite Mode) and PSD/PSB in
  Layers mode carry skins; an `.aseprite` sheet or a `.spriteatlas` refuses an edit with a message
  (a Sprite of an atlas skins its source texture, and the atlas remap keeps the frame's skin).
- **P8 (F49) Auto Geometry.** The mesh is a triangle grid of Cell Size pixels kept where a cell holds
  an opaque pixel (it follows the alpha outline to within one cell), not a traced outline with a
  constrained triangulation: deterministic, never a sliver, and dense where the skin bends.
- **P8 (F49) what draws skinned.** A frame drawn Sliced, or instanced by a Sprite Batch, draws its
  quad (a 9-slice and the instance buffer carry no bone weights). Preview Pose bends the mesh in the
  Skinning Editor only; the scene's pose is the bone GameObjects'.
- **P3, F15 (gradient ambient), AC15, F-PF5.** Not delivered here: S99 (PR #149) implements the
  same Unity Environment Lighting Source under the same keys (`ambientMode` Color/Gradient,
  `ambientSkyColor`, `ambientEquatorColor`, `ambientGroundColor`) and lands first, so by the lead's
  decision this branch dropped its own version (MESH bit 55 `'Agr'`, `includes/ambient.glsl`, the
  cooked format minor 7, the `AmbientGpu`/`AmbientGradientShader` tests and the
  `guard_ambient_gradient_setting` eval case) and uses S99's. S99's default colours go through the
  engine's gamma-2.2 conversion where this branch's were exact sRGB.
- **P3, F17 (sky after opaques).** No scene-input exception was needed: the scene-colour copy is
  taken after the opaque queue and the sky in both orders. Besides a camera without depth test,
  the sky also stays first under a Video Player's far plane (drawn over it without depth). The
  pass-list check of AC17 is the draw order in `SkyGpu.DrawsAfterTheOpaqueQueue`.
  An opaque-queue mesh with vertex alpha below 1 (the legacy pipelines blend) now composites over the
  clear colour instead of the sky, and its depth hides the sky behind it: such a mesh belongs in the
  transparent queue (the navigation and voxel overlays' fills were moved there).
- **P3, F18.** Per-Vertex Mapping samples the panorama's base level (`textureLod` 0): no
  per-fragment LOD without derivatives or a transcendental. Documented: keep it off for a
  panorama minified on screen.
- **P3, F21.** The cube map is not mirrored by a z flip: the engine samples the cube in its own
  right-handed space with the GPU's face rule, so a Unity face set loads with +X and -X swapped
  and every face mirrored left to right (and `rotation` = -`_Rotation`). That is a convention, not
  a bug, so no scene migration. The formula for Skybox/Panoramic is in the manifest description
  (and so `docs/shaders/library.md`); both are in `docs/unity-glossary.md` (the cube map is not a
  library shader).
- **P3, F16.** An inactive sky entity stops drawing and feeding probes, as a hidden one; its IBL
  keeps lighting the scene, as `visible = false` does.
- **P6, F38.** `Ragdoll.build` is not in P6: the package brief gave the runtime ragdoll builder and
  its Lua API to S96 (R13 split). P6 does only F37, the Animator pause.
- **P6, F32 definitions.** Body Orientation is the heading of the hips line (upper legs) plus the
  shoulders line (upper arms, when both are mapped). Center of Mass weights the humanoid slots'
  joints with Dempster-like mass fractions. Feet is the lower foot joint. "At start" aligns the
  clip's frame 0 on the model's rest pose: the body faces the root's forward, and the feet or the
  centre of mass stand where the rest pose has them. The Inspector shows Unity's labels
  ("Center of Mass").
- **P6, F34.** The Animation Player adds the root motion of its base layer's states by weight. The
  pose's root is removed with the heaviest of those states' settings, after the blend. A state that
  plays through the humanoid path (a humanoid clip or a Retarget entry) has no root motion on the
  Player and says so once; the Animator is the humanoid root motion path.
- **P6, F35.** An imported clip gets a `root_motion` block only when one of Unity's Bake Into Pose
  flags is set, so a prop clip does not start playing in place under F33. The block then carries
  Unity's Based Upon and offsets.
- **P7, F41 (R22).** `FbxImportTest.SkinnedGeometricTransform` (a GeometricTranslation on the
  skinned mesh, a PreRotation of -90° and a scale of 0.5 on its parent) matched ufbx's evaluated
  skin within 1e-4 before any change: it was green in the red run. The import already converts
  geometric transforms with ufbx (`MODIFY_GEOMETRY`), so `FbxImporter`/`GlbBuilder` needed no fix
  and F40 alone covers R22. CLEIVER's `cleiver@T-Pose.fbx`, reimported on a scratch editor, shows
  the cause: its only clip (`mixamo.com`) starts with the hips at y = 2.82 where the node has -2.80
  (`first_frame_differs`, distance 5.6); with Rest Pose: First Frame the difference is gone.
- **P7, F40 default take.** "The first take" is the first clip the import makes: takes that animate
  nothing make no clip and are skipped (CLEIVER has 13 empty takes before `mixamo.com`), so the
  default matches the Take the Inspector shows. `restPoseTake` names a clip; one that is not a clip
  of the file falls back to the first clip, with a warning.
- **P7, F42.** An empty slot before the last drawn one is kept as a primitive of one degenerate
  triangle (`extras.fbxEmptySlot`; glTF has no empty primitive); empty slots after the last drawn
  one write nothing. The FBX importer version is 2: a reimport whose submesh order moved logs a
  Console note naming the mesh. ARCADE's B `Walk.fbx` reimported on a scratch editor gives slot 0
  `Material.003` (Unity's `EntityBody`, by the Unity `.meta`'s external objects) then slot 1
  `Material.004` (`Eyes`): Unity's renderer order (body, eyes).
- **P7, F42, and P6, F33/F34 (old projects).** By the lead's rule that old projects behave as
  before, neither change reaches saved content. The project 1 -> 2 migration sets Project Settings >
  Mesh > FBX Submesh Order to Used Materials (the old order: the used slots, an empty one shifting the
  next ones); new projects take Material Slots. The scene 8 -> 9 and prefab 9 -> 10 migrations tick
  Legacy Root Drift (`legacyRootDrift`) on every Animator and Animation Player without Apply Root
  Motion, which keeps a clip's drift with Apply Root Motion off; new components play in place.
- **P7, F43.** Tangents: Calculate is meshoptimizer's `meshopt_generateTangents` in its
  MikkTSpace-compatible mode (the vendored library, unpatched). Normals: None also drops the
  tangents. Smoothing groups go from the FBX conversion to the cook as a `_SMOOTHING_GROUP` vertex
  attribute that the cook always drops. `smoothnessSource`, `restPose` and `restPoseTake` are
  per-model keys read by the FBX conversion, never per platform.
- **P7, F44 behaviour changes.** The GameObjects the editor makes to draw the Model's own Mesh
  (Vertex Animator placements, the terrain's Instanced Mesh hosts) keep Preserve Hierarchy off.
  `edit_lattice` treats an empty Mesh (a Model drawn by its parts) as no mesh of its own and binds
  the children. A Lattice Modifier on such a Model reaches its nodes' meshes, as Include
  Children does, in the draw and the bake (`LatticeRender::reachesChildren`). MCP tests that inspect the Model GameObject's own Mesh (`test_render_stats`,
  `test_render_correctness`) place their models with `preserveHierarchy` false.
- **P7, F45.** The error names the root for `set_properties` Mesh values and for `link_material`
  (the two routes that set a mesh's material or mesh values on a GameObject).
- **P7, F47 red control.** With the create order reverted, AC44's first check (no per-clip
  GameObject after `create_entity` with `animationEntities` false) still passed: the MCP load
  finishes on a later frame and reads the value then. F47 makes the order explicit. The reload in
  one undo step and the Auto suppression by an Animator were red (each with its code reverted).
- **P7, MCP budget.** `set_model_import_settings` has a budget exception (1765 bytes) for the five
  new keys.
