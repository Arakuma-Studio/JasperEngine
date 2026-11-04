# JasperEngine: editor UX audit and design for mesh optimization (human users)

Repo: `/var/lib/orquester/workspaces/jaspersito/forge-meshopt`, commit `b4f7816ea` (main, clean).

**Method and labels.** Read-only. No build, editor or MCP call was run, so nothing here was observed
on screen. Every label refers to reading the code at this commit:

- **[V]** verified: read at the cited `file:line` (paths are repo-relative).
- **[I]** inferred: my conclusion from the code, or general knowledge of Unity or meshoptimizer. Not tested.
- **[P]** proposal: a design decision, not current behaviour.

Unity names and behaviour quoted in the design are from general knowledge, not checked against
current Unity documentation **[I]**.

---

## 0. Summary

**The texture importer is the template, and it is good.** `editor/window/TextureImporterView.cpp` has
Unity-like Apply/Revert, platform tabs with Override, multi-edit with mixed values, a Source/Cooked
preview, a per-platform table, one undo step per Apply, a content-addressed cache, a status-bar
counter with Cancel, Project window badges, and export integration.

**Models have none of that.**

- A model file has no import settings. The `.meta` holds identity only.
- A model without clips does not get an asset view at all: the Inspector falls back to the scene selection.
- Models ship byte for byte.
- LODs are simplified at runtime on every load, are off by default per scene, and have no preview or readout.
- The Scene view has no wireframe, overdraw, LOD or density mode.
- The Stats overlay shows only draw calls and triangles.
- No Build Report exists.
- A `.glb` that requires `EXT_meshopt_compression` produces one Console line and nothing else.

**S36's audio import settings do not exist in this tree.** S36 is spec-only, so it is not a usable
precedent. S14 is the only implemented importer.

**Recommended v1:** a `ModelImporterView` built on the texture importer's parts, Project Settings > Mesh,
a model cook queue on the same status bar, badge and cache UI, Draw Mode (Wireframe, Shaded Wireframe, LOD
Level), extra Stats lines, a Mesh Optimization Audit window, a small Build Report, and failures made
visible and actionable. Quantization, Overdraw, presets and folder defaults come after v1.

---

## 1. How import works today

### 1.1 The `.meta` sidecar (S04)

- **[V]** Models are sidecar-identity assets with sub-assets: `.gltf .glb .obj .fbx` (`editor/asset/AssetTypes.cpp:30`).
- **[V]** The meta holds `metaVersion`, `guid`, `formerGuids`, `subAssets`, plus opaque `import` and `labels` sections
  (`editor/asset/AssetMeta.h:6-12`). Sections it does not own are kept byte for byte, and metas hold no volatile data (`:14-17`).
- **[V]** A new importer can therefore add `import.modelImporter` without a meta format bump: `import:` is opaque to S04,
  and S14 sits in the same section (S14 Deviation 1, `docs/specs/S14-texture-compression.md:539-542`).
- **[V]** Sub-assets are read from the header and JSON only; a GLB's binary chunk is not touched (`editor/asset/ModelSubAssets.cpp:148-167`).
  An FBX has no sub-assets (`:168-173`). `ModelContents` holds sub-assets and URIs, with no triangle or vertex counts
  (`editor/asset/ModelSubAssets.h:26-29`).
- **[I]** Accessor `count` values sit in the same JSON, so an Audit can list triangles and vertices for 500 models
  cheaply, without decoding buffers or textures.

### 1.2 Model "import" today

- **[V]** `Import New Asset...` and `Import Folder...` exist (`editor/window/ProjectWindow.cpp:1105-1109`).
- **[V]** The only model conversion is FBX to `.glb` (`editor/Project.cpp:1040-1075`), with a Reimport menu item for FBX only
  (`ProjectWindow.cpp:2196-2202`). glTF and OBJ have no Reimport item, and the Texture one is gated on `FileType::IMAGE` (`:2068`).
- **[V]** Models hot-reload: parse on a job, adopt on the main thread (`editor/reload/handlers/Models.cpp:38-60`).
  If the hierarchy changed the reload stops with a note, and rebuilding marks the scene modified (`:43-46`, `:55`).
- **[V]** Export copies models as they are. `copyAssets` skips metas (`editor/Exporter.cpp:2804`), replaces cooked textures (`:2806-2809`), and only special-cases
  FBX to its `.glb` (`:2827-2844`). There is no model cook.
- **[V]** `MeshLodPool` is the only runtime use of meshoptimizer on render geometry (`engine/core/pool/MeshLodPool.cpp:164-177`). The other uses are placement collision
  simplification (`engine/core/physics/PlacementWorld.cpp:1345`) and a half-float helper (`engine/renders/sokol/SokolTexture.cpp:312`).
  No dedup, fetch, overdraw or codec call exists. The library is vendored with those sources (`engine/libs/meshoptimizer/`, `NOTICE.md:30`).

### 1.3 S36: spec only

- **[V]** Index says "spec-first" (`docs/specs/README.md:105`). The spec names `editor/window/InspectorAudioClip.cpp` and `import.audioImporter`
  (`docs/specs/S36-sound-library.md:10,31`).
- **[V]** A grep of `editor/` and `engine/core/` for `AudioImport|audioImporter|audio_import` finds nothing, nor does
  `InspectorAudioClip|set_audio_import_settings|provenance` (those also over `tools/mcp`). `editor/audio/` holds only `AudioMixerDocument`.
- **[I]** Take from S36 only the idea worth keeping: settings do not reach the game through metas, so a shipped table is needed
  (`S36:73-74`), and the same is true of cooked models.

### 1.4 S14: the implemented texture importer (the pattern to follow)

**Data**

| Behaviour | Evidence |
|---|---|
| Settings at `import.textureImporter`, `version: 1`; absent fields resolve to type defaults, then project per-family defaults, then platform override; unknown keys survive | **[V]** `editor/texture/TextureImportSettings.h:4-16` |
| Families: Desktop, Mobile, Web, plus Android ETC2 when enabled | **[V]** `TextureImporterView.cpp:166-171` |
| Settings inferred once and written to the meta **outside undo history** ("the settings a texture always had, written down") | **[V]** `editor/texture/TextureCookQueue.cpp:832-835` |

**Inspector flow** (`editor/window/TextureImporterView.cpp`)

| Behaviour | Evidence |
|---|---|
| Entry: `InspectorWindow` draws it first when `getInspectedAssets()` is non-empty, read-only under a VCS lock, and returns early | **[V]** `editor/window/InspectorWindow.cpp:12365-12384` |
| Fed by the Project window's selection on click, arrow keys or A | **[V]** `ProjectWindow.cpp:4234-4243` |
| Layout: header, preview, info line, tabs `Default / Desktop / Mobile / Web`, foldouts General / Advanced / Mip Maps, Compression, warnings, platform table, Revert / Apply | **[V]** `:728-813` |
| Multi-edit: `touched` holds only the edited keys, `mixed` flags differing ones, `ImGuiItemFlags_MixedValue` shows them | **[V]** `:188-192`, `:252-267`, `:319-335` |
| Apply: `beginInteractive("Set Texture Import Settings")`, one no-merge `SetAssetMetaCmd` per asset, no-ops dropped, one history entry | **[V]** `:280-307` |
| Revert: clears `touched` and re-reads | **[V]** `:275-278` |
| External change (undo, MCP): re-read when nothing is pending | **[V]** `:209-219` |
| Preview: source and cooked decoded on Background jobs, composited on the CPU with a split line, R/G/B/A toggles, mip slider | **[V]** `:513-611`, `:613-681`, `:683-721` |
| Status: failed row with tooltip, inline warnings and error | **[V]** `:500-508`, `:795-800` |

**Cook and status**

| Behaviour | Evidence |
|---|---|
| Queue: Selected > Loaded > Background priorities; `invalidate`, `cancelAll`, working-memory budget | **[V]** `editor/texture/TextureCookQueue.h:4-10`, `:112-126` |
| Cache: `.jasperengine/cache/textures/v1/<k[0:2]>/<k>.ktx2`, keyed on content, resolved settings and encoder versions; renames do not recook | **[V]** S14 R7, D4 (`:157-168`, `:342-343`) |
| Status bar: `Cooking textures 12/40` with Cancel and a tooltip | **[V]** `editor/window/StatusBar.cpp:56-71` |
| Project badge: queued, cooking, failed ("the Inspector shows why"); images only | **[V]** `ProjectWindow.cpp:4624-4646` |
| Menu: Reimport, Reimport All Textures (desktop family only) | **[V]** `ProjectWindow.cpp:2068-2087` |
| Export window: Textures foldout with Family / Cooked / Download / VRAM and Cook Textures Now | **[V]** `editor/window/dialog/ExportWindow.cpp:1034-1037`, `:1111-1176` |
| Game view: Simulate platform textures | **[V]** `editor/window/GameWindow.cpp:729-769` |
| Project Settings > Texture | **[V]** `ProjectSettingsWindow.cpp:1989-2099` |
| Real-input UI test instead of hand screenshots | **[V]** S14 Deviation 20 (`:608-611`) |

**Do not copy these rough edges**

1. **[V] Unapplied edits are silently lost.** `hasPendingEdits()` is declared (`TextureImporterView.h:43`) and a repo-wide grep finds no caller.
   Selecting another file clears `touched` with no prompt (`:210`, `:226-229`). Unity prompts here **[I]**. This should be fixed once, in shared code.
2. **[V] Multi-select shows the first file only.** Preview, info line, table and warnings use `assetPaths[0]` (`:481-483`, `:747`), and only the first
   is queued at Selected priority (`:270`). The header says "N textures" with no aggregate (`:734`).
3. **[V] Failed parse in Apply is silent:** `continue` with no message (`:292-294`).
4. **[V] Jargon reaches users:** "RDO / ETC1S Level", "Rate-distortion lambda", "Negative: the table's" (`:458-461`), and "Automatic follows the type and quality (R5)", a spec ID (`:446`).
   Also "the spec's table" in `ProjectSettingsWindow.cpp:2004-2005`.
5. **[V] Mixed-value gap:** Alpha Cutoff has no mixed flag (`:417-419`), while Mip Limit does (`:404-407`).
6. **[V] Project Settings > Texture is not undoable:** `setTextureSettings` runs directly on OK (`ProjectSettingsWindow.cpp:4342`), while Debug, Threading,
   Navigation and others are project-history commands (`:4523-4543`). A setting that recooks everything should be undoable.
7. **[V] Reusable code is private.** `importerGet/Set/Leaves` sit in an anonymous namespace (`:27-153`), and `comboField/boolField` are private members
   (`TextureImporterView.h:93-95`). A model view needs these extracted, not copied.
8. **[V] `SetAssetMetaCmd` has one global listener slot** (`editor/command/type/SetAssetMetaCmd.cpp:114-122`), taken by the texture queue
   (`TextureCookQueue.cpp:199-205`) and cleared to null on detach (`:174`). **A second queue installing its own would silently break texture recooks.**
   It must become a subscriber list or a dispatcher.
   The key filter `rfind("import", 0)` matches any importer, and `settingsChanged` already returns early for non-textures (`:449-451`), so that part is safe.

---

## 2. Model and Mesh inspector today

### 2.1 Selecting a model file in the Project window

- **[V]** `drawInspectedAsset`: for a model, `drawModelClipsAsset` only when it has clips, otherwise `return false`, and the Inspector shows the scene selection
  (`editor/window/InspectorWindowClips.cpp:1598-1613`). The clips view is a header "Model" and a read-only clip table (`:1702-1724`).
- **[V]** The code itself says so: "this editor's Inspector shows GameObjects only" (`editor/window/ProjectWindowSearch.cpp:681-683`, labels strip comment).
- **Result [V]:** no triangle count, vertex count, memory, material list, preview or import setting for a model asset anywhere in the Inspector.

### 2.2 ModelComponent (scene selection)

- **[V]** Model File with thumbnail, browse, clear, drag and drop (`InspectorWindow.cpp:7713-7829`), Skeleton (`:7835`), a Rig foldout (`InspectorModelRig.cpp:31-100`) and Animations.
- **[V]** Rig is the useful precedent for a model-level setting: stored in a sidecar, one project undo step, disabled outside STOPPED (`InspectorModelRig.cpp:4-7`, `:50`).

### 2.3 MeshComponent

- **[V]** Rows include Cast/Receive Shadows, Static Shadow Caster, Culling Mode (front/back), **Detail Levels** (`lodEnabled`), **Detail Bias** (`lodBias`, only when the scene and mesh have LOD on), Render in Probes, Transparent (`InspectorWindow.cpp:7424-7461`).
- **[V] Naming is inconsistent.** Reflection labels the field "LOD Bias" (`editor/reflect/components/RenderReflect.cpp:191`), the Inspector says "Detail Bias" (`:7452`), the scene section says "Mesh Detail (LOD)" (`:12673`).
  **[P]** Use Unity's words: **Generate LODs**, **LOD Bias**.
- **[V]** No row shows level count, triangles per level or the current LOD. A grep for `numLods` in `editor/` finds nothing.

### 2.4 Scene LOD settings and runtime LOD

- **[V]** Per scene: Mesh Detail (LOD) > Enabled, Threshold (0.25-8 pixels) (`InspectorWindow.cpp:12673-12687`).
- **[V] Off by default:** `meshLodEnabled = false` (`engine/core/SceneSettings.h:354`), including new 3D scenes (`editor/resources/templates/3d/Main.scene:8`). Doing nothing gets no LODs.
- **[V] Built at runtime.** On mesh load, `updateMeshLods` hashes attributes and calls `MeshLodPool::build` (`engine/core/subsystem/RenderSystem.cpp:7217-7270`), a Background/Heavy job (`MeshLodPool.cpp:125-134`).
  Results are in-memory only (`:79-82`), so **every shipped game rebuilds them on every run**. **[I]** That is a real load-time and mobile cost cooking can remove without visible change.
- **[V] Fixed recipe:** targets 1, .5, .25, .125 (`MeshLodPool.cpp:18`), `MAX_MESH_LODS` 4 (`engine/core/Engine.h:93-94`), no knobs. Early stop when a level does not shrink by 10% (`:173`).
- **[V] Screen-error selection, not screen height:** `allowed = threshold * (perUnit * distance + constant) / lodBias`, then the highest level whose `lodError * worldScale <= allowed`
  (`RenderSystem.cpp:8222-8234`).
  **[I]** Unity's percentage bar does not map directly. Per-LOD screen height can be *derived and displayed*, but it cannot be edited without adding a new stored runtime concept. See section 6.2.
- **[V]** No crossfade (`docs/openengine-analysis/01-rendering.md:136`), so enabling LOD can pop.
- **[V]** LOD needs FLOAT xyz positions (`RenderSystem.cpp:7298-7299`). Picking uses LOD0 (`docs/specs/S26-scene-view.md:144`). LODs reuse the source's vertices, and the VAT design relies on that (`docs/superpowers/specs/2026-10-27-vertex-animation-design.md:135-138`).

### 2.5 Previews and thumbnails

- **[V]** Project window model thumbnails render through a hidden scene off-thread with textures capped at 256 (`ProjectWindow.cpp:685-770`, `:3338-3350`).
- **[V]** `MeshPreviewRender` (Scene, Camera, Light, Mesh, `getTexture()`) is used only for the shape popup (`editor/render/preview/MeshPreviewRender.h`, `InspectorWindow.h:215`).
  `VatPreviewRender` and `ParticlePreviewRender` are siblings.
- **[V]** A grep for orbit controls in `AvatarWindow`, `ImageViewerWindow`, `InspectorWindow` and `VertexAnimationBakerWindow` finds none. Interactive orbit would be new in a preview, though simple.
- **[V]** The Scene view renders extra panes through `SceneRender::drawPaneView` with `PaneViewFlags` (`editor/sceneview/SceneViewController.cpp:1279-1283`). That flag struct is the natural home for Draw Mode.

### 2.6 Debug draw modes

| Mode | Status | Evidence |
|---|---|---|
| Wireframe (meshes) | **Absent.** "wireframe" appears only for physics, bones and tooling | **[V]** grep: `SceneRender3D.cpp:765,825`, `SceneWindow.cpp:2484`, `PhysicsDebugLines.h:5` |
| Overdraw | **Absent.** Only appears in a help text and a comment | **[V]** `InspectorWindow.cpp:12696`, `SceneSettings.h:357` |
| LOD colouring, triangle density | **Absent** | **[V]** grep empty |
| Shading choice | Only for inactive panes: Shaded, Shaded + Shadows | **[V]** `SceneViewState.h:68-69`, `SceneViewToolbar.cpp:459-464` |
| Debug views | Gizmos menu > Rendering > "Shadow debug" combo (None, Cascades, Shadow Factor, Texel Density, Cache, Atlas), "Disable face culling", fog, clouds, post | **[V]** `SceneWindow.cpp:2393`, `:2488-2510` |
| S26 and S15 | Neither specifies mesh draw modes | **[V]** grep of both specs |

- **[V]** The shadow debug view is the best precedent: "editor state, not a setting (never saved)" (`engine/core/Scene.h:45-46`, `:221-223`).
- **[V]** A grep of `engine/libs/sokol/sokol_gfx.h` and renders finds no `glPolygonMode` or fill mode.
  **[I]** Wireframe has to be real line geometry built from index data (or barycentric), which is also the only approach that works on every backend. Build it lazily and drop it when the mode is off.

### 2.7 Stats

- **[V]** Game view Stats toggle (`GameWindow.cpp:728`, `:814`) and `drawStats`: FPS, Draw calls, Tris, Screen, Target/VSync, CPU/GPU, Device, Safe area, HTML UI
  (`:1221-1293`). The buffer holds at most 11 lines (`:1226`). Adding lines must raise it.
- **[V]** `Engine::FrameStats` holds only `drawCalls, instances, triangles` (`engine/core/Engine.h:226-230`), filled in `ObjectRender.cpp:128`. No vertices, no LOD histogram.
- **[V]** The main status strip shows draws and triangles (`editor/App.cpp:1583-1590`). The Scene view has no Stats overlay.
- **[V]** Frame Debugger shows triangles per pass (`FrameDebuggerWindow.cpp:263`). Profiler has a VRAM graph by category, with a `Meshes` category
  (`engine/core/profiler/GpuMemory.h:27-37`, `ProfilerWindow.cpp:534-537`), and its detail text lists textures only (`:555-560`).
- **[V]** On-device debug overlay: stats off/fps/full on F3 (`editor/DebugSettings.h:25-30`).

---

## 3. Project settings, export and reports

**Project Settings** uses a registry-driven searchable page list (`SettingsCategoryList.h:42-46`, `SettingsRows.cpp:2927`), with reset-to-default per row (`projectBeginSettingsRow`).

- **[V] Quality page:** VSync plus Atmosphere Quality for Desktop, Mobile, Web, and nothing else (`ProjectSettingsWindow.cpp:1960-1987`).
- **[V] Graphics page:** includes per-platform Shadow Tier rows (`:1876-1878`).
- **[V] Texture page:** per-family defaults, runtime quality and budget per Desktop/Mobile/Web, cache (`:1989-2099`).
- **[V] Player page:** per-OS tabs (`:2867-2910`).
- **Missing [V]:** no mesh settings, no Unity-style named Quality Levels. **[I]** Per-platform is done as repeated rows per platform group, which is a convention worth keeping over a new "levels" concept.
- **[V]** Scene-level render settings (LOD, SSAO, prepass) live in the Inspector's scene section, so "LOD per platform" has no home yet.

**Culling options today**

- **[V]** Camera and light Culling Mask (`InspectorWindow.cpp:8848`, `:10141`), Instances: Cull Instances and Cull Distance (`InspectorRenderers.cpp:651-656`), Mesh Culling Mode and per-submesh Face Culling (`:7440-7443`, `:7699`).
- **[V]** Frustum culling is CPU (`docs/openengine-analysis/01-rendering.md:136`). No occlusion culling or per-mesh draw distance in a UI.

**Build and export (S30, S34)**

- **[V]** Export window: Build and Publish tabs (`ExportWindow.cpp:1178-1185`), Shaders and Textures foldouts (`:1029-1037`), a modal progress bar with step, detail line and Cancel (`:1581-1655`).
- **[V]** Android shows engine library size per ABI and a download warning (`ExportWindowShipping.cpp:242-263`).
- **[V]** Completion shows method, engine modules and left-out modules with evidence, warnings, notes (`ExportWindow.cpp:1613-1639`, S34 R16).

**What ships and how big: no Build Report**

- **[V]** `ExportResult` has no sizes (`editor/Exporter.h:111-127`).
- **[V]** The pack logs "N files, M bytes" to the Console only (`ExporterPrebuilt.cpp:741`). `JPakStats` is just entries and bytes (`editor/cook/JPakWriter.h:17-20`).
- **[V]** With a pack, the manifest lists the pack, and per-file data is not kept (`ExporterPrebuilt.cpp:817-841`). `TreeManifest` records size and hash per file only for releases (`editor/publish/TreeManifest.h:21-32`).
- **[V]** A grep for `BuildReport` finds nothing.
- **[I]** The data exists at the moment before packing: `filesBelow(contentDir)` (`ExporterPrebuilt.cpp:736`). A small category and largest-files report is cheap.

---

## 4. Bulk workflows (500 models)

**What already works**

- **[V]** Ctrl and Shift multi-select (`ProjectWindow.cpp:1985-2012`). Ctrl+A through the Project command context (`:4227-4228`).
- **[V]** Search grammar with `t:model`, `l:<label>`, `path:`, `ext:` (`editor/search/AssetQuery.h:6-15`). An All scope makes the project root current and entries become relative paths (`ProjectWindow.h:214-217`).
- **[I]** So `t:Model` + All + Ctrl+A selects across folders, and `getSelectedFiles` still yields correct paths (`:4648-4654`). This should be confirmed with a UI test. S25 R21 caps shown results at 2000 (spec; the cap was not read in code).
- **[V]** Labels with tri-state menus over a selection (S25 R22-R23, `ProjectWindowSearch.cpp:238`) and saved searches give users named groups.
- **[V]** Multi-edit in the texture importer gives the exact behaviour needed: one edit, N assets, one undo step.

**What is missing**

- **[V]** No import presets, no "Copy Settings", no folder defaults. A grep of `editor/texture`, `editor/asset`, the Project window and the importer finds only unrelated shake and post-processing presets.
- **[V]** Folders are not assets with metas: `AssetTypes.cpp` registers no folder type (grep empty). Folder defaults therefore need a new store.
- **[V]** Project Settings per-family defaults do exist for textures, and apply only to a texture's unset fields (`TextureImportSettings.h:4-7`).
- **[I]** The texture approach writes every inferred setting into its meta (`TextureCookQueue.cpp:832-835`). For 500 models that is 500 changed files in version control for no user intent. **Models should not do that** (section 6.7).
- **[I]** `SetAssetMetaCmd` loads and atomically writes YAML per asset on the main thread (`SetAssetMetaCmd.cpp:33-46`, `:169`). 500 Applies could hitch for a noticeable time. Measure, then batch if needed. The same applies to one undo of the group.

**Undo interplay (S24)**

- **[V]** Asset and import edits always go to the project history `P` (`docs/specs/S24-undo.md:287-292`), and S14 uses `SetAssetMetaCmd` (`:654`).
- **[V]** `InteractiveEdit` commits N commands as one entry (`editor/command/CommandHistory.h:68-72`, `:336`).
- **[V]** Undo writes the old value back, removes the key, and removes the sidecar if it did not exist (`SetAssetMetaCmd.h:18-22`). Listeners fire after every write, undo included (`:45-48`).

---

## 5. Long-running work and failures

**Job UI today**

- **[V]** Status bar has dedicated counters: textures with Cancel (`StatusBar.cpp:56-71`), shaders without Cancel (`:72-87`), "Reloading N..." (`:104`). A click opens Preferences > Asset Pipeline (`StatusBar.h:13`, `App.cpp:184`).
- **[V]** Jobs & Memory (Window > Analysis): workers, lanes, gate, channels, I/O reads with Cancel, memory table (`JobsMemoryWindow.cpp:85-171`). **No named job list.** `mesh-lod-simplify` (`MeshLodPool.cpp:130`) is invisible to a person.
- **[V]** `hasPendingMeshLods` has one editor caller, the benchmark (`App.cpp:4408`). A person cannot tell LODs are still building.
- **[V]** Per-window bakes use `ProgressBar` with Cancel (VAT baker `VertexAnimationBakerWindow.cpp:2187-2194`, Texture Synthesis, Navigation), and the export a modal.
- **[V]** Agents have `get_job` (`CLAUDE.md`, Debugging instruments) and the CLI has `cook-textures` (S14 R29). Neither is human UI.

**How reimport is triggered**

- **[V]** Settings write: listener, `settingsChanged`, cancel and requeue (`TextureCookQueue.h:117-124`).
- **[V]** Source change: S05 watcher, `sourceChanged`. For models, `reloadModels` (`HotReloadService.cpp:407`), only when a GameObject uses the file (`Models.cpp:36`).
- **[V]** Manual: Reimport, Reimport All Textures, Cook Textures Now, Clear Texture Cache (`ProjectSettingsWindow.cpp:2092-2096`).

**The "unsupported required extension" case**

- **[V]** `engine/tests/core/subsystem/GltfMaterialExtensionsTest.cpp:255-259`: a required unsupported extension logs one error, and the model still loads. Comment at `:7`.
- **[V]** Message: `glTF %s requires %s, which the engine does not read; it is loaded without it` (`engine/core/subsystem/MeshSystemGltfPbr.cpp:138-141`). The supported list begins with material extensions (`:31-34`). The test's fixture is `engine/tests/core/fixtures/gltf/unsupported_required.gltf`.
- **[V] What a person gets:** one Console line per file (`reportOnce`). Nothing in the Inspector (no model view exists), no Project badge (images only, `ProjectWindow.cpp:4625`), no `check` finding (its scope has no models, `editor/cli/CommandLine.cpp:287`), no export warning or failure (`copyAssets` has no model check).
- **[I] Not actionable, and possibly misleading.** "Loaded without it" reads as harmless. For `EXT_meshopt_compression` or `KHR_draco_mesh_compression` the geometry itself is what was compressed, so the mesh may be empty or wrong, depending on fallback buffers. **Not tested here; the existing test asserts the log count only, on a fixture with uncompressed geometry (section 6.9).** The message also offers no fix.
- **[V] A better message already exists in-house:** the lattice bake refuses with "is compressed with X, which the bake cannot decode and rewrite: export the model uncompressed" (`editor/lattice/LatticeBakeGltf.cpp:285-290`). The texture importer's "failed" row, badge and export failure naming the file (S14 R9) are the right pattern.

Other model failures: FBX import failure is a Console error, and a stale import keeps loading (`Project.cpp:1044-1047`). Console double-click opens detected source locations (`ConsoleWindow.cpp:293`, `:761-762`). **[I]** Whether a `.glb` path in a message becomes a useful link was not determined.

---

## 6. Design proposal

All of section 6 is **[P]** unless labelled, with the existing piece named where one is reused.

### 6.1 Principles

1. **Doing nothing gets good results, and nothing surprising.** New projects cook meshes losslessly with sensible per-family defaults. Sources are never rewritten.
2. **Five controls before any Advanced foldout.** Everything else is Automatic.
3. **Say what it does in the user's terms:** triangles, memory, download, how far it may differ. No meshlets, ACMR, simplifier targets, or lambdas in labels.
4. **Every setting answers "what will this cost or save?"** with measured numbers, not guesses.
5. **Import settings never modify the source file.** That is what makes Apply safe to try and undo instant.

### 6.2 Model Import inspector

Selecting one model in the Project window:

```
 [cube] Robot.glb                                                  (reused: drawAssetHeader)
 Model Import Settings
 --------------------------------------------------------------------------------------
 +--------------------------------------------------------------+
 |                                                              |
 |              (orbitable preview, drag to rotate)             |   ImGui::Image over a hidden
 |                                                              |   scene render target
 +--------------------------------------------------------------+
 [ Shaded v ]  [ ] Wireframe   Show: ( Source | Cooked )   LOD [==|=====] Auto
 Mobile cook: 41,204 tris, 22,118 verts, 1.3 MB memory, 0.6 MB download (source 4.1 MB)     info line
 --------------------------------------------------------------------------------------
 [ Model ] [ Animation ]                                           tabs (Animation = today's clip table)
 --------------------------------------------------------------------------------------
 [ Default ] [ Desktop ] [ Mobile ] [ Web ]                         same tab bar as textures

 v Meshes
   Optimize Mesh       [ Everything v ]         Everything | Vertex Order | Polygon Order | Nothing
   Mesh Compression    [ Lossless   v ]         Off | Lossless  (v1.1: Low | Medium | High)
   Read/Write          [ Automatic  v ]         Automatic | On        "Kept: this model has a Mesh Collider"
   Index Format        [ Auto       v ]         Auto | 16 bit | 32 bit
   Normals             [ Import     v ]         Import (v1.1: Calculate)

 v Level of Detail
   Generate LODs       [ Automatic  v ]         Automatic | On | Off
   LOD Count           [ 3  v ]                 extra levels: 1 | 2 | 3
   Reduction           [ Balanced v ]           Preserve Detail | Balanced | Smallest

     LOD 0 ===================|  LOD 1 =========|  LOD 2 ====|  LOD 3 ==|  Culled: never
          100% screen height       31%             14%          6%
     (read-only: from the scene's Threshold and each mesh's LOD Bias)

     Level   Triangles    % of source   Differs by at most        Switches below
     LOD 0     82,410        100%       exact                     -
     LOD 1     41,204         50%       0.4% of model size        31% of screen
     LOD 2     20,601         25%       1.1% of model size        14% of screen
     LOD 3     10,300         12%       2.8% of model size         6% of screen

 > Advanced (collapsed)
   Ship Source File    [ ]        Exports keep the original .glb next to the cook (code that reads the file itself).

 Before and after (Mobile)
                  Source      Cooked      Saved
   Triangles      82,410      82,410      -         (LOD0 is never reduced)
   Vertices       41,020      22,118      46%       (exact duplicates removed)
   Mesh Memory    3.2 MB      1.3 MB      59%
   Download       4.1 MB      0.6 MB      85%
   Visual Change  None                               "Same image; only stored differently"

 Platform     Memory    Download   LODs   Status
 Desktop      1.9 MB    0.8 MB     3      cooked                               (drawPlatformTable pattern)
 Mobile       1.3 MB    0.6 MB     3      cooked
 Web          1.3 MB    0.5 MB     2      cooking...

 ! warning rows, each with an action (section 6.9)
 --------------------------------------------------------------------------------------
                                                                  [ Revert ]  [ Apply ]
```

**Notes on the mock**

- **Index Format, Normals and Read/Write in v1:** Index Format is a safe real choice. Normals shows the current behaviour plus the Calculate slot later. Read/Write is **Automatic** with the reason shown, like the texture importer's Read/Write tooltip listing what needs CPU pixels (`TextureImporterView.cpp:377-379`).
  **[I]** A real "off" needs an audit of every CPU reader of mesh data (picking, colliders, navigation, LOD input). I did not find a CPU-release path for meshes in this pass, so v1 can display the state without acting on it. If the runtime audit finds it unsafe, hide the row.
- **LOD bar:** draw with `ImDrawList` rectangles (`AddRectFilled`) inside an `InvisibleButton`. It is **read-only in v1**, because selection is error-based (`RenderSystem.cpp:8226-8232`). Draggable Unity handles would mean a new stored screen-height per level and a second selection rule.
  The percentages are derived from Threshold, LOD Bias, `lodError` and the mesh's bounds, using one reference camera field of view stated in the tooltip. Label them "about".
- **Colours:** LOD bar colours equal Draw Mode's LOD Level colours, so learning one teaches the other.
- **Numbers:** measured from the cook's record (the `TextureCookRecord` analogue). Before a cook lands, show "cooking..." or "not cooked yet" rather than estimates.
  **Differs by at most** uses `MeshLodLevel::error` (`MeshLodPool.h:22`, "deviation from the source, in mesh units"), shown relative to model size. Tooltip: "about 1 px at 6 m" for the current camera, if cheap.
- **Preview before Apply:** the cache is content-addressed, so a debounced cook of the *working* settings for the selected model and shown family is cheap, and Apply then hits the cache. `makeTextureCookRequest` already separates inputs from the queue (`TextureCookQueue.h:83`). **[I]** Check the cost with large models before committing. Fall back to post-Apply cooks like textures if needed. This is what stops the design being "destructive without preview".
- **Hierarchy stays intact.** No v1 setting may change nodes, submesh count or material slots, because that triggers the "hierarchy changed" path and scene modification (`Models.cpp:43-46`, `:55`), and could disturb submesh overrides (`GltfMaterialExtensionsTest.cpp:289-300`). A cook keeps submesh identity. S04 sub-asset keys are unaffected.

**Multi-select (say 37 models)**

```
 [cube] 37 models
 Model Import Settings
 Optimize Mesh       [ Everything v ]
 Mesh Compression    [ -          v ]     mixed: the em dash, same as textures
 Generate LODs       [ Automatic  v ]

 Totals (Mobile):  1.2M tris  |  38 MB memory -> 21 MB  |  52 MB download -> 17 MB  |  35/37 cooked, 1 cooking, 1 failed
 Largest: Robot.glb, Tank.glb, Tower.glb                                      (click selects that one)
                                                                  [ Revert ]  [ Apply ]
```

No preview for N > 1, and **aggregates instead of first-file rows** (a fix over the texture view). Model plus texture selected: "Narrow the selection: 3 models, 2 textures" with buttons **Select Models** and **Select Textures**.

**Unapplied edits prompt** (`hasPendingEdits` gets a caller, shared with textures, `ImGui::BeginPopupModal` as at `InspectorWindow.cpp:7795`):
`Unapplied import settings for 'Robot.glb'   [Apply] [Revert] [Cancel]`

### 6.3 Scene view Draw Mode

Placement: a labelled combo at the left of the Scene view toolbar (`SceneViewToolbar.cpp`), per active pane, the same list in the pane header menu (`:450-470`).

```
 [ Shaded v ]
 --------------
   Shaded                       v1
   Wireframe                    v1
   Shaded Wireframe             v1
 Mesh Analysis
   LOD Level                    v1     tint per level, with a legend in the corner
   Mesh Density                 v1.1   tint per object: triangles per on-screen pixel
   Overdraw                     v1.1   additive heat, with a legend
 --------------
   Shadow Debug >               existing views moved or linked here (Cascades, Shadow Factor...)
```

Corner legend for LOD Level (drawn with `ImDrawList` like `drawStats`):

```
 LOD 0  LOD 1  LOD 2  LOD 3
 [red]  [yel]  [grn]  [blu]    Force: ( Auto | 0 | 1 | 2 | 3 )
```

- **Editor-only state, never saved** (`Scene.h:45-46` precedent), per pane through `PaneViewFlags` (`SceneViewController.cpp:1279-1283`). MCP: extend the view tool as `shadows_debug_view` does.
- **Zero cost when off:** wireframe line buffers are built lazily per submesh and level, keyed by `lodKey`, and released on leaving the mode.
- **Not Game view, not Play, not export.** No `meshlet` entry. The do-not-copy list excludes GPU-driven culling (`docs/specs/README.md:23`), so there is nothing for it to show. Do not reserve a greyed-out item.
- **Colour-blind safe** LOD ramp, and a text label in the legend.
- **Force LOD** needs one editor hook in `selectMeshLod` (`RenderSystem.cpp:8222`). Picking keeps LOD0 (`S26:144`).
- **[I] Why Overdraw is v1.1:** it needs a blend and depth override across all material and skin permutations, unlike a tint. Its value is mostly foliage and particles, not import settings. Mesh Density needs projected-area calculation per object.

### 6.4 Stats overlay

Extend `GameWindow::drawStats` (raise the 11-line buffer, `GameWindow.cpp:1226`) and add the same toggle on the Scene toolbar.

```
 Statistics
 Graphics: 60.0 FPS (16.6 ms)
 CPU 4.21 ms  GPU 7.80 ms
 Draw calls: 184   Tris: 1.21M   Verts: 702K              <- Verts new
 LOD: 0: 41  1: 62  2: 58  3: 23                          <- new; hidden when the scene's LOD is off
 Saved by LOD: 2.9M tris (71%)                            <- new
 Mesh memory: 86 MB                                       <- new: gpumem Category::Meshes
 Screen: 1920x1080
```

Add `lodDraws[MAX_MESH_LODS]` and tris-saved to `Engine::FrameStats`. **[I]** Counted only while Stats or `get_render_stats` asks, so the disabled cost stays zero or a trivial increment. **[I]** `Tris` appears to count all passes (`ObjectRender.cpp:128`); say so in the tooltip if shadow passes are included. Clicking a line opens the Audit window.

Profiler: add `Meshes  N resident, X MB` beside the Textures line (`ProfilerWindow.cpp:555-560`).

### 6.5 Mesh Optimization Audit window

Window > Analysis > **Mesh Optimization Audit**, next to Jobs & Memory (`App.cpp:704-709`). It opens non-modal, and scans on a Background job with a progress bar. It reads glTF JSON counts and the cook cache; **no scene changes**.

```
 Mesh Optimization Audit                                          Platform [ Mobile v ]   [ Rescan ]
 Project total: 2.1M tris | 214 MB memory -> 96 MB possible | 310 MB download -> 88 MB possible
 Filter [ t:Model l:Props       ]   Show [ All | Problems | Not in any scene ]

 Model             Used In    Tris    Memory   Download   LODs   Issue                       Fix
 ---------------------------------------------------------------------------------------------------------
 ! Tank.glb        3 scenes   840K    61 MB    88 MB      Off    Requires Draco: not read    [Re-export...]
 Tree_01.glb       48 inst    320K    22 MB    19 MB      Off    Dense, far from camera      [Generate LODs]
 Rock_Big.glb      12 inst    210K    14 MB    31 MB      3      Uncooked                    [Cook]
 HeroHQ.gltf       2 scenes   150K    11 MB    26 MB      3      Uses 32-bit indices         [Auto Index]
 OldCar.fbx        not used   98K     6 MB     9 MB       -      Ships but nothing uses it   [Find References]
 ---------------------------------------------------------------------------------------------------------
 [x] 37 selected   Fix preview:  LODs on: 22 models, Cook: 15 models  ->  saves 118 MB memory, 222 MB download
                                                     [ Preview in Inspector ]   [ Apply Fixes ]    (one undo step)
```

- **ImGui:** `BeginTable` with `Sortable | RowBg | ScrollY | Resizable`, `ImGuiListClipper`, row click selects in the Project window and pings, the existing `AssetQuery` filter, and the S04 graph for **Used In** (`ref:` and Find References, `ProjectWindow.cpp:2186-2190`).
- **"Fix" never acts on one click without showing the numbers.** It writes settings through the Apply path. Apply Fixes is one `Set Model Import Settings (37 models)` step, so it is undoable like any edit.
- **Scene-level fix:** "LOD is off in Main.scene" is a separate row, with **Enable** as a normal scene property edit, because it can visibly pop (section 6.8).
- **Sort default:** memory x instance count, so one tree used 48 times outranks a hero prop.
- **Reference camera note:** "Dense, far from camera" needs live scene data, so **[I]** v1 should limit it to size and instance heuristics with words like "Dense for its size", until Mesh Density exists.

### 6.6 Project settings and export

**Project Settings > Mesh** (new page in the registry, keys `mesh.*`, searchable, each row with a reset):

```
 Mesh Cooking                 [x]        Cook models at export. Off: they ship as they are.
                             Desktop     Mobile      Web
 Optimize Mesh               Everything  Everything  Everything
 Mesh Compression            Lossless    Lossless    Lossless
 Generate LODs               Automatic   Automatic   Automatic
 LOD Count                   3           3           2
 LOD Bias (Quality page)     1.0         1.0         0.7
 Cache Size                  shared with the cook cache
 [ Clear Mesh Cache ]        [ Open Mesh Optimization Audit ]
```

Use a command on `P` like `DebugSettingsCmd` (`ProjectSettingsWindow.cpp:4538-4543`), not the direct set of Texture (`:4342`). **LOD Bias per platform** is a Quality page row, like Atmosphere Quality (`:1972-1985`), because Scene-level Threshold stays what it is. Avoid named Quality Levels in v1.

**Export window > Meshes foldout**, a copy of `drawTextureSection` (`ExportWindow.cpp:1111-1176`):

```
 v Meshes
   Family    Cooked      Download   Memory
   Mobile    412 / 500   88 MB      96 MB
   The totals cover the cooked models; the export cooks the rest.
   [ Cook Models Now ]   [ Open Audit ]
```

**Build Report** on the completion page and a window, in the same place as modules (`ExportWindow.cpp:1620-1634`):

```
 Build Report (Web): 142 MB
   Textures   61 MB  ===========
   Meshes     38 MB  =======
   Engine     21 MB  ====
   Audio      14 MB  ===
   Scripts     5 MB  =
   Other       3 MB
 Largest files                                      Size    Source
   models/Tank.glb (cook)                           31 MB   88 MB     [Show in Project]
   ...
 [ Save JSON ]  [ Compare with previous build ]
```

Measure before packing (`ExporterPrebuilt.cpp:736`), add to `ExportResult`, and write `build-report.json` next to the build. Also `build --json`. "Compare" is post-v1.

**Game view:** rename "Simulate platform textures" to cover meshes when cooks exist: one family, as S14 Deviation 14 has it (`GameWindow.cpp:729-769`).

### 6.7 Data and compatibility

```yaml
# Robot.glb.meta
import:
  modelImporter:
    version: 1
    generateLods: on
    lodCount: 2
    platforms:
      mobile: {override: true, generateLods: off}
```

```yaml
# project.yaml
mesh:
  cooking: true
  defaults: {desktop: {...}, mobile: {...}, web: {...}}
```

- **Sparse:** only fields the user set are stored. Absent means Project Settings default. **No auto-written meta** (unlike `TextureCookQueue.cpp:832-835`), so opening an old project touches zero files, 500 models produce no diff, and a later project-default change applies to every model that was not overridden.
- **Load:** an absent section, absent `mesh:` and a missing meta all resolve to defaults; `version` plus preserved unknown keys, as S14 does. Scene format is untouched. New scene or `lodBias` defaults need no bump. Metas never ship (`Exporter.cpp:2804`), so the cooked index must carry anything runtime needs.
- **Old projects (decided here):** `mesh:` is absent, so `cooking` resolves to **off**, with the build's Meshes foldout offering one click to enable. New projects from templates get `cooking: true`. Reason: cooking changes the files a build contains. S14 did flip old projects to cooked textures on export (R9, R30); a lossless model cook would be defensible the same way, so this default is easy to reverse if the owner prefers parity.
- **New 3D templates:** `meshLodEnabled: true` (`templates/3d/Main.scene:8`). Existing scenes keep their saved `false`.

### 6.8 What may be automatic, and what must be opt-in

**Automatic (no prompt, once cooking is on):** the shipped image should be the same, and sources are untouched. Each needs a golden or byte test proving no visual difference.

| Step | Why it can be automatic | Guard |
|---|---|---|
| Exact-duplicate vertex removal | No vertex moves. Compare every attribute: skin, morph deltas, `_VAT_INDEX` | VAT survives because it uses an attribute, not vertex index (`vertex-animation-design.md:135-138`); test it |
| Vertex Cache, Overdraw and Vertex Fetch reorder | Depth-tested opaque output unchanged **[I]** | **Skip BLEND submeshes:** triangle order is visible in alpha blending (`MeshSystemGltfPbr.cpp:350`). Scene-level blend overrides exist (`SubmeshOverride_BlendMode`, `MeshComponent.h:61`), so **check references or treat as a stated risk** |
| Lossless payload compression | Only size and decode time change | **Measure load time on Web and mobile**; per-family default can stay off |
| Index width shrink (Auto) | Same indices, narrower | LOD pack type must match (`MeshLodPool.h:28`) |
| Cooking LODs that scenes would already generate at runtime | Same levels, built ahead of time | Same recipe and version in the key. Otherwise the images differ, and this moves to opt-in |
| Content cache, dedupe, stats, audit scan, thumbnails | No effect on the game | Size cap and clear button |

**Must be opt-in, with preview and numbers:**

| Change | Why | How |
|---|---|---|
| Enabling LOD in a scene that had it off, or changing Threshold | Pops are visible; no crossfade | Scene property edit (undoable), Audit row, default only for new templates |
| LOD Count, Reduction, or LODs differing from today's recipe | Changes the image at distance | Importer with Force LOD preview |
| Mesh Compression Low, Medium, High (quantization, v1.1) | Vertices move; seams between meshes may crack; CPU readers expect FLOAT positions (`RenderSystem.cpp:7298`) | Importer with shown error |
| Normals: Calculate; welding by tolerance; removing attributes | Shading changes; custom shaders may read a stripped attribute | Importer, Advanced |
| Merging meshes or nodes; reducing LOD 0 | Changes hierarchy, overrides, identity | **Out of v1.** If ever, a separate named action |
| Read/Write off | Scripts or colliders can break | Explicit, with a list of what uses it |
| Per-platform overrides | Build differs from what the editor shows | Tab with Override; Game view Simulate |
| Turning on cooking for an **existing** project | Changes the file layout of builds; code reading `.glb` itself would break | One-click prompt; **Ship Source File** per model |
| Rewriting any source file | Destructive | **Never.** Not a feature |

**Silent-downgrade rule:** S14 fails export on a cook error with "no silent RGBA8 substitution" (R9). Model cooks follow suit: **fail naming the file**, or an explicit fallback with a `BuildWarning`. Never ship a partial result silently.

### 6.9 Failures made actionable

States reuse `TextureCookStatus` semantics. Severity shows in three places: **Project badge** (`drawTextureCookBadge` generalised), **Inspector row**, **Audit row**. Also `check` findings (`ProjectChecks`) and the Console.

| Case | Inspector message | Actions |
|---|---|---|
| Requires `EXT_meshopt_compression` or `KHR_meshopt_compression` | If decoding lands: "Compressed with meshopt; decoded at import." (info). Otherwise as Draco below | - |
| Requires `KHR_draco_mesh_compression` | "This file's geometry is Draco-compressed, which JasperEngine does not read. It may look empty or wrong." | **[Show in File Manager]**, **[How to re-export]** (link to doc) |
| Other required extension | "Requires X, which JasperEngine does not read. It is ignored, so the model may look different." | **[Copy Name]** |
| Used but optional | Quiet line in Advanced | - |
| Cook failed | Reason, as textures | **[Retry]**, **[Copy Error]** |
| Hierarchy changed on reimport | Today's note (`Models.cpp:44`) moved into a row | **[Rebuild GameObjects]** |
| Model has a blended submesh | Info: "Triangle order kept on 2 blended materials." | - |
| Nothing to gain | "Already optimized; cook saves under 3%." | - |

**Export:** a required, unreadable, geometry-carrying extension fails the build by default, naming the file. Tighten the runtime message too: "requires X, not supported: the model may be empty or wrong" with a pointer to the fix, and keep the test's error count (`GltfMaterialExtensionsTest.cpp:258`).

**Red control for that test [P]:** add a fixture with real meshopt-compressed geometry. The existing `unsupported_required.gltf` does not cover this: **[V]** it declares the extension as required, but no bufferView carries it, so its geometry is plain (`engine/tests/core/fixtures/gltf/unsupported_required.gltf`, parsed with Python).

### 6.10 Undo semantics for import settings (precise)

**Scope of history**

| Action | On the timeline? | Effect of undo |
|---|---|---|
| Field edits before Apply | **No.** View state only. Ctrl+Z does not unpick them | Use **Revert** |
| Apply (N assets) | **Yes:** one entry on the project history `P`, `Set Model Import Settings (Robot.glb)` or `(37 models)` | Restores each asset's whole `import.modelImporter` section |
| Audit Apply Fixes | **Yes:** one entry | Same |
| Project Settings > Mesh on OK | **Yes:** a Cmd like `DebugSettingsCmd`, one entry | Restores defaults; models without own values recook, or hit the cache |
| Enable LOD on a scene | **Yes:** on that scene's history | Normal scene property |
| Reimport, Cook Now, Cancel | **No.** No settings changed | - |
| Clear Cache, cache eviction | **No.** Derived data only | Cooks again when needed |
| Export output | **No** | - |

**Exact rules**

1. **What is stored.** `SetAssetMetaCmd(project, asset, "import.modelImporter", sparseSection)`. Old value: the section as it was, or null when absent. Undo of a first Apply **removes the key**, and a sidecar it created, returning the meta to the bytes around it (`SetAssetMetaCmd.h:18-22`; S04 lines are kept, `SetAssetMetaCmd.cpp:146-163`).
2. **What undo triggers.** The write listener fires after the write, undo included (`SetAssetMetaCmd.h:45-48`). The queue cancels running cooks for that asset, recomputes the key, usually **gets a cache hit**, and swaps in place. **Undo is therefore immediate and loses no work.**
3. **The source is never written,** so undo cannot lose content. Scenes are not dirtied: `affectsStructure()` is false (`SetAssetMetaCmd.h:52`), and v1 settings cannot change hierarchy (section 6.2).
4. **While a cook runs:** Apply, Undo or Redo cancels and requeues. The stale state shows until the new artifact lands. The old mesh stays on screen; it never blinks empty.
5. **Pending edits and undo together.** If something else changes stored settings while the view has pending edits (Ctrl+Z, agent, version control), the view keeps its pending edits against the old base, as today (`TextureImporterView.cpp:211`). **Add a one-line notice**: "Settings changed elsewhere. Applying overwrites only the fields you edited." `touched`-only merging (`:285-288`) makes that accurate.
6. **Selection change with pending edits:** the prompt (section 6.2). Today it silently drops them.
7. **Multi-asset partial failure.** If one meta cannot be written, Apply reports it by name and **the whole step cancels** (`InteractiveEdit::cancel`, `CommandHistory.h:68-70`), rather than today's `continue` (`TextureImporterView.cpp:293-294`).
8. **Locks.** A locked asset is read-only (S29 R28, `InspectorWindow.cpp:12367-12368`). In a multi-select containing locked files, name them and block Apply.
9. **Play mode.** Apply is disabled outside STOPPED, as Rig does (`InspectorModelRig.cpp:50`), with a tooltip. Otherwise a mid-Play reload could disrupt the session. **[I]** The texture importer does not gate this, so it is a deliberate difference.
10. **Merging.** `setNoMerge` per command, one group per Apply, as today (`:301`).
11. **Agents.** MCP `set_model_import_settings` follows the same command path, so an agent's change shows in Undo History and a person can undo it.
12. **History trimming.** A group trimmed off the depth cap leaves files as they are. Only the ability to undo goes away.
13. **Version control.** External meta change (pull) is not an undo step; it just invalidates. Whether external `.meta` edits reach the queue today was **not verified** (`settingsChanged` has one caller, the listener at `TextureCookQueue.cpp:204`). The model design needs it handled, through the watcher or `AssetDatabase.cpp:1053`.

### 6.11 Reused pieces

| Need | Reuse | New |
|---|---|---|
| Importer fields, mixed values, Apply, Revert | `comboField`, `boolField`, `importerGet/Set/Leaves`, `touched`, `mixed`, `InteractiveEdit`, `SetAssetMetaCmd` | Extract into `ImporterFields` / a shared base |
| Sections | `ImGui::CollapsingHeader` + 2-column `BeginTable` (`:773-782`), `BeginTabBar` | - |
| Combos | `BeginCombo` + `Selectable` + `SetTooltip` | - |
| Preview | `ImGui::Image` + `Backend::getImGuiTexture`, hidden-scene render as model thumbnails, `MeshPreviewRender` | Drag orbit, wireframe; `SliderInt` for Force LOD |
| LOD bar | `ImDrawList::AddRectFilled` + `InvisibleButton` | The widget, small |
| Status | `StatusBar` segment (`:56-71`), badge (`:4624-4646`) | Generalise over asset type |
| Platform table | `drawPlatformTable` shape (`:471-511`) | Numbers for meshes |
| Export | `drawTextureSection`, `Theme::foldoutHeader`, `ProgressBar` | Meshes foldout, Build Report |
| Project Settings | `settingsui::beginRow`, `drawComboSetting`, `projectBeginSettingsRow`, registry rows, `DebugSettingsCmd` shape | Mesh page |
| Audit | `BeginTable` sorting, `ImGuiListClipper`, `AssetQuery`, S04 graph, `Window > Analysis` catalog item | Window |
| Draw Mode | `PaneViewFlags`, `drawGizmosMenu` Rendering section, `ShadowDebugView` pattern | Line buffers, LOD tint uniform |
| Cook | `TextureCookCache` layout, jobs Background lane, `.jasperengine/cache` | `ModelCookQueue`; **listener becomes multi-subscriber** |
| Errors | Console, `BuildWarning`, `ProjectChecks::Finding` | Model check |
| Tests | `test_texture_importer_ui.py` pattern with real input | `test_model_importer_ui.py` |

### 6.12 Smallest coherent v1

**Ship together, since each makes the others meaningful:**

1. **Shared importer base and listener fix,** plus the unapplied-edits prompt for both importers.
2. **Model cook** (lossless, per-family, content cache) with status bar, badges, Reimport, export integration, Meshes foldout.
3. **Model Import inspector:** Optimize Mesh, Mesh Compression (Off, Lossless), Index Format, Generate LODs, LOD Count, Reduction; Read/Write and Normals as shown state; preview with Wireframe and Force LOD; before/after numbers; platform tabs; multi-select aggregates.
4. **Project Settings > Mesh,** with the sparse-section rule.
5. **Draw Mode:** Shaded, Wireframe, Shaded Wireframe, LOD Level.
6. **Stats:** Verts, LOD histogram, mesh memory.
7. **Mesh Optimization Audit:** table, filters, numbered fixes, undo.
8. **Actionable failures** for required extensions: Inspector, badge, `check`, build failure.
9. **Build Report (minimal):** categories and largest files.
10. **MCP and Lua parity** to the project's rule (section 6.14).

**After v1, in order:** quantization (Low, Medium, High); Overdraw and Mesh Density; Presets and **Copy Settings**; folder defaults; Build Report comparison; LOD crossfade; per-mesh draw distance.

### 6.13 What to avoid

- **Too many knobs.** No per-attribute bit counts, simplifier targets, attribute weights, lock-border flags, cache sizes or codec levels in the UI. If a power user needs one, add it after someone asks.
- **Jargon.** Banned from labels: meshlet, cluster, ACMR, ATVR, quantize (use Mesh Compression), simplify error, remap, stripify. The texture importer's "RDO / ETC1S Level" shows what to avoid.
- **Spec IDs in tooltips** ("R5").
- **A Meshlets draw mode or setting**, greyed out or otherwise. It would promise something the do-not-copy list rules out.
- **Hidden destructive paths.** No "Optimize Source File". No auto-applied Audit fixes. No fix button without numbers.
- **Silent loss:** dropped edits, dropped parse failures, silent fallbacks.
- **Editable-looking read-only controls.** The LOD bar must look like a chart, not handles.
- **A second overlapping LOD concept.** Do not add a LOD Group component while `MeshComponent` LOD and the scene Threshold exist. Unity's LOD Group handles different meshes per level, which is a separate future feature, and it should keep that name.
- **Meta churn** from writing defaults.
- **Fake precision.** "About" for derived percentages, and measured values only for numbers presented as facts.
- **A modal progress dialog for cooks.** Background only, with the editor usable. Modal stays for export.

### 6.14 Agent parity (brief, per project values)

MCP group for model cooks: `get_model_import_settings`, `set_model_import_settings`, `reimport_models`, `get_model_cook_report`, modelled on the texture tools (`editor/mcp/tools/AssetTools.cpp:4707-4739`), plus `audit_meshes`, and a `draw_mode` argument on the view tool beside `shadows_debug_view`. CLI `cook-models` and a `check --models` flag. Lua: `Scene.meshLodEnabled` exists (`CoreClassesLua.cpp:3198`); add per-platform LOD Bias. A guide page in `docs/mcp/guide/`.

---

## 7. Uncertainties to resolve

1. **Actual behaviour of a truly compressed glTF** at load: not run. The existing test uses a fixture with plain geometry and asserts log counts only.
2. **Main-thread cost** of Apply and undo for 500 metas: not measured.
3. **Cross-folder selection and Ctrl+A** in All search: inferred from code; needs a UI test.
4. **Cook time** of a preview-before-Apply on big models: not measured.
5. **Triangle-order safety with scene-level blend overrides:** a stated risk until references are checked.
6. **Read/Write:** whether the engine can release CPU mesh data safely needs its own audit. I did not find a release path in this pass, and that search was narrow.
7. **External `.meta` changes** reaching a cook queue: not verified.
8. **Web and mobile decode cost** of lossless compression versus download saved: needs measuring before it is the default for each family.
9. **Old-project default for cooking** (off, with a one-click prompt) is a product call for the owner; section 6.7 gives the reasoning.
10. **Unity naming:** from general knowledge. Worth a check against current Unity documentation before labels are frozen.
