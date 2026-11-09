# S81 — Title Art runtime: the Title Art Renderer, Baked / Realtime / Glyph Atlas, live layers, Lua `TitleArt`, the `titleart` module and export

**Wave / Depends on.** Title Art program (spec-first), **wave 6B** (with S82, S83, S86; program
graph in §8, program rules in S80 §6.15). Depends on:
- **S80** (wave 6): the `.titleart` document (`engine/core/titleart/TitleArtIO.*`), Exposed
  parameters (§6.1), Outputs, the bake and its sidecar `<file>.bake.json` (S80 §6.9), the shared
  renderer, its render-job members, field overrides, kept splits and glyph cells (S80 R18, R18a),
  its float/RGBA8 pixel paths, the `title_art` MCP group and its tools;
- **S83** (same wave; S83 owns the values) classifies every effect and generator for Glyph Atlas
  (`glyph_safety`, a registry member S80 declares, R31) and supplies the live kernels R9 runs;
- **S85** (wave 6C) supplies the animations `TitleArt:play` plays (R44, §6.6); S81 owns the API and
  the `title_fx` ANIM feature they play through.

Owner area:
- `engine/core/component/TitleArtRendererComponent.h`;
- `engine/core/subsystem/TitleArtSystem.*` (core: always built);
- `engine/core/titleart/runtime/` (inside the `titleart` module: scheduler, Realtime, Glyph Atlas,
  fonts at runtime) and `engine/core/subsystem/TitleArtRuntimeNone.cpp` (the module-off stub, core);
- the player-module wiring of `JASPERENGINE_TITLE_ART` and its sub-options
  `JASPERENGINE_TITLE_ART_3D` and `JASPERENGINE_TITLE_ART_MESHER` (S80 §6.15: S80 owns the option and
  the sources file);
- `engine/core/object/TitleArt.*` and `engine/core/script/binding/TitleArtClassesLua.cpp`;
- `engine/shaderlib/library/title_fx.*`, `title_fx_sprite.*` and `engine/shaderlib/includes/title_fx.glsl`;
- `editor/titleart/runtime/` (export, font subsetting, Characters, checks);
- `editor/window/InspectorTitleArtRenderer.cpp` (S80's `InspectorWindowTitleArt.cpp` is the asset summary);
- the `### Runtime` section of `docs/mcp/guide/title-art.md`.

## 1. Goal

A title made in the Title Designer goes into a running game as one component, the **Title Art
Renderer**, and looks exactly like its bake. The component works on a UI Image or a world Sprite.
Its **Render Mode** sets the price of changing it:

- **Baked** shows the bake. It costs what an Image costs and adds no engine code.
- **Realtime** re-renders the title with the editor's own renderer whenever its text or a
  parameter changes. The render is spread over frames within a budget, and the old image stays up
  until the new one is ready.
- **Glyph Atlas** renders each glyph once, with the full look, into an atlas, and lays out
  changing text every frame (counters, scores, timers).

All three modes take **live layers** on top: a sweep shine that a clip can key, a glow pulse, a
tint and glint twinkles. Scripts drive titles through the Lua `TitleArt` API. Exported games
include the renderer only when a shipped scene needs it, and they ship only the glyphs their
titles can show.

## 2. Scope

**In scope**
- The Title Art Renderer component:
  - its fields, Inspector, undo and prefab overrides;
  - how it draws through the existing Image and Sprite paths;
  - its create menus and drag and drop.
- **Baked mode**: the bake's PNG, its sidecar (anchors, frame, pivot) and live layers.
- **Realtime mode**:
  - the scheduler: render on change, time slices with a per-frame budget, priority, coalescing,
    visibility;
  - double buffering, the readiness event, failure behaviour;
  - the float and RGBA8 paths and the forced fallback;
  - the prebaked initial state.
- **Glyph Atlas mode**:
  - glyph cells (back and front), runtime layout through `TextShaper`, the compose pass;
  - plates for the layers that are not text;
  - glyph safety, atlas growth and eviction, the prebaked atlas.
- **Live layers**: `library:title_fx` (UI) and its mesh twin `library:title_fx_sprite`, with the
  component's Live Time. **Highlight anchors**: one CPU function, shared with the bake.
- **Particles around titles**: four presets built on S20.
- **Animation**:
  - property rows in `engine/core/animation/PropertyBindings.cpp`;
  - the reserved clip events that change text, fonts, images and color schemes;
  - the hand-off to S85.
- The **Lua `TitleArt` API** and its C++ class, with errors that say how to fix them.
- The **`titleart` module** (`JASPERENGINE_TITLE_ART`):
  - detection (`ModuleUsage`) and the prebuilt variants;
  - shader variant collection and warm-up (S32).
- **Export**:
  - `.titleart`, font and image shipping rules;
  - font subsetting with HarfBuzz's subsetter (vendored here);
  - the Characters setting and its checks;
  - font embedding (OS/2 `fsType`) and OFL Reserved Font Names;
  - the font credits file.
- Performance budgets on the S13 device bar, measured with the S15 tools.
- **MCP**: parameters on `get_title` and `render_title`; screenshot verification in Play.
- Tests: unit, GPU goldens, MCP, the web gate, the Android render gate, player variants and
  forced fallback.

**Out of scope**
- The document, the renderer's passes, effects and the bake itself (S80, S83). The Title Designer
  (S82). Looks, Color Schemes and Templates (S84).
- Title-internal animation: S85 defines animations, flipbooks and how they play. S81 gives them
  the API and the frame budget.
- 3D titles (S86). Glyph Atlas refuses Dimension 3D text layers (R30). Realtime 3D uses this
  scheduler with S86's quality tiers.
- Per-language text selection (S87). S81 subsets the union of all languages (R53).
- HTML UI titles. `<img>` of a bake works today. A Title Art element in RmlUi is not planned.
- An HDR scene path. Titles composite in the existing 8-bit scene (`docs/mcp/guide/post-processing.md`
  "Limits"). Float precision lives inside the title renderer only.
- Do-not-copy list (`docs/specs/README.md`): no render graph is introduced. The scheduler runs a
  flat list of the renderer's steps (§6.3).

## 3. Current state (feature/title-art 3ffafdfaa, read)

- **No title runtime exists.** `engine/core/titleart/` and `editor/titleart/` are absent. S80
  (spec, written in parallel) defines what S81 builds on:
  - the top-level member `runtime`, reserved for S81;
  - Exposed parameters of types `text`, `color`, `number`, `bool`, `font`, `image` and
    `color_scheme`;
  - Outputs, including `content: "glow"` and the file names `logo.titleart.png` and
    `logo.titleart.glow.png`;
  - R8: `TitleArtExport::isNeeded`, which S81 fills;
  - R10: one `TitleArtRenderer`;
  - R12: the output pass writes straight-alpha sRGB;
  - R18: `TitleRenderJob::step(budget)`, resumable, byte-identical to a one-shot render;
  - R19: device loss through `SystemRender::getContextId()`;
  - R38: `TITLE_ART_TEST_NO_FLOAT_TARGETS`;
  - R40: `JASPERENGINE_TITLE_ART`, which compiles all of `engine/core/titleart` and links ThorVG
    and Clipper2.

  S83 makes highlight anchors an analysis result shipped with bakes and Realtime export data
  (S83 R13-R14), and gives effects live kinds (S83 D10).
- **The draw paths to reuse:**
  - An Image is a `UIComponent` + `ImageComponent` + `RectTransformComponent`.
    `UIComponent` carries `texture`, `customShader`, `shaderUniforms`, `shaderTextures`,
    `shaderFeatures` and `blendMode` (`engine/core/component/UIComponent.h:37-71`).
  - A Sprite is a `MeshComponent` + `SpriteComponent`, with `pivotPreset`, `width` and `height`
    (`SpriteComponent.h:13-46`).
  - Both take a `Framebuffer` as their texture: `Image::setTexture(Framebuffer*)`
    (`engine/core/object/ui/Image.h:39`) and `Mesh::setTexture(Framebuffer*)`
    (`engine/core/object/Mesh.h:32`).
- **Offscreen work before the cameras.** `RenderSystem` steps the fluid simulations before any
  camera samples them (`engine/core/subsystem/RenderSystem.cpp:15266-15270`). The fluid
  simulator's caps are `renderRGBA16F`/`filterRGBA16F` minus its test flags
  (`engine/core/fluid/FluidSimulator.cpp:98-115`, flags `FLUID_TEST_NO_FLOAT_TARGETS`… in
  `FluidSimulator.h:50-58`). Required targets that fail are retried every few seconds
  (`FluidSimulator.h:40-44`). `RenderFeatures` has `renderRGBA16F`, `blendRGBA16F` and
  `filterRGBA16F` (`engine/core/render/SystemRender.h:24-39`). `ShaderType` is append-only
  (`engine/core/render/Render.h:164`), and `FLUID` is the precedent of a module's own pass type
  (:172). The collector adds those keys only when a scene has a fluid
  (`editor/shader/ShaderVariantCollector.cpp:1139-1146`, :1253-1268).
- **`library:ui_fx`** (`engine/shaderlib/library/ui_fx.frag`) has a shine whose centre is
  `mod(time, interval) * speed`, where `time` is engine-written (:30, :89). A clip cannot
  place the band at an exact moment, and the shine cannot be scrubbed. One shader file declares one
  `@shader` kind (`engine/core/shader/ShaderManifest.cpp:171-181`). `sprite.frag` is `@shader mesh`
  and `ui_fx.frag` is `@shader ui`.
- **Clip property tracks**:
  - `engine/core/animation/PropertyBindings.h` is "the one binding mechanism". Its rows are
    built in `buildRows` (`PropertyBindings.cpp:498-650`).
  - Parametrized rows exist (`shaderUniforms.<member>`, `submeshes[N]...`, :494, :548).
  - `PropertyKind` has no string kind (`ClipTypes.h:45-56`).
  - Clip events reach `AnimationPlayerComponent::onEvent` subscribers and then the Lua method of
    the same name. An event with neither handler warns once
    (`engine/core/subsystem/ActionSystemPlayers.cpp:1321-1333`).
  - `onEvent` is a tagged multi-subscriber `FunctionSubscribe`
    (`engine/core/util/FunctionSubscribe.h:246`).
  - Event args are JSON scalars, at most 8 (`AnimationEvent.h:29-38`).
- **Keyed collections as properties.** `Catalog::splitShaderEntryProperty` makes each
  `shaderUniforms.<member>` entry its own property for `set_properties`, prefab overrides and
  tracks (`editor/Catalog.cpp:651-670`). Particle exposed values are one opaque vector property
  instead (`editor/reflect/components/ParticlesReflect.cpp:49`,
  `editor/window/InspectorParticles.cpp:833-910`), so a prefab overrides all of them or none.
- **Modules (S34)**:
  - `JASPERENGINE_NAVIGATION` is the precedent of an option, a definition and a sources file
    (`engine/CMakeLists.txt:39-42`, :1139-1141, :1902-1904, :2055;
    `engine/core/navigation/NavigationSources.cmake`).
  - `player::allModules()` lists six modules (`engine/core/player/PlayerModules.cpp:12-15`).
  - `engine/player/variants.json` has `full`, `ui`, `core`, `lite3d` and `lite2d`. The lite ones
    have no `runtimeshaders`.
  - `editor/players/ModuleUsage.cpp:263-286` detects module needs from components, Lua names and
    settings.
  - `editor/players/ShaderNeutralApi.cpp` lists the Lua classes reviewed as unable to ask for an
    uncooked shader variant. A new class is relevant until reviewed (`ShaderNeutralApi.h:6-21`).
- **Libraries**:
  - HarfBuzz 14.3.0 is vendored for shaping only, built with `HB_NO_DRAW` (no outline
    extraction). The subset sources are absent, though `hb-subset.h` and some `hb-subset-*.hh`
    headers are present (`engine/libs/harfbuzz/CMakeLists.txt:1-34`).
  - ThorVG is built and linked only with `JASPERENGINE_HTMLUI` (`engine/CMakeLists.txt:1134`,
    :1192-1196), and `engine/notices.json:39` says `"when": "htmlui"`.
  - `tools/ci/check-notices.py:117` accepts one `when` value per library.
- **Text**: `engine/core/util/TextShaper.h` shapes over a fallback chain (`ShapedGlyph` with
  `face`, `glyphIndex`, offsets, advance, `cluster` and `line`).
- **Jobs**: `jobs::Lane::Background` (`engine/core/jobs/JobTypes.h:19-23`). With 0 workers (the
  single-thread web build) the engine pumps the queues once a frame (`JobSystem.h:24-25`), so a
  long job runs inside a frame there.
- **Profiling (S15)**: `get_render_stats`, `get_frame_times`, `capture_profile`, `get_gpu_passes`
  and `get_vram_report` (`editor/mcp/tools/ProfilerTools.cpp:1028-1136`,
  `docs/mcp/guide/profiling.md`).
- **The S13 device bar** is a Mali-G57 / Adreno 610 class phone at 1080p (S13 R14). It was never
  measured on a device: S13 records AC23 as not measured, with the costs bounded statically
  (`docs/specs/S13-post-fx.md:463-465`).
- **Tests**:
  - the Android render gate is a C++ project with the scenes `lit3d`, `ui2d` and `htmlui`
    (`tools/test-projects/render-gate/main.cpp:4-11`);
  - web gates are `tools/web/gates/<name>.json`;
  - `tools/ci/test_player_cli.py --only variants` checks variant choice (:23-34).
- **Export**:
  - `copyAssets` ships every asset file not skipped (`editor/Exporter.cpp:2753-2830`);
  - `W-PLAYER-VARIANT` is the variant warning (`editor/ExporterPrebuilt.cpp:382`);
  - S36 writes `licenses/THIRD-PARTY-AUDIO.txt`, the model for a font credits file.

## 4. Reference

### 4.1 Pachislop (`/tmp/pachislop-analysis/repo`, used with its author's permission; ported, `NOTICE.md` entry from S80)

| Pachislop | Where | S81 |
|---|---|---|
| Cache cascade: a colour change never redoes the SDF (`geomKey` → `frame.key` → `maskKey` → `sdfKey` → `heightKey` → `stackKey`) | `src/engine/layer2d/Title2DRenderer.ts`; report p1 §3.2 | Realtime keeps each step's key. A request whose geometry key is unchanged starts after the mask and SDF steps (R16) |
| Immutable data as the change signal (`version(o)`) | `src/engine/layerSource.ts` | The component hashes the resolved state (document revision + overrides). Same hash, no render (R14) |
| Interactive draft quality vs final | `src/engine/interactive.ts` | Not taken: a game never shows a draft. Realtime always renders final quality and keeps the old image instead (R17) |
| Highlight peaks and greedy star picking with tie smoothing (`pickHighlights`, `HL_FADE`, `HL_TIE_REACH`, `highlightSize`) | `src/generators/glints.ts:390-470` | S83 ports the analysis (S83 R13-R14). S81 consumes its anchors for the live glints and re-runs it only when an analysed layer changes (R36) |
| Sweep band: a gradient of length `uLen` centred at `uPos` −1..1 "of the way from just off one side of the box to the other" | `src/effects/a/stylize.ts:43`, :66-111 | `title_fx` SWEEP with `sweepPosition` 0..1 = (uPos + 1) / 2, so 0 and 1 are both fully off the title (R34) |
| Perf budgets with warm-up, median and p90 | `tests/perf/budgets.json`, `tests/perf/perf.spec.ts` | Same method (warm-up, median, p90) in the S81 bench (AC17), with our own budgets (§6.8) |

Improved on purpose:
- Pachislop holds one full-frame target per top-level node while idle (report p1 §3.5). Idle
  Realtime holds one RGBA8 display target. Intermediates come from a pool that is released when
  idle (R20).
- Pachislop never time-slices: a heavy frame blocks for 202 ms. S81 splits every render into
  steps under a budget (R15).

### 4.2 Engine precedents copied

| Need | Precedent |
|---|---|
| Module option, sources file, stub when OFF | `JASPERENGINE_NAVIGATION` (`engine/CMakeLists.txt:39-42`, `NavigationSources.cmake`), `UIDocumentSystemNone` (S34 D3) |
| Float caps, test flags, retry of failed targets | `FluidSimulator` (`FluidSimulator.cpp:98-115`, `.h:40-58`) |
| Module pass type and collector keys | `ShaderType::FLUID`, `ShaderVariantCollector::addFluidKeys` |
| Offscreen passes before the cameras | `RenderSystem.cpp:15266-15270` |
| Keyed entries as properties | `Catalog::splitShaderEntryProperty` (`editor/Catalog.cpp:651`) |
| Exposed values Inspector (checkbox per override) | `InspectorParticles.cpp:833-910` |
| GameObject accessor in Lua | `GameObject:getAnimationPlayer` (`GameObjectClassesLua.cpp:1303-1315`) |
| Credits file in the build | S36 `licenses/THIRD-PARTY-AUDIO.txt` |
| UI particles under an element | `docs/mcp/guide/ui.md` "Particles in the UI" |

HarfBuzz's subsetter (`hb-subset*.cc`, `hb-repacker`, upstream 14.3.0, Old MIT) is vendored next
to the shaping build as an editor-only target (R50). It gets a `NOTICE.md` row update and an
`engine/notices.json` entry with `"when": "editor"`.

## 5. Requirements

### Functional — the component

- **R1. Title Art Renderer** (`TitleArtRendererComponent`, Catalog key `titleart`, Inspector
  "Title Art Renderer"; fields in §6.1). It drives a **companion** on the same GameObject:
  - an Image (`UIComponent` + `ImageComponent`) puts it in **UI space**: canvas, Hierarchy order,
    Canvas Groups, masks;
  - a Sprite (`MeshComponent` + `SpriteComponent`) puts it in **world space**.

  The space is read from the companion. There is no field, so the two cannot disagree. Adding the
  component to a GameObject with neither adds an Image in a UI scene, else a Sprite, in the same
  undo step. With both, the Image wins and the Inspector warns.
- **R2. It draws through the companion.** The system sets the companion's texture, size and pivot
  (when `pivotFromTitle`), its blend mode (Alpha: the display target holds S80's output encoding, straight-alpha sRGB, S80 R12, exactly as the baked PNG does), and,
  with live layers on, its custom shader and uniforms. These writes are runtime state, never
  saved:
  - the companion's saved texture and shader fields stay as the user left them;
  - the companion's Inspector rows that the renderer drives show "Driven by Title Art Renderer",
    read-only;
  - removing the component restores the companion's saved values.

  Sorting is the companion's: Hierarchy order in UI, and for sprites the transform order (S52's
  Sorting Layer and Order in Layer once S52 lands). The renderer adds no sorting field.
- **R3. Size and pivot.**
  - **Set Native Size** (Inspector button and Lua `setNativeSize()`, one undo step "Set Native
    Size (Logo)") sizes the companion to the title's bake frame:
    - UI: frame px × `uiScale`, in UI units;
    - world: frame px / `pixelsPerUnit` (default: the project's Pixels Per Unit from S41 when set,
      else 100).
  - With `pivotFromTitle` (default true), the companion's pivot follows the frame's canvas origin:
    the frame-relative position of the document's (0, 0), from the sidecar (R8).
- **R4. Exposed overrides.**
  - `overrides` is a keyed collection: one entry per overridden Exposed parameter (S80 §6.1),
    addressed as the property `overrides.<name>`.
  - Each entry is a property of its own for `set_properties`, prefab overrides (the Inspector's
    `propertyRow`, bold label and Apply/Revert per entry), undo and clip tracks.
  - An entry whose name the document no longer exposes is kept. The Inspector shows "Missing
    parameter 'glow_amount'" with Remove, and nothing is deleted on save.
  - An entry whose type differs from the document's is ignored with the same row and a type note.
- **R5. Resolution order.** The effective value of an Exposed parameter is the component's
  override, else S87's per-language value (when S87 lands), else the document's value. `text`
  overrides win in every language. A localised instance uses S87's keys instead of a literal
  override.
- **R6. Create.**
  - GameObject > UI > Title Art creates an Image + Title Art Renderer at native size.
  - GameObject > 2D Object > Title Art creates a Sprite + Title Art Renderer.
  - Dragging a `.titleart` from the Project window into the Hierarchy, the Scene view or the Game
    view does the same: UI when it lands in a UI scene or under a UI element, else world.
  - Dropping a `.titleart` on the Texture field of an Image or a Sprite adds the component instead
    of failing.
  - Each is one undo step named "Create Title Art (logo)".

### Functional — Baked

- **R7. Baked** shows the bake's main output (`logo.titleart.png`, S80 Outputs) as the companion's
  texture, through S14's cooked texture like any PNG.
  - Overrides are not applied. The Inspector disables their rows: "Baked: the image is the
    asset's bake. Realtime applies overrides."
  - Live layers work (R33-R38). Glow Pulse uses the first output with `content: "glow"`.
  - A missing bake shows nothing and logs once:
    "Title Art 'Logo': assets/titles/logo.titleart has no bake (logo.titleart.png). Bake it in
    the Title Designer (Bake) or with export_title."
  - A stale bake (S80 recipe) draws and is reported by S80's checks.
- **R8. Bake sidecar.** S80's bake writes `logo.titleart.bake.json` next to its PNGs (format
  `jasperengine.titleart.bakeinfo`, version 1, S80 §6.9). It is canonical JSON:
  - `frame {x, y, w, h}` in canvas px, image space;
  - `origin [x, y]`: the document origin in frame px;
  - `outputs [{id, file, content}]`;
  - `anchors`: R36's list, in frame-normalised coordinates (S83's analysis fills it);
  - `alpha_bounds`;
  - `source_hash`, `bake_version`;
  - optional `languages` (S87's per-language sets table, source sidecar only) and `language`.

  S81's `TitleArtBakeInfo` (`engine/core/util/TitleArtBakeInfo.{h,cpp}`, always built) reads it.

  Baked mode reads only this file and the PNGs. The sidecar is shipped in every mode (≤ 4 KB).
  The `.titleart` ships only for Realtime and Glyph Atlas titles (R52).

### Functional — Realtime

- **R9. Render on change.** A Realtime title renders when its **state hash** changes. The hash
  covers:
  - the document's content hash;
  - the effective Exposed values;
  - the resolution (display scale × `resolutionScale`, rounded to 1/8);
  - the pixel path (float or RGBA8).

  It does not render per frame, on movement, on companion resize within the same resolution, or
  on alpha, tint or live-layer changes. With no change, its frame cost is the companion's (R48).

  **S83 live kinds.** When the only changed values are S83 L params (an Exposed parameter whose
  every target is an L param of an effect of `live.kind` `scale_split` or `kernel`, S83 D10), the
  change is a **live compose**, not a re-render. In the same frame, before the cameras, the
  effect's live passes run over its cached splits into the back display target, and the targets
  swap. This step is not time-sliced; it is counted against a per-title live budget (0.5 ms on
  mobile and web, 1 ms on desktop, from S83's cost model). Over that budget the change falls back
  to a re-render, and `check_title`'s `fx_live_budget` reports it. `live.kind: none` targets
  always re-render. S83's kernels are TITLE_ART pass variants (`TFX_LIVE`) run by this compose.
  The companion's `library:title_fx` stays the four built-in live layers (R33).
- **R10. Same renderer.** Realtime runs S80's renderer (`TitleArtRenderer`, S80 R10) with the same document
  and state that the bake uses. The display target holds what the main output PNG holds, so a
  Realtime title and its bake match within S80's GPU tolerance (AC3: the "both ways" golden of
  contract decision 1).
- **R11. Prebaked initial state** (`prebakeInitialState`, default true).
  - At export, every distinct initial state of a shipped Realtime instance is rendered in the
    editor and shipped as a texture. A distinct state is a document hash + overrides + resolution
    for the target's reference display (Project Settings > Title Art > Reference Height, default
    1080).
  - Each texture is cooked by S14 as `type: ui`, with no mips by default, and deduplicated by
    state hash.
  - The first frame shows it at once, and nothing renders until the state changes.
  - Without prebaking (or in editor Play) the first render is a normal request (R12). In the
    meantime the companion shows the Baked image when the state equals the document's defaults,
    else nothing (alpha 0). A wrong text never flashes.
- **R12. Requests.** A state change queues one request per title. A new change while a request is
  queued replaces it. A new change while the title is rendering:
  - restarts the render with the new state when under 50 % of its estimated cost is done;
  - otherwise finishes, then renders the latest state.

  So a title never shows an image more than one render older than its state, and a keyed
  parameter that changes every frame cannot starve the render.
- **R13. Scheduler** (`titleart::TitleArtScheduler`, one per Engine, shared by every scene):
  - Each frame, in `RenderSystem` before the camera passes (where fluids step), it runs steps of
    queued renders until the frame's budget is spent.
  - Order: higher `priority` first (−100..100, default 0); then visible before not visible; then
    the oldest request.
  - Visible means:
    - UI: the element is active, enabled, under an active canvas, and its rect intersects the
      screen;
    - world: the companion's `worldAABB` intersects a camera frustum of the scene.
  - A title that is not visible gets no steps unless `renderWhenHidden` is true (default false).
    Its request waits, so a hidden counter costs nothing.
- **R14. Hash short-circuit.** A request whose hash equals the displayed image's is dropped. A
  request whose hash equals an image held in the cache (R20) swaps it in on the next frame without
  rendering, which covers toggling between two texts.
- **R15. Time slices.**
  - A render is S80's **render job** (`TitleRenderJob`, S80 R18): an ordered list of steps (CPU
    stages and GPU passes), each with a cost estimate (§6.3).
  - The scheduler runs whole steps while `spent + estimate ≤ budget`. It always runs at least one
    step in a frame where a visible request waits, so progress is guaranteed.
  - GPU steps whose estimate exceeds the budget are split by the plan into scissor tiles of at
    least 256 × 256 px (every S80 pass writes per output pixel). A step that still exceeds it runs
    alone in its frame and is counted (`over_budget_steps`).
  - **Budget** (Project Settings > Title Art > Frame Budget, ms of estimated GPU + main-thread CPU
    work): 2.0 on Android, iOS and web; 4.0 on desktop; 8.0 in the editor's Edit mode.
    `TitleArt.setFrameBudget(ms)` changes it at runtime (0.25..16).
- **R16. CPU stages** (shaping, outlines through `hb-draw`, ThorVG mask rasterization, anchors,
  S80's CPU distance fields):
  - They run as `jobs::Lane::Background` jobs, split into units of at most ~1 ms (one glyph run,
    one mask tile).
  - With 0 workers (single-thread web) the scheduler runs the units on the main thread inside the
    same budget instead of submitting them.
  - A CPU job owns its own `hb_blob`/`hb_face`/`hb_font` over the shared font bytes. HarfBuzz is
    built `HB_NO_MT`, so HarfBuzz objects are never shared across threads.
  - A request whose geometry key is unchanged (colour, material or effect parameter only) skips
    straight to the first GPU step that depends on the change (the cache cascade, §4.1).
- **R17. Double buffering.**
  - The companion shows the **front** display target (RGBA8, S80's output encoding).
  - A render writes the **back** target. When its last step completes, the targets swap before
    the frame's camera passes, so the new image appears in the same frame.
  - The old front is kept as the back while the title has a pending request, else released after
    `Release Delay` (R20).
  - **Transition** `cut` (default) swaps instantly. `crossfade` blends old to new over
    `transitionDuration` (default 0.15 s) through `title_fx`'s CROSSFADE feature (R35).
- **R18. Readiness.**
  - `ready` is true when the displayed image's hash equals the state hash.
  - `onReady` fires once per state on the frame its image first displays. A dropped
    (R14-equal) request fires it on the next frame.
  - `pending` reports what it waits for: `queued`, `hidden`, `shaders`, `rendering n/m`,
    `failed`.
  - The renderer never draws with S32 stand-ins: a plan starts only when all its variants are
    ready, and waits as `shaders` until then. A stand-in would render wrong pixels into a cached
    image.
- **R19. Failure.** A render fails when a target cannot be allocated, a variant failed to build,
  the document does not parse, or a font cannot load. Then:
  - the previous image stays (or the R11 fallback when there is none);
  - `failed` is true and `error` holds one sentence with the cause and the fix;
  - `onFailed(message)` fires;
  - the Console gets one error per title and cause.

  Allocation failures retry after 2, 4 and 8 s, then stop until the state changes (the
  FluidSimulator policy). A float target that fails to allocate while the caps said it could
  switches this session to the RGBA8 path once, with a warning, and the render restarts. Missing
  glyphs are not failures (R55).
- **R20. Memory.**
  - Intermediates (RGBA16F, or RGBA8 on the fallback path) come from S80's `TitleTargetPool`,
    shared by all titles, sized to the largest active render.
  - The pool is released after `Release Delay` with no render: 3 s on mobile and web, 10 s on
    desktop.
  - Cached front images of states shown earlier (R14) and S80's per-step caches (masks, SDF) live
    in an LRU cache bounded by Cache Budget (16 MiB mobile and web, 64 MiB desktop). The LRU is
    evicted before any allocation that would exceed the budget.
  - `get_vram_report` lists the pool, the cache and the display targets under owner
    `TitleArt/<GameObject>`.
- **R21. Pixel path.**
  - The float path (RGBA16F intermediates) runs where S80 R12's condition holds
    (`renderRGBA16F && filterRGBA16F`); otherwise the RGBA8 path runs, with S80's structure and
    tolerance.
  - Test hooks:
    - S80's `TITLE_ART_TEST_NO_FLOAT_TARGETS` (and its siblings), plus S81's
      `TITLE_ART_TEST_FAIL_TARGETS` and `TITLE_ART_TEST_SLOW_STEPS`, in S80's per-request
      `RenderRequest::testFlags` (never global: the scheduler copies the instance's test state,
      set through `setDisabledFeaturesForTesting`, into each request it builds);
    - the environment variable `JASPER_TITLEART_FORCE_RGBA8=1`, read once at start by players and
      the editor;
    - the Android property `debug.jasperengine.titleart_rgba8`.
  - `pixelPath` (`float` | `rgba8`) is readable from Lua, MCP and the Inspector.
- **R22. Device and context loss.** When `SystemRender::getContextId()` changes (S80 R19:
  Android `EGL_CONTEXT_LOST`, `engine/platform/android/NativeEngine.cpp:1388`; web context
  restore), each
  Realtime and Glyph Atlas title is invalidated and re-renders from its document. It shows its
  prebaked or Baked fallback until ready. No CPU copy of pixels is kept for this.
- **R23. Hot reload.**
  - In the editor (Edit and Play), a committed change to the `.titleart`, a font or an image it
    uses invalidates every instance: the S05 route plus S80's document commit, which bypasses the
    expected-write ledger.
  - Realtime re-renders. Baked follows when the bake's PNG reloads.
- **R24. Edit mode.** Realtime and Glyph Atlas titles render in Edit mode too (budget 8 ms), so
  the Scene and Game views show the instance's overrides before Play. The Title Designer's
  previews are S82's and do not use this scheduler.

### Functional — Glyph Atlas

- **R25. The dynamic layer.** A Glyph Atlas title needs one 2D Text layer whose `text` is the
  target of the Exposed parameter `text` (S80 creates it by default).
  - Layers below it form the **back plate** and layers above it the **front plate**. Each plate
    is rendered by the Realtime renderer once per state of its own Exposed values (R9-R19 apply).
  - The text layer's ancestor groups may have only `normal` blend, opacity and transforms.
    Otherwise the title falls back to Realtime with the warning `glyph_atlas_unsupported`, naming
    the group.
- **R26. Glyph cells.**
  - Each distinct (font face, glyph index, variation axes, `CharAdjust.face` material hash,
    effective style hash) is rendered once by S80's renderer: a single-glyph run at the layer's
    size × resolution, into a cell padded by the style's reach (S80 `RenderRequest::glyphCell`, padded by
    `reachBounds`, S80 §6.6).
  - Each glyph is split into two cells:
    - **back**: every part and effect S80 draws before the face (rims outermost first, drop
      shadow, outer glow);
    - **front**: the face and everything after it.
  - The compose pass draws every back cell, then every front cell. Rims of neighbouring letters
    therefore stay behind all faces, as in the merged render.
- **R27. Layout** (`titleart::GlyphLayout`):
  - `TextShaper::shapeRange` over the layer's font, Fallback Fonts and built-in face, with
    `kerning`, `tracking` (→ `letterSpacing`), `features`, `language`, `line_height`, `align`,
    `h_scale` and `v_scale`;
  - `vertical`, `tate_chu_yoko` and `proportional`, as S80 lays them out (the same layout code,
    `engine/core/titleart/text/TitleTextLayout.*`);
  - `chars.<index>` offsets, scales and rotations per glyph quad, by code point index of the
    current text;
  - `lines.<index>` per line.

  Each glyph quad is placed at its pen position minus the cell padding.
- **R28. Compose.**
  - Pass 1 draws into a premultiplied pool target (cleared): the back plate, back cells, front
    cells, then the front plate. It is a single draw of 2N + 2 quads from one atlas page set,
    plus one bind per plate texture.
  - Pass 2 is S80's output pass, which writes the back display target in S80's output encoding.
  - With a non-`normal` text layer `blend`, the glyphs compose into their own pool target and
    S80's blend pass puts them over the back plate (+1 pass).
  - It swaps the same frame (R17), so a counter that changes every frame is current every frame.
- **R29. Atlas.**
  - Pages are RGBA8 holding premultiplied cells in S80's RGBA8-path encoding. `atlasPageSize` is
    1024 (512 | 1024 | 2048), with up to `atlasMaxPages` 4 (1..8), packed by a skyline packer.
  - A new glyph queues a cell render (one glyph a step). The string stays on its previous image
    until every glyph it uses is ready, so a counter never shows a hole.
  - A full atlas evicts least recently used cells that no displayed string uses.
  - When nothing can be evicted, the request fails (R19) with:
    "Title Art 'Score': the glyph atlas is full (4 pages of 1024). Raise Max Pages, lower
    Resolution Scale, or reduce the Characters."
  - A style or color scheme change rebuilds the cells (time sliced, old image kept).
- **R30. Restrictions.** Glyph Atlas refuses a Dimension 3D text layer
  (`glyph_atlas_unsupported: 3D text`): the title falls back to Realtime, with the warning in the
  Inspector, `get_title` and the Console.
- **R31. Glyph safety.** Every effect, generator and style member has a `glyph_safety`: `safe`,
  `approximate` or `unsafe` (`GlyphSafety`, declared on S80's `EffectDef`/`GeneratorDef` and
  `TitleArtSchema`; S83 §6.13 sets the values of every definition). S81's defaults for S80's
  members:

  | Class | Members | In Glyph Atlas |
  |---|---|---|
  | safe | fill `solid`, gradient fills with `span: "char"`, bevel, reflection, finish, pattern, satin, texture, inner glow, gloss, sheen, glass, rims with `plate.kind: "contour"`, `knockout`, `fill_counters`, `weight`, `h_scale`/`v_scale`/`shear`, CharAdjust, part effects that read only the part | Exact per glyph |
  | approximate | gradient fills with `span: "layer"` or `"line"` (mapped to `char`), outer glow, drop shadow, Light Glow, erode (its noise moves with the glyph) | Per glyph. Overlaps differ from the merged render. Listed as info |
  | unsafe | `warp`, `distort`, rims with `plate.kind: "hull"` or `"box"`, layer masks, layer-scope effects that read neighbours beyond the cell (`motion_blur`, `shatter`, `extrude`), generators that `follow` the text layer | Skipped (hull/box draw as `contour`). Warning `glyph_atlas_effects` lists them, with fix ops |

  `check_title {checks: ["glyph_atlas"]}` (S80's tool, S81 adds the check) returns the list
  without an instance. The Inspector shows "2 effects don't apply per glyph: Warp, Plate (Hull)"
  under Render Mode.
- **R32. Prebaked atlas.**
  - At export (`prebakeInitialState`), every glyph of the title's Characters (R53) that the fonts
    have is rendered into atlas pages, shipped as RGBA8 PNGs without compression (S14 import
    `compression: none`, so cells never bleed through block artefacts). This is done when the set
    is at most 1,024 glyphs; above that, only the document's text and Digits are prebaked.
  - The plates of the initial state are prebaked as in R11.
  - A shipped counter then renders nothing at runtime unless a glyph outside the prebaked set
    appears or a parameter changes.

### Functional — live layers (any mode)

- **R33. Live Effects** (fields in §6.1) draw in the companion's single draw through
  `library:title_fx` (UI) or `library:title_fx_sprite` (world).
  - With no Live Effect and no crossfade, the companion keeps the built-in shader and
    `title_fx` is not used or collected.
  - `ui_fx` (`engine/shaderlib/library/ui_fx.frag`) is not changed.
  - A user's own custom shader on the companion is replaced while a Live Effect is on, and the
    Inspector warns.
- **R34. Sweep** (SWEEP). A band across the title's frame along `sweepAngle` (deg clockwise from
  +x, S80's convention, default 25):
  - `sweepPosition` 0..1: 0 and 1 are fully off the frame on either side;
  - `sweepWidth`: fraction of the frame's diagonal extent along the angle, default 0.15;
  - `sweepSoftness` 0..1, default 0.6;
  - `sweepColor`: HDR colour, alpha = strength, default (1, 1, 1, 0.8).

  The band adds light inside the title's alpha only, so it follows any shape. With `sweepAuto`
  (default false), the system writes `sweepPosition = min(1, fract(liveTime / sweepInterval) ×
  sweepInterval × sweepSpeed)`. `sweepSpeed` is passes per second (default 1.5: one pass takes
  0.67 s) and `sweepInterval` is seconds between sweep starts (default 2.5). A clip that keys `sweepPosition`
  turns `sweepAuto` off for the clip's duration (the track writes after the system).
- **R35. Glow Pulse** (GLOW). Adds `glowColor` × `glowPulse` × the bake's or render's glow output.
  - Baked: the first output with `content: "glow"`. Realtime: the renderer's glow output target,
    an R8 at half resolution, produced only when this feature is on.
  - Without a glow output, Glow is unavailable and the Inspector says: "Enable a Glow output in
    the Title Designer (Outputs) to pulse the glow."
  - `glowPulse` defaults to 0 (keyed by clips). `glowColor` is HDR, default (1, 0.85, 0.4, 1).

  **Tint** (TINT): `tintColor` and `tintAmount` with the overlay blend `ui_fx` uses, so the
  shading is kept. **Crossfade** (CROSSFADE): R17.

  **Animation** (ANIM, for S85): uniforms `animAffine0`, `animAffine1` (a 2×3 affine in
  frame-normalised space about the pivot), `animColor` (opacity, and flash colour × amount); and
  the texture rect the companion samples, `frameRect` plus a second `frameRect2` with
  `frameBlend` for blended flipbook frames. `TitleArtSystem` writes them from S85's
  `TitleAnimPlayer`; with no animation playing the feature is off and R48 holds. The companion
  shows a flipbook page by texture rect (Image: `ImageComponent` UV rect; Sprite: the sprite's
  frame rect), so a flipbook is the same one draw.
- **R36. Highlight anchors** are S83's analysis (S83 R13-R14: an analysis grid fixed in canvas px,
  Pachislop's `pickHighlights`):
  - **Baked** reads them from the sidecar (R8).
  - **Realtime** reads them from the export data of its prebaked state. When an analysed layer
    changes at runtime, the analysis re-runs as the last steps of the render job: its grid pass,
    then one `FrameReadback` of the grid, issued in a slice of its own with a fixed estimate of
    1 ms on mobile and web. The new image and its anchors swap together.
  - **Glyph Atlas** analyses each cell when the cell renders (its grid read with the cell's step).
    The compose keeps the 16 strongest anchors of the laid-out string.
  - Each anchor is `{x, y, size, strength, fade}` in frame-normalised coordinates, at most 16 per
    title.
  - Live Glints, sweep and glow read no pixels at runtime, so a title whose analysed layers never
    change does no readback (S83 R14).
- **R37. Glints** (GLINTS). A four-point star twinkles at each anchor.
  - Phase per anchor: `hash(glintSeed, index)`. Brightness: `glintAmount × strength × fade ×
    twinkle(liveTime × glintRate + phase)`.
  - `twinkle` is a smooth bump with duty 0.25.
  - Size: `glintSize` × anchor size; `glintSize` is a fraction of the frame height, default 0.06.
  - `glintColor`: HDR, default (1, 1, 1, 1).

  Anchors go in a uniform array of 16 `vec4` (x, y, size, strength × fade) plus a count. Stars
  are drawn analytically in the fragment shader (no extra geometry), clipped to the frame plus
  the star radius, which the vertex shader expands.
- **R38. Live Time.** `liveTime` (seconds) is the component's clock for sweep auto and glints:
  - it starts at 0 when the title is enabled;
  - it advances by scaled time, or unscaled time with `liveUnscaledTime`;
  - it is keyable (a clip scrubbing it previews exactly) and settable from Lua.

  Two runs at `--fixed-fps` give identical pixels (AC9). No live layer reads engine time.

### Functional — particles

- **R39. Title particle presets** (`editor/titleart/runtime/TitleParticlePresets.cpp`). These are
  S20 effects built from `create_particles` presets (`editor/mcp/tools/ParticleShared.h:17-19`)
  with overrides, emitting from a Rectangle shape fitted to the sidecar's `alpha_bounds`:

  | Preset | Base | What changes |
  |---|---|---|
  | Sparkle | `magic` | Additive four-point stars, small, slow, colours from the title's color scheme highlight |
  | Embers | `sparks` | Rise from the bottom edge, orange to red over life, drift noise |
  | Confetti | `explosion` | One burst, gravity, rotation, five scheme colours |
  | Coin Shower | `sparks` | Falling gold discs with rotation, a burst then a 2 s rate |

  The Inspector's **Add Particles** menu creates a child Particle System as one undo step "Add
  Title Particles (Sparkle)". It is a UI particle system under a UI title (unscaled time, UI
  units, `docs/mcp/guide/ui.md:224-235`), and a world one under a sprite. `get_title {entity}`
  returns the same as ready-to-call `create_particles` arguments (R71).

### Functional — animation

- **R40. Keyable rows** (`PropertyBindings.cpp` `buildRows`, component `titleart`):
  - `enabled` (Bool).
  - `liveTime` (Float).
  - `sweepPosition`, `sweepWidth`, `sweepSoftness`, `glowPulse`, `tintAmount`, `glintAmount`
    (Float).
  - `sweepColor`, `glowColor`, `glintColor` (Color4, SRGB display).
  - `tintColor` (Color4, SRGB).
  - `overrides.<name>`: parametrized (`PropertyParams::member` = the name; the kind comes from the
    document at bind):
    - `number` → Float;
    - `color` → Color3, SRGB (S80 colours have no alpha);
    - `bool` → Bool (discrete).

    `has()` is true only for numeric Exposed parameters of the bound document.
  - Writes set the component's dirty flag and do nothing for an unchanged value (the file's
    invariant).
  - `text`, `font`, `image` and `color_scheme` parameters are not keyable (no string kind). They
    change through events (R41).
- **R41. Reserved clip events.** The `TitleArtSystem` subscribes, with tag `titleart`, to
  `onEvent` of the Animation Player and Animator on each GameObject holding a Title Art Renderer
  or one of its ancestors up to the clip root. It handles:

  | Event (functionName) | Args | Does |
  |---|---|---|
  | `titleArtSetText` | `text` or `path, text` | Sets the `text` override |
  | `titleArtSetParameter` | `name, value` or `path, name, value` | Sets any Exposed parameter (number, bool, `"#rrggbb"`, a font or image asset path, a scheme id) |
  | `titleArtSetColorScheme` | `id` or `path, id` | Sets the `color_scheme` parameter |
  | `titleArtPlay` | `name` or `path, name` | `play(name)` (S85) |

  - `path` is relative to the player's GameObject, as clip tracks address children. Without
    `path`, the target is the Title Art Renderer on the player's GameObject.
  - Handled events count as subscribed, so no "has no handler" warning. The script method of the
    same name still runs if one exists.
  - An event on a Baked title, an unknown path or a wrong arg logs one warning per (clip, event)
    with the fix. A Development build also shows it in the overlay.
- **R42. Animation window.**
  - Add Property lists the R40 rows under "Title Art Renderer".
  - Keying an `overrides.*` row of a Realtime title shows the note: "Re-renders the title on each
    change (up to the frame budget). For smooth motion use Live Effects or a Title Art animation
    (S85)."
  - Glyph Atlas `overrides.*` rows other than plate parameters show: "Rebuilds the glyph atlas on
    each change."
- **R43. Record mode.** Editing a live field or an override in the Inspector while recording keys
  it, through the existing record hooks (`editor/animation/ClipRecordHooks.h`) with the new rows.
- **R44. S85 animations.**
  - `play(name, {speed, loop, unscaled})`, `stop()`, `isPlaying`, `animations` and
    `onAnimationFinished(name)` are S81's API; S85 defines what they play.
  - Baked plays S85's baked flipbook (texture rect frames on the companion).
  - Realtime renders S85 frames through this scheduler: while an animation plays, the title is
    **busy** and may spend up to S85's per-title share of the budget each frame.
  - Glyph Atlas plays per-glyph transforms at layout time, without re-rendering cells.
  - When the animation ends, the title returns to idle cost.
  - S85 adds its own rows (`titleart.animationTime`); S81 reserves that name.

### Platform & backends

- **R45.** Everything runs on GL 3.3 core, GLES3/WebGL2, Metal, D3D11 and Vulkan, and on all six
  export targets. Fragment passes and fullscreen or quad draws only: no compute, SSBOs, bindless,
  indirect draws or 64-bit atomics. Display targets and atlas pages are RGBA8, renderable
  everywhere. Float intermediates have the R21 fallback.
- **R46.** The renderer's passes use S80's `ShaderType` (called `TITLE_ART` here, appended after
  `HTMLUI_PRIM`, `Render.h:183`; the pass and pixel path in the property bits). They are cooked
  per backend like `FLUID`. `title_fx` and `title_fx_sprite` are library shaders that GLES3/WebGL2
  compile within its limits (one `@texture glowTexture`, one `prevTexture`, 16 + 2 + 5 `vec4`
  (ANIM's three, the two frame rects)
  uniforms).
- **R47. Warm-up (S32).**
  - The collector adds the TITLE_ART keys of every shipped Realtime and Glyph Atlas document, for
    both pixel paths. Only one path runs, but the device decides at runtime, as with fluid's
    linear filter.
  - The keys of titles in the start scene go to the front of the shipped warm-up list
    (`ShaderWarmup`, `engine/core/shader/ShaderWarmup.h`).
  - With Hold First Scene for warm-up (S32), the first frame waits for them.
  - Without warm-up, R18 applies: the prebaked image shows, and the render starts when its
    variants are ready.

### Performance (Release builds; device rows on the S13 bar)

- **R48. Idle.** An idle title adds the same draws, passes, pipelines and bindings as an Image (UI)
  or Sprite (world) with the same texture. That is +0, measured by `get_render_stats` against a
  twin without the component (AC12). It holds whether the title is Baked or Realtime, and whether
  it is Glyph Atlas or not, with no Live Effect. Main-thread CPU of `TitleArtSystem::update` and
  the scheduler with only idle titles: ≤ 2 µs per title on server A, and ≤ 10 µs on the device bar.
- **R49. Budgets.** The full list is in §6.8. Measured with `capture_profile` + `get_gpu_passes
  capture_id` (pass labels `TitleArt/<GameObject>/<step>`), `get_frame_times` and
  `get_vram_report`. Desktop and emulator figures are gated automatically (AC17). Device figures
  are a manual run recorded in the guide (AC18), as S13 AC23 is.

### Data & compatibility — module, export, fonts

- **R50. Libraries.**
  - **HarfBuzz subsetter.** Vendor HarfBuzz 14.3.0's `hb-subset*.cc`, `hb-subset-*.hh` (missing
    ones), `graph/` and `hb-repacker.hh` dependencies into `engine/libs/harfbuzz/subset/`.
    - It becomes the target `harfbuzz-subset`, linked only by the editor (in the
      `JASPERENGINE_TEXTURE_ENCODERS` tree), with the same `-fvisibility=hidden`.
    - `NOTICE.md`'s HarfBuzz row names the subset sources.
    - `engine/notices.json` gets `"harfbuzz-subset": {"dir": "engine/libs/harfbuzz/subset",
      "when": "editor"}`.
  - `hb-draw` (`HB_NO_DRAW` only when the module is OFF), ThorVG (`JASPERENGINE_HTMLUI OR
    JASPERENGINE_TITLE_ART`, `engine/CMakeLists.txt:1134` and :1192-1196, `util/ThorVGRuntime`
    kept when either is ON, notices `"when": ["htmlui", "titleart"]` with `check-notices.py` taking
    a list) and Clipper2 (`"when": "titleart"`) are S80's (S80 R40, §6.15). S81 adds `titleart` to
    the player features (`player-info.json features[]`) that `when` names, and S86's earcut uses
    the same feature.
- **R51. The `titleart` module.**
  - The option `JASPERENGINE_TITLE_ART` (next to `JASPERENGINE_NAVIGATION`,
    `JASPERENGINE_TITLE_ART=$<BOOL:...>` in the public definitions) and
    `engine/core/titleart/TitleArtSources.cmake` (every title spec's module sources) are S80's
    (S80 R40, §6.15). S81 adds `runtime/` to it and, when the module is OFF, compiles
    `engine/core/subsystem/TitleArtRuntimeNone.cpp` (core, outside the module folder, the
    `UIDocumentSystemNone` precedent).
  - **Sub-options** (S81 owns their CMake and wiring; S86's code is behind them):
    `cmake_dependent_option(JASPERENGINE_TITLE_ART_3D ... ON "JASPERENGINE_TITLE_ART" OFF)`
    (`engine/core/titleart/three_d/`, S83's `three_d/Glint3D.*` included) and
    `JASPERENGINE_TITLE_ART_MESHER` (S86's mesher and earcut). Prebuilt variants with `titleart`
    carry both. In a Compile build `ModuleUsage` turns `_3D` off when no shipped Realtime title has
    a Dimension 3D layer, and `_MESHER` off when every shipped Realtime 3D title has fixed text
    (S86 R38's cooked `.titlemesh`); the evidence lines name the titles.
  - Without the module, a Realtime or Glyph Atlas title shows its Baked image and logs once:
    "Title Art 'Logo' is Realtime, but this build has no Title Art module (titleart): showing its
    bake. Builds add the module when a shipped scene or prefab uses Realtime or Glyph Atlas;
    check Build Settings > Modules."
  - The editor's shared engine requires it ON: the `JASPERENGINE_SHARED` FATAL_ERROR list
    (:1126-1127) grows by `JASPERENGINE_TITLE_ART`.
  - `player::allModules()` appends `titleart`, with `moduleDisplayName` "Title Art (Realtime and
    Glyph Atlas)" and `builtModules` under `#if JASPERENGINE_TITLE_ART`.
  - `variants.json` adds `titleart` to `modules` and to `full`, `ui` and `core`. **`lite3d` and
    `lite2d` stay without it** (R59, §9).
  - **Always in core:** the component, `TitleArtSystem` (Baked binding, live layers, anchors from
    the sidecar), the sidecar reader (`engine/core/util/TitleArtBakeInfo.{h,cpp}`) and the Lua `TitleArt` class.
    None of it is in `engine/core/titleart/`, which S80 R40 compiles only with the module. The
    program's always-built part also holds S85's `engine/core/titleanim/` and S87's
    `engine/core/util/TitleArtLanguage.*` (S80 §6.15). Target for the whole core part ≤ 60 KiB
    stripped arm64 and ≤ 40 KiB wasm gzip: S81's own ≤ 34 / 25, S85's ≤ 20 / 12, S87's ≤ 6 / 3;
    S81 measures all three.
- **R52. Detection** (`ModuleUsage`, `editor/players/ModuleUsage.cpp:263-286`).
  - `titleart` is needed when any shipped scene or prefab has an enabled Title Art Renderer with
    `renderMode` Realtime or Glyph Atlas. Evidence reads "Title Art Renderer (Realtime) on
    Logo". Lua cannot create such a title (R67), so components alone decide. S84's
    `ColorSchemeApply` is part of the module (S84 R39), so a shipped Realtime or Glyph Atlas
    document with a `color_schemes` member needs nothing more; a Baked title never recolours.
  - Shipping:
    - **Baked-only titles** ship their PNGs and sidecar. The `.titleart` and its fonts are
      excluded (S80's `.titleart` rule, plus R54 for fonts).
    - **Realtime and Glyph Atlas** titles ship their `.titleart`, the subset fonts (R53), the
      images their layers use, the prebaked textures (R11, R32) and the sidecar.
- **R53. Characters** (the document's `runtime.characters`, §6.2, Title Designer > Runtime and
  the Inspector). The characters a Realtime or Glyph Atlas title can show in a build:
  - presets (any combination): **Digits** (`0123456789+-.,:/%×x$€£¥` and space, 25 code points),
    **Latin** (U+0020–U+007E, 95), **Latin Extended** (Latin + U+00A0–U+024F, U+1E00–U+1EFF,
    U+2000–U+206F, U+20A0–U+20CF), **Kana** (U+3000–U+303F, U+3040–U+309F, U+30A0–U+30FF,
    U+31F0–U+31FF, U+FF00–U+FFEF), **Joyo Kanji** (the 2,136 characters of the 2010 Jōyō list,
    `editor/resources/titleart/charsets/joyo-kanji.txt`);
  - **Custom**: a string;
  - **Custom File**: a text asset whose characters are added (a localisation table);
  - **Characters used by the document** (default on): every text in the document, its S87
    languages and Exposed defaults.

  The component adds `additionalCharacters` (a string, default "") for instance-specific needs.
  A build subsets each font to the union over every shipped instance using it: its document's
  set, plus `additionalCharacters`, plus every literal `titleArtSetText` and
  `titleArtSetParameter` text arg in shipped clips. Default presets for a new document: Latin.
- **R54. Font subsetting at export** (`editor/titleart/runtime/TitleFontSubsetter.{h,cpp}`,
  `hb_subset_or_fail`):
  - The subset keeps the set's code points and their closure (GSUB alternates such as `vert`,
    ligatures). It also keeps the layout tables (`GSUB`, `GPOS`, `GDEF`), `fvar`/`gvar`/`avar`
    for variable fonts, and `name`, `OS/2` and `post`. Hinting is dropped.
  - The subset is written to the build under the font's path, so references resolve unchanged.
  - Fonts used only by Baked titles are excluded when no other referrer exists in the S04
    dependency graph and no shipped Lua string literal names the file. The build result lists each
    excluded font.
  - A font shared with Text components, HTML UI or Lua ships whole (info
    `I-TITLEART-FONT-SHARED`).
  - Budget: Latin from a 400 KB TTF ≤ 200 ms; Joyo + Kana from an 8 MB CJK OTF ≤ 2 s, on server
    A. The editor caches subsets by (font hash, set hash).
- **R55. Character checks.**
  - At export:
    - a character in the document, an override, a clip event literal or `additionalCharacters`
      that is not in its font chain: warning `W-TITLEART-MISSING-GLYPH`, naming the character,
      the title and the font;
    - a literal arg of a Lua `setText` / `text =` / `setParameter` call on a `TitleArt` value
      (best effort, through `ModuleUsage`'s Lua lexer) that falls outside the Characters set:
      warning `W-TITLEART-CHARS` (and `check_title`'s check `characters` in the editor), with the
      file:line and the fix "add them to Characters >
      Custom".
  - At runtime: a text with characters outside the subset draws them with the next face of the
    chain that has them, else as nothing (never tofu, contract decision 6). The title logs once
    per character set, and fires `onMissingCharacters(chars)`.
  - **Play in the editor simulates the export** (Project Settings > Title Art > Simulate Export
    Characters in Play, default on). Characters outside the set behave as in the build, and the
    warning names the fix.
- **R56. Font embedding** (OS/2 `fsType`, read at export for every font that ships):
  - bit 1 (0x0002, Restricted License): error `E-TITLEART-FONT-RESTRICTED`, and the build stops,
    unless the font's `.meta` has `import.font.embeddingAcknowledged: true`. The Inspector of the
    font asset shows a checkbox "I have a licence that permits embedding this font in a game";
  - bit 8 (0x0100, No Subsetting): the font ships whole, warning `W-TITLEART-FONT-NO-SUBSET`;
  - bit 9 (0x0200, Bitmap Embedding Only): error `E-TITLEART-FONT-BITMAP-ONLY`; a Baked title is
    the fix;
  - bits 2 and 3 (Preview & Print, Editable) and 0 (Installable): allowed.

  Baked titles ship no font and get no check.
- **R57. OFL and credits.**
  - A font whose `name` table licence (ID 13) mentions the SIL Open Font License and whose
    copyright (ID 0) declares a Reserved Font Name is renamed in its subset: name IDs 1, 4, 6 and
    16 become `TA <8 hex of the font hash>`, so the modified font does not carry the reserved
    name.
  - Every build that ships a font writes `licenses/THIRD-PARTY-FONTS.txt`: per font, the family,
    the file, copyright (ID 0), licence (ID 13), licence URL (ID 14), "subset" or "whole", and
    the OFL text when the licence is OFL.
- **R58. Serialization.**
  - The component and its fields use S08 reflection with defaults omitted. `ComponentType`
    appends `TitleArtRendererComponent` after the last entry at landing (`editor/Catalog.h:113`).
  - No scene or prefab version bump: an added component, S03 R22.
  - `title` and font or image override values are AssetRef rows in
    `editor/asset/AssetRefSchema.cpp`, so `AssetRefSchema.CoversAllFields` stays green and
    dependencies ship.
  - The sidecar is versioned (`"version": 1`) and lenient like S80's codec.
  - The project settings join S80's `title_art.*` keys (Project Settings > Title Art):
    `title_art.frame_budget_ms` (`{mobile: 2, web: 2, desktop: 4, editor: 8}`),
    `title_art.reference_height` (1080), `title_art.release_delay_s` (`{mobile: 3, web: 3,
    desktop: 10}`), `title_art.cache_budget_mib` (`{mobile: 16, web: 16, desktop: 64}`),
    `title_art.simulate_export_characters` (true); S86 adds `title_art.realtime_3d_quality`.
    Absent = defaults, and no migration.
- **R59. Player variants.** A project whose titles are all Baked keeps its smallest variant
  (`lite2d` and `lite3d` included). A project with a Realtime or Glyph Atlas title takes the
  smallest variant with `titleart` (`core` at least), and `W-PLAYER-VARIANT` names the evidence
  and the fix: "Make these titles Baked to use lite2d". A Compile build includes exactly the
  modules detected.

### Editor UX (human)

- **R60. Inspector** (`editor/window/InspectorTitleArtRenderer.cpp`; layout in §6.4). Unity-like rows:
  - every field through `propertyRow`, with prefab override marks, multi-edit, undo named
    "Set <Label> (<GameObject>)";
  - drags preview live and commit on release; Esc reverts;
  - "Open in Title Designer" opens S82 on the asset;
  - the **Status** line shows `pending`, the last render (frames, estimated and measured ms,
    `pixelPath`) and memory.
- **R61. Render Mode** shows, under the dropdown, one line on what the mode costs. For example:
  - "Baked: fixed image, 2.0 MiB, no engine code";
  - "Realtime: re-renders on change, adds the titleart module (≈ 0.5 MiB), font subset 38 KB";
  - "Glyph Atlas: 1 atlas page (4 MiB) + plates".

  Switching modes is one undo step. Switching to Baked with no bake offers **Bake Now** (S80's
  bake job).
- **R62. Characters UI** (Title Designer > Runtime, from S82, and the component's foldout):
  - preset checkboxes, Custom, Custom File and "Characters used by the document";
  - the count, the estimated subset size per font, and a **Check Characters** button that runs
    R55 for this title and lists problems with "Add to Custom" fixes.
- **R63. Scene view.** A selected world title draws its frame and pivot. The `ready` state shows
  as a small spinner glyph at the frame corner while rendering (editor only).

### Agent UX (MCP, Lua, CLI)

- **R64. Lua `TitleArt`** (full API in §6.6):
  - a GameObject subclass, constructed `TitleArt(scene, entity)` over an existing Title Art
    Renderer, or reached with `GameObject:getTitleArt()` (nil without one);
  - getters are properties (`.claude/rules/scripting.md`);
  - bound in `TitleArtClassesLua.cpp` in core, not under `#if JASPERENGINE_TITLE_ART`, so naming
    it never pulls the module (R52).
- **R65. Errors that teach.** An operation the mode cannot do raises a Lua error naming the
  title, what failed, why, and how to fix it (§6.6). For example, `t.text = "WIN"` on a Baked
  title:
  "TitleArt 'Logo': text cannot change on a Baked title (its pixels are the asset's bake). Set
  Render Mode to Realtime (or Glyph Atlas for text that changes every frame) in the Title Art
  Renderer, or bake one Title Art per text and enable the one you need."
  Unknown parameter names list the exposed ones with a "did you mean". Wrong value types name the
  expected type.
- **R66. Only Exposed names.** `setParameter`/`getParameter` take Exposed parameter names only
  (contract decision 13), never document paths. An effect param a script must drive
  (S83 R47's target `{layer: "Title", field: "fx:light_glow.intensity"}`) is exposed under a name
  (S80 `expose` op), and S83
  L params so exposed change through the live compose (R9). This keeps prefabs, clip tracks and
  Lua on one vocabulary, and lets the export know every value a script can change.
- **R67. No structural changes from Lua.** `title` and `renderMode` are read-only, and Lua cannot
  add the component. Every Realtime or Glyph Atlas title therefore exists in shipped content,
  which makes R47's collection and R52's detection exact. `TitleArt` is reviewed into
  `ShaderNeutralApi::neutralClasses()` with its member hash, and `getTitleArt` into
  `GameObject`'s neutral members. A script using `TitleArt` keeps `lite2d` possible (AC14).
- **R68. C++.** `engine/core/object/TitleArt.{h,cpp}` has the same members for C++ scripts and the
  render gate project.
- **R69. MCP `get_title {entity}`** (S80's tool; `entity` is an alternative to `path`, plus
  `scene`, `wait_ready` (bool, default false) and `timeout_seconds` (default 10)). It returns the
  component's state (§6.7). `wait_ready` returns when `ready` (a job past 1 s), so an agent's loop
  is `play` → `eval_lua` (change) → `get_title {entity, wait_ready}` → `screenshot {view:
  "game"}`.
- **R70. MCP `render_title {entity}`** (S80's tool; `entity` is an alternative to `path`):
  - It renders the component's current state, or the state with what-if `overrides` (an object,
    nothing saved), in an isolated editor render. It returns the image the game shows (the
    display target, with live layers at `live_time`, default the component's).
  - `rerender: true` instead invalidates the live instance and returns once it is ready (Play or
    Edit).
  - S83's `options.cost` (S83 R46), with `entity`, adds the instance's measured runtime cost: per
    step CPU and GPU ms (S15 timer queries where available, else estimates, said so), slices,
    frames to ready, peak pool bytes and display bytes. S81 adds no second cost parameter.
  - S80's `fallback: "compare"` renders both paths and returns `pixel_paths_diff {max, mean,
    changed_pct}` against the float path.
  - `overrides`, `rerender` and `live_time` are keys of S80's free-form `render_title.options`
    (S80 §6.11); `entity` and `scene` are top-level parameters.
  - All of it is a job past 1 s, allowed while playing.
- **R71. Discoverability.** `get_title {entity}` also returns:
  - `particles`: the four R39 presets as `create_particles` argument objects for this title;
  - `glyph_atlas`: the unsafe and approximate list (R31) when the mode is Glyph Atlas.

  Component fields are set with `set_properties {component: "titleart", ...}` and
  `add_component`, keys as §6.1 (`overrides.text`, `sweepPosition`). Budget: S81's row of S80
  §6.11's program budget table (≤ 300 characters: `get_title` 220, `render_title` 80).
- **R72. Guide.** `docs/mcp/guide/title-art.md` (S80's file) gets a `### Runtime` section: the
  modes and when to pick each, the Play verification loop, the reserved events, Characters and
  the Lua errors. No line is added to the agent guide (`McpInstructions.cpp`): the "Other areas"
  entry S80 adds points at it.
- **R73. CLI.** `jasperengine-editor call get_title --entity ...` works through S31. `check`
  (S80's `--titles`) adds the R31 and R55 checks (`check_title` ids `glyph_atlas` and
  `characters`, registered in S80's check registry) for documents and their instances in scenes.

## 6. Design constraints & decisions

### 6.1 The component

```cpp
// engine/core/component/TitleArtRendererComponent.h
enum class TitleArtRenderMode : uint8_t { BAKED, REALTIME, GLYPH_ATLAS };
enum class TitleArtTransition : uint8_t { CUT, CROSSFADE };

struct TitleArtOverride {            // one Exposed parameter's instance value (S80 §6.1 types)
    std::string name;                // the exposed name
    std::string type;                // "text" "color" "number" "bool" "font" "image" "color_scheme"
    Vector4 value;                   // number: x; bool: x 0/1; color: linear rgb
    std::string text;                // text; font / image: asset path; color_scheme: scheme id
};

struct JASPERENGINE_API TitleArtRendererComponent {
    bool enabled = true;
    std::string title;               // .titleart path (AssetRef row, GUID tracked)
    TitleArtRenderMode renderMode = TitleArtRenderMode::BAKED;
    std::vector<TitleArtOverride> overrides;   // "overrides.<name>" properties
    // size and placement
    bool pivotFromTitle = true;
    float uiScale = 1.0f;            // Set Native Size, UI
    float pixelsPerUnit = 0.0f;      // Set Native Size, world; 0 = project PPU (S41) else 100
    // Realtime and Glyph Atlas
    float resolutionScale = 1.0f;    // 0.25..2 of the display size
    int priority = 0;                // -100..100
    bool renderWhenHidden = false;
    bool prebakeInitialState = true;
    TitleArtTransition transition = TitleArtTransition::CUT;
    float transitionDuration = 0.15f;  // 0..2 s
    std::string additionalCharacters;
    int atlasPageSize = 1024;        // 512 | 1024 | 2048
    int atlasMaxPages = 4;           // 1..8
    // Realtime 3D (fields S81 owns; S86 R26, R37 define what they do)
    std::string qualityTier = "auto";             // "auto" | "high" | "medium" | "low"; Lua qualityTier
    std::vector<std::string> runtimeEnvironments; // extra S86 environment ids or assets the export cooks
    // Live Effects
    float liveTime = 0.0f;
    bool liveUnscaledTime = false;
    bool sweepEnabled = false;  float sweepPosition = 0.0f;  float sweepWidth = 0.15f;
    float sweepAngle = 25.0f;   float sweepSoftness = 0.6f;  Vector4 sweepColor = {1, 1, 1, 0.8f};
    bool sweepAuto = false;     float sweepSpeed = 1.5f;     float sweepInterval = 2.5f;
    bool glowEnabled = false;   float glowPulse = 0.0f;      Vector4 glowColor = {1, 0.85f, 0.4f, 1};
    bool tintEnabled = false;   Vector4 tintColor = {1, 1, 1, 1};  float tintAmount = 0.0f;
    bool glintsEnabled = false; float glintAmount = 1.0f;   float glintSize = 0.06f;
    float glintRate = 0.5f;     uint32_t glintSeed = 0;      Vector4 glintColor = {1, 1, 1, 1};
    // runtime, never serialized: the system's handle (scheduler entry, targets, hashes, state)
    std::shared_ptr<titleart::TitleArtInstance> runtime;
    bool needUpdate = true;
};
```

Inspector labels (Unity naming): Title Art, Render Mode, Text, Color Scheme, Parameters,
Pivot From Title, UI Scale, Pixels Per Unit, Resolution Scale, Priority, Render When Hidden,
Prebake Initial State, Transition, Transition Duration, Additional Characters, Atlas Page Size,
Atlas Max Pages, Quality Tier, Runtime Environments, Live Effects > Sweep (Position, Width, Angle, Softness, Color, Auto, Speed,
Interval), Glow (Pulse, Color), Tint (Color, Amount), Glints (Amount, Size, Rate, Seed, Color),
Time (Live Time, Unscaled Time). Colours are stored linear and shown sRGB, as `ui.color` is.

**Why a driver component on an Image or Sprite, not a new renderer.** Every UI and sprite feature
works with no new code paths: Canvas Groups, masks, stencil, S16 batching, S52 batching and
sorting, the Frame Debugger, `ui.color` and `recttransform.*` tracks. Unity users already know the
pattern (a component that feeds an Image). The idle-cost requirement R48 holds by construction.

**Why `overrides` is a keyed collection (`overrides.<name>`).** A prefab variant then overrides
`text` alone and keeps following the base's color scheme. The particle exposed-values model (one
opaque vector) cannot do that. `Catalog::splitTitleArtOverrideProperty` mirrors
`splitShaderEntryProperty` (`editor/Catalog.cpp:651`).

### 6.2 The document's `runtime` member (S80 reserved it)

| Member | Type | Default | Notes |
|---|---|---|---|
| `runtime.characters.presets` | [enum] | `["latin"]` | `digits`, `latin`, `latin_extended`, `kana`, `joyo_kanji` |
| `runtime.characters.custom` | string | `""` | |
| `runtime.characters.custom_file` | AssetRef | `null` | A `.txt`/`.csv`/`.json` asset; every code point counts |
| `runtime.characters.document_text` | bool | true | |
| `runtime.glyph_layer` | Id | `""` | The Glyph Atlas text layer; empty = the first target of the exposed `text` |

Adding the member does not bump `TITLE_ART_VERSION` (S80 §6.1 migration rule 1). S80's canonical
writer writes it with every default. The ops are S80's `set` (`runtime.characters.presets`).

### 6.3 What S81 needs from S80's render job

S80 R18 and §6.3 define `TitleRenderJob` (resumable, double-buffered, byte-identical to a
one-shot render) with the members the scheduler drives: `step(StepBudget, StepReport*)` (whole
steps while their estimates fit, at least one), `done()`, `progress()` (R12's 50 % rule),
`nextStepCostUs()` (packing), `splitOversizedSteps(maxUs)` (scissor tiles ≥ 256 × 256),
`shaderKeys()` (R18 readiness, R47 collection), `nodeCosts()`, `error()` and `splits()`. The
scheduler builds the request from the instance: `RenderRequest::overrides` carries the resolved
Exposed values (R5), `keepSplits` the layers S83's live kernels and S85's layer splits read,
`glyphCell` the Glyph Atlas cells (R26), and `pxScale` the resolution; the pixel path comes from
the request's test flags and S80's caps. `TitleArtScheduler` keeps its own state per instance
(`titleart::TitleArtInstance`: request, back and front display targets, hashes).

- **The cost estimate** of a step = output pixels × the pass's `costWeight` (S80 §6.3). The weight
  comes from the S80/S83 pass definition (S83's cost model, in ns per pixel on the desktop reference).
  It is scaled by a platform factor: mobile 6, web 4, desktop 1.
- When S15 GPU timing runs (Development builds, the editor), measured step times replace the
  factor with a running median per pass label. Release builds use the static model. AC17 checks
  that the model keeps frames within budget on the reference devices.
- The job is a flat ordered list of steps, not a graph (do-not-copy list). The bake runs the same
  job to the end in one call.

### 6.4 Inspector layout

```
Title Art Renderer                                            [?] [⋮]
  Title Art            [ logo.titleart                ◎ ]  [Open in Title Designer]
  Render Mode          [ Realtime ▾ ]
                       Realtime: re-renders on change · adds titleart (≈0.5 MiB) · subset 38 KB
  Text                 ☑ [ JACKPOT                         ]
  Color Scheme         ☐ [ Gold ▾ ]
  ▸ Parameters (3)     ☐ Glow Amount [====|----] 0.6 ...
  ▸ Live Effects       ☑ Sweep  ☐ Glow  ☐ Tint  ☑ Glints   Time: Live Time 0.00  ☐ Unscaled
  ▸ Realtime           Resolution Scale 1 · Priority 0 · Render When Hidden ☐ · Prebake ☑ · Transition Cut
  ▸ Characters         Latin + document (97) · Additional Characters [ ] · [Check Characters]
  Status               Ready · last render 9 frames, 21.4 ms GPU (est.) · float · 2.0 MiB
  [Set Native Size] [Add Particles ▾] [Re-render]
```

- The Text and Color Scheme rows show the document's value greyed while not overridden. The
  checkbox overrides, as particle exposed values do.
- Unsupported rows for the mode are disabled with a tooltip, never hidden, so the layout does not
  jump.
- The Glyph Atlas foldout replaces Realtime's foldout and adds Atlas Page Size, Max Pages and the
  R31 warning line.

### 6.5 MCP

Tools (S80's group `title_art`, S80's handlers in `editor/mcp/tools/TitleArtTools*.cpp`). S81 adds
parameters only:

| Tool | Added | Chars (target) |
|---|---|---|
| `get_title` | `entity` (entityRef), `scene` (sceneRef), `wait_ready` (bool), `timeout_seconds` (number 0..120) | ≤ 220 |
| `render_title` | `entity`, `scene`; the keys `overrides`, `rerender`, `live_time` of S80's `options`; S80's `fallback: "compare"` and S83's `options.cost` gain the instance meaning | ≤ 80 |

- `path` stops being required when `entity` is given. The handler refuses both, and refuses
  neither, with `{kind: "bad_args", fix}`.
- `allowedWhilePlaying` is true for both. `render_title {rerender}` is an Edit activity on the
  live instance (no file write, no undo).
- S81's row of S80 §6.11's program budget table: ≤ 300 characters; the group total is that
  table's (7,735 of the default 15,000, 6 of 12 tools). S81 adds no tool, no group and no guide
  bytes.
- Tests assert the group is hidden until `tool_groups` enables it (S80's test covers the tools;
  `test_title_runtime.py` re-checks the new parameters are listed).

### 6.6 Lua API (`TitleArt`, `engine/core/script/binding/TitleArtClassesLua.cpp`)

```lua
---@class TitleArt : GameObject
---@field title string                       -- ro: the .titleart path
---@field renderMode TitleArtRenderMode      -- ro: TitleArtRenderMode.BAKED | REALTIME | GLYPH_ATLAS
---@field text string                        -- rw: the `text` parameter (error on Baked, R65)
---@field colorScheme string                 -- rw: the `color_scheme` parameter's scheme id (error on Baked)
---@field colorSchemes string[]              -- ro: the ids an instance may pick (S80 `options`, else all of S84's)
---@field parameterNames string[]            -- ro: the exposed names
---@field ready boolean                      -- ro: the displayed image is the current state (R18)
---@field pending string                     -- ro: "" | "queued" | "hidden" | "shaders" | "rendering 3/12" | "failed"
---@field failed boolean                     -- ro
---@field error string                       -- ro: the last failure's sentence, "" when none
---@field pixelPath string                   -- ro: "float" | "rgba8" | "" (Baked)
---@field priority integer                   -- rw: -100..100
---@field liveTime number                    -- rw
---@field sweepPosition number               -- rw (also sweepWidth, sweepAngle, sweepSoftness, sweepColor, sweepAuto)
---@field glowPulse number                   -- rw (also glowColor)
---@field tintColor Vector4                  -- rw (also tintAmount)
---@field glintAmount number                 -- rw (also glintSize, glintRate, glintSeed, glintColor)
---@field animations string[]                -- ro: S85's animation names
---@field isPlaying boolean                  -- ro
---@field onReady FunctionSubscribe          -- ()
---@field onFailed FunctionSubscribe         -- (message: string)
---@field onMissingCharacters FunctionSubscribe  -- (chars: string)
---@field onAnimationFinished FunctionSubscribe  -- (name: string)
TitleArt = {}

---@param scene Scene  @param entity integer  @return TitleArt     -- error when the entity has no Title Art Renderer
function TitleArt(scene, entity) end
---@param name string  @param value number|boolean|string|Vector3|Vector4
function TitleArt:setParameter(name, value) end   -- number, bool, "#rrggbb" or Vector3/4 (linear), asset path, scheme id
---@param name string  @return number|boolean|string|Vector4
function TitleArt:getParameter(name) end          -- the effective value (R5)
---@param name string  @return boolean
function TitleArt:hasParameter(name) end
---@param name string
function TitleArt:resetParameter(name) end        -- removes the override
function TitleArt:setColorScheme(id) end          -- = colorScheme = id
function TitleArt:refresh() end                   -- re-render now (Realtime, Glyph Atlas); no-op on Baked
function TitleArt:setNativeSize() end
---@param name string  @param options? {speed?: number, loop?: boolean, unscaled?: boolean}  @return boolean
function TitleArt:play(name, options) end         -- S85; false and a warning for an unknown name
function TitleArt:stop() end
---@param ms number
function TitleArt.setFrameBudget(ms) end          -- static, 0.25..16 (R15)
---@return number
function TitleArt.getFrameBudget() end

---@return TitleArt|nil   -- GameObject member (reviewed neutral, R67)
function GameObject:getTitleArt() end
```

Errors (`luaL_error`, one sentence each; `<T>` is the GameObject name):

| Call | Condition | Message |
|---|---|---|
| `text =`, `setParameter`, `colorScheme =` | Baked | `TitleArt '<T>': <what> cannot change on a Baked title (its pixels are the asset's bake). Set Render Mode to Realtime (or Glyph Atlas for text that changes every frame) in the Title Art Renderer, or bake one Title Art per <what> and enable the one you need.` |
| `text =` | the document exposes no `text` | `TitleArt '<T>': logo.titleart exposes no 'text' parameter. Expose its text layer's text in the Title Designer (Exposed Parameters > Expose Text).` |
| `setParameter`, `getParameter` | unknown name | `TitleArt '<T>': no exposed parameter 'glowamount' (did you mean 'glow_amount'?). Exposed: text, color_scheme, glow_amount.` |
| `setParameter` | wrong type | `TitleArt '<T>': 'glow_amount' is a number; got string.` |
| `setParameter` | number out of range | Clamped to `min..max` with one warning, no error (as `edit_title` clamps) |
| `colorScheme =` | id not allowed | `TitleArt '<T>': color scheme 'rainbw' is not one of gold, silver, neon (did you mean...)` |
| `TitleArt(scene, e)` | no component | `TitleArt: GameObject '<T>' has no Title Art Renderer. Add one in the Inspector (Add Component > Rendering > Title Art Renderer).` |
| any | module absent, Realtime title | Works on values (stored, `ready` stays false, `pending` = "failed", `error` = R51's sentence); no Lua error, so shared scripts keep running |

`text` assignments longer than 1,024 code points are cut with a warning (S80 limit). Setting a
value equal to the current one does nothing (no render).

### 6.7 `get_title {entity}` result

```json
{"entity": "Canvas/Logo", "title": "assets/titles/logo.titleart", "mode": "realtime",
 "space": "ui", "ready": false, "pending": "rendering 5/12", "state_hash": "9c1e…",
 "pixel_path": "float", "failed": false, "error": "",
 "overrides": {"text": "JACKPOT"}, "effective": {"text": "JACKPOT", "color_scheme": "gold", "glow_amount": 0.6},
 "display": {"size": [1024, 512], "bytes": 2097152}, "pool_bytes": 12582912,
 "last_render": {"frames": 9, "gpu_ms_est": 21.4, "gpu_ms": null, "cpu_ms": 3.1, "over_budget_steps": 0},
 "live": {"sweep": true, "glow": false, "tint": false, "glints": true, "live_time": 3.25, "anchors": 9},
 "characters": {"count": 97, "missing": []},
 "glyph_atlas": null,
 "particles": [{"label": "Sparkle", "call": {"tool": "create_particles", "args": {"parent": "Canvas/Logo", "preset": "magic", "...": "..."}}}],
 "module": {"built": true}}
```

### 6.8 Performance budgets

Reference content: `tools/fixtures/title-art-runtime/assets/titles/reference.titleart`. It is a
1024 × 512 canvas with one Text layer "JACKPOT", 200 px, a gradient face, two rims (3 % and 2 %),
a smooth bevel, an inner shadow and an outer glow of 24 px, plus Glow output on. `counter.titleart`
is the same look on "1,234,567" at 128 px.

| Case | Device bar (Mali-G57 / Adreno 610, 1080p) | WebGL2 (Chrome, Intel UHD 620 class) | Desktop (server A, Release, GL) |
|---|---|---|---|
| Idle, any mode, no Live Effect | +0 draws/passes vs an Image; CPU ≤ 10 µs/title | same; ≤ 10 µs | same; ≤ 2 µs |
| Live Effects (all four) | +0 draws/passes; GPU ≤ 0.10 ms at full title coverage | ≤ 0.05 ms | ≤ 0.02 ms |
| Text change, `reference` Realtime | ready ≤ 0.30 s (≤ 18 frames at 60 fps); total GPU ≤ 24 ms; total CPU ≤ 8 ms; no frame exceeds budget + 0.5 ms | ready ≤ 0.30 s; no frame over 17.7 ms | ready ≤ 3 frames; total GPU ≤ 6 ms |
| Colour-only change (geometry cached) | total GPU ≤ 12 ms | ≤ 12 ms | ≤ 3 ms |
| Glyph Atlas update, `counter`, all glyphs cached | layout CPU ≤ 0.05 ms; compose + output pass ≤ 0.15 ms GPU | ≤ 0.10 ms GPU | ≤ 0.03 ms |
| Glyph Atlas new glyph cell (128 px em) | ≤ 3 ms GPU per glyph, one per step | ≤ 3 ms | ≤ 0.6 ms |
| Memory, `reference` Realtime | idle ≤ 2.2 MiB; peak during render ≤ 14 MiB (pool) | same | same |
| Module size | `titleart` ≤ 0.6 MiB (arm64 `.so`, deflated) | ≤ 0.35 MiB (wasm gzip) | measured, reported |
| Core part (always built: S81 + S85 + S87, S80 §6.15) | ≤ 60 KiB stripped arm64 (S81's own ≤ 34) | ≤ 40 KiB gzip (S81's own ≤ 25) | — |

Module sizes are measured with `tools/players/lib_size.py --modules` and
`tools/players/player_sizes.py` as S34 M8 did.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1-R3, R6, R58) | `editor/tests/titleart/TitleArtComponentTest.cpp`: defaults; S08 round trip with defaults omitted; unknown keys kept; the add-to-empty GameObject adds an Image in a UI scene, a Sprite elsewhere, in one undo step; Set Native Size for UI and world (PPU 100 and S41 PPU); pivot from the sidecar origin; removing the component restores the companion's saved texture and shader (red control: skip the restore, the test sees the framebuffer texture left behind) |
| AC2 (R4, R5, R60) | `test_title_runtime.py::test_prefab_overrides`: a prefab with `overrides.text`, a variant overriding only `overrides.color_scheme`; editing the base's text reaches the variant, the variant keeps its scheme; `set_properties {component: "titleart", properties: {"overrides.text": "WIN"}}` undoes in one step; a missing parameter entry survives save (red control: store overrides as one opaque vector, the variant test fails) |
| AC3 (R10, contract decision 1) | `engine/tests/core/titleart/TitleArtRuntimeGpu.RealtimeMatchesBake`: `reference.titleart` rendered through the scheduler (budget 0.5 ms, so it slices) equals the S80 bake of the same state within S80's tolerance, glcore golden, Vulkan matches; red control: let the scheduler skip the cache-cascade check (R16) so a step reads a stale field, the comparison fails |
| AC4 (R12, R13, R15) | `engine/tests/core/titleart/TitleArtSchedulerTest.cpp` (fake `TitleRenderJob` steps, fake clock): no frame runs more than budget except the guaranteed step; priority then visibility then age; hidden titles get no steps; restart under 50 %, finish-then-latest over 50 %; `TITLE_ART_TEST_SLOW_STEPS` splits into tiles; red control: disable coalescing, the "always fresh" test fails |
| AC5 (R14, R17, R18) | `TitleArtRuntimeGpu.DoubleBuffer`: frame by frame readback of the companion's texture during a text change shows the old image on every frame until the swap, then the new one; `onReady` fires once on the swap frame; toggling back to the previous text swaps from cache without a render (step count 0); red control: render into the front target, a mixed frame appears |
| AC6 (R19, R20, R22) | `TitleArtRuntimeGpu.Failures`: `TITLE_ART_TEST_FAIL_TARGETS` keeps the previous image, sets `failed`, retries at 2/4/8 s then stops, `onFailed` once; a simulated GPU object loss re-renders and shows the fallback meanwhile; the pool is released after Release Delay (`get_vram_report` owner rows vanish) |
| AC7 (R21) | `TitleArtRuntimeGpu.ForcedRgba8`: `TITLE_ART_TEST_NO_FLOAT_TARGETS` renders `reference` on the RGBA8 path within S80's float/RGBA8 tolerance of the float golden; `pixelPath` reports `rgba8`; MCP `render_title {entity, fallback: "compare"}` returns `pixel_paths_diff` within it; `JASPER_TITLEART_FORCE_RGBA8=1` in a Linux player run gives the RGBA8 golden |
| AC8 (R11, R32) | `tools/mcp/tests/test_title_export.py` (`JASPER_SLOW_BUILD=1`): a Realtime instance with an override ships a prebaked texture (dedup: two instances, one texture); headless player `--fixed-fps 60 --screenshot` at frame 1 equals the editor's render (`pngdiff --tolerance 24 --max-fraction 0.01`); `capture_profile` shows 0 TitleArt steps before a change; with `prebakeInitialState: false`, frame 1 is transparent and the image appears later (red control) |
| AC9 (R33-R38) | `engine/tests/core/titleart/TitleFxGpu`: sweep at positions 0, 0.5, 1 (0 and 1 show no band); glints at 9 anchors at live times 0 and 1.3; glow pulse 0 and 1; tint; crossfade at 50 %; a title with no Live Effect is byte-identical to an Image with the same texture; two `--fixed-fps` runs of the fixture are pixel-identical (red control: read engine time instead of `liveTime`, the second run differs) |
| AC10 (R36) | `engine/tests/core/titleart/TitleArtRuntimeGpu.Anchors`: a Baked title's glints use the sidecar anchors; a Realtime title with an unchanged analysed layer does no readback (readback counter 0 over 300 frames); after a text change the new anchors equal S83's bake analysis of the same state, arrive on the swap frame and are at most 16; red control: swap the image before the analysis step, the frame shows glints at the old anchors |
| AC11 (R25-R31) | `engine/tests/core/titleart/TitleGlyphAtlasGpu`: `counter.titleart` "1,234,567" vs a Realtime render of the same text within the approximate-class tolerance (rims behind faces: a golden where an overlapping kern pair keeps faces over neighbour rims; red control: draw cells in string order, the golden fails); atlas growth to a second page; LRU eviction; full atlas error text; `warp` skipped with the warning; Dimension 3D falls back to Realtime |
| AC12 (R48) | `test_title_runtime.py::test_idle_cost`: a scene with a Baked, a Realtime and a Glyph Atlas title (idle, no Live Effects) and three Image twins; `get_render_stats` draws, passes, pipelines and bindings applied are equal with and without the components; red control: request a render every frame, passes rise |
| AC13 (R40-R43) | `engine/tests/core/animation/PropertyTracksTest.cpp` new cases: every `titleart.*` row reads and writes; `overrides.<name>` binds Float/Color3/Bool from the document, `has()` false for text; `sweepPosition` keyed in a clip renders the band at the keyed place at `--fixed-fps` frames. `test_title_runtime.py::test_clip_events`: a clip with `titleArtSetText("WIN")` at 0.5 s changes the text; with a child `path`; no "has no handler" warning; on a Baked title one warning with the fix |
| AC14 (R51, R52, R59, R67) | `editor/tests/titleart/TitleArtModuleUsageTest.cpp`: Baked-only content → no `titleart`; one Realtime prefab → `titleart` with evidence; a Lua script naming `TitleArt` keeps `runtimeshaders` out (`ShaderNeutralApi` hash test updated); `tools/ci/test_player_cli.py --only variants` with `tools/fixtures/title-art-baked` (gets `lite2d`, no W-PLAYER-VARIANT) and `tools/fixtures/title-art-runtime` (gets `core`, W-PLAYER-VARIANT names the title); a Compile build with the module off has no `titleart` symbols (`nm` grep for `TitleArtScheduler`) and its HarfBuzz object equals today's (`HB_NO_DRAW`); red control: put the Lua class under `#if JASPERENGINE_TITLE_ART`, the Baked fixture loses `lite2d` |
| AC15 (R50, R53-R57) | `editor/tests/titleart/TitleFontSubsetTest.cpp`: Latin subset of a fixture OFL TTF keeps 95 code points + `vert` closure, shapes identically to the full font for those strings (TextShaper glyph ids map), variable axes kept; Digits/Latin/Kana/Joyo counts (25/95/…/2,136); fsType 0x0002 fails with E-TITLEART-FONT-RESTRICTED and passes with the acknowledgement; 0x0100 ships whole; RFN font renamed; `licenses/THIRD-PARTY-FONTS.txt` content; timing within R54 on server A; `python3 tools/ci/check-notices.py` passes with the list `when` and `harfbuzz-subset` (red control: drop the entry, it fails) |
| AC16 (R55) | `test_title_runtime.py::test_characters`: in Play with simulation on, `t.text = "ÆØ"` on a Latin title draws nothing for them, `onMissingCharacters("ÆØ")` fires once, the Console names "Characters > Custom"; export of a project whose clip sets "ÆØ" warns W-TITLEART-MISSING-GLYPH or W-TITLEART-CHARS; `check --titles` lists the same |
| AC17 (R48, R49, §6.8) | `tools/perf/title_art_bench.py` (5 warm-ups, median and p90, thresholds in `tools/perf/bench-thresholds.json`): desktop rows on server A; the WebGL2 rows through `tools/web/browser_gate.py` timing in headless Chrome on server A (scaled thresholds, documented); the emulator run (SwiftShader) checks "no frame over budget + 0.5 ms" only. Red control: budget 64 ms, the frame bound fails |
| AC18 (§6.8 device rows) | Manual, as S13 AC23: a Mali-G57 or Adreno 610 class Android phone at 1080p, the fixture's `bench` scene, a Development build; `capture_profile frames=300` + `get_gpu_passes capture_id` (or a vendor profiler); results recorded in `docs/mcp/guide/title-art.md` "Runtime costs" with the device name. If no device is available, the stream records it in Deviations with the desktop and emulator figures |
| AC19 (R45-R47) | `tools/web/gates/title-art.json` on `tools/fixtures/title-art-runtime` (console has "[title-art] ready", no GL errors, centre pixel of the title within tolerance); Android render gate: new scene `titleart` in `tools/test-projects/render-gate/main.cpp` (a Realtime and a Glyph Atlas title with sweep and glints at a fixed `liveTime`), golden `golden/titleart.png`, run twice: default and with `debug.jasperengine.titleart_rgba8=1` against the same golden within the gate's tolerance; `tools/build.sh web` and `android` build; warm-up list of the fixture contains the TITLE_ART keys of its start scene |
| AC20 (R64-R68) | `test_title_runtime.py::test_lua`: `eval_lua` in Play: `text`, `setParameter`, `getParameter`, `resetParameter`, `colorScheme`, `ready`, `onReady`, `pending`; each §6.6 error message exactly; `setParameter("Title/fx:outer_glow.size", 3)` is refused listing the exposed names (R66); `renderMode` and `title` are read-only (assignment errors); `python3 -m lua_api doctest` passes the `lua run` blocks added to `docs/mcp/guide/title-art.md` |
| AC21 (R69-R71, §6.5) | `test_title_runtime.py::test_mcp`: `get_title {entity}` shape; `wait_ready` returns ready after an `eval_lua` text change; `screenshot {view: "game"}` after it shows the new text (non-blank title box, differs from before by ≥ 2 %); `render_title {entity, options: {cost: "desktop"}}` has per-step costs; `render_title {entity, options: {overrides: {text: "A"}}}` leaves the instance unchanged; `tool_budget.py --check tools/mcp/budget.json`, `test_tool_conventions.py`, `gen_tool_docs.py --check` pass; the new parameters absent from the default listing until `tool_groups` enables `title_art` |
| AC22 (R60-R63) | `tools/editor-shot.sh` screenshots (isolated editor, `tools/fixtures/title-art-runtime` copy) of the Inspector in each mode, the R31 warning, Characters with a missing character, and the Status line while rendering; `editor/tests/titleart/InspectorTitleArtTest.cpp` (S08 inspector layout) checks the rows, the disabled rows in Baked and the undo names |

## 8. Dependencies & conflicts

**The Title Art program graph and waves** (the same in S80-S87; S80 §8 is the source, S80 §6.15
the shared rules; an arrow means "needs"):

| Wave | Specs | Needs |
|---|---|---|
| 6 (title art) | S80 core | merged work only |
| 6B | S81 runtime, S82 Title Designer, S83 effects and generators, S86 3D | S80 (every hook of S80 §6.15); within the wave, by owner: S81 owns the component, scheduler, live compose, `title_fx`, the module wiring and the 3D and mesher sub-options; S82 owns the window, its §6.8 hooks, `DocRow` and the 3D gizmo; S83 owns the catalogue values of the registry members (glyph safety, live variants, classes, cost model), `Glint3D` and the light encoding; S86 owns 3D layers, `Gizmo3DTarget`, `quality`/`debug_view`, environments and the compressed-light encoding |
| 6C | S84 library, S85 animation, S87 localisation and interchange | S80 and wave 6B; within the wave, by owner: S84 owns `PachislopPort`, `pachislop-map.json`, `TitleLibraryApply` (and `lookFromStyle`), the denied terms; S85 owns sampling, `TitleAnimPlan`, flipbooks and `<file>.anim.json`; S87 owns the sidecar `languages` table and the per-language bake sets |

Edges: S81 → S80. S82 → S80, S81 (Preview As Glyph Atlas, create menus). S83 → S80, S81 (live
compose runs its kernels; sidecar anchors). S86 → S80, S81 (scheduler, sub-options, component
fields), S82 (sections, gizmo), S83 (`Glint3D`). S84 → S80, S82 (Library panel), S83, S86. S85 →
S80, S81, S82, S83 (S86 optional). S87 → S80, S81, S82, S83, S84, S85, S86. One exception crosses a
wave: S82's Library panel milestone (its M6) draws S84's data and lands with S84 in wave 6C; the
rest of S82 lands in 6B.

- **S81 needs from S80** (all in S80 §6.15's hook table): the document codec and Exposed
  parameters; Outputs, the bake and the sidecar it writes (R8); `TitleArtRenderer` and
  `TitleRenderJob` with its scheduling members, `RenderRequest::overrides`, `keepSplits` and
  `glyphCell`; `ShaderType::TITLE_ART`; the float/RGBA8 paths and their tolerance;
  `TitleTextLayout` and `reachBounds`; the registry's `glyphSafety`, `live` and cost members; the
  `title_art` tools with `render_title.options` and `fallback: "compare"`; the check registry.
- **From the same wave (6B):** S83 fills the registry values (glyph safety §6.13, live kernels D10,
  cost weights), runs its analysis into the sidecar's `anchors`, and owns `render_title`'s
  `options.cost`; S86 defines the semantics of the component fields `qualityTier` and
  `runtimeEnvironments` (S81 owns the fields, §6.1) and puts its code behind S81's sub-options;
  S82 hosts R62's Characters UI through its §6.8 `addSection` and uses R6's create functions.
- **Later (6C):** S84 Color Scheme ids for `colorScheme` and the particle colours; S85 the
  animations behind `play`, its rows and component fields, through the ANIM feature (R35); S87
  the per-language values in R5's order, the `language` field and events, and their text in the
  Characters union.
- Existing: S04 (AssetRefs, dependency graph), S05 (hot reload), S06 (jobs, background lane), S08
  (reflection, Inspector), S14 (cook), S15 (timings, VRAM), S20 (particles), S21 /
  anim-properties (rows, events), S24 (undo), S30 / S34 (players, variants, ModuleUsage,
  ShaderNeutralApi), S32 (warm-up, async variants), S41 (PPU, optional).
- **Blocks:** S85's Realtime playback (scheduler, ANIM) and S86's Realtime 3D (scheduler, budget,
  pixel paths, sub-options); S87's Baked language switch (the sidecar reader).
- **Name coordination:**
  - S81 owns the component key `titleart`, the module name `titleart`, the shader names
    `library:title_fx` and `library:title_fx_sprite`, the event names `titleArt*`, the document
    member `runtime` and the sidecar `*.bake.json`.
  - S80 writes the sidecar file in its bake.
- **Merge hotspots:**
  - `engine/core/animation/PropertyBindings.cpp`;
  - `editor/Catalog.{h,cpp}` (ComponentType, the overrides splitter);
  - `engine/core/registry/EntityRegistry.*`, `EntityCloner.cpp`, `Factory.*`, `Stream.*`,
    `RuntimeFields.gen.inc`, `McpValues.cpp`, `ComponentAddDialog.cpp`, `JasperEngine.h`, the
    Xcode project (the `TextEffectComponent` footprint);
  - `editor/window/InspectorWindow.h`;
  - `editor/shader/ShaderVariantCollector.cpp`;
  - `editor/players/ModuleUsage.cpp`, `ShaderNeutralApi.cpp`;
  - `engine/core/player/PlayerModules.cpp`, `engine/player/variants.json`;
  - `engine/CMakeLists.txt`, `engine/libs/harfbuzz/CMakeLists.txt`;
  - `engine/notices.json`, `tools/ci/check-notices.py`, `NOTICE.md`;
  - `editor/Exporter.cpp`, `ExporterPrebuilt.cpp`;
  - `RenderSystem.cpp` (the pre-camera hook), `Scene.cpp` (`registerSystem<TitleArtSystem>()`
    after `FluidSystem`).

## 9. Risks & defaults

- **S83's live kernels and Lua names** (resolved): kernels run in S81's live compose into the
  display target (R9), never inside `title_fx`, so the companion stays one draw; Lua reaches
  effect params only through Exposed names (R66, contract decision 13). S83 D10 and R47 say so.
- **Prebuilt variants and web size.** A lite2d web game with one Realtime title moves to `core`
  (S34 M8: 5.32 instead of 3.91 MiB gzip). Default: no new variant now. Five release runtimes per
  target is S34 D2's decision, and Baked plus live layers covers most titles. M6 measures the
  module. If it is ≤ 0.15 MiB gzip on web, `titleart` joins `lite2d` and `lite3d` (navigation's
  precedent, S34 deviation d), recorded in Deviations. Otherwise a `lite2d`+`titleart` variant
  needs the owner's decision.
- **Cost estimates on WebGL2.** Release builds have no GPU timer, so slicing relies on the static
  model. Default: conservative factors (web 4×), tuned by AC17. A device that exceeds the budget
  only makes the render take more frames; frames stay within budget + one step.
- **Prebaked states multiply with overrides.** Default: deduplicated by state hash. A build with
  more than 64 distinct initial states warns `W-TITLEART-PREBAKE-COUNT` with the total texture
  bytes, and still prebakes them.
- **Glyph Atlas approximations.** Overlapping outer glows add per glyph instead of once. Default:
  accept, report as info (R31), and let S83 tighten classes per effect.
- **OFL Reserved Font Names** (not legal advice). Default: rename subsets of RFN fonts (R57). The
  original licence file and copyright ship in the credits.
- **Font licences that forbid embedding.** Default: the build fails with a fix (use Baked, or
  acknowledge a licence). It never silently ships.
- **Lua literal scan for characters** is best effort. Default: warnings only, plus the Play
  simulation (R55), which catches dynamic strings during testing.
- **A user forks `ui_fx` expecting `title_fx` features.** Default: `title_fx` is separate, and the
  Inspector names the shader it drives.
- **S80's render job API** (resolved): S80 R18 and §6.3 now carry every member §6.3 here uses,
  under S80's names (`step(StepBudget, StepReport*)`).

## Milestones

- **M1** Component, Baked, sidecar read, Inspector, create menus, Lua core, idle cost
  (R1-R8, R48, R58, R60-R61, R64-R68); AC1, AC2, AC12, AC20 (Baked part).
- **M2** Live layers, anchors, Live Time, particles, rows and events (R33-R43); AC9, AC10, AC13.
- **M3** Module, scheduler, Realtime, double buffer, failures, pixel paths, warm-up
  (R9-R24, R45-R47, R51-R52); AC3-AC7, AC19 (part).
- **M4** Glyph Atlas (R25-R32); AC11.
- **M5** Export: subsetting, Characters, fsType, credits, prebake, variants
  (R11, R32, R50, R53-R57, R59); AC8, AC14-AC16.
- **M6** MCP parameters, guide, budgets and device run (R49, R69-R73); AC17, AC18, AC21, AC22,
  AC19 (rest).

## Deviations

1. **Reconciled: S81 no longer defines the `JASPERENGINE_TITLE_ART` option, ThorVG, `hb-draw` or
   Clipper2 conditions** (S80 R40, §6.15 owns them, because S80 lands first and its code sits
   behind them); S81 owns the player wiring, the stub (moved to
   `engine/core/subsystem/TitleArtRuntimeNone.cpp`, outside the module folder) and the `_3D` and
   `_MESHER` sub-options S86 needs.
2. **Reconciled: the always-built target is 60 KiB, not 40 KiB**, shared with S85's
   `engine/core/titleanim/` and S87's `TitleArtLanguage` (S81's own part ≤ 34 KiB).
3. **Reconciled: `force_rgba8` is S80's `fallback: "compare"`, and `overrides`, `rerender`,
   `live_time` are keys of `render_title.options`,** so `render_title` fits the 1,500-character
   tool limit with every spec's parameters (S80 §6.11).
4. **Reconciled: the pool is S80's `TitleTargetPool`** (S81 said `TitleArtTargetPool`).
5. **Reconciled: `title_fx` gains the ANIM feature and texture-rect frames here** (S85's A2), and
   the component gains `qualityTier` and `runtimeEnvironments` (S86), so the wave-6C and same-wave
   specs need nothing new from S81's files.
6. **Implementation: budgets on S56's platform groups.** Frame Budget, Release Delay and Cache
   Budget are per S56 `DevicePlatformGroup` (`QualityResolver::getPlatformGroup()`: Desktop,
   Mobile, Web; the editor's Game view simulation follows it in Play), with the editor's Edit mode
   apart; the §6.3 platform factor is the group's (desktop 1, mobile 6, web 4). S86's `qualityTier`
   "auto" maps from the S56 `DeviceTier` (`titleQualityTierFor`: Low → low, Medium → medium, High and
   Ultra → high). No parallel tier system.
7. **Implementation: the image is lent at draw time.** As `FluidSystem::getMaterialTexture` does,
   `TitleArtSystem::getDisplayTexture` lends the companion its image in RenderSystem's loads and draws
   (`RenderSystem::lentTexture` serves both): the companion's texture field is never written, so it
   is never saved and never becomes a prefab override. Only a Live Effect writes companion fields
   (custom shader, uniforms, shader textures and features); `TitleArtCompanionScope` gives the
   editor's encoders and the exported code the companion's own values while they are driven.
8. **Implementation: Pivot From Title applies with Set Native Size** (and at creation), as an
   undoable edit, instead of driving the pivot every frame (a driven pivot would be saved). Sprites
   have no free pivot: they take the nearest of the nine presets.
9. **Implementation: world sizes.** `SpriteComponent` sizes are whole units and a 2D scene's unit is
   a pixel; S41's project Pixels Per Unit is not in this tree, so Pixels Per Unit 0 means 1 pixel per
   unit (a value > 0 divides the frame).
10. **Implementation: glints stay inside the frame.** A companion's vertices (nine-slice Images,
    sprite rects) are not a plain quad, so the vertex stage does not grow the quad by the star
    radius; the bake frame's padding holds most stars and a larger one is cut at the frame edge.
11. **Implementation: field shapes.** `title` is an `AssetRef` (path and GUID in one value); the
    `overrides` entries save as a list of `{name, type, value, text}` and list as one property per
    entry, `overrides.<name>` (a String for text, font, image and color_scheme; a Float for number
    and bool; a linear Vector4 for color); prefab overrides copy an entry by name
    (`Catalog::copyPropertyValue`), set_properties writes entries by name into the component's YAML.
12. **Implementation: title_fx names.** Its textures are `u_glowTexture` and `u_prevTexture`; ANIM
    packs the opacity in `animAffine0.w`, the flipbook blend in `animAffine1.w`, and `animColor` is
    the flash colour (rgb) and amount (a). The anchors are `anchors[16]`, written through the custom
    block's array members (`anchors[i]` values).
13. **Implementation: Realtime renders the bake's frame.** `RenderRequest::region` is the sidecar's
    frame, so the display image covers the rectangle the PNG covers and the companion's native size
    stays right; the resolution multiplies the main output's own scale. The display scale is the UI
    rect's in reference units (so a build's prebaked state matches any device resolution); a world
    title renders at its canvas pixels.
14. **Implementation: the prebake key leaves out the pixel path** (`TitleArtRuntime::prebakeKeyOf`):
    the export renders each distinct state once and a device of either path finds it at
    `<title>.prebake-<16 hex>.png` beside the document.
15. **Implementation: Realtime's glow output** is S80's Glow content rendered RGBA8 at half the
    resolution (no R8 output format exists in the renderer).
16. **Implementation: CPU stages run as S80's job steps.** The scheduler steps a job one step at a
    time; how a CPU stage uses the job system is S80's renderer's.
17. **Implementation: TITLE_ART_TEST_FAIL_TARGETS** (bit 6) fails the request's first step with
    `out_of_target_memory` in S81's job wrapper (`RendererJob`), and `TITLE_ART_TEST_SLOW_STEPS` (bit
    7) multiplies its estimates by 64; neither touches S80's pool.
18. **Implementation: `render_title {entity}`** renders the instance's state (or a what-if) through
    S80's render sheet, cut to the bake frame, and answers `live_layers_composited: false`: the live
    layers draw in the companion only.
19. **Implementation: Realtime anchors** are the sidecar's (the bake's analysis) until S83's runtime
    analysis lands in the tree; the render job does no readback.
20. **Implementation: hot reload** goes through `TitleArtSystem::notifyTitleChanged`, called by the
    editor's document change hook (commits, undo, outside edits, assets a document reads) and by
    every finished bake (the sidecar and PNGs).
21. **Implementation: the web gate's project** is `tools/test-projects/title-art` (a C++ test project,
    as every gate of `tools/web/gates/` is: `WEB_PROJECT` names a directory of `tools/test-projects`),
    not `tools/fixtures/title-art-runtime`. Its document is a plate shape (no font, so the gate needs
    no font file); the gate checks two ready lines (the second after an Exposed colour changes from
    C++) and the centre pixel in the second colour.
22. **Open: the Android render gate's golden and the device run (AC18).** The gate's `titleart`
    scene draws a Realtime and a Glyph Atlas title of a plate (the Glyph Atlas document has no
    exposed text layer, so it falls back to Realtime, R25) with the sweep at a fixed position and the
    tint; no glints, as the gate's document has no bake sidecar to take anchors from. The render-gate
    APK builds (`ANDROID_PROJECT=render-gate tools/build.sh android`), but `golden/titleart.png` is not
    in the tree and AC18's device figures are not recorded: the build host has no KVM (no
    `/dev/kvm`, no vmx or svm CPU flag), so the emulator cannot run, and no phone is attached. The
    first emulator run (`tools/android-emulator-test.sh --scene titleart`, default and with
    `debug.jasperengine.titleart_rgba8=1`) makes the golden; AC18 stays a manual device run.
23. **Implementation: Glyph Atlas** (`titleart/runtime/TitleGlyphAtlas.*`, R25-R32), complete
    with these choices:
    - Cells are S80's `glyphCell` renders (back: rims and the layer's effects; front: the face) of
      a cell document (unsafe members made safe), keyed by the cluster's text (a cluster renders
      as a glyph of the same chain, rather than by glyph index and face), the style (the
      document's content hash, the instance's values other than the text, the resolution), the
      character's and its line's adjustments, and the sub-pixel phase.
    - Per-character looks (`chars.<index>`: scale, h/v scale, rotation, `face` material) and the
      line's (`lines.<index>`) render in the cell: the cell document carries them as the single
      glyph's (`chars."0"`, `lines."0"`), one cell document per distinct adjustment; offsets move
      the glyph like the string does.
    - Phase: a cell renders at the 1/32-pixel phase the string puts its glyph at (its character
      offset moved by that fraction; moving the layer would resample it), so its quad moves by
      whole pixels. S80 centres a text by its ink, so each value moves the string by a fraction of
      a pixel: Glyph Atlas snaps the string (its first pen) to the pixel grid, under half a pixel
      from Realtime's place, so a counter's glyphs keep their phases and cells from value to
      value. AC11's golden compares with the Realtime render of the same text at that snap (every
      character's offset; mean <= 1/255, at most 1 % of pixels over 8/255).
    - A cell keeps only the pixels its content and reach can cover (S80's planner now records
      that rectangle, `TitleImage::reach`, beside its 64-pixel ROI grid). Both parts of a glyph
      share one atlas rectangle (one page per glyph). LRU eviction frees whole pages (the least
      recently used page no shown or wanted string uses), not single cells.
    - The compose uses S80's passes and a new renderer program `GLYPH_COMPOSE` (42): up to 20
      quads per pass from one page. A text change is a clear, the back plate copy, one pass per 20
      quads, the front plate copy and the output pass. A text layer with an opacity (its own and
      its groups') or a non-normal blend composes its glyphs into their own target, then onto the
      back plate (a copy with the opacity, or S80's blend pass over a backdrop copy: +2 passes).
      The front plate renders with a transparent canvas background.
    - R32: the export renders each Glyph Atlas instance's initial text and every digit at every
      digit place of it (a counter's every value of that length; at most 1,024 cells), reads the
      pages and plates back and ships them beside the title (`<title>.glyphs-<style>.json`,
      `-p<n>.png`, `-plate<n>.png`; RGBA8 linear premultiplied, not cooked). A player's first job
      of that style restores them and renders no cell they hold. Because cells are per phase and
      place, "every glyph of the Characters" is not prebaked: characters outside the initial text's
      digit places render on the device on first use, one per step. Prebaked plates are 8-bit
      linear (a few codes in the darkest colours against the float plates). The export now keys
      every prebake (R11 and R32) by the build's copy of the document, after the fonts' relink,
      as the player hashes it. The prebake's driver, files and restore are tested in the core
      tests on the headless GPU; the export's glue (`writeTitleArtPrebakes`: the shipped
      document's hash, the paths) builds but is not run by a unit test (the editor test binary
      has no GPU context).
24. **Implementation: the Digits preset has 24 code points** (the explicit list; the spec said 25),
    and the test asserts 24.
25. **Implementation: the Reserved Font Name rename** (R57) also covers name IDs 3, 18, 21 and 25;
    PostScript names (IDs 6, 25) become `TA-<hex>` (no spaces allowed); names inside CFF stay.
26. **Implementation: the Characters check's Lua literal scan** has its own lexer (ModuleUsage's is
    local to its file and does not decode escapes); the check warns only for characters given one by
    one (Custom, Custom File, texts exposed text reaches); presets' gaps are counted
    (`preset_gaps`), not warned. `check-notices.py` gained `NESTED_LIBRARIES` for the
    `harfbuzz-subset` entry, and `editor/publish/Notices.cpp` reads a list `when` (ThorVG ships with
    htmlui or titleart).
27. **Implementation: where subsets go.** The export writes a font's subset next to it as
    `<stem>.subset-<font hash 8><characters hash 8><ext>` and points the build's copies of the
    runtime documents at it; the original leaves the build only when the asset index shows nothing
    but Title Art documents referencing it (a Text component or a page keeps it). A font collection
    (.ttc/.otc) ships whole (W-TITLEART-FONT-COLLECTION): its subset would be one face and the
    documents' face indices would change. An Exposed parameter of type `font` overridden on an
    instance is not followed: only the fonts the document names are subset.
28. **Implementation: the AC17 bench** is the core test `TitleArtRuntimeBench.HostNumbers`
    (5 warm-ups, then median and p90 of 30 text changes), not `tools/perf/title_art_bench.py` with
    `bench-thresholds.json`; there is no WebGL2 timing through `browser_gate.py`. It prints
    wall-clock frame times on whatever device runs it and checks only what holds on any device (a
    cached Glyph Atlas change costs less than a Realtime render; a render is sliced over frames).
    The "budget + 0.5 ms" frame bound is the scheduler's estimate, tested in
    `TitleArtSchedulerTest`; on a software rasterizer the measured frames exceed it. Host run
    (this build host: llvmpipe, no GPU, float path; a counter of 4 tabular digits at 96 px on a
    640 x 160 canvas; 5 warm-ups, 30 samples): Realtime text change (one unsliced render) median
    72.7 ms, p90 79.5 ms; Glyph Atlas change with every glyph cached (22 of 30 samples) median
    5.3 ms, p90 6.1 ms; Glyph Atlas over all samples (new cells on first use) median 5.5 ms, p90
    51.7 ms; a Realtime render sliced at a 4 ms budget took 3 frames, the largest multi-step frame
    measuring 81.3 ms (the estimates are desktop-GPU figures; llvmpipe runs the passes on the CPU).
    These are host measurements, not device figures (AC18 is open, Deviation 22).
29. **Implementation (W1 wiring): the live compose** (R9, `titleart/runtime/TitleLiveCompose.*`,
    `TitleArtLive::runLive`). A full render of a Realtime document with Exposed parameters whose
    every target is an L param of a live variant keeps, per affected layer, a copy of its content and
    of the stack input of the live effect (`RenderRequest::keepLive`, `TitleSplits::live`), and per
    following generator the cut of its followed layer; those layers and their ancestors are always
    planned, never node-cache hits. A later state whose only difference is such values (nothing else
    of the render: resolution, region, path, tiers, flags) plans the same request with
    `RenderRequest::liveInputs`: the stack starts at the live effect, which runs its passes again
    (its alpha-only results, the scale_split's glow and shadow blurs, come from S80's alpha-keyed
    cache) followed by the rest of its stack, the composites above it and the output; the other
    layers are node-cache hits. A live param that changes the stack's reach re-places the inputs in
    the new region (a clear and a copy). The compose runs whole in its frame before the slices
    (no cost-model gate), cuts to the new image and does not cache the old one. A live compose keeps
    the render's inputs on the new image and records what it touched (`TitleDisplayImage::liveTouched`):
    an input that a later change would read after an earlier live change altered it (an effect before
    it, a layer inside it, the layer a generator follows) re-renders, which keeps fresh ones. The first
    enabled effect of a stack reads the kept content itself, so its alpha-keyed results stay valid
    (a colour-like change runs no blur or distance field). The live budget is
    predicted with S83's §6.4 model on the device's tier (a scale_split one full-resolution
    equivalent, a kernel its estimate), the same function check_title's `fx_live_budget` uses, so
    the decision is the same on every device; the scheduler's measured costs (Deviation 30) apply to
    the sliced renders. A live effect followed by an enabled effect without a live variant
    re-renders (S83 §6.12's suffix rule) and `get_title`'s `live.fallback` gives the reason; so does
    a change over the budget. Glyph Atlas titles, titles with a glow output (Glow Pulse) and part
    effect stacks re-render. The kernels are the bake programs themselves: S83's `TFX_LIVE` is
    defined but no pass family's source tests it, so a 'Liv' variant would be the same program
    compiled twice (S83 Deviation 39).
30. **Implementation: one scheduler, measured costs** (R15, S80 Deviation 54). The runtime scheduler
    keeps one `TitleStepCosts` per engine whose prior is the platform factor (desktop 1, web 4,
    mobile 6): its first measurements replace the factor, so steps are packed and split
    (`resplit`, once a frame) by what this device measures; on a software GPU steps wait for the GPU
    fence as the editor's previews do. A new platform group (the Game view's simulation) or device
    starts a new model. The editor's previews keep their own model and 4 ms budget; the runtime keeps
    R15's budgets. `TitleFrameReport::lateStarts` counts steps after a frame's first that took the
    frame over its budget. `TitleArtSchedulerTest.MeasuredCostsKeepASlowDevicesFramesWithinBudget`
    (a fake clock: a device 3-40x slower than the estimates per kind of step) has 0 late starts and
    converging predictions; the estimate-trusting packing over the same steps starts late (its control).
31. **Implementation: device tiers** (S83 D11, D12, §6.7; S86 R37; S56). `TitleArtRuntime::setState`
    resolves both tiers into the state: S83's `FxTier::Mobile` on S56's Mobile and Web platform
    groups and on a device of S56's Low tier, else Desktop; S86's tier through `resolveTitleTier`
    (the component's Quality Tier, else S56's device tier, else the project tier, GLES3/WebGL2
    clamping High to Medium). The tree has no `title_art.realtime_3d_quality` setting, so the project
    tier is S86's per-platform default (the simulated group's: desktop High, mobile and web Low).
    Both are in the state hash (a new tier renders again), not in the prebake key (either tier shows
    the export's image). Glyph Atlas cells are keyed by style without the tier.
32. **Tests: AC17's pass count.** `TitleFxLiveGpu.EveryLiveParamComposesLikeARerender` checks each live
    frame against a re-render at the value (F0 / F1), no re-render, no readback, composed in its
    frame, and that it runs fewer passes than the re-render (the content and the effects before the
    live one, or the followed layer, do not run; a generator that follows nothing runs no more than
    a re-render): the frame also composites the layers above the effect, so its count is the
    kernel's plus those, not the kernel's alone.
33. **Tests: AC27 is an MCP test** (`test_title_runtime.py::test_lua_live_effect_params`, AC20's
    harness) rather than a `TitleArtLuaFxTest` binary: Lua's `setParameter` in Play reaches the live
    compose through the component, the system and the scheduler, which a unit test would stub.
34. **Bug fixed on the way: the node cache ignored a generator's params.** `LayerHashes::content`,
    the node cache's key, did not include a generator layer's `generator` member (its params hash in
    the node scope with the placement), so a new param value hit the old pixels in any renderer that
    had drawn the layer. `content` now chains the member (`TitleArtHashTest.AGeneratorsParamsChangeItsNodeCacheKey`);
    the document's hash, and so bake freshness, is unchanged.
35. **Tests: R32's export path** is `test_title_export.py` (JASPER_SLOW_BUILD=1, the prebuilt glcore
    player): the build holds the atlas manifest and pages, and the player logs, once per atlas,
    `the glyph atlas showed its first string after N cell renders (M cells from the build's
    prebaked atlas)`; the test reads N = 0.
