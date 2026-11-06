# S80 — Title Art core: the `.titleart` document, its renderer, text, core effects, bake and MCP tools

**Wave / Depends on.** Title Art program, **wave 6 (title art)**: the first spec, alone in its
wave; S81-S87 build on it (program graph and waves in §8, program rules in §6.15). Depends only on
merged work: S03 (atomic writes, migrations), S04 (GUIDs, dependency graph, Missing Assets), S05
(file watcher, hot reload), S06 (jobs), S14 (texture cooks of the baked PNGs), S24 (named undo,
agent attribution) and S31 (`call` CLI, tool groups, budget). S80 is the document, renderer, bake,
the hooks every later title spec needs (§6.15) and the agent surface; the Title Designer window
(S82) follows in wave 6B. Owner area:
- `engine/core/titleart/` (document codec and migrations, member schema, effect/generator registry,
  text and geometry, renderer), behind `JASPERENGINE_TITLE_ART`, and the module's CMake
  (`engine/core/titleart/TitleArtSources.cmake`, §6.15);
- `engine/shaderlib/titleart.vert`, `titleart.frag`, `engine/shaderlib/titleart/*.glsl`;
- `engine/libs/clipper2/`;
- `editor/titleart/` root files (document, ops, bake, checks, `TitlePreviewQueue`),
  `editor/mcp/tools/TitleArtTools.cpp`, `editor/mcp/tools/ImageDiffTools.cpp`,
  `editor/window/InspectorWindowTitleArt.cpp`;
- `docs/title-art.md`, `docs/mcp/guide/title-art.md` (the file and its section plan, §6.11; each
  later spec writes its own section), `docs/mcp/guide/title-art-effects.md` (generated).

## 1. Goal

A project can hold **Title Art**: a `.titleart` document of layers (text, shapes, images, SVG,
groups, generators, adjustment layers) with stacked rims, bevels, metal, glass and glow, in any
style from chrome logos and neon signs to brush-kanji pachislot titles. One renderer in
`engine/core/titleart`, a port of Pachislop's compositor and 2D title passes made cheaper (regions
of interest, bounded distance fields, cached fields), draws it in the editor and, through S81, in
games. The editor bakes it to ordinary textures that ship on all six targets at zero runtime cost,
and reports when a bake is stale. Agents read, edit, render, check and bake titles through six
MCP tools with the same operations the Title Designer uses, and see real alpha, before/after
diffs and pass/fail checks with ready fixes.

## 2. Scope

**In scope**
- The `.titleart` format v1 (§6.1), its canonical codec, lenient reader, unknown-data
  preservation and a JSON migration framework.
- The asset type: AssetType, AssetTypes row, GUID in the file, dependency edges to fonts,
  images, SVGs, reflection maps and baked PNGs, Missing Assets, rename and GUID relink, hot reload,
  Assets > Create > Title Art, exclusion from exports, `.ttc` as a Font asset.
- The document operations API (`TitleArtOps`, §6.2), shared by every editor surface and MCP.
- The shared renderer (§6.3-6.7): draw tree, pass order, target pool, regions of interest,
  content-hash caches, RGBA16F with an RGBA8 fallback, the 18 blend modes, clipping, masks,
  adjustment layers, ink vs light, time-sliced stepping, device-loss recovery.
- Text and fonts core (§6.6): shaping over the fallback chain, glyph outlines through HarfBuzz
  `hb-draw` with variable-font axes, `.ttf`/`.otf`/`.ttc`, vertical Japanese layout,
  tate-chu-yoko, proportional spacing, tracking, leading, kerning, per-character and per-line
  transforms, Patchy warp, free distort, weight, missing-glyph reporting. Shape, Vector (SVG) and
  Image layers.
- The 2D title passes (§6.7) ported from Pachislop: coverage masks (ThorVG fill and mitred stroke),
  counter filling, plates (Clipper2), jump-flood distance fields with step counts bounded by
  the reach actually needed, Fit bevels, heights and profiles, element shading (bevel light, gloss
  contours, refraction, reflection maps, gradient-map finish, patterns, satin, textures, sheen,
  inner glow, gloss cap, glass), knockout, erode, placement.
- Ten core effects (Outer Glow, Inner Glow, Drop Shadow, Inner Shadow, Outline, Color Overlay,
  Gradient Overlay, Pattern Overlay, Gaussian Blur, Adjust), the fourteen adjustment operations,
  two generators (Fill, Pattern Fill), and the declarative effect/generator registry (§6.5).
- Bake (§6.9): outputs, shared frame, recipe chunk, the bake sidecar `<file>.bake.json`, stale
  detection, Auto Bake, export behaviour, `check --titles`, the CLI through
  `jasperengine-editor call`.
- The program hooks later specs build on (§6.15): the member schema with animation classes, the
  render job's scheduling members, field overrides, kept layer splits and glyph cells, depth and
  MSAA keys in the target pool, the shared preview queue, the light and max blend modes, reserved
  document members, ops and tool parameters, and the program's module, folder, library and budget
  rules.
- Float readback (RGBA16F, RGBA32F) on GL core, Vulkan, D3D11 and Metal (§6.10).
- MCP (§6.11-6.12): group `title_art` with `list_title_library`, `get_title`, `edit_title`,
  `render_title`, `check_title`, `export_title`; generic `diff_images` in `editor_tools`;
  `docs/mcp/guide/title-art.md`; `view_asset` and `get_asset_info` on `.titleart`.
- The minimal editor surface (§6.13): Project window type, icon and thumbnail; the Inspector
  summary of a selected `.titleart` with Open in Title Designer, outputs and Bake; two project
  settings.
- Tests: unit, MCP, goldens against our references and against Pachislop's references with a
  tolerance, both render paths, the forced RGBA8 path, red controls (§7).

**Out of scope** (and where it goes)
- The Title Designer window, its canvas, gizmo, layer panel, live search, style-lint UI, font
  pickers and `.woff`/`.woff2` import: S82.
- The Title Art Renderer component, Render Mode (Baked / Realtime / Glyph Atlas), the Lua
  `TitleArt` API, live layers, clip property tracks, the player module wiring (`PlayerModules`,
  variants, `ModuleUsage`), export of `.titleart` and fonts for Realtime titles, font subsetting:
  S81. S80 adds the CMake option and the code behind it; S81 decides when a game links it.
- The rest of the effect and generator catalogue (Light Glow, glints, fire, lightning, shatter...):
  S83. Looks, Color Schemes, Templates, the Pachislop converter, the font pack: S84
  (`apply_look`, `apply_color_scheme`, `apply_template` are reserved op names here). Time axis:
  S85. Dimension 3D: S86. Languages, PSD/ASL import, PSD/SVG export: S87. S80 reserves their names
  and provides their hooks (§6.15) but implements none of them.
- Pachislop's interactive similarity-move cache, enlarged frames and lit-edge probes: they exist
  because its targets are full-frame; regions of interest and cached fields replace them (§6.3).
- Print features (ICC conversion, CMYK, mm sizes, bleed): games do not need them (S87 keeps only a
  `dpi` tag for PNG and PSD exports and generates its own sRGB profile). The Japan Color 2011 ICC
  profile is never taken.
- Copying the do-not-copy list's render graph: the renderer is an immediate pass list, as
  `docs/specs/README.md` requires.

## 3. Current state (worktree at 3ffafdfaa, read, not run)

- **No title art exists.** There is no `engine/core/titleart/`, no `editor/titleart/`, no
  `.titleart` row in `editor/asset/AssetTypes.cpp:23-56` and no `AssetType` for it
  (`engine/core/asset/AssetType.h:15-40`, last value `VertexAnimation = 23`). Trap:
  `assetTypeFromName` loops up to `AssetType::VertexAnimation` by name (`AssetType.h:73-81`), so a
  new value must move that bound too.
- **Fonts:** the Font row lists `.ttf` and `.otf` only (`AssetTypes.cpp:32`). `FontFace` loads a
  collection member by `faceIndex` and keeps one shared, immutable `hb_font_t`
  (`engine/core/util/FontFace.h`). `TextShaper` shapes horizontal runs over a fallback chain
  with kerning, letter and word spacing and bidi (`engine/core/util/TextShaper.h`); it has no
  vertical direction, no feature list and no variation coordinates.
- **HarfBuzz is built with `HB_NO_DRAW`** (`engine/libs/harfbuzz/CMakeLists.txt`): outline
  extraction is compiled out, though `hb-draw.{h,cc}` are vendored. The subsetter sources are not.
  Glyphs are rasterized by stb_truetype; the only outline user is the HTML UI's MSDF path
  (`stbtt_GetGlyphShape`, `engine/core/htmlui/RmlFontMsdf.cpp`).
- **Vector:** ThorVG (with `strokeJoin` and `strokeMiterlimit`, `engine/libs/thorvg/inc/thorvg.h:1467-1478`)
  is linked only with `JASPERENGINE_HTMLUI` (`engine/CMakeLists.txt:1129-1134`); LunaSVG always.
  No polygon offset or boolean library is vendored.
- **Render targets:** `ColorFormat` has `RGBA16F`, `RGBA32F`, `R32F`, `RG16F`, `R16F`, `RGBA`,
  `RED` (`engine/core/render/Render.h:379-400`). `RenderFeatures` reports render, blend and filter
  per float format and `originTopLeft` (false on GL) (`engine/core/render/SystemRender.h:24-39`).
  Float rendering needs `EXT_color_buffer_float` on GLES3/WebGL2. The precedent for a
  self-contained multi-pass module with a float fallback is `FluidSimulator`: `Caps` with test
  overrides (`engine/core/fluid/FluidSimulator.cpp:97-115`, flags `FLUID_TEST_*`,
  `FluidSimulator.h:51`), dynamic pipelines (`PipelineState`, `engine/core/render/PipelineState.h:60`;
  `ObjectRender::endLoadDynamic`, `beginDraw`), one `ShaderType` with the pass in the property
  bits (`ShaderType::FLUID`, `Render.h:172`; the enum is append-only), and the memory-row
  convention (`engine/shaderlib/fluid.frag:11-14`: `gl_FragCoord` row 0 is uv.y 0 on every
  backend, ping-pong passes never flip).
- **Blending:** fixed-function modes are alpha, premultiplied, additive, multiply and a few
  premultiplied variants (`Render.h:47-59`). The 18 Photoshop modes exist only as shader maths in
  `engine/shaderlib/effects/lib/fam_sim_blend.glsl` (MIT, shaders.com; straight-alpha inputs and an
  "over" tail).
- **Readback:** `FrameReadback::readTexture` takes `bytesPerPixel` 4 or 1 only, synchronous, on
  all five backends (`engine/core/render/FrameReadback.h`). Float targets read wrong or fail today
  (report e1 §1.3).
- **PNG and recipes:** `editor/util/PngWriter.h` has `encodePng16` (RGBA16 in memory) and
  `writePng`. `editor/texsynth/SynthesisRecipe.h:52-145` puts a JSON recipe in an iTXt chunk
  (`kRecipeKeyword = "jasper.synthesis"`, `insertRecipeChunk`, `writeFilesAllOrNothing`, readers)
  and `ProjectChecks` reports stale synthesized PNGs (`editor/util/ProjectChecks.h:196-213`;
  `check --textures`, `editor/cli/CommandLine.cpp:2129-2147`).
- **Asset documents:** `.controller`/`.mask`/`.montage` are canonical JSON with `format`, `version`
  and `guid`, lenient readers that keep unknown members (`engine/core/animation/AnimatorAssetIO.h`),
  edited through `AnimatorAssetDocument::commit` (whole-file `WriteFilesCmd` on the project history,
  merge keys for drags, reload on `filesChanged`, `relinkGuids`;
  `editor/animation/AnimatorAssetDocument.h`). `StreamMigrations` migrates YAML documents only
  (`editor/StreamMigrations.h`); JSON assets carry their own version handling.
- **Dependency graph:** `AssetIndex::isReferrerType` (`editor/asset/AssetIndex.cpp:160-171`) and
  `extractFile` (JSON precedents `.particles` and `.controller` at `:283-338`, edges of kind
  `Other` with a field locator); `DepEdgeKind` (`editor/asset/DepEdge.h:25-43`);
  `MissingAssetTracker` (`editor/asset/MissingAssetTracker.h`).
- **Export:** `copyAssets` ships every file of the assets directory not skipped
  (`editor/Exporter.cpp:2803-2810` skips support files, metas and cooked texture sources).
- **Editor:** `ProjectWindow::FileType` (`editor/window/ProjectWindow.h:37-58`) gets thumbnails
  for images, materials, models, prefabs and pose animations; the Inspector draws a selected asset
  through `drawInspectedAsset` (`editor/window/InspectorWindow.cpp:12392`); offscreen preview
  renderers live in `editor/render/preview/` (`PoseRender`, `VatPreviewRender`...).
- **MCP:** no `title_art` group (`editor/mcp/McpToolGroups.cpp:18-36`); `editor_tools` holds 5 tools
  (`docs/mcp/TOOLS.md:7`). `view_asset` and `get_asset_info` (`editor/mcp/tools/AssetTools.cpp:3798`,
  `:3815`) know images, models, materials, prefabs and pose animations. Image comparison exists only
  for tests: `tools/mcp/pngdiff.py` (`compare`, straight RGBA, `:225`) and
  `engine/tests/support/ImageCompare.h` (thresholds, blocks, histograms). The agent guide was
  6,241 bytes after `fe3e90b97` (its message, measured), against a 6,692-byte ceiling (8,192 minus
  the 1,500 headroom, `tools/mcp/budget.json`).

## 4. Reference

Pachislop (`/tmp/pachislop-analysis/repo`, commit `1c32502`; by SafEight, used with the author's
permission). Port freely, TypeScript/GLSL ES to C++/GLSL 450; ported files keep a provenance header
and `NOTICE.md` gets one entry for Pachislop and one per third-party part.

| Pachislop | What S80 takes | What S80 changes |
|---|---|---|
| `src/model/types.ts`, `defaults.ts`, `gradient.ts`, `parts.ts`, `links.ts`, `equal.ts` | The document model, defaults, part keys, material links | snake_case keys, a real tree (no flat array + `parentId` + contiguity invariant), one id space, every member written, y-up kept, Vec2 arrays |
| `src/io/project.ts`, `src/io/migrate/*`, `migrateText.ts` | The migration rules: idempotent, fill the old behaviour, detect by shape where possible | An ordered `vN -> vN+1` table (not one growing function), fixtures per version |
| `src/engine/drawTree.ts` | The pure visibility pre-pass: clip bases, hidden subtrees, guides | Solo is view state, not document data |
| `src/engine/compositor.ts`, `effectsRun.ts`, `masks.ts`, `adjustments.ts`, `layerSource.ts`, `shaders.ts` (`blendMaterial`, `adjustMaterial`, `outputMaterial`) | Pass order, clip groups to content alpha, mask stack, adjustment scope and mixing, ink vs light, layer sources with cycle guard, output dither | Regions of interest and scissors instead of full-frame targets; Normal and Add by fixed-function blending into the parent; RAII target handles instead of manual release; 18 blend modes instead of 5 |
| `src/engine/passkit.ts`, `frame.ts`, `readback.ts` | The pixel contract | One pool keyed by (width, height, format), no depth buffers on 2D targets, hashes computed once per commit instead of `JSON.stringify` per frame |
| `src/engine/layer2d/Title2DRenderer.ts`, `materialPass.ts`, `outline2d.ts`, `contours.ts`, `images.ts`, `reflectionMaps.ts`, `shaders.ts` | Frames, masks, counter fill, seeds, JFA, Fit bevel, height, satin, span maps, element shading, knockout, erode, placement | JFA steps bounded by the needed reach (report p2 §1.5, §6.5); blur loops sized by radius instead of a fixed −96..96 loop; ThorVG instead of Canvas2D |
| `src/engine/patterns.ts` | The 15 procedural patterns and their analytic AA | none |
| `src/text/layout.ts`, `warp.ts` (Patchy), `distort.ts`, `remap.ts`, `offset.ts`, `plate.ts`, `shapes.ts`, `ink.ts`, `trace.ts` | Layout rules (tsume, vertical, TCY, per-char transforms, frame vs bounds), warp, distort, refit, weight, plates, shapes | Real shaping (HarfBuzz) instead of one glyph per code point; fallback fonts; variable axes |
| `src/effects/a/glow.ts` (`outerGlow`, `innerGlow`), `shadow.ts` (`dropShadow`, `innerShadow`), `finish.ts` (`keyline`), `stylize.ts` (`gradientOverlay`), `blur.ts` (`gaussianBlur`), `color.ts` (`adjust`), `globalLight.ts`, `lib.ts` | The ten core effects' params, defaults and kernels | `keyline` becomes Outline; Color Overlay and Pattern Overlay are new |
| `src/generators/pattern.ts` | `patternFill` | |
| `src/engine/contracts.ts` (`ParamDef`, `EffectDef`, `GeneratorDef`) | The declarative definition: params with units and `when`, `reach`, `gather`, `fillsComp`, `addsLight`, `outline` | One C++ table drives inspector, MCP, docs and goldens (§6.5) |
| `tests/golden/*`, `tests/harness/cases.ts`, `tests/golden/refs/{effect,scene}` | Registry-driven cases, render twice, premultiplied compare, the reference images | Our references per backend; Pachislop's as cross-implementation references with a looser tolerance (§7) |

Third-party parts inside Pachislop, credited in `NOTICE.md`: Patchy warp (MIT, Seth A. Robinson,
`src/text/warp.ts`; ported from upstream `shabanmohd/patchy`, its header kept). Inigo Quilez's SDF
functions are credited where S83 and S86 port them (`particles.ts`, `three/materials.ts`); S80's
files use none. New vendored library: **Clipper2** (Angus Johnson, Boost Software License 1.0) in
`engine/libs/clipper2` with its licence file, an `engine/notices.json` entry (shipped with
`JASPERENGINE_TITLE_ART`) and a `NOTICE.md` row. The test font is Dela Gothic One (OFL 1.1) from
`google/fonts` (`ofl/delagothicone`, the font Pachislop's references were drawn with), with its OFL
file and a `NOTICE.md` row, never copied out of the Pachislop repository.

Engine precedents to copy: `AnimatorAssetIO`/`AnimatorAssetDocument` (codec, document, undo,
relink); `FluidSimulator` (module, caps, fallback flags, dynamic pipelines, `ShaderType`);
`SynthesisRecipe` and `ProjectChecks` (recipe chunk, staleness, `check`); `VatAssetFiles` (a
document and its generated sibling files renamed together); `PoseRender` and `MaterialRender`
(offscreen preview renders, `isPending` loops); `edit_pose_animation` and `edit_graph` (all-or-
nothing ops, did-you-mean errors); `synthesize_texture` (preview, then the same arguments write).

## 5. Requirements

### Functional — document and asset

- **R1. Format.** `.titleart` v1 exactly as §6.1, through `TitleArtIO` (`parseTitleArt`,
  `writeTitleArt`, `normalizeTitleArt`): canonical text, lenient read with JSON-path warnings,
  unknown members and unknown types preserved through any number of edits and saves.
- **R2. Migrations.** `TitleArtMigrations` runs the ordered table before decode; a newer version is
  refused for editing and rendering with `version_newer`; older versions load and are rewritten only
  by the next commit (§6.1 rules 1-4).
- **R3. Asset type.** `AssetType::TitleArt = 29` (appended after S37's `SoundEvent = 28`; 24-27 are reserved for the pixel art program; `assetTypeName` `"title_art"`;
  `assetTypeFromName`'s bound moved to it), row `{TitleArt, InFile, Assets, false, false}` for
  `.titleart`, `Util::isTitleArtFile`. The Font row gains `.ttc`.
- **R4. Dependency edges.** `AssetIndex` extracts a `.titleart`'s references (fonts, fallback
  fonts, images, SVGs, reflection maps, fill and mask images, pattern images) as edges of the new
  kind `TitleArtAsset` with the JSON path as `field`, and its outputs as `TitleArtOutput` edges to the
  baked PNGs. A missing font or image appears in Missing Assets naming the document and field; a
  missing output appears as "not baked" with a Bake fix.
- **R5. Rename, move, relink.** Renaming or moving a referenced asset rewrites the document's
  `path` hints (GUIDs unchanged) in one project undo step (`TitleArtDocument::relinkGuids`, the
  `AnimatorAssetDocument` design). Renaming or moving the `.titleart` in the editor moves its
  derived PNGs and their `.meta` with it and rewrites their recipe's source path (`VatAssetFiles`
  pattern); references to the PNGs keep working by GUID.
- **R6. Hot reload.** An external change of a `.titleart` reloads its open document, invalidates
  previews and thumbnails, and with Auto Bake on rebakes its stale outputs; a changed font, image or
  SVG it uses does the same for every document that uses it. Load errors become problems
  `{path, line, message}` (`editor/reload/handlers/TitleArt.cpp`).
- **R7. Create.** Assets > Create > Title Art makes `New Title Art.titleart` (unique name) in the
  selected folder: canvas 1920 x 1080, one Text layer "Title" with text "Title" in the built-in
  font, exposed parameter `text`, output `main`; it bakes at once so a texture exists to place.
  One undo step "Create Title Art" (undo deletes the document, its PNGs and their metas).
- **R8. Export.** A `.titleart` is not copied into exports (a skip next to the cooked-texture skip),
  unless `TitleArtExport::isNeeded` says a shipped scene or prefab uses it in Realtime or Glyph
  Atlas mode (S81 fills that function; until then it returns false). Baked PNGs ship as ordinary
  textures through S14.
- **R9. Operations.** `TitleArtOps` implements every op of §6.2 with all-or-nothing batches,
  clamping echoed per field, `%` fields echoed with their pixel value, did-you-mean errors listing
  valid alternatives, and the commit-time invariants (normalize, link sync, tree checks). A batch is
  one project undo step named by its ops ("Set Rim Width", "Add Effect Outer Glow", or
  "Edit Title Art (5 changes)"). `add_layer {from}` inserts a layer object (pasted or dragged from
  another document) with fresh ids for it and everything in it. Op names later specs implement are
  reserved by §6.15 and answer `not_available` (naming the spec) until that spec lands.
- **R9a. Member schema.** `TitleArtSchema` (`engine/core/titleart/TitleArtSchema.{h,cpp}`) is one
  table of every document member by field path pattern (`style.rims[*].width`): value type, hard
  range, default, enum values, unit, Inspector label and animation class (`FxAnim`, §6.5).
  `normalizeTitleArt` clamps by it, `TitleArtOps` validates and suggests by it, S82's rows and S85's
  track classes read it, and later specs register their members into it (§6.15).

### Functional — renderer

- **R10. One renderer.** `TitleArtRenderer` (`engine/core/titleart/`) renders a document (or one
  layer, one output's content, a what-if copy) into a target at a `px_scale`; the editor's previews,
  thumbnails, bakes and checks and S81's Realtime all call it. No editor-only rendering code path.
- **R11. Pass order.** For each node: content → part effects → layer effects → clip group → masks
  → blend and opacity onto the parent (§6.3), exactly Pachislop's order, with groups isolated and
  adjustment layers scoped to what is below them in their parent (or their clip group).
- **R12. Pixel contract.** Linear, premultiplied RGBA16F intermediates where `renderRGBA16F` and
  `filterRGBA16F` hold; values above 1 survive between passes. Otherwise the RGBA8 fallback path
  (§6.3) with the same pass structure. The output pass writes straight-alpha sRGB with an
  interleaved-gradient-noise dither of under one code that never moves exact codes (alpha 0, alpha
  255, flat colours).
- **R13. Regions of interest.** Every node renders into a target covering only its bounds (content
  bounds grown by the definitions' `reach`, clipped to the canvas, aligned to 64 px from the canvas
  origin); passes draw only inside it (viewport and scissor). Full-canvas nodes (Fill, Pattern Fill,
  adjustment layers, effects whose `fills_canvas` holds) use the canvas.
- **R14. Caches.** A layer's work is cached by content hashes computed once per commit (§6.3): a
  colour change never recomputes masks or distance fields; a position change of a 2D title
  re-places its cached stack. Caches are bounded (LRU by bytes) and dropped on device loss.
- **R15. Blend modes.** All 18 modes of §6.4 between layers, groups and parts, plus the internal
  `disjoint`; Normal and Add by fixed-function blending, the others through a backdrop copy of the
  region only.
- **R16. Clipping, masks, adjustment layers, ink vs light** as §6.4.
- **R17. Determinism.** Every random element is seeded from document data (`seed` + element
  seed); rendering never depends on ids, id order, frame timing, cache state or time slicing. Two
  renders of a document on one machine and backend are byte-identical.
- **R18. Time slicing.** `TitleRenderJob::step(budget)` runs whole steps (CPU stages and GPU
  passes) until the budget is spent and resumes on the next call; the previous complete image stays
  valid until the new one is done (double-buffered). The result equals a one-shot render byte for
  byte. The job is a flat ordered step list and exposes what schedulers and progress readouts need
  (§6.3): `progress()` and `passesDone/passesTotal`, `nextStepCostUs()` from each pass's
  `costWeight` (ns per output pixel) times its pixels, `splitOversizedSteps(maxUs)` into scissor
  tiles ≥ 256 × 256, `shaderKeys()`, `nodeCosts()` per layer, and `error()`.
- **R18a. Render variants of one document.** A `RenderRequest` may carry field overrides (exposed
  values, a sampled animation pose, a resolved language: values applied to the snapshot copy, never
  to the document), `keepSplits` (layers whose own composite is kept as a cached split for a later
  live compose) and `glyphCell` (one glyph of any text layer rendered alone, back or front part, in
  a cell padded by the style's reach), §6.3. S81, S85 and S87 use these; none of them needs a second
  renderer.
- **R19. Device loss.** A new `SystemRender::getContextId()` drops every target, cache and pipeline
  and re-renders open previews; no stale handle is used.

### Functional — text, geometry and 2D passes

- **R20. Shaping.** Text shapes through `TextShaper` over `font`, `fallback_fonts`, then the
  built-in face, with `language`, `features`, `kerning`, `tracking`, bidi and grapheme-cluster
  fallback; `TextShaper` gains vertical (TTB) shaping, feature lists and per-call `hb_font_t`
  sub-fonts (§6.6).
- **R21. Outlines.** Glyph outlines come from `hb_font_draw_glyph` on a sub-font carrying the
  layer's `variations`; HarfBuzz is built without `HB_NO_DRAW` when `JASPERENGINE_TITLE_ART` is ON.
  `.ttf`, `.otf` (CFF and CFF2 outlines) and `.ttc` members work.
- **R22. Layout** per §6.6: lines and columns, `align`, `line_height`, vertical columns right to
  left with `vert`/`vrt2`, tate-chu-yoko runs, `proportional` through `palt`/`vpal` or fit-to-ink,
  `h_scale`/`v_scale`, `shear`, `chars` and `lines` adjustments, the frame excluding per-character
  adjustments.
- **R23. Missing glyphs.** A code point no face of the chain covers is reported as problem
  `glyph_missing {layer, chars: [{index, codepoint, char}], fonts_with_it: [...]}` (project fonts
  that have it) in the Inspector, `get_title`, `check_title` and `check --titles`; it draws the
  primary face's `.notdef` so the layout does not jump, and a bake with missing glyphs succeeds with
  that warning.
- **R24. Outline operations.** Warp (the 15 Patchy styles) → distort (corners, mesh) → weight
  (Clipper2 offset, round joins), with curves refit so warps stay curves at any scale (§6.6).
- **R25. Shapes, Vector, Image.** The 11 shapes; SVG in `shape` mode (our path reader over tinyxml2,
  strokes kept as strokes, unsupported SVG features reported) and `artwork` mode (LunaSVG raster at
  exact scale); images with `linear` or `nearest` filtering and SDF-grown rims.
- **R26. Masks of elements.** Each element (face groups, enabled rims) is rasterized to an R8
  coverage mask by ThorVG: fill, plus for rims a stroke of width 2 x grow with the rim's join and
  miter limit 4; `fill_counters` by a CPU flood fill from the border; plates `contour` with `close`
  (Clipper2 union, offset +r, offset −r), `hull`, `box`.
- **R27. Distance fields.** Sub-pixel-seeded jump flooding (seed, flood passes, JFA+1, resolve)
  whose first step is `2^ceil(log2(R + 2))`, R the largest distance any consumer of that element
  needs (bevel size, inner glow size, outline width, satin is blur-based and needs none); shape-
  burst fills and Fit bevels request the unbounded field. Beyond R the field reads "at least R".
- **R28. Element shading** in Pachislop's order (§6.7) with every Material member of §6.1.
- **R29. Stack.** Elements drawn outermost first with premultiplied over, knockout punches, erode
  applied to every mask before its distance field, per-character faces as split face elements, the
  span map for `line`/`char` gradients.

### Functional — effects, registry, bake

- **R30. Core effects.** The ten effects of §6.5 with the listed params, defaults, units, `reach`,
  `adds_light` and `outline` flags; `adjust` and adjustment layers share one implementation of the
  fourteen operations.
- **R31. Registry.** One declarative table per effect and generator (`TitleEffectRegistry`, §6.5)
  drives document defaults and validation, the Inspector rows (S82 and the S80 summary), the MCP
  validation and `list_title_library`, the exposed-parameter targets (S81), the generated docs page
  and the golden case list. Adding an effect is one definition plus its pass code. S80 declares
  every member the program's definitions carry (§6.5: cost class, fallback class, live variant,
  glyph safety, animation class per param, aliases, inputs, colour roles), with defaults, so S81,
  S82, S83 and S86 build against one header; S83 fills the values for the catalogue.
- **R32. Bake outputs** as §6.9: shared trimmed frame, `round_to`, `alpha_bleed` (reach 8 px,
  a parameter of the routine for exports), 8- or 16-bit, recipe chunk `jasper.titleart`, the
  sidecar `<file>.bake.json`, `.meta` with texture import defaults on first write, all files of a
  bake written all-or-nothing.
- **R33. Stale detection.** An output is `fresh` when its recipe's source hash, asset hashes and
  bake version equal the current ones, else `stale` (with what changed), `missing` or `invalid`;
  shown in the Inspector, `get_title`, `get_asset_info`, `check --titles` and the export.
- **R34. Auto Bake.** With project setting `title_art.auto_bake` (default true) a commit to a
  document whose outputs exist rebakes them in the background 500 ms after the last edit; bakes are
  not undo steps (derived files), are written through the expected-write ledger and reload their
  textures explicitly.
- **R35. Export behaviour.** `title_art.stale_bakes_on_export`: `rebake` (default: the export bakes
  stale or missing outputs first), `warn`, `fail`.
- **R36. Float readback** `FrameReadback::readTexture` with `bytesPerPixel` 8 (RGBA16F, half
  floats) and 16 (RGBA32F) on GL core, Vulkan, D3D11 and Metal; GLES3/WebGL2 read RGBA32F where
  `EXT_color_buffer_float` holds and refuse otherwise with a message.

### Platform & backends

- **R37.** The renderer uses only fragment and vertex shaders, dynamic pipelines, RGBA8/R8 and
  (when available) float targets, at most 12 samplers and 4 uniform blocks per program, no MRT, no
  depth buffers in the 2D passes, no float blending (only RGBA16F with `blendRGBA16F`, else a
  shader over), no compute, SSBOs, bindless, indirect draws or 64-bit atomics. It compiles for
  every shader target (GLSL 410, GLSL ES 300, HLSL 5, MSL macOS and iOS, SPIR-V). The target pool
  also serves depth and multisampled targets to the one consumer that needs them (S86's 3D layer
  passes, §6.3); everything handed back to the compositor is a plain colour target.
- **R38.** The RGBA8 fallback runs wherever RGBA8 renders (all five backends, all six targets) and
  agrees with the float path within §6.3's tolerance; it is forced in tests by
  `TITLE_ART_TEST_NO_FLOAT_TARGETS` (and `..._NO_FLOAT_FILTER`, `..._NO_R16F`), the FluidSimulator
  pattern.
- **R39.** Frame space is memory rows on every backend (§6.3); an asymmetric golden catches a flip
  on GL core and Vulkan, and the fallback golden on the same fixture.
- **R40.** `JASPERENGINE_TITLE_ART` (CMake option, default ON) compiles `engine/core/titleart`
  (sources in `TitleArtSources.cmake`), builds and links ThorVG (`JASPERENGINE_HTMLUI OR
  JASPERENGINE_TITLE_ART`) and Clipper2, and drops `HB_NO_DRAW`; OFF compiles none of it (zero
  bytes, zero work) and HarfBuzz is byte for byte today's. `engine/notices.json` gains `clipper2`
  (`"when": "titleart"`) and ThorVG's `when` becomes the list `["htmlui", "titleart"]`
  (`tools/ci/check-notices.py` accepts a list). The program's module story (sub-options, the
  always-built part, player wiring) is §6.15.

### Performance (budgets)

- **R41.** Classic fixture (§7: 確定 in the classic style: gold face with bevel, finish, inner glow and
  gloss; 4 rims, 3 bevelled; Outer Glow and Drop Shadow) at canvas 1920 x 1080, scale 1: a full
  render ≤ 40 ms GPU on the build server's RTX lane, ≤ 8 s wall on llvmpipe.
- **R42.** Cached re-renders of the classic fixture (pass counter, deterministic on any GPU):
  face colour change ≤ 8 passes; position change ≤ 2 passes; outermost rim width change ≤ 60 % of
  a full render's passes (the full count is recorded by the test).
- **R43.** Distance-field flood passes for an element whose largest consumer reach is R mask px:
  ≤ ceil(log2(R + 2)) + 1 (Pachislop: ceil(log2(frame side)) + 1). For the classic fixture at scale
  1 the total flood passes are ≤ 50 % of the unbounded count.
- **R44.** Peak live GPU target memory of the classic fixture at scale 1 ≤ 96 MB (Pachislop's
  full-frame design: about 25 MB per target at this canvas).
- **R45.** Editor previews and thumbnails spend ≤ 4 ms of main-thread CPU per editor frame (p95
  over 200 frames); visible items first; cancelled when their window closes. One arbiter,
  `TitlePreviewQueue` (`editor/titleart/TitlePreviewQueue.{h,cpp}`, §6.13), runs every title
  preview of the editor (S80's thumbnails, S82's canvas thumbnails and hovers, S84's tiles, S86's
  picker thumbnails, S87's language thumbnails), so the budget holds for all of them together.
- **R46.** Bake of the classic fixture with readback, 8-bit encode and recipe ≤ 1.5 s wall on the GPU
  lane (≤ 12 s on llvmpipe).
- **R47.** Codec: parse + normalize + write of a 256-layer, 1 MB document ≤ 60 ms; an op batch of 20
  `set`s ≤ 10 ms CPU without rendering.
- **R48.** Shaping and outlines of 64 CJK glyphs with two fallback fonts ≤ 6 ms CPU, cached per
  (font set, text, layout members).
- **R49.** With no title previewed or baking, the renderer does no work and holds no targets
  (0 passes, 0 bytes, checked by the module's counters).

### Editor UX (human)

- **R50.** Project window: `FileType::TITLE_ART`, a Title Art icon and colour, a thumbnail (the fresh
  main output's PNG when there is one, else a 128 px render), double-click opens the Title Designer
  (S82), `t:title_art` search.
- **R51.** Inspector summary of one selected `.titleart` (§6.13): preview, canvas, layers, fonts with
  missing ones, outputs with Fresh/Stale/Missing and Bake, exposed parameters, problems, **Open in
  Title Designer**, Auto Bake state; read-only except Bake and the outputs' Add/Remove (undoable).
- **R52.** Project Settings > Title Art: Auto Bake, Stale Bakes On Export (Rebake / Warn / Fail),
  each an undoable settings change.

### Agent UX (MCP, Lua, CLI)

- **R53.** Group `title_art` ("Title Art documents (.titleart): library, edit, render, check,
  export.") with the six tools of §6.11, hidden until `tool_groups` enables it, within §6.11's
  character caps; `diff_images` in `editor_tools`. §6.11's program budget table is the one place
  the title specs' character claims are added up; every title spec stays inside its row.
- **R54.** `edit_title` and the Designer call the same `TitleArtOps`; every call is one undo step
  attributed to the agent (`McpUndo::foldCall`), with `expect_rev`, `dry_run`, and a `render`
  option returning the image in the same call.
- **R55.** `render_title` returns real alpha on chosen backgrounds, crops, numbered `try` sheets of
  what-if ops (file untouched), `compare` with a previous `rev` or a PNG and `changed_pct`, `layer`
  isolation, `fallback: "rgba8"` for the RGBA8 path (`"compare"` for both), metrics-only mode, and the
  free-form `options` object later specs add keys to (§6.11).
- **R56.** `check_title` returns PASS/WARN/FAIL per check with measured values, thresholds, crops,
  method and ready-to-apply `fix` ops, and always says that passing is not aesthetic approval
  (§6.12).
- **R57.** `export_title` bakes outputs as a job (`dry_run` reports sizes, bytes and VRAM without
  writing).
- **R58.** CLI: every tool through `jasperengine-editor call <tool> --project <p>`;
  `jasperengine-editor check --titles` (in the default set and `--all`; `--files` with a `.titleart`
  or a baked PNG) reports `title-*` problems without a GPU (document checks and recipe freshness).
- **R59.** `view_asset` on a `.titleart` returns its main output (or a 512 px render);
  `get_asset_info` adds canvas, layers, fonts, outputs with freshness and problems.
- **R60.** `docs/mcp/guide/title-art.md` (`jasper://docs/title-art`) holds the agent guidance; the
  agent guide gets one entry in its "Other areas" index, ≤ 40 bytes with the generated group name.
- **R61.** Editor Lua reaches everything through `Editor.call("edit_title", {...})`; S80 adds no
  runtime Lua (S81's `TitleArt`). Runtime Lua reaches document values only through Exposed
  parameter names (contract decision 13, S81 R66); the registry's `type` ids and param keys are the
  field paths exposed targets name (`{layer: "<id>", field: "fx:outer_glow.size"}`).

### Data & compatibility

- **R62.** No existing format changes: scenes, prefabs, materials and metas are untouched; the Font
  row's `.ttc` and the new asset type only add.
- **R63.** Every `.titleart` written by any build loads in every later build (the migration rules);
  the v1 fixtures `engine/tests/core/titleart/fixtures/v1/*.titleart` are kept forever.
- **R64.** Baked PNGs are committed derived files: a teammate without the same GPU gets the same
  art; their recipes, not their bytes, decide freshness.

## 6. Design constraints & decisions

### 6.1 The `.titleart` document, format version 1

**Serialization: canonical JSON with the GUID in the file**, the format of the engine's other
editable asset documents that a dedicated window edits (`.controller`, `.mask`, `.montage`:
`engine/core/animation/AnimatorAssetIO.h`; `.anim`: `AnimationClipIO.h`; `.vat`). Why:

- The Title Designer (S82) and MCP edit it through a document class on the project history with
  whole-file before/after text (`WriteFilesCmd`), the `AnimatorAssetDocument` pattern
  (`editor/animation/AnimatorAssetDocument.h`). `WriteFilesCmd` handles contents only and no
  `.meta`, which is why those assets keep `"guid"` in the file (`AssetIdentity::InFile`,
  `editor/asset/AssetTypes.cpp:41-53`). A sidecar identity (the `.particles` model) would need
  a second command for the meta on create and on undo of create.
- Exported Realtime and Glyph Atlas titles (S81) parse it in the player, so the codec lives in
  `engine/core/titleart/` and uses the engine's JSON library (`engine/libs/json/json.hpp`, already
  linked into players). YAML (scenes, materials) would add `yaml-cpp` parsing to a runtime path
  that does not need it.
- An agent hand-edits it with its own file tools using the same field paths as `edit_title`.

**Codec rules** (`engine/core/titleart/TitleArtIO.{h,cpp}`, the `AnimatorAssetIO` contract):

- UTF-8 JSON object. First members `"format": "jasperengine.titleart"`, `"version": 1`,
  `"guid"` (32 lowercase hex, S04). Constants `TITLE_ART_FORMAT`, `TITLE_ART_VERSION = 1`,
  `TITLE_ART_EXTENSION = ".titleart"`.
- **Canonical text:** fixed member order (the order of the tables below), 2-space indent, one
  member per line, arrays of numbers on one line, numbers `%.7g` (integers plain), colours
  lowercase. **Every member is written, defaults included**, so a later change of a default never
  changes how an existing file renders. One exception, by design: S87's Translation objects
  (`translations.<code>`) are sparse overrides of their source layer, written with `text`,
  `status`, `source_hash` and then only the members present (S87 D5), because a written default
  would freeze the source value into every language; the writer takes a per-member-kind sparse
  flag from `TitleArtSchema` for exactly that kind. Maps (`chars`, `lines`, `part_effects`, `variations`,
  `features`, `material_links`, effect `params`) are written sorted by key (`chars` and `lines`
  numerically). `write(parse(write(d))) == write(d)`.
- **Lenient read:** only a wrong or missing `format`, a `version` newer than this build, or text
  that is not a JSON object fails (with line:col). A member of the wrong type or out of range is
  dropped or clamped with a warning carrying its JSON path
  (`layers[2].style.rims[0].width: 140 clamped to 100`). A missing member takes its default.
- **Unknown data survives:** unknown members of every object are kept and written back after the
  known ones; an effect, generator, mask or layer `type` this build does not know is kept
  verbatim, draws nothing, and is reported as problem `unknown_type` (never deleted on save).
- **Newer version:** refused for editing (`version_newer`, the Inspector and `get_title` say which
  build wrote it); it renders nothing and its outputs are not rebaked.
- **Older version:** migrated in memory before decode (below); the file is rewritten in the
  current version only by the next commit, never on open.
- Thread-safety: pure functions. `parseTitleArt(text) -> TitleArtParseResult {ok, error, doc,
  warnings, version_read, migrations_applied}`, `writeTitleArt(const TitleArtDoc&) -> string`,
  `normalizeTitleArt(TitleArtDoc&, warnings*)` (unique ids, clamps, tree limits).

**Migration framework** (`engine/core/titleart/TitleArtMigrations.{h,cpp}`), the
`editor/StreamMigrations.h` design for JSON: an ordered table `{from, to, name, apply}` with
`apply(nlohmann::json& root, MigrationLog& log)`, run on the parsed JSON before decode, pure (no
editor state, so the engine, the CLI and tests run it). Rules every later spec follows:

1. Adding an optional member, a layer type, an effect, a generator or an enum value does **not**
   bump the version (defaults and unknown-data preservation cover it). Renaming, moving or
   re-typing a member, or changing what a value means, bumps `TITLE_ART_VERSION`, appends one
   `vN -> vN+1` step and a fixture `engine/tests/core/titleart/fixtures/v<N>/*.titleart`.
2. **Fill the old behaviour, not the new default** (Pachislop's rule, report p1 §1.14): a member a
   step introduces gets the value that keeps old files rendering as they did.
3. Every step is idempotent and has a test "migrating twice equals once"; the chain from every
   fixture to the current version is tested on every build. Old documents load forever: no step
   is ever removed.
4. A bake whose renderer changed output without a format change bumps `TITLE_ART_BAKE_VERSION`
   (in the PNG recipe, §6.7) instead, which marks bakes stale without touching documents.

The v1 table is empty. The framework ships with its test harness and a test-only step
(`TitleArtMigrationsTest.ChainRunsInOrder`, registered from the test, never in the product).

#### Value types used below

| Type | JSON | Notes |
|---|---|---|
| Color | `"#rrggbb"` | sRGB, lowercase. Alpha is always a separate member. MCP accepts `#rgb`, `#rrggbb` or `[r,g,b]` 0..1 and stores `#rrggbb` |
| Vec2 | `[x, y]` | |
| Gradient | `{"stops": [Stop...], "interp": "rgb"}` | Stop = `{"pos": 0..1, "color", "alpha": 1, "mid": 0.5}` (`mid`: Photoshop midpoint to the next stop, 0.05..0.95). `interp`: `rgb` (lerp of sRGB-encoded channels, what Pachislop's code does, report p1 §2.5) or `vivid` (OKLCH, shorter hue arc). Default: `#000000` at 0, `#ffffff` at 1. 2..32 stops |
| Curve | `{"points": [{"x", "y", "corner": false}...]}` | 0..1 both axes; sorted by x; x = 0 and x = 1 always present; monotone cubic (Fritsch-Butland) between smooth points, one-sided tangents at corners; baked to 256 samples. Default identity `[[0,0],[1,1]]` (the tables write curves as `[[x, y]...]` for short; the file holds point objects). 2..32 points |
| AssetRef | `{"path": "fonts/x.ttf", "guid": "<32 hex>"}` or `null` | `path` relative to the assets directory (S04 `AssetRef`, `engine/core/asset/AssetRef.h`); the GUID wins when both resolve |
| Id | `"3fa0b2c1"` | 8 lowercase hex, unique in the document (layers, rims, masks, effects share one id space). Ids are identity only: **rendering never depends on an id or on id order** (AC "render twice with fresh ids") |

**Units.** `%` = percent of the layer's source `size` (font em size, shape height, SVG/image larger
side): every style dimension, so a look transfers across sizes (contract decision 5). `px` =
canvas pixels, multiplied by the render's `px_scale` (bake scale or display scale): every effect,
generator and mask distance. `deg` angles are **clockwise on screen from +x** everywhere, except
light directions (`global_light.angle`, `bevel.light_angle`, `satin.angle`), which keep
Photoshop's convention: the direction the light comes from, counter-clockwise from +x, 120 = upper
left. A shadow's `angle` is the direction it falls (clockwise, 60 = down right), as Pachislop's;
with `use_global_light` it is the global light's opposite.

**Coordinates.** Document space is canvas pixels with the origin at the canvas centre, +x right,
**+y up** (Pachislop's comp space, so ported looks and templates keep their values). Image space
(PNG rows, `crop`, `alpha_bounds`, check boxes in tool results) is top-left origin, +y down; tools
that report positions say which space. The renderer converts once, in `TitleFrame` (§6.3).

#### Top level

| Member | Type | Default | Notes |
|---|---|---|---|
| `format` | string | `"jasperengine.titleart"` | |
| `version` | int | 1 | |
| `guid` | string | new | |
| `name` | string | `""` | Display name; empty = the file stem |
| `canvas` | Canvas | | |
| `global_light` | `{angle: 120, altitude: 30}` | | deg (Photoshop convention), altitude 0..90. Read by members with `use_global_light` |
| `seed` | int | 1 | 0..2147483647. Added to every element seed (`seed` members, generator `seed` params), so "Reseed" on the document changes every random element at once while each stays individually controllable (decision 8) |
| `material_links` | map id → `{name}` | `{}` | Linked materials (report p1 §1.10): every `Material.link` equal to a key is one material, kept equal by the document's commit (§6.2); renderers ignore links |
| `layers` | [Layer] | `[]` | Top level, **bottom first** (draw order, like Unity's hierarchy; the Layers panel lists top first, S82) |
| `exposed` | [Exposed] | `[]` | §6.1 "Exposed parameters" |
| `outputs` | [Output] | `[{"id": "main"}]` (all its defaults) | §6.1 "Outputs" |

Reserved top-level names (later specs add them; until then they are unknown members and
preserved; the owner's spec defines their content): `runtime` (S81: Characters, the Glyph Atlas
layer), `scene_3d` (S86: camera, environment, lights, tone mapping), `color_schemes` (S84: the
scheme data an exposed `color_scheme` parameter may pick), `animation` (S85), `source_language`,
`languages`, `string_namespace`, `dpi` and `import_notes` (S87). (`variants` is not reserved: S87
needs no such member.)

Members later specs add inside S80's objects (optional, defaults that render exactly as S80 v1,
no version bump, registered in `TitleArtSchema` by their owner): Transform `z`, `rotation_x`,
`rotation_y` (S86) and `stretch` (S85); every layer `tint`, `flash` (S85) and `only_languages`
(S87); layer `look.owner` and effect `from_look` (S84, below); text layer `glyph_mode` (S85),
`text_key`, `translate`, `max_chars`, `translator_note`, `fit`, `translations` (S87); Text, Shape,
Vector and Image `style_3d` (S86); CharAdjust `dz`, `tilt_x`, `tilt_y`, `face_3d`, `side_3d`,
`bands_3d`, `rig` (S86); Image and Vector `translations` (S87).

**Limits** (normalize clamps, with a warning): 256 layers in total, groups nested 16 deep, 32
effects per stack, 16 rims, 8 masks per layer, 64 exposed parameters, 8 outputs, 1,024 code points
of text per layer, canvas 16..8192 per side and at most 33,554,432 pixels.

#### Canvas

| Member | Type | Default | Notes |
|---|---|---|---|
| `width`, `height` | int | 1920, 1080 | Canvas px. The bake frame is trimmed from it (Outputs) |
| `background` | `{transparent: true, color: "#000000"}` | | What bakes and renders composite over: transparent = real alpha. Preview backdrops (checker, black, a scene) are view state, not document data |

#### Layer: members every layer has

| Member | Type | Default | Notes |
|---|---|---|---|
| `id` | Id | new | |
| `type` | enum | | `text`, `shape`, `image`, `vector`, `group`, `generator`, `adjustment` (Unity-like UI names: Text, Shape, Image, Vector (SVG), Group, Generator, Adjustment) |
| `name` | string | type name (`"Text"`, `"Shape"`...) | Need not be unique; paths add `#N` (§6.2) |
| `visible` | bool | true | |
| `locked` | bool | false | Editor only: no canvas picking or gizmo; ops still edit it |
| `guide` | bool | false | Drawn in the editor, never in outputs, Realtime or Glyph Atlas |
| `blend` | enum | `"normal"` | Blend modes, §6.4 |
| `opacity` | number | 1 | 0..1 |
| `clip` | bool | false | Clip to the base below (the nearest sibling below with `clip` false), Photoshop clipping mask; ignored with no base or when the base is an adjustment layer |
| `transform` | Transform | | |
| `masks` | [Mask] | `[]` | Multiplied in order |
| `effects` | [Effect] | `[]` | Applied in order, index 0 first |
| `look` | `{id: "", color_scheme: "", owner: ""}` | | Informational: the Look and Color Scheme last applied (S84 writes it; `get_title` shows it; the renderer never reads it). `owner` (an Id, S84): on a generator layer or auto group a look created, the layer whose look created it |

Transform: `position` Vec2 px `[0, 0]`; `rotation` deg 0 (clockwise); `scale` 1 (0.01..100,
uniform); `flip_x`, `flip_y` false (mirror about the position, before rotation). S86 adds `z`,
`rotation_x`, `rotation_y` (default 0) for Dimension 3D; S85 adds `stretch` (Vec2, `[1, 1]`).
**A group's transform is concatenated
into its children's placement** (children are positioned in group space and render sharp; the
group composite is never resampled).

#### Text layer (`type: "text"`)

| Member | Type | Default | Notes |
|---|---|---|---|
| `text` | string | `"Title"` | `\n` breaks lines (columns when vertical) |
| `font` | AssetRef | `null` | A Font asset (`.ttf`, `.otf`, `.ttc`). `null` = the engine's built-in face (`engine/core/util/DefaultFont.h`). Library fonts (S84) are copied into the project on first use, so every font a document uses is a project asset with a GUID |
| `face_index` | int | 0 | Member of a `.ttc` collection |
| `fallback_fonts` | [AssetRef] | `[]` | Fallback Fonts, tried in order after `font`, then the built-in face |
| `variations` | map tag → number | `{}` | Variable-font axes (`"wght": 800`, `"wdth": 75`); unknown axes kept, ignored, reported |
| `features` | map tag → int | `{}` | OpenType features on top of the defaults (`"smcp": 1`, `"liga": 0`) |
| `language` | string | `""` | BCP 47 (`"ja"`); empty = guessed from the script |
| `size` | number | 200 | Em size, canvas px. The unit of every `%` member |
| `tracking` | number | 0 | 1/1000 em added after each cluster |
| `line_height` | number | 1.1 | × size |
| `kerning` | bool | true | |
| `align` | enum | `"center"` | `start`, `center`, `end` (per line; per column when vertical) |
| `vertical` | bool | false | 縦書き: columns top to bottom, right to left; `vert`/`vrt2` glyphs |
| `tate_chu_yoko` | bool | true | Vertical only: runs of up to `tate_chu_yoko_max` half-width characters set upright across the column |
| `tate_chu_yoko_max` | int | 3 | 1..4 |
| `proportional` | number | 0 | 詰め 0..1: kana, CJK punctuation and full-width symbols move from full-width advances toward `palt`/`vpal` (or fitted-to-ink when the font has neither); Latin keeps its metrics |
| `h_scale`, `v_scale` | number | 1 | 長体/平体, 0.1..10; advances and line spacing follow |
| `shear` | number | 0 | Faux italic, deg, positive leans right, about the block centre |
| `chars` | map index → CharAdjust | `{}` | Key: index of the code point in `text` (newlines counted) |
| `lines` | map index → LineAdjust | `{}` | Key: line (column) index, 0 = first |
| `warp` | Warp | | Outline operations run warp → distort → weight, before the style |
| `distort` | Distort | | |
| `weight` | number | 0 | %, −20..20: offset of the glyph outlines (Illustrator Offset Path) |
| `dimension` | enum | `"2d"` | `2d`, `3d`. **The Dimension rule:** Text, Shape, Vector and Image layers have `dimension`; Group, Generator and Adjustment layers do not. S86 owns `3d` and the layer's `style_3d`; Text and Shape layers give S86 their outlines (after warp, distort, weight), Vector `shape` mode its paths, and Vector `artwork` mode and Image layers their alpha `trace` outline. While 3D the 2D `style` is kept untouched. Until S86 lands (or with `JASPERENGINE_TITLE_ART_3D` off), a `3d` layer renders as `2d` with problem `dimension_unavailable` |
| `style` | Style2D | | |
| `part_effects` | map PartKey → [Effect] | `{}` | Effects on one part's pixels, before the layer's effects |

CharAdjust (all optional on read, all written): `offset` Vec2 % `[0, 0]` (+y up); `scale` 1 (the
glyph's advance scales and the line reflows); `h_scale`, `v_scale` 1; `rotation` deg 0 (about the
em-cell centre); `face` Material or `null` (`null` = the layer's face; a non-null face splits the
face element per material while rims stay continuous around the word). S86 adds `dz`, `tilt_x`,
`tilt_y`, `face_3d`, `bands_3d`, `side_3d`.

LineAdjust: `scale` 1, `offset` Vec2 % `[0, 0]`, `h_scale` 1, `v_scale` 1.

Warp: `style` `none` | `arc` | `arc_lower` | `arc_upper` | `arch` | `bulge` | `shell_lower` |
`shell_upper` | `flag` | `wave` | `fish` | `rise` | `fisheye` | `inflate` | `squeeze` | `twist`
(Patchy's fifteen, default `none`); `bend` 0, `horizontal` 0, `vertical` 0 (−100..100).

Distort: `mode` `none` | `corners` | `mesh` (default `none`); `corners` 4 × Vec2 (offsets of the
frame corners TL, TR, BR, BL in fractions of the undistorted frame, +y up), default all `[0, 0]`;
`mesh` 16 × Vec2 (4 × 4 bicubic control-point offsets, row-major from top-left), default all
`[0, 0]`.

#### Shape layer (`type: "shape"`)

| Member | Default | Notes |
|---|---|---|
| `shape` | `"rect"` | `rect`, `parallelogram`, `ellipse`, `orb`, `polygon`, `star`, `burst`, `shield`, `speech`, `shout`, `ribbon` |
| `size` | 200 | Height, px; the `%` unit |
| `aspect` | 1 | Width / height, 0.05..20 |
| `corner` | 0 | %, rect, parallelogram, polygon, shield, speech |
| `slant` | 0 | deg, parallelogram, ribbon |
| `points` | 5 | 3..64: spikes (star, burst, shout) or sides (polygon) |
| `inner` | 0.5 | 0..1: inner radius (star, burst, shout); notch depth (ribbon) |
| `jitter` | 0 | 0..1 (burst, shout) |
| `seed` | 1 | |
| `tail` | `{x: 0, y: -60, width: 20}` | %, speech/shout tail tip (+y up) and base width; ribbon: `x` = fold length |
| `warp`, `distort`, `weight`, `dimension`, `style`, `part_effects` | | As the text layer |

#### Vector layer (`type: "vector"`, UI "Vector (SVG)")

| Member | Default | Notes |
|---|---|---|
| `svg` | `null` | AssetRef to an `.svg` (typed Texture by `AssetTypes.cpp:28-29`; the dependency edge says "vector source", §6.6) |
| `size` | 200 | Larger side, px |
| `mode` | `"shape"` | `shape`: fills and strokes become outlines (the SVG's colours are ignored) and the style applies like text. `artwork`: the SVG's own colours, rasterized at exact scale; the face's `fill.kind: "source"` shows them and rims grow from its alpha |
| `warp`, `distort`, `weight` | | `shape` mode only |
| `dimension`, `style`, `part_effects` | | |

#### Image layer (`type: "image"`)

| Member | Default | Notes |
|---|---|---|
| `image` | `null` | AssetRef to a Texture |
| `size` | 200 | Larger side, px |
| `filter` | `"linear"` | `linear`, `nearest` (pixel art) |
| `trace` | `{threshold: 128, smooth: 0, despeckle: 0}` | Alpha to outline, for Dimension 3D (S86) and hull/box plates |
| `dimension`, `style`, `part_effects` | | A new image layer's face has `fill.kind: "source"` (and a missing `style.face.fill.kind` of an image or `artwork` vector layer reads as `source`); rims grow from the alpha through its distance field (round joins) |

#### Style2D and parts

| Member | Default | Notes |
|---|---|---|
| `face` | Material (solid `#ffffff`, everything off) | |
| `rims` | `[]` | Rim list, **innermost first**; rim k's outline is the face outline grown by the sum of the widths of rims 0..k; drawn outermost first |
| `knockout` | false | The face punches through every rim to transparency (hollow tubes) |
| `erode` | Erode | かすれ dry-brush edges, one mask for face and rims |

Rim: `id`; `enabled` true; `width` 3 (%, 0..100); `join` `"miter"` (`miter` with limit 4, `round`,
`bevel`); `fill_counters` false (fill holes so the rim is a solid plate); `plate` `{kind:
"contour", close: 0, corner: 4, slant: 0}` (`contour` | `hull` | `box`; `close` and `corner` in
%, `slant` deg); `offset` Vec2 % `[0, 0]` (+y up; a dropped "seated" base uses a negative y);
`material` Material.

Erode: `enabled` false, `amount` 0.5 (0..1), `edge` 6 (%), `scale` 12 (%), `angle` 0 (deg),
`seed` 1.

**Part keys** (contract: the parts of a text or shape layer). 2D: `face` and `rim:<rim id>`. 3D
(S86): `face` (the letter's front), `bevel` (the front bevel surface), `side` (the side wall) and
`band:<band id>`. Stored keys always use ids; ops and Looks also accept positional aliases
`rim#<n>` / `band#<n>` (0 = innermost), which Looks store (S84) because a Look has no ids. The
Inspector names them Face, Rim 1..N, Bevel, Side, Band 1..N. A part key whose part no longer
exists is dropped by normalize with a warning.

#### Material (2D)

Defaults are Pachislop's (`src/model/defaults.ts` `material2D`, `bevel2D`, ...), so converted
looks render the same.

| Member | Defaults |
|---|---|
| `fill` | `kind: "solid"` (`solid`, `linear`, `radial`, `angle`, `reflected`, `diamond`, `shape_burst`, `image`, `source`), `color: "#ffffff"`, `gradient` (default), `angle: 90` (deg, 90 = top to bottom), `scale: 1`, `offset: 0` (−1..1), `span: "layer"` (`layer`, `line`, `char`), `image: {image: null, scale: 1, offset: [0, 0], rotation: 0, tile: false}`. `source` = the layer's own pixels (image, vector artwork); on text and shapes it is solid white with problem `fill_source_without_pixels` |
| `opacity` | 1 (fill opacity: the element's own colour fades, effects still see the full shape) |
| `bevel` | `enabled: false`, `profile: "smooth"` (`smooth`, `button`, `mesa`, `chamfer`, `ridge`, `cove`, `stepped`, `custom`), `profile_curve: [[0,0],[0.5,0.75],[1,1]]`, `size: 3` (%), `size_mode: "fixed"` (`fixed`, `fit`), `fit: 70` (% of local half-thickness), `depth: 3` (0..10), `direction: "up"` (`up`, `down`: Photoshop's Direction; `down` negates the height so the bevel reads pressed in, S83's Deboss and S87's Photoshop import use it), `soften: 0` (%), `light_angle: 120`, `light_altitude: 30`, `use_global_light: true`, `gloss: "ring"` (`linear`, `ring`, `double_ring`, `cove`, `rounded_steps`, `sawtooth`, `custom`), `gloss_curve` (identity), `highlight_color: "#ffffff"`, `highlight_opacity: 0.85`, `shadow_color: "#000000"`, `shadow_opacity: 0.6` |
| `reflection` | `enabled: false`, `map: null` (AssetRef to an equirectangular Texture; library maps are copied into the project on first use, S84), `amount: 1`, `blur: 75` (0..100), `rotation: 0` (deg), `colorize: 0` (0..1) |
| `finish` | `enabled: false`, `gradient` (default), `amount: 1` (gradient map of the lit luminance: the gold trick) |
| `pattern` | `kind: "none"` (`none`, `zebra`, `tiger`, `giraffe`, `leopard`, `checker`, `hazard`, `stripes`, `dots`, `sakura`, `seigaiha`, `asanoha`, `shippo`, `kikko`, `uroko`, `custom`; S83 §6.6.28 adds twelve procedural kinds), `color: "#000000"`, `color2: "#ffffff"`, `opacity: 1`, `scale: 12` (%), `angle: 0`, `seed: 1`, `image: null` (custom), `shade: 0.6`, `relief: 0` |
| `satin` | `enabled: false`, `color: "#000000"`, `blend: "multiply"` (`normal`, `multiply`, `screen`, `overlay`), `opacity: 0.5`, `angle: 19`, `use_global_light: false`, `distance: 5` (%), `size: 7` (%), `contour: [[0,0],[0.5,1],[1,0]]`, `invert: true` |
| `texture` | `kind: "none"` (`none`, `noise`, `sparkle`, `brushed`, `hairline`, `spun`, `flakes`, `ripple`, `grunge`, `clouds`, `hammered`, `crumple`, `liquid`), `amount: 0.3`, `scale: 4` (%), `blend: "overlay"` (`overlay`, `soft_light`, `screen`, `multiply`), `seed: 1`, `target: "color"` (`color`, `surface`: joins the bevel slope before lighting), `angle: 0` |
| `inner_glow` | `enabled: false`, `color: "#ffffff"`, `size: 2` (%), `opacity: 0.6`, `noise: 0` |
| `gloss` | `enabled: false`, `opacity: 0.55`, `position: 0.45`, `softness: 0.15`, `curve: 0.25`, `angle: 0` |
| `refraction` | 0 (%, negative mirrors) |
| `glass` | `enabled: false`, `color: "#3cc8ff"`, `tint: 1`, `opacity: 0.7`, `edge: 0.7`, `caustic: 0.7` (0..2) |
| `sheen` | `enabled: false`, `color: "#ffffff"`, `amount: 0.7` (0..3), `position: -0.25` (−1..1), `width: 0.35` (0.05..2) |
| `link` | `""` (a `material_links` key; empty = unlinked) |

#### Group (`type: "group"`)

`children` [Layer] `[]` (bottom first); `collapsed` false (Layers panel only). Members composite in
isolation on a cleared buffer, the group's effects run on that composite, then it is drawn with
the group's blend, opacity and masks. **No pass-through mode in v1** (Pachislop has none; a
Photoshop file's pass-through groups import as Normal with a note, S87). Adjustment layers inside a
group adjust only the members below them.

#### Generator (`type: "generator"`)

`generator` `{type: "<registry id>", params: {...}}` (every param of the definition written; §6.5);
`follow` `""` (a layer id: the generator draws where that layer is, its own transform an offset on
top; empty = placed by its own transform). Generators are layers, never effects (contract).
S80 ships two generators to exercise the type end to end: `fill` (a solid or gradient covering the
canvas, which replaces Pachislop's `background` layer kind) and `pattern_fill` (a full-canvas
procedural pattern, Pachislop's `patternFill`, sharing the material patterns' GLSL); the catalogue
is S83.

#### Adjustment (`type: "adjustment"`)

`adjustment` `{type, ...}`, the same object the `adjust` effect holds. All fourteen operations are
S80's (closed-form or one LUT each, so splitting them would only churn the schema):

| `type` | Members (defaults) |
|---|---|
| `brightness_contrast` | `brightness: 0` (−150..150), `contrast: 0` (−50..100) |
| `levels` | `channels: {rgb, r, g, b}`, each `{in_black: 0, in_white: 255, gamma: 1, out_black: 0, out_white: 255}` |
| `curves` | `channels: {rgb, r, g, b}`, each a Curve (identity) |
| `exposure` | `exposure: 0` (stops, −20..20), `offset: 0` (−0.5..0.5), `gamma: 1` |
| `vibrance` | `vibrance: 0`, `saturation: 0` (−100..100) |
| `hue_saturation` | `hue: 0` (−180..180), `saturation: 0`, `lightness: 0` (−100..100), `colorize: false` |
| `color_balance` | `shadows`, `midtones`, `highlights`: `{cyan_red: 0, magenta_green: 0, yellow_blue: 0}` (−100..100); `preserve_luminosity: true` |
| `black_white` | `reds: 40`, `yellows: 60`, `greens: 40`, `cyans: 60`, `blues: 20`, `magentas: 80` (−200..300), `tint: false`, `tint_color: "#e1d3b3"` |
| `photo_filter` | `color: "#ec8a00"`, `density: 25` (0..100), `preserve_luminosity: true` |
| `gradient_map` | `gradient` (default), `reverse: false` |
| `invert` | |
| `posterize` | `levels: 4` (2..255) |
| `threshold` | `level: 128` (1..255) |
| `effects_only` | (no colour change; the layer carries effects over everything below) |

Default `type` of a new adjustment layer: `hue_saturation`.

#### Mask

| Member | Default | Notes |
|---|---|---|
| `id` | new | |
| `type` | | `shape`, `gradient`, `layer`, `image` |
| `enabled` | true | |
| `invert` | false | |
| `feather` | 0 | px, Gaussian |
| `density` | 1 | 0..1 |
| `linked` | true | Moves, turns and scales with the layer; unlinked = canvas space. `layer` masks are always canvas space |
| `shape` mask | `shape: {shape: "rect", size: 200, aspect: 1, corner: 0, slant: 0, points: 5, inner: 0.5, jitter: 0, seed: 1, tail: {...}}`, `position: [0, 0]`, `rotation: 0` | The shape layer's geometry members |
| `gradient` mask | `shape: "linear"` (`linear`, `radial`), `position: [0, 0]`, `angle: 0`, `length: 200` (px), `inner: 0` (radial: opaque share), `mid: 0.5` | Opaque at the start, transparent at the end |
| `layer` mask | `layer: ""` (a layer id), `channel: "alpha"` (`alpha`, `luminance`) | That layer drawn alone with its effects (track matte); cycles draw as fully shown with problem `mask_cycle` |
| `image` mask | `image: null`, `channel: "alpha"`, `position: [0, 0]`, `size: 200`, `rotation: 0` | A texture's alpha or luminance |

#### Effect entry

`{"id", "type": "<registry id>", "enabled": true, "params": {...}}`, plus `from_look` (bool,
false; S84: on an auto group's effects, the ones a look brought). Every param of the
definition is written; a missing param on read takes the definition's default (for v1 files) or
the value a migration fills. Unknown params are kept. The S80 catalogue and every param's type,
unit, range and default are in §6.5.

#### Exposed parameters

What an instance (the Title Art Renderer, prefabs, Lua, clip tracks: S81, S85) may override,
by name (decision 13):

| Member | Default | Notes |
|---|---|---|
| `name` | | `[a-z][a-z0-9_]{0,31}`, unique; the name Lua, prefabs and tracks use |
| `label` | `""` | Inspector label; empty = `name` in Title Case |
| `type` | | `text`, `color`, `number`, `bool`, `font`, `image`, `color_scheme` |
| `targets` | `[]` | `[{"layer": "<id>", "field": "<field path>"}]` (§6.2 field paths); one value drives every target. A `color_scheme` parameter has no targets: it applies a Color Scheme to the document (S84) |
| `min`, `max` | `null` | `number` only |
| `options` | `[]` | `color_scheme` only: the scheme ids an instance may pick; empty = any |

The parameter's default is the first target's value in the document (not stored twice). A target
whose layer or field no longer exists is reported (`exposed_target_missing`) and skipped.
`edit_title` op `expose` (§6.2) creates them; a new document exposes its first text layer's
`text` as `text`.

#### Outputs

| Member | Default | Notes |
|---|---|---|
| `id` | `"main"` | `[a-z0-9_]{1,32}`, unique |
| `content` | `"all"` | `all`; `layers` (only `layers`, in place, the rest hidden); `ink` (everything but light: effects whose definition `adds_light`, Add-blended generators); `glow` (only that light); `mask` (coverage as a grey PNG) |
| `layers` | `[]` | Layer ids, `content: "layers"` only |
| `file` | `""` | Relative to the document's directory. Empty: `<file>.png` for `main` (`logo.titleart.png`), `<file>.<id>.png` for the others (`logo.titleart.glow.png`), the VAT naming (`x.vat.glb`) |
| `scale` | 1 | Bake resolution × canvas, 0.1..8 |
| `trim` | true | Crop to the content (alpha ≥ 1/255) plus `padding`; **all outputs of a document share one frame** (the union), so parts register exactly when layered |
| `padding` | 4 | px, at output scale |
| `round_to` | 4 | 1, 2, 4, 8: frame sides padded to a multiple (block-compressed textures need 4, S14 R6) |
| `bit_depth` | 8 | 8 or 16 (16 needs float readback, §6.7; refused with `bit_depth_unsupported` where the backend has none) |
| `alpha_bleed` | true | Straight-alpha PNG whose fully transparent pixels take the nearest opaque colour, so bilinear filtering and mips get no dark fringes |
| `max_size` | 4096 | Larger output side; above it the scale is reduced with problem `output_downscaled` |

### 6.2 Addressing and the document operations

One operations API, `TitleArtOps` (`editor/titleart/TitleArtOps.{h,cpp}`), is called by the Title
Designer, the Inspector, MCP and editor Lua (contract decision 12). An op batch is applied to a
copy of the document, validated, then committed as **one** project undo step; any failing op
applies nothing and returns its index, the path and a fix.

**Layer references** (`layer`): a path of names from the top level joined by `/`, each followed by
`#N` for the Nth same-named sibling after the first in document order, exactly as
`editor/util/EntityPath.h` does for GameObjects (`Logo/Title`, `Spark#2`); or `id:<8 hex>`. A name
that matches several layers at different paths is refused listing the matches.

**Field paths** (`values` keys, exposed `field`, fix ops): dot paths into the layer object with
`[i]` indexes and map keys as path segments: `opacity`, `transform.position`, `text`,
`chars.3.rotation`, `style.face.fill.gradient.stops[0].color`, `style.rims[1].width`. Shorthands,
accepted everywhere and echoed in their long form:

| Shorthand | Means |
|---|---|
| `face.` | `style.face.` |
| `rim:<id>.` / `rim#<n>.` | `style.rims[<index of that rim>].` (`rim#0` = innermost) |
| `fx:<type>.` / `fx:<type>#<n>.` | `effects[<index of the nth effect of that type, 0-based>].params.` |
| `<part>/fx:<type>.` | the same in `part_effects.<part key>` (`rim#1/fx:outer_glow.size`) |
| `mask:<id>.` / `mask#<n>.` | `masks[...]` |
| `gen.` | `generator.params.` |
| `adj.` | `adjustment.` |
| `band:<id>.` / `band#<n>.`, `3d.` | S86: `style_3d.bands[...].`, `style_3d.` |

Document members are addressed with `layer` omitted: `canvas.width`, `global_light.angle`, `seed`,
`outputs[0].scale`.

**Ops** (JSON objects with `op`; `edit_title {ops}` takes a list of them). S80 defines these; later
specs add ops, never tools:

| Op | Members | Effect |
|---|---|---|
| `set` | `layer?`, `values: {path: value}` | Sets fields; values are clamped (echoed `clamped`) |
| `set_text` | `layer`, `text`, `font?`, `keep_chars?` (default false: char adjustments past the new length are dropped) | |
| `add_layer` | `type`, `name?`, `parent?`, `before?` / `after?` (layer refs; default: top of `parent`), `values?`, `look?`, `generator?` (type, for generators), `adjustment?` (type), or `from` (a complete layer object, `type` then optional; S82's paste and cross-document drag); S83 adds `preset?`, `follow?`, `place?`, `group?` for generators | Returns the new id; with `from`, new ids for the layer and everything in it (references inside the copy remapped, references outside it dropped with a `changes` note) |
| `remove_layer` | `layer` | Also clears masks, exposed targets and `follow` that name it (each listed in `changes`) |
| `duplicate_layer` | `layer`, `name?` | New ids for the copy and everything in it |
| `move_layer` | `layer`, `parent?`, `before?` / `after?` | |
| `group` | `layers: [refs]`, `name?` | Siblings only; the group takes the topmost member's place |
| `ungroup` | `layer` | Children move to the group's place; the group transform is folded into them |
| `add_effect` | `layer?`, `part?`, `type`, `params?`, `index?` (default: end) | |
| `remove_effect` / `move_effect` | `layer?`, `part?`, `effect` (`fx:<type>[#n]` or `id:`), `index` (move) | |
| `add_rim` / `remove_rim` / `move_rim` | `layer`, `rim` (`rim#n` / `rim:<id>`), `index?`, `values?` | |
| `add_mask` / `remove_mask` | `layer`, `type` / `mask`, `values?` | |
| `expose` / `unexpose` | `name`, `type`, `targets` / `name` | |
| `link_material` / `unlink_material` | `layer`, `material` (field path of a Material), `link` (`name?` for a new link) | |
| `apply_look`, `apply_color_scheme`, `apply_template` and the other reserved ops of §6.15 | (their owner spec) | Reserved names; S80 returns `not_available` naming the owner spec until it lands |

Commit-time invariants (`TitleArtDocument::commit`, the Pachislop `commit` hook, report p1 §5.2):
`normalizeTitleArt`, then **link sync** (the first material of a link group changed in this commit,
in layer then part order, is copied to the others in the same step), then tree checks (clip with no
base cleared, dead part keys and mask/`follow`/exposed targets reported). Editors never reason
about links or tree shape.

### 6.3 Renderer architecture

**Layout** (`engine/core/titleart/`, compiled only with `JASPERENGINE_TITLE_ART`):

| File | Holds |
|---|---|
| `TitleArtDoc.h`, `TitleArtIO.*`, `TitleArtMigrations.*`, `TitleArtHash.*` | The document structs, codec, migrations, per-node content hashes |
| `TitleEffectRegistry.*`, `effects/*.cpp`, `generators/*.cpp` | §6.5 |
| `text/TitleTextLayout.*`, `text/GlyphOutlines.*` | §6.6 |
| `geometry/Path.h`, `Warp.cpp` (Patchy), `Distort.cpp`, `Remap.cpp`, `Offset.cpp` (Clipper2), `Plate.cpp`, `Shapes.cpp`, `SvgPaths.cpp` (tinyxml2), `MaskRaster.cpp` (ThorVG) | Outlines and coverage masks |
| `render/TitleArtRenderer.*`, `render/TitleRenderJob.*`, `render/DrawTree.*`, `render/TitleFrame.h`, `render/TitleTargetPool.*`, `render/TitleArtCaps.*`, `render/Compositor.cpp`, `render/Title2DPass.cpp`, `render/FieldPasses.cpp`, `render/Masks.cpp`, `render/Adjustments.cpp`, `render/LayerSources.cpp` | The renderer |

Shaders follow the existing layout: `engine/shaderlib/titleart.vert` and `titleart.frag`, includes in
`engine/shaderlib/titleart/` (`common.glsl`, `blend.glsl` (adapted from `fam_sim_blend.glsl`),
`color.glsl`, `fields.glsl`, `element.glsl`, `patterns.glsl`, `effects.glsl`, `adjust.glsl`), one
appended `ShaderType::TITLE_ART` whose property bits select the program (`TA_PROGRAM_*` defines),
as `ShaderType::FLUID` does. Pipelines are dynamic (`PipelineState`), cached by
`PipelineState::hash()` plus the program.

```cpp
namespace jasperengine::titleart {
    struct FieldOverride { std::string layerId; std::string path; nlohmann::json value; };   // layer "" = document
    struct GlyphCellRequest { std::string layerId; uint32_t cluster = 0; enum Part : uint8_t { Back, Front } part; };
    struct RenderRequest {
        std::shared_ptr<const TitleArtDoc> doc;     // immutable snapshot (what-if copies are snapshots too)
        float pxScale = 1.0f;                       // output pixels per canvas pixel
        RectI region;                               // canvas pixels, y down from the canvas top-left; empty = canvas
        std::string isolateLayer;                   // render one layer alone, with its effects and masks
        OutputContent content = OutputContent::All; // All, Layers, Ink, Glow, Mask
        std::vector<std::string> contentLayers;
        std::vector<FieldOverride> overrides;       // applied to a copy of the snapshot: S81 exposed values,
                                                    // S85 sampled poses (render at a pose), S87 resolved languages
        std::vector<std::string> keepSplits;        // layer ids whose own composite is kept as a split (S81, S85)
        std::optional<GlyphCellRequest> glyphCell;  // one glyph of any text layer alone (S81 Glyph Atlas, S85 cells)
        int tier = 0;                               // 0 = bake quality; S86's TitleTier for Realtime 3D
        bool includeGuides = false;                 // the editor's views draw guides; outputs never do
        uint32_t testFlags = 0;                     // TITLE_ART_TEST_*: per request, never global
        AssetResolver* assets = nullptr;            // fonts, images, SVGs by AssetRef (editor: AssetDatabase or
                                                    // S82's TransientAssetResolver; game: S81)
    };
    struct RendererLimits { size_t poolBytes = 512u << 20; size_t nodeCacheBytes = 128u << 20; int maxSide = 8192; };

    class TitleArtRenderer {
    public:
        explicit TitleArtRenderer(const RendererLimits& limits = {});
        std::unique_ptr<TitleRenderJob> begin(const RenderRequest& request);
        const RendererStats& stats() const;     // passes, bytes live, cache hits, path (float / rgba8)
        void trim();                            // frees idle targets
    };
    struct StepBudget { uint32_t budgetUs = 0; uint32_t maxPasses = 0; };   // 0 = unlimited
    class TitleRenderJob {
    public:
        bool step(const StepBudget& budget, StepReport* report = nullptr); // runs whole steps while their
                                                // estimates fit (at least one); true when done; false + error() on failure
        bool done() const;
        float progress() const;                 // estimated cost done / total (S81 R12's 50 % rule)
        uint32_t passesDone() const; uint32_t passesTotal() const;          // S82's progress readout
        uint32_t nextStepCostUs() const;        // pixels × the pass's costWeight (ns/px, desktop reference)
        void splitOversizedSteps(uint32_t maxUs);   // GPU steps into scissor tiles >= 256 x 256
        std::vector<ShaderKey> shaderKeys() const;  // every program variant the job will bind (S81 R18, R47)
        std::vector<NodeCost> nodeCosts() const;    // per layer: passes, estimated and measured us (S82 R71)
        const std::string& error() const;
        const TitleImage& image() const;        // texture (output format), frame in canvas pixels, alpha bounds
        const TitleSplits& splits() const;      // the kept layer splits, the glow split, glyph-cell back/front
        const std::vector<Problem>& problems() const;   // glyph_missing, asset_missing, unknown_type, mask_cycle...
    };
}
```

The job is a flat ordered step list (no graph, the do-not-copy list): CPU stages (shaping,
outlines, ThorVG masks, S83's analysis, S86's mesh units) and GPU passes, each with its cost
estimate. Every pass definition carries `costWeight` (ns per output pixel on the desktop
reference); S83's cost model (§6.5 `estimate`) supplies the weights of its definitions and S81
scales them per platform. The bake runs the same job to the end in one call. Overrides are
resolved before the draw tree, so a pose or an exposed value hashes like a document edit and
reuses every cache an unchanged member allows (a colour override never recomputes masks).

**Frame space and the origin pitfall** (report p1 §4.2). Pachislop's shaders assume a bottom-left
origin and y-up `gl_FragCoord`. Here **every title-art target is addressed in memory rows on every
backend**, the fluid convention (`fluid.frag:11-14`): `gl_FragCoord.xy` is (column, row) with row 0
the first memory row, which is the top row of the PNG after readback. `TitleFrame` (CPU) is the one
place that maps document space (y-up, centred) to frame pixels: `x_px = (x + W/2) * s - region.x`,
`y_px = (H/2 - y) * s - region.y`. Every Pachislop formula that uses a direction goes through
`ta_dir(angle_deg, convention)` in `titleart/common.glsl` (conventions `cw_from_right`,
`ps_light_ccw`), and the seed pass's Sobel gradient, the bevel normal's y and the reflection
vector's y are negated once in `fields.glsl`/`element.glsl`, each with a comment saying why. Only
the editor's display of a target (an ImGui image) flips, never a pass. Golden `TitleArtGpu.OriginAsymmetric`
(an "F", a top-to-bottom gradient, a shadow falling down-right) fails on any flip.

**Draw tree** (`DrawTree.cpp`, Pachislop's `drawTree`): a pure function from a document snapshot and
the request to `DrawNode`s (`leaf`, `group {children}`, `adjust`; each with `clipped[]`) with the
single visibility rule: a layer draws when `visible`, `opacity > 0` and (`includeGuides` or not
`guide`); a hidden group hides its subtree; an empty group is dropped; the clip base is the nearest
sibling below with `clip` false; clipped layers of a hidden or empty base hide; a clip with no base
or an adjustment base is ignored. Previews, bakes, thumbnails, checks and S81 all go through it.

**Pass order for one node** (report p1 §2.4):
1. **Content.** A leaf's renderer draws into the node target (position, rotation, scale applied;
   opacity and blend not). A 2D text or shape layer with part effects renders in runs, back to
   front: consecutive parts without part effects together, a part with effects alone through its
   stack, runs combined with Normal. A group composites its children recursively on a cleared target.
2. **Effects** in order; nothing enabled = the same target, no pass.
3. **Clip group**: starts from the base's post-effect pixels; each clipped node composites with its
   own blend, opacity and effects, multiplied by the base's content alpha **before** its effects
   (part effects included); clipped adjustment layers adjust inside that alpha.
4. **Masks** multiply the whole result (premultiplied), so they also cut glows and the clip group.
5. **Blend and opacity** onto the parent.

**Targets** (`TitleTargetPool`, the one pool name of the program): one pool keyed by (width,
height, format, filter, depth format, sample count); `TargetRef` is a move-only RAII handle that
returns its target on destruction (no manual release). Contents are undefined on acquire; a pass
that needs a clear clears. The 2D passes request neither depth nor MSAA; S86's 3D layer passes
request depth (and MSAA where its tier says) through the same pool and hand plain colour targets
back to the compositor. S81's Realtime intermediates and S86's bake targets come from this pool. A target unused for 3 complete renders is freed; the pool never exceeds
`RendererLimits::poolBytes` (an acquire beyond it frees idle targets first, then fails the render
with problem `out_of_target_memory`, never an exhaustion of the engine's GPU pools).

**Regions of interest** (R13). For each node, bounds = content bounds (glyph and shape outline
bounds after transform, image and SVG quads, generator `bounds(params)`; a group the union of its
children) grown by the sum of its effects' `reach(params) × px_scale` (part effects on their part's
bounds) and by 1.5 × feather of its masks, unioned with its clip group's own reach inside the base,
intersected with the render region and aligned outward to 64 px from the region origin (so blur
pyramids keep the same texel grid whatever the region, Pachislop's `ALIGN`). The node target has the
ROI's size; every pass sets viewport and scissor to it. An empty ROI skips the node (0 passes).
Nodes whose definitions say `fills_canvas`, adjustment layers and anything that reads the backdrop
use the whole region. **Light past the canvas edge is cut there**: the canvas is the frame of every
output; `check_title`'s `not_clipped` warns when ink or light touches it (Pachislop's enlarged
export frames are not ported).

**Content hashes and caches** (R14). `TitleArtHash` computes per layer, once per commit and on
load (never per frame), 64-bit FNV-1a hashes over the canonical text of: `geometry` (source members,
font/image/SVG content revisions, layout members, outline ops), per element `mask` (geometry + rim
widths, joins, plates, erode), `field` (mask + bevel/inner-glow/satin sizes and profiles), `stack`
(fields + materials + rotation, flips and `global_light`: the bevel light stays fixed on screen,
so rotation re-shades), `effects`, `masks`, and `node` (all of these, children, clip group, `seed`).
Cascade for 2D titles: geometry (CPU outlines) → masks (R8) → fields (SDF, cover, height, satin) →
stack (frame space) → placement (one pass into the node target). Node pixels are kept in an LRU
(`nodeCacheBytes`) by (node hash, px_scale, region); a node whose hash and placement match blends
from the cache in one pass. A font, image or SVG change bumps its content revision, which reaches
every hash that names it. The cache is per layer node, so a re-render that changes one layer (an
edit, an override, S85's re-render tracks) recomposes the others from the cache. Device loss (`SystemRender::getContextId()` changes) clears pool, caches
and pipelines (R19).

**Formats and the RGBA8 fallback** (`TitleArtCaps`, FluidSimulator's pattern; flags
`TITLE_ART_TEST_NO_FLOAT_TARGETS`, `_NO_FLOAT_FILTER`, `_NO_R16F`, `_NO_RGBA32F`, `_NO_FLOAT_BLEND`, read per request; S81 R21 and S86 R41 add theirs to the same per-request set):

| Buffer | Preferred | Fallback chain |
|---|---|---|
| Accumulators, node, effect and stack targets | RGBA16F, linear premultiplied (needs render + filter) | **SRGB8A8** premultiplied linear: the hardware encodes on write, decodes on read and blends in linear (core in GLES3/WebGL2, D3D11, Metal, Vulkan; sokol enables `GL_FRAMEBUFFER_SRGB` for GL core offscreen passes, `sokol_gfx.h:11671-11676`), keeping dark precision; values above 1 clip |
| Element coverage | R8 | R8 |
| Jump-flood seeds (x, y, valid) | RGBA32F, nearest | RGBA8: x and y as 12.4 fixed point in two bytes each (frames ≤ 4,096 px, Pachislop's own cap), 0xFFFF = no seed |
| Signed distance | R16F | RGBA8, 16-bit fixed point in RG (1/64 px steps, ±512 px, clamped beyond) |
| Height, Fit cover radius | R32F, nearest | R16F → RGBA8 16-bit fixed in RG |
| Satin blur, combined layer mask | R16F, linear | R8, linear |
| Gradient, curve LUTs | RGBA8 256 x 1 (sRGB stops as Pachislop's code does) | same |
| Tone LUTs (levels, curves, brightness/contrast) | RGBA16F 256 x 1 | RGBA8 |

Each buffer takes the first format the device can render (and filter where it is sampled linearly);
the stats name the path. **Tolerance** (AC17): on every S80 golden, the forced fallback's 8-bit output
against the float path's: mean ≤ 1.0/255, at most 0.5 % of pixels over 8/255, max ≤ 48/255
(premultiplied comparison); fixtures that attenuate light above 1 after it is drawn (a glow under a
later 50 % opacity) are listed in the test with a measured bound instead, and S80's set has none.

### 6.4 Blend modes and compositing

| `blend` | UI label | | `blend` | UI label |
|---|---|---|---|---|
| `normal` | Normal | | `color_dodge` | Color Dodge |
| `add` | Add (Linear Dodge) | | `color_burn` | Color Burn |
| `screen` | Screen | | `linear_burn` | Linear Burn |
| `multiply` | Multiply | | `difference` | Difference |
| `overlay` | Overlay | | `exclusion` | Exclusion |
| `soft_light` | Soft Light | | `hue` | Hue |
| `hard_light` | Hard Light | | `saturation` | Saturation |
| `darken` | Darken | | `color` | Color |
| `lighten` | Lighten | | `luminosity` | Luminosity |

- Enum values 0..17 in this order, the order of `fam_sim_blend.glsl`'s modes, whose formulas
  `titleart/blend.glsl` adapts: premultiplied inputs are unpremultiplied (alpha > 0), the mode
  runs on **linear straight colour** (Pachislop's choice for its five modes, so ported looks
  match), and the result is composed premultiplied: `co = s·(1 − a_b) + b·(1 − a_s) + a_s·a_b·B(cb, cs)`.
  Modes whose formula uses `1 − x` (screen, overlay, soft/hard light, dodge, burn, exclusion) clamp
  their colour inputs to 0..1; normal, add, multiply, darken, lighten and difference keep values
  above 1. Photoshop blends in sRGB-encoded space, so overlay pivots differently: documented in
  `docs/title-art.md`, and an sRGB blend option is not offered in v1 (§9).
- **Normal and Add use fixed-function blending** straight into the parent target inside the ROI
  scissor (`BlendMode::PREMULTIPLIED`, `ADDITIVE_PREMULTIPLIED`; opacity, clip alpha and nothing
  else multiplied in the drawing shader): no ping-pong, no extra target, the cheapest path on tilers.
  Where the target is RGBA16F and `blendRGBA16F` is false they take the shader path.
- **Every other mode** copies the parent's ROI rectangle to a scratch target and draws
  `mode(backdrop, source)` over the ROI with blending off: two passes, region-sized.
- **Light and max blending** (fixed function, added to `BlendMode` in `Render.h` by S80 and mapped
  in `SokolPipelineCache.cpp` and `SokolObject.cpp` on all five backends): `LIGHT_PREMULTIPLIED`
  (colour ONE/ONE, alpha ONE/ONE_MINUS_SRC_ALPHA: light adds colour and unions alpha, where
  `ADDITIVE_PREMULTIPLIED` keeps adding alpha) and `MAX_PREMULTIPLIED` (`BlendOperation` gains
  `MAX`; all factors ONE, colour and alpha). S80's Add-blended light effects use the first; S83's
  passes use both (lightning cores, drips). Test `BlendModesGpu.LightAndMax`.
- `disjoint` (internal, not in the enum): `o = s + d·min(1, (1 − s.a)/d.a)`, for coverage-
  partitioned part runs (S86's 3D parts).
- **Adjustment layers** (Pachislop's `adjustOnto`): an identity adjustment runs only its effects on
  the parent buffer (nothing enabled: zero passes). Otherwise the operation runs on unpremultiplied
  colour clamped to 0..1 and sRGB-encoded (Photoshop parity; **an adjustment layer clips values above
  1**, documented), then its effects, then a mix by `opacity × clip alpha × mask` with alpha following
  the effects; with opacity 1, Normal, no clip and no mask the result replaces the parent. Curves,
  levels and brightness/contrast use a 256-entry LUT baked on the CPU per adjustment hash; the others
  are closed form (Pachislop's `adjustments.ts`). The contrast pivot is fixed at 50 %.
- **Ink vs light** (report p1 §2.8): definitions declare `adds_light` and `outline`. Only when an
  `outline` effect follows an `adds_light` one in the same stack does the runner keep a parallel ink
  buffer (non-light effects in between run twice); the outline reads the ink and draws under the
  light. A group's ink omits children that hold light and Add-blended generators. Nothing extra runs
  otherwise.
- **Layer sources** (masks of type `layer`, `follow` generators): the source layer is rendered alone
  once per render (its ancestors shown, guides drawn), shared both ways with the main pass, keyed by
  its node hash; a source that is being rendered (a cycle) yields none and problem `mask_cycle` or
  `follow_cycle`.
- **Masks**: per enabled mask, rasterize (shape: ThorVG R8; gradient: analytic, `t^γ` with
  `γ = log 0.5 / log mid`; layer: the layer source, alpha or sRGB-encoded Rec.709 luminance × alpha;
  image: the texture), extract the value, feather (Gaussian, radius `feather × px_scale`), invert,
  then density `v = 1 − density·(1 − v)`, multiply into the combined mask (R16F, fallback R8),
  cached by (masks hash, placement when linked, source hashes).

### 6.5 The effect and generator registry

**Decision: one new declarative C++ table in the engine, `TitleEffectRegistry`
(`engine/core/titleart/TitleEffectRegistry.h`).** Why neither existing table:
- `editor/reflect` (S08) is editor-only and describes ECS component fields; a game running a
  Realtime title (S81) needs defaults, units, `reach` and validation in the player, and title effects
  are per-instance polymorphic parameter sets inside a document, not component fields.
- The `@effect` manifest (`engine/shaderlib/effects/*.frag` headers) describes one HTML UI shader
  with one uniform block; a title effect is multi-pass C++ (jump flooding, pyramids, ink buffers)
  with behaviour the comment syntax cannot express (`reach`, `gather`, `adds_light` as functions of
  params) and parameter kinds it lacks (gradient, curve, adjustment, asset, layer).
- It is not a third schema for the same thing: nothing else describes title effects. The S81
  component's own fields go through `editor/reflect` as every component does.

```cpp
namespace jasperengine::titleart {
    enum class ParamKind : uint8_t { Number, Integer, Bool, Enum, Color, Gradient, Curve, Vec2, Asset, Layer,
                                     Adjustment, String, Material, RowArray };   // the last three for S83
    enum class ParamUnit : uint8_t { None, Px, Percent, Deg, Factor };   // Px scales with px_scale (decision 5)
    // the program's shared classifications: S80 declares them, S83 fills them for the catalogue
    enum class FxAnim : uint8_t { None, Live, Rerender, Step, Relayout };  // animation class of a param or a
                // member (S85 R11): Live = changes without a re-render when the definition has a live variant;
                // Rerender = smooth re-render; Step = stepped re-render; Relayout = re-runs layout/analysis;
                // None = not animatable (ids, structure)
    enum class ParamInterp : uint8_t { Smooth, Stepped };   // how keys interpolate (a Live seed is Stepped)
    enum class ParamCombine : uint8_t { Replace, Multiply }; // S85 relative tracks: intensities multiply
    enum class CostClass : uint8_t { C0, C1, C2, C3, C4 };   // S83 §6.4: trivial .. bake only
    enum class FallbackClass : uint8_t { F0, F1, F2 };       // S83 §6.5: RGBA8 agreement class
    enum class LiveKind : uint8_t { None, ScaleSplit, Kernel };   // S83 D10; run by S81's live compose
    enum class GlyphSafety : uint8_t { Safe, Approximate, Unsafe };   // S81 R31's Glyph Atlas classes
    enum class FxTier : uint8_t { Desktop, Mobile };
    struct ParamDef {
        const char* key;            // snake_case: the stored key, the MCP path segment, the Lua name
        const char* label;          // Inspector label, Unity-like ("Size", "Use Global Light")
        ParamKind kind; ParamUnit unit = ParamUnit::None;
        double min = 0, max = 0, step = 0;
        nlohmann::json defaultValue;
        std::vector<EnumOption> options;    // {value, label}
        When when;                  // shown while another param has one of these values (hidden ones still apply)
        const char* description;    // one line, for docs, tooltips and list_title_library
        FxAnim anim = FxAnim::Rerender;             // S85's class source; marks L/R/S/X in S83's tables
        ParamInterp interp = ParamInterp::Smooth;   // Stepped for enums, bools, ints, seeds
        ParamCombine combine = ParamCombine::Replace;
        bool light = false;                         // a colour of light: S84's schemes map it onto light_ramp
    };
    struct FxInputs { bool input = true, content = false, sdf = false; AnalysisSource analysis = AnalysisSource::None; };
    struct LiveVariant { LiveKind kind = LiveKind::None; std::vector<const char*> params; uint32_t program = 0; };
    struct SpectrumSwitch { const char* param = nullptr; const char* from = nullptr; const char* to = nullptr; };
    struct EffectDef {
        const char* type; const char* label; const char* category; const char* description;
        std::vector<ParamDef> params;
        std::vector<GizmoHint> gizmos;              // direction (angle [+ length]), point, light (angle + altitude): S82's canvas
        float (*reach)(const ParamValues&);         // canvas px it may draw outside the ink
        float (*gather)(const ParamValues&);        // how far each output pixel reads; < 0 = the whole layer
        bool (*fillsCanvas)(const ParamValues&);
        bool (*addsLight)(const ParamValues&);
        bool outline = false;                       // traces ink
        bool onParts = true;                        // allowed in part_effects
        CostClass costClass = CostClass::C1;        // at defaults; list_title_library's `cost`
        FallbackClass fallback = FallbackClass::F0;
        LiveVariant live;                           // S81's live compose runs it (never title_fx)
        GlyphSafety glyphSafety = GlyphSafety::Approximate;
        std::vector<const char*> aliases;           // searched by menus, search_editor, list_title_library
        FxInputs inputs;
        SpectrumSwitch spectrum;                    // S84's spectrum schemes
        FxCost (*estimate)(const ParamValues&, const FxSize&, FxTier) = nullptr;   // S83's static cost model;
                                                    // null = one pass of costWeight 1 per output pixel
        void (*analyse)(AnalysisContext&, const ParamValues&, AnalysisResult&) = nullptr;   // S83, CPU job
        const char* presetFile = nullptr;           // editor/resources/titleart/fx-presets/<type>.yaml (S83)
        uint32_t passVersion = 1;                   // bumped with its pass code; part of the bake version (§6.9)
        void (*apply)(EffectContext&, const TargetRef& input, TargetRef& output, const ParamValues&);
    };
    struct GeneratorDef {   // the same params, plus
        const char* type; /* ... */ RectF (*bounds)(const ParamValues&);   // empty = the canvas
        FollowMode follow; float (*followReach)(const ParamValues&);
        void (*render)(GeneratorContext&, TargetRef& target, const ParamValues&);
    };
    const EffectDef* findEffect(std::string_view type);
    const GeneratorDef* findGenerator(std::string_view type);
    // the Photoshop "Use Global Light" decorator (Pachislop's lit()): adds use_global_light after
    // the angle param and substitutes global_light in apply(); relation From, Away or Floor
    EffectDef withGlobalLight(EffectDef def, const char* angleKey, LightRelation relation);
}
```

`GeneratorDef` carries the same classification members. S80's own twelve definitions declare, at
defaults: `color_overlay`, `gradient_overlay`, `pattern_overlay`, `adjust`, `fill`, `pattern_fill`
C0 / F0; `outer_glow`, `inner_glow`, `drop_shadow`, `inner_shadow`, `outline`, `gaussian_blur`
C1 / F1; glyph safety, live variants and animation classes as S83 §6.12 and §6.13 list them (S83
lands those values in wave 6B and its `TitleFxBudget.StaticModel` checks the classes). Before S83
lands, every S80 param is `Rerender` (numbers, colours) or `Step` (the rest) and has no live
variant.

One definition drives: document defaults, read-time fill and clamping (R1); `TitleArtOps` validation
and did-you-mean (`fx:` paths); the Inspector rows (S82, one switch over `ParamKind` honouring
`when`, gizmo hints for the canvas); `list_title_library {kind: "effects"}` and its `id` detail;
`docs/mcp/guide/title-art-effects.md`, generated by `tools/titleart/gen_effect_docs.py` (from
`jasperengine-editor call list_title_library`, with `--check` in `test_docs.py`); the S81 Lua
names (`type` ids and param keys); and the golden list (every effect at defaults on the standard
title, every generator alone: a definition without a reference fails, a reference without a
definition fails).

**The S80 catalogue** (params ported from Pachislop one to one, renamed snake_case, same defaults,
ranges and units; S83 adds the rest):

| `type` | UI name | Params (default; unit; range) | Flags |
|---|---|---|---|
| `outer_glow` | Outer Glow | `fill` `solid` (`solid`, `gradient`, `layer`, `distance`); `color` `#ffb000`; `gradient` (glow ramp); `angle` 90 deg; `size` 40 px 1..400; `intensity` 1.6 0..6; `spread` 0.15 0..1; `opacity` 1; `mode` `behind` (`behind`, `add`); `on_face` 0.15 0..1 (when `add`); `technique` `softer` (`softer`: pyramid blur, `precise`: distance field) | reach `size`; adds light when `add` |
| `inner_glow` | Inner Glow | `fill` `solid` (`solid`, `distance`); `color` `#fff4a0`; `gradient`; `size` 18 px 1..200; `intensity` 1 0..4; `choke` 0; `opacity` 1; `mode` `screen` (`normal`, `screen`, `add`); `source` `edge` (`edge`, `center`); `technique` `softer` | gather 1.5 × `size`; adds light when `add` |
| `drop_shadow` | Drop Shadow | `angle` 60 deg (falls, clockwise); `use_global_light` true; `distance` 10 px 0..500; `size` 14 px 0..300; `spread` 0.1 0..0.95; `color` `#000000`; `opacity` 0.75 | reach `distance + size` |
| `inner_shadow` | Inner Shadow | `angle` 60; `use_global_light` true; `distance` 6 px 0..300; `size` 8 px 0..200; `choke` 0 0..0.95; `color` `#000000`; `opacity` 0.6 | |
| `outline` | Outline (Pachislop's Keyline) | `width` 1.5 px 0.5..100; `color` `#0a0604`; `opacity` 1; `softness` 0 0..1; `threshold` 0.5 0.05..0.95; `follow` `layer` (`layer`, `stack`) | outline; reach `width`; draws under light |
| `color_overlay` | Color Overlay | `color` `#ff0000`; `opacity` 1; `mode` `normal` (the 18 modes) | new |
| `gradient_overlay` | Gradient Overlay | `gradient` (gold bands); `style` `linear` (`linear`, `radial`, `sweep`); `angle` 90; `scale` 1 0.1..10; `offset` 0 −2..2; `length` 200 px (sweep); `position` 0 % −100..100 (sweep); `wrap` `clamp` (`clamp`, `repeat`, `mirror`); `amount` 1; `mode` `normal` (the 18 modes, `light` (add), `shade` (multiply)) | |
| `pattern_overlay` | Pattern Overlay | `pattern` `stripes` (the Material pattern kinds but `none`); `color` `#000000`; `color2` `#ffffff`; `scale` 40 px; `angle` 0; `seed` 1; `image` null (`custom`); `opacity` 1; `mode` `normal`; `align` `layer` (`layer`, `canvas`) | new |
| `gaussian_blur` | Gaussian Blur | `radius` 8 px 0..500; `scale_x` 100 % 0..200; `scale_y` 100 %; `repeat_edges` true | reach 3 × radius × max scale |
| `adjust` | Adjust | `adjustment` (Adjustment object, default `hue_saturation` with saturation 25) | gather 0 |

Generators: `fill` (Fill: `kind` `solid` (Material fill kinds but `image`, `source`), `color`
`#202020`, `gradient`, `angle` 90, `scale` 1, `offset` 0; fills the canvas) and `pattern_fill`
(Pattern Fill: Pachislop's `patternFill` params, snake_case; fills the canvas).

### 6.6 Text and fonts core

- **Faces.** `GlyphOutlines` opens each `FontFace` once per document (shared, as today) and makes,
  per (face, `variations`), a HarfBuzz sub-font (`hb_font_create_sub_font` +
  `hb_font_set_variations`), owned by the renderer, so the shared face stays immutable. Shaping and
  outlines use the same sub-font, so a variable font's advances match its outlines. `.ttc` members by
  `face_index`. Axes the font lacks are reported (`variation_axis_unknown`), clamped to the axis range
  otherwise.
- **`TextShaper` additions** (`engine/core/util/TextShaper.h`): `ShapeOptions::direction`
  (`Horizontal`, `Vertical`: HarfBuzz TTB, which applies `vert`/`vrt2`), `features`
  (`std::vector<hb_feature_t>`-equivalent tag/value pairs), and an optional per-face `hb_font_t*`
  override list. Existing callers are unchanged (defaults reproduce today's behaviour; the
  `TextShaper` unit tests stay green).
- **Fallback chain:** `font`, `fallback_fonts` in order, then the built-in face; per grapheme cluster
  as `TextShaper::selectClusterFace`. A cluster no face covers is a missing glyph (R23): the
  primary's `.notdef` advance keeps the layout, the problem lists the code points and the project fonts
  that cover them (from a per-font coverage cache).
- **Outlines** via `hb_font_draw_glyph` into `geometry::Path` (M/L/Q/C/Z in em units, then layer
  space, y-up, the font's own winding); CFF and CFF2 cubic outlines kept as cubics.
- **Horizontal layout** (Pachislop's `layoutText` plus shaping): per character `s` = line scale ×
  char scale and the `h_scale`/`v_scale` products; advances, kerning (GPOS through HarfBuzz) and
  `tracking` (thousandths of an em at that character's size) use the character's own factors, so a
  smaller character reflows the line; `proportional` applies to kana, CJK punctuation and full-width
  forms through `palt` when the font has it, else fits the advance to the ink box + 5 % of the em each
  side; line boxes `line_height × v` tall; `align` per line; `lines` offsets.
- **Vertical layout:** columns top to bottom, right to left; glyphs from TTB shaping (vertical
  forms); the column cell is one em (× `v_scale`), centred, with `vpal` (or fit-to-ink) when
  `proportional > 0`; column pitch from the mean of adjacent widest characters × `line_height`.
  **Tate-chu-yoko:** runs of 1..`tate_chu_yoko_max` half-width characters are set upright across the
  column, compressed horizontally to one em if wider, ink centred in a cell as tall as its tallest
  character; longer runs get one upright cell per character; Latin is never rotated sideways.
- **Per-character transforms** after placement: rotation about the em-cell centre and offset, in
  pre-shear space; then `shear` about the block's centre line; then centring on the **frame**, which
  excludes per-character adjustments (moving one glyph moves no other, and warps use the frame).
- **Outline operations:** warp (Patchy's 15 Bezier-patch styles, ported from upstream Patchy MIT,
  recentred so the layer does not drift as `bend` changes) → distort (`corners`: projective map with
  denominator floor 0.02; `mesh`: bicubic patch with a C1 linear extension outside the frame) →
  curve refit (segments ≤ max(w, h)/8, quadratic through the mapped midpoint, error tested at ¼ and ¾,
  tolerance max(1.5e−4 × size, 1e−3), depth ≤ 12) → weight (Clipper2: flatten with tolerance
  max(5e−5 × size, 1e−3) px, union non-zero, `InflatePaths` round join, precision 0.001 px; strokes
  thinner than 2|δ| vanish). Each stage is memoised on its input hash.
- **Shapes** (`Shapes.cpp`, Pachislop's `shapes.ts`): clockwise y-up outlines, true circular corners
  (`k = 4/3·tan(φ/4)·r`) clamped to half an edge, seeded burst jitter, speech/shout tails spliced into
  one contour, ribbons as three same-winding contours.
- **Vector (SVG):** LunaSVG's public API renders but exposes no path data, and the vendored ThorVG
  has no SVG loader (`engine/libs/thorvg/CMakeLists.txt:4`), so `shape` mode reads geometry with
  `SvgPaths.cpp` over tinyxml2 (always linked): path data (the whole `d` grammar, arcs converted to
  cubics), `rect`, `circle`, `ellipse`, `line`, `polyline`, `polygon`, `g` nesting, `transform`,
  local `use`, `fill-rule`, `stroke`, `stroke-width`, `stroke-linejoin`, `stroke-linecap`,
  `stroke-miterlimit`, `display`/`visibility`, and those properties from `style` attributes and
  simple class rules. Fills become outlines; strokes stay strokes, widened by grow in rim masks.
  Anything else (text, filters, clip paths, masks) is ignored with problem `svg_feature_ignored`.
  `artwork` mode rasterizes the file with LunaSVG (`Document::renderToBitmap`) at the exact frame
  scale into the element's colour source.
- **Layout and geometry queries** (`TitleTextLayout` and `GlyphOutlines`, public, used by S81's Glyph
  Atlas, S82's canvas and S87's fit): `layout(layer, overrides) -> LaidOutText` with per grapheme
  cluster `{index, codepoints, line, face, glyphs, pen, advance, ink box, em cell}` in document
  space (y-up; S82's caret, selection and per-glyph gizmo, S81's cell placement, S85's stagger
  units), the frame and line boxes; `outlines(layer)` the final outline paths per glyph after warp,
  distort and weight (S86's 3D input, S87's SVG export and fit); `rimBands(layer)` the cumulative
  grow width of each enabled rim (S82's part picking: inside the outline = Face, within rim k's
  cumulative band = Rim k+1); `contentBounds(layer)` and `reachBounds(layer)` (content bounds plus
  the effects' reach, the ROI rule of §6.3). All are pure CPU functions of a snapshot and cached
  with the geometry hash.
- **Coverage masks** (`MaskRaster.cpp`): ThorVG software canvas at frame resolution, non-zero fill;
  a rim element adds a stroke of width 2 × grow with its join (miter limit 4); the alpha becomes the R8
  coverage. `fill_counters`: 4-connected flood from the border through alpha < 128, every unreached
  pixel opaque (closes pockets between letters as well, the Illustrator Offset + Divide look). Each
  glyph keeps `all` and `outer` contour sets (outer = contours bounding the region from outside,
  reoriented CCW) for solid plates. Images and `artwork` SVGs grow rims through their distance field
  (round joins only).

### 6.7 The 2D title passes

The title frame encloses every element's base bounds plus pad (miter: grow × 4, else max(grow, close),
plus 3 px), at `m = layer scale × max(px_scale, 1)` mask px per layer unit, so a 0.5× preview shades
exactly like a 1× bake and is filtered down when placed. Caps: 4,096 px per side and 6,000,000 px, then
`m` is reduced (problem `title_frame_reduced`). Per element, each step cached by its hash (§6.3):

| # | Pass | Target | Notes |
|---|---|---|---|
| 1 | Geometry | CPU | §6.6 |
| 2 | Coverage mask | R8 | ThorVG; flood fill for `fill_counters` |
| 3 | Erode (if enabled) | R8 | MAX union of all masks → its distance field → keep-mask (lanes, dryness, `exitDist` march ≤ 96 steps + 6 bisections) → mask × keep, **before** the element fields |
| 4 | Distance field (bevel, inner glow, shape burst, Outline on the part, or Fit) | seeds + floods + resolve | Seeds: partial coverage (0.003..0.997) or hard edges, Sobel normal, seed `p − n·(a − 0.5)`. Floods from `2^ceil(log2(R + 2))` down to 1 plus one extra step-1 pass (JFA+1), R per R27; unbounded R = the frame side. 9 `texelFetch` per pixel per pass |
| 5 | Fit cover (faces with `size_mode: fit`) | circle JFA → radius → 2 blurs | σ = clamp(0.8 × unit, 1.5, 32) |
| 6 | Height | profile(`inside / size`) then 2 blurs | Profiles smooth, button, mesa, chamfer, ridge, cove, stepped, custom (PCHIP LUT, `texelFetch` + manual lerp); σ = clamp(soften_px / 2, 1, 32); rims in Fit are tubes |
| 7 | Shape burst max | 8 x 8 max reductions to 1 x 1 | no readback |
| 8 | Satin | 2 blurs of the mask | |
| 9 | Span map (`line`/`char` gradients) | nearest glyph index | once per geometry + frame |
| 10 | Shade, outermost first | stack (frame) | knockout punch first when on; `element.glsl` order below |
| 11 | Place | node target | inverse transform per fragment, `min(4, ceil(mask px / target px))` taps per axis |

Blur loops take the radius as a uniform (no fixed −96..96 loop) and switch to a pyramid above
σ = 6 (`lib.ts:blur`'s levels, ≤ 24 taps per side per level).

**Element shading order** (`element.glsl`, Pachislop's `ELEMENT_FRAG`; colour maths on sRGB-encoded
values like Photoshop, output linear premultiplied): pattern sample and relief → normal from bevel
slope (`K = bevel_px × depth × 0.35`, resolution-independent) + pattern relief + `surface` texture slope
→ refraction offset → fill (8 kinds + `source`, spans, analytic seam AA) → glass → reflection map
(screen-space reflect, equirect lookup, colorize, replaces the fill by `amount`) → bevel light
(`s = n·L − L.z`, highlight screen and shadow multiply through the gloss contour) → finish (gradient
map of Rec.601 luma, after lighting) → pattern marks (skip the finish) → satin → colour texture →
sheen → inner glow (with grit hash) → gloss cap → `alpha = coverage × fill alpha × opacity` (glass
raises it), output `toLinear(c) × alpha`. The program binds ≤ 12 samplers (coverage, field, height,
satin, span, pattern image, fill image, reflection map, gradient LUT, finish LUT, gloss LUT, profile/
contour LUT).

### 6.8 Asset integration

| Place | Change |
|---|---|
| `engine/core/asset/AssetType.h` | `TitleArt = 29` appended, `assetTypeName` → `"title_art"`, `assetTypeFromName`'s bound → `TitleArt`; test `AssetTypeTest.NamesRoundTripEveryValue` iterates to the last value so the next append cannot repeat the trap |
| `editor/asset/AssetTypes.cpp` | `{{TitleArt, InFile, Assets, false, false}, {".titleart"}}`; Font row `{".ttf", ".otf", ".ttc"}` |
| `editor/util/Util.h` | `isTitleArtFile`, in the combined asset test |
| `editor/asset/DepEdge.h` | `TitleArtAsset` (a font, image, SVG or map a document reads; `field` = JSON path) and `TitleArtOutput` (a baked PNG); names `"title_art_asset"`, `"title_art_output"` |
| `editor/asset/AssetIndex.cpp` | `isReferrerType` true for `TitleArt`; `extractFile` parses (errors still yield the refs that parsed) and visits refs with `visitTitleArtRefs` |
| `editor/titleart/TitleArtDocument.*` | The `AnimatorAssetDocument` design: `open`, `create`, `commit(doc, undoName, mergeKey)`, `filesChanged`, `filesMoved` (moves outputs and metas, rewrites recipes), `relinkGuids`, `checkDiskChanges`, listeners, `revision()` (the `rev` agents see); `commitText(text, undoName)` (a whole-document write, S82's Restore This Version) and `stepText(historyEntryId, before\|after)` (the stored text of a step, from `WriteFilesCmd`'s before/after contents, for S82's History hover); `changed` notifications carry the origin (person, agent, disk) |
| `editor/reload/handlers/TitleArt.cpp`, `ReloadHandlers.h`, `HotReloadService.cpp` | R6; a font/image/SVG change finds its documents through the dependency graph |
| `editor/Exporter.cpp` (`copyAssets`) | R8 skip, `TitleArtExport::isNeeded`; stale handling R35 before the copy |
| `editor/App.cpp`, `AppMenuCommand.h`, `DefaultCommands.cpp` | Assets > Create > Title Art (R7) |
| `editor/window/ProjectWindow.*` | `FileType::TITLE_ART`, icon, thumbnail queue, `openFile` → Title Designer (S82) |
| `editor/window/InspectorWindowTitleArt.cpp`, `InspectorWindow.cpp` (`drawInspectedAsset`) | §6.13 |
| `editor/util/ProjectChecks.*`, `editor/cli/CommandLine.cpp` | `--titles` (R58) |
| `editor/mcp/McpServer` file sync | `.titleart` in the tracked extensions, so outside edits arrive in `external_changes` |

The derived PNGs are ordinary Texture assets with `.meta` sidecars; a PNG's recipe names its source
by GUID and path, so `get_asset_info` on the PNG says which document made it and whether it is fresh.

### 6.9 Bake

- **Frame.** All outputs of a document share one frame: the union of every output's content bounds
  (alpha ≥ 1/255) at its scale, plus `padding`, rounded outward to `round_to`, clipped to the canvas;
  each output writes that frame at its own `scale` (sizes rounded to `round_to`). The frame (canvas px,
  top-left origin) goes in the recipe, so S81's component places split parts exactly.
- **Readback.** 8-bit: the output pass writes straight sRGB with dither into RGBA8, read through the
  existing 4-byte path. 16-bit: the output pass writes straight sRGB-encoded half floats into RGBA16F,
  read with the 8-byte path (§6.10), rounded to 0..65535 on the CPU (`encodePng16`). `mask` content
  writes a 1-channel 8-bit PNG.
- **Alpha bleed** (`bleedAlpha(RgbaImage&, int reachPx = 8)` in `editor/titleart/TitleArtBake.{h,cpp}`).
  Fully transparent pixels within `reachPx` take the colour of the nearest pixel with alpha > 0
  (two-pass distance on the CPU), farther ones are written black, alpha stays 0; no colour change
  where alpha > 0. Bakes use 8 px (three mip levels); S87's exports pass their `bleed_px` (up to 64).
- **Recipe** (iTXt keyword `jasper.titleart`, written with `insertRecipeChunk`):
  `{"format": "jasperengine.titleart.bake", "version": 1, "source": {"guid", "path"},
  "source_hash": "<SHA-256 of the canonical document text>", "assets": [{"guid", "path", "sha256"}],
  "bake_version": N, "output": "<id>", "frame": [x, y, w, h], "scale", "bit_depth", "path_used":
  "rgba16f" | "rgba8", "backend", "gpu", "editor_version"}`. Later specs add optional recipe members
  for the derived files they bake through the same writer: `title_3d_version` and environment
  hashes (S86), `animation_hash` (S85's flipbook pages), `language` and `resolved_hash` (S87's
  language sets). `TITLE_ART_BAKE_VERSION` = a base number
  (1) plus the sum of the `passVersion`s of the definitions the document uses, so changing one kernel
  makes only the titles using it stale.
- **Sidecar** `<file>.bake.json` (`logo.titleart.bake.json`), written by every bake beside its PNGs
  and shipped with Baked titles in every mode (≤ 4 KB; S81 R8 reads it through its always-built
  `TitleArtBakeInfo`, so a Baked title needs neither the `.titleart` nor the module). Canonical JSON:
  `{"format": "jasperengine.titleart.bakeinfo", "version": 1, "frame": {x, y, w, h}` (canvas px,
  image space), `"origin": [x, y]` (the document origin in frame px), `"outputs": [{id, file,
  content}]`, `"alpha_bounds": {x, y, w, h}`, `"source_hash", "bake_version"}`. Optional members
  owned by later specs: `anchors` (S83's highlight analysis, at most 16 `{x, y, size, strength,
  fade}` in frame-normalised coordinates, read by S81's live glints), `languages` (S87's per-language
  sets table, source sidecar only) and `language` (a language set's own sidecar). A reader keeps
  unknown members and refuses a newer `version` with a message naming the build.
- **Extensibility.** `TitleArtBake` writes any set of derived files of a document all-or-nothing
  with the recipe chunk: S85 adds flipbook pages and `<file>.anim.json`, S86 the 3D environment
  hashes in the recipe, S87 one full set per language (`<file>.<code>.png`, `<file>.<code>.bake.json`,
  ...). The bake is a resumable render job (§6.3) and reports `done/total` renders as job progress.
- **Writing.** All files of a bake (PNGs and the sidecar) are staged and written with `writeFilesAllOrNothing`
  inside a `ScopedExpectedWrite`, then their textures are invalidated explicitly (editor writes are
  not watcher events) so S14 recooks and scenes refresh. A first write creates the `.meta` with texture
  import defaults: type `ui`, quality `high`, mipmaps on, `maxSize` = the next power of two ≥ the larger
  side (S14 R4, R6 traps). Bakes are not undo steps; undoing the edit that caused one rebakes again.
- **Auto Bake** (R34), **export** (R35), **check** (R58): `check --titles` reports
  `title-version-newer` (error), `title-parse-failed` (error), `title-asset-missing` (error),
  `title-glyph-missing` (warning), `title-bake-missing`, `title-bake-stale`, `title-recipe-invalid`
  (warnings), with the regenerate command (`jasperengine-editor call export_title --project <p> path=<doc>`).
  It needs no GPU (hashes and recipes only) and caches hashes by mtime and size like the synthesis
  check.

### 6.10 Float readback

`FrameReadback::readTexture(texture, w, h, bytesPerPixel, pixels, error)` accepts 8 (RGBA16F,
IEEE half per channel) and 16 (RGBA32F) besides 4 and 1:
- GL core: `glGetTexImage` with `GL_HALF_FLOAT` / `GL_FLOAT`;
- Vulkan: staging buffer sized by the format's texel size (today's code assumes 4 bytes);
- D3D11: staging texture of the same DXGI format, rows copied by `RowPitch` and the texel size;
- Metal: `MetalReadback.h` blit with the texel size;
- GLES3/WebGL2: `glReadPixels(RGBA, FLOAT)` into 16 bytes per pixel where `EXT_color_buffer_float`
  holds (an 8-byte request converts on the CPU); otherwise false with "float readback needs
  EXT_color_buffer_float".
Rows stay top first. GPU test `FrameReadbackGpu.FloatTargetsRoundTrip` writes known values (0, 0.5,
1, 4, 1/3, −1) into RGBA16F and RGBA32F targets and reads them back exactly (half precision for 16F).

### 6.11 MCP tools

Group row in `editor/mcp/McpToolGroups.cpp`: `{"title_art", "Title Art documents (.titleart):
library, edit, render, check, export."}`; module row `{registerTitleArtTools, "Title Art",
"title_art"}`. Schemas from `McpSchema.h` builders only; `ops` items are `anyObject` validated by
`TitleArtOps` (document keys are snake_case already). Parameters added by S81-S87 carry no
per-parameter description (each is explained in `jasper://docs/title-art`), the conventions'
rule; options that only one format or one feature needs are keys of a free-form `options`
object the handler checks against a closed table (`bad_option` listing the valid keys, the S47 D4
pattern), never new top-level parameters.

**The program budget** (the one table every title spec references; measured by `tool_budget.py`,
gated by `test_tool_budget.py`; each cell is a cap on what that spec adds):

| Tool | S80 | S81 | S82 | S83 | S84 | S85 | S86 | S87 | Cap (≤ 1,500 tool default) |
|---|---|---|---|---|---|---|---|---|---|
| `list_title_library` | 860 (every program `kind` value) | 0 | 0 | 120 (`category`) | 450 (`family`, `dimension`, `tags`, `scope`, `favorites`, `sheet`) | 0 | 0 | 0 | 1,430 |
| `get_title` | 700 | 220 (`entity`, `scene`, `wait_ready`, `timeout_seconds`) | 0 (result `designer`) | 0 | 0 | 50 (`animation`) | 0 (result `style_3d`, `stats_3d`) | 30 (`language`) | 1,000 |
| `edit_title` | 1,350 (the description names every op family of the program) | 0 | 0 | 0 (`apply_recipe` is an op) | 0 (ops) | 0 (ops) | 0 (ops) | 30 (`create.from`) | 1,380 |
| `render_title` | 1,300 (with `options` and `fallback`) | 80 (`entity`, `scene`) | 0 | 0 (`options.cost`) | 0 (`sheet` is on `list_title_library`) | 0 (`options.animation`) | 0 (`options.quality`, `options.debug_view`) | 0 (`options.languages`) | 1,380 |
| `check_title` | 1,300 | 0 (check ids) | 0 | 0 (check ids) | 0 (check ids) | 50 (`animation`) | 0 (check ids) | 45 (`languages`) | 1,395 |
| `export_title` | 1,000 (with `format` and `options`) | 0 | 0 | 0 | 0 | 60 (`animations`) | 0 (`format: "glb"`, `options.glb`) | 90 (`languages`, `out`, `path` as a list) | 1,150 |
| **Group `title_art`** | 6,510 | 300 | 0 | 120 | 450 | 160 | 0 | 195 | **7,735 characters, 6 tools** of the group default 15,000 / 12: no `budget.json` entry |
| `diff_images` (`editor_tools`) | 700 | | | | | | | | 700 |
| Core listing | 0 | 0 | 120 (`focus_window` ≤ 95, `screenshot` ≤ 25), 0 tools | 0 | 0 | 0 | 0 | 0 | within 99,000 / 150 |
| Agent guide (bytes) | ≤ 40 (one "Other areas" entry with the group name) | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 6,241 measured after `fe3e90b97` + 40 ≤ 6,692 (8,192 − 1,500 headroom) |

No title spec adds a tool or a group, raises a limit or adds guide bytes beyond S80's entry; a
spec that cannot fit its cell moves description text into `jasper://docs/title-art` and records
it in its Deviations.

| Tool | Annotations, activity | While playing | Job |
|---|---|---|---|
| `list_title_library` | readOnly, idempotent; Read | yes | `sheet` past 1 s |
| `get_title` | readOnly, idempotent; Read | yes | `wait_ready` past 1 s (S81) |
| `edit_title` | writesFiles; Edit | yes (asset edit; hot reload shows it in Play) | no (its `render` follows `render_title`'s job rule; imports by `create.from` past 1 s) |
| `render_title` | readOnly, idempotent, writesFiles (`save_path`); Read | yes | past 1 s |
| `check_title` | readOnly, idempotent, writesFiles (`save_baseline`); Read | yes | past 1 s |
| `export_title` | writesFiles, idempotent; Edit | yes | past 1 s |
| `diff_images` (`editor_tools`) | readOnly, idempotent, writesFiles (`save_diff`); Read | yes | no |

**`list_title_library`** `{kind?, id?, query?, limit? (20), offset?}`. `kind` (one enum S80 writes
for the whole program): `effects`, `generators`, `blend_modes`, `fonts` (S80); `looks`,
`color_schemes`, `templates` (S84); `environments`, `materials_3d`, `bevel_profiles` (S86);
`animation_presets` (S85). No arguments: counts per kind. Rows: `{id, name, category, cost,
adds_light, on_parts}` for effects (`cost` the class `C0`..`C4`, §6.5); `{path, guid, family,
styles, axes: [{tag, min, default, max}], scripts: ["latin", "kana", "kanji", ...], glyphs}` for
fonts (project Font assets and the built-in face; S84 adds the pack rows). `id` returns the
parameter lines (`size px 40 [1..400]: how far the glow reaches`) and the `add_effect` op ready to
paste. A kind whose owner has not landed returns an empty list with "<Kind> arrive with S5x".

**`get_title`** `{path, layer?, detail? ("outline" | "overrides" | "full"), fields? [path globs]}`.
Returns `{path, guid, rev, version, canvas, layers: [{path, id, type, name, text?, font?, look?,
parts?, effects: ["fx:outer_glow"], masks, clip, blend, overrides}], exposed, outputs: [{id, file,
status: "fresh" | "stale" | "missing", changed?: [...]}], problems}`. `overrides` lists only values that
differ from defaults (a 15 KB title is a few hundred bytes); `full` the layer objects; `fields` exact
values (`style.rims[*].width`).

**`edit_title`** `{path?, create? {canvas?, text?, overwrite?, template?, look?, from?}, ops? [...],
dry_run?, expect_rev?, render? (true | {max_width, backgrounds, crop}), bake? ("auto" | "now" |
"never")}`. `create` makes the file (R7's document, with `canvas` and `text` applied; `template` and
`look` are S84's, `from` (a Pachislop, PSD or SVG file to import) is S87's). `path` is required
except for a batch made only of library ops (S84's `save_*`, `copy_library_item`,
`delete_library_item`, S87's `import_ps_styles`). The description lists the op families of the
whole program (document, layers, effects, rims, masks, expose, links; S83 recipes, S84 library,
S85 animation, S86 3D, S87 language and import) and points at `jasper://docs/title-art#ops`, so
later specs add ops without schema characters.
Result: `{path, rev, undo: {id, name}, applied, changes: [{layer, path, from, to, clamped?, px?}],
new_ids: {...}, problems: [{severity, code, op, path, message, nearest?, valid_here?, fix?}],
bake: {status, job_id?}}`; with `render`, the image and `render_title`'s metrics. Errors always say
"Nothing was applied." `expect_rev` refuses when the file moved on (another editor or person).

**`render_title`** `{path, ops?, try? [{label, ops}] (≤ 9), layer?, output?, backgrounds? (["checker"];
`transparent`, `checker`, `black`, `white`, `#rrggbb`, an image path), crop? [x, y, w, h] (image px of
the canvas), max_width? (768), columns?, compare? ("rev:<n>" | png path), image? ("always" | "none"),
fallback? ("rgba8" | "compare"), options? {...}, save_path?, wait?, timeout_seconds?}`. `ops`/`try`
render what-if copies and never touch the file; `output` renders exactly an output's frame and
content; `fallback: "rgba8"` forces the RGBA8 path, `"compare"` renders both paths and adds
`pixel_paths_diff {max, mean, changed_pct}` (S81 uses it on instances). `options` (free-form, the
handler's closed key table; keys in `jasper://docs/title-art`): `overrides`, `rerender`,
`live_time` (S81, with `entity`); `cost: "desktop" | "mobile"` (S83's readout); `animation {name,
time | times | frames, as}` (S85); `quality`, `debug_view` (S86; `stats_3d` is returned whenever
the render has a 3D layer); `languages` (S87). An unknown key fails listing the keys of the
features this build has.
Result: `{rev, size, render_ms, path_used, alpha_bounds: {x, y, w, h, margins}, coverage_pct, flat,
cells: [{n, label}], diff?: {against, changed_pct, max, mean, bbox}, saved_to?}`. The last 16
revisions' renders of each document are kept in `.jasperengine/cache/titleart/renders/<guid>/` (a narrow
editor-internal write) so `compare: "rev:<n>"` works without re-rendering old states.

**`check_title`** `{path, checks? [ids], backgrounds? (["black", "white"]), baseline? (png |
"rev:<n>"), save_baseline?, fail_on? ("fail" | "warn"), screenshot? ("on_failure" | "always" |
"never")}` (§6.12).

**`export_title`** `{path, outputs? [ids], format?, options?, dry_run?, force?, wait?, timeout_seconds?}`. Bakes the
outputs (all by default; fresh ones are skipped unless `force`); `dry_run` returns what would be
written. Result: `{files: [{output, path, guid, size, frame, bytes, vram_estimate, status}], warnings}`.
Outputs are configured with `edit_title` ops on `outputs` (`set {"outputs[1].scale": 2}`), not here.
`format` (default `bake`; the enum S80 writes for the program: `bake`, `glb` (S86), `png`, `psd`,
`svg`, `sheet`, `atlas`, `sequence`, `ktx2`, `csv`, `xliff` (S87)) selects a delivery format whose
owner has landed (else `not_available` naming it); `options` (free-form, checked per format
against a closed table before anything renders) holds that format's keys (S86's `glb {units_per_px,
texture_size, include_bands}`, S87's per-format options). S85 adds `animations`, S87 `languages`,
`out` and a list `path`.

**`diff_images`** `{a, b, mode? ("premultiplied" | "straight"), tolerance? (8), max_fraction? (0.001),
max_mean? (0.25), max_diff? (48), crop?, image? ("on_fail" | "always" | "none"), save_diff?}` on two
project images of equal size (else an error naming both sizes). Result `{ok, size, max, mean,
fraction_over, changed_pct, bbox, thresholds}` and a three-panel image (a, b, difference ×8 in red
over faint luminance). Shared C++ in `editor/util/ImageDiff.{h,cpp}` (also used by `check_title`
and `render_title`'s `compare`); `tools/mcp/pngdiff.py` gains `premultiplied=True` and `--premultiplied`
with the same maths, so tests and tools agree.

`view_asset` on a `.titleart`: its fresh main output, else a 512 px render. `get_asset_info`: canvas,
layer counts, fonts (missing marked), outputs with status, problems. Both are result additions only.

**Agent guide.** `docs/mcp/guide/title-art.md` (`jasper://docs/title-art`): units (`%` of size vs
`px`), addressing and ops with examples, the render → check → fix loop, `try` sheets, baking and
freshness, the y-up document vs top-left image coordinates. S80 owns the file and its section plan;
each spec writes only its own `### ` section (so the file splits into section resources): `### Runtime`
(S81), `### Title Designer` (S82, ≤ 1,200 bytes), `### Effects and generators` (S83, ≤ 2,500 bytes),
`### Library` (S84, ≤ 3 KB), `### Animation` (S85), `### 3D` (S86), `### Languages` and
`### Import and export` (S87). The generated catalogues are separate files:
`title-art-effects.md` (S80's generator, S83's columns) and `title-library.md` (S84). The guide
(`McpInstructions.cpp`) gets one
"Other areas" entry, `title art (edit_title)`, and the generated `title_art` group name: ≤ 40 bytes
together, with a case `make_title_logo` in `tools/mcp/eval/cases.py` that fails without the entry.

### 6.12 Checks (`check_title`)

Result: `{verdict: "PASS" | "WARN" | "FAIL", checks: [{id, kind, status, measured, threshold, at?:
[{crop}], message, fix?: {ops}, method}], look?: [{crop, why}], guidance: "These are measurements;
passing them does not mean the title looks good. Show the render to a person."}`. A check never edits.

| id | Kind | Measures | Default threshold | Fix |
|---|---|---|---|---|
| `document_valid` | document | Parses, version, unknown types, normalize warnings | none | |
| `assets_present` | document | Every AssetRef resolves | all | the asset's nearest match by name |
| `glyphs_present` | document | No missing glyphs after the fallback chain | 0 | `set font` / add to `fallback_fonts` a project font that has them |
| `exposed_targets` | document | Every exposed target resolves | all | `unexpose` |
| `bake_fresh` | document | Every output fresh | all | `export_title` |
| `not_flat` | pixels | The render is not blank or one colour | not flat | the layer that contributes nothing |
| `not_clipped` | pixels | Alpha > 2/255 within 1 px of the canvas edge; per side; attributed to layers by isolated renders | margin ≥ 8 px | `set canvas.width/height` or the layer's `size` |
| `alpha_bounds` | pixels | Trim box, coverage, centre offset | coverage 10..90 % | |
| `legibility_contrast` | pixels | CIE L* contrast of the silhouette's outer ring against each background, measured on the render scaled to `CheckOptions::displayHeightPx` when the caller gives one (S82's Device view) | ΔL* ≥ 30 | add a dark/light outer rim (an `add_rim` op) |
| `counter_survival` | pixels | Holes of the plain-text glyph mask still visible in the render | ≥ 0.8 per glyph | thinner rims (`set rim#n.width`) |
| `noop_effects` | pixels | An enabled effect whose removal changes < 0.01 % of pixels | none | `remove_effect` |
| `clipped_highlights` | pixels | Ink pixels with every channel ≥ 250 | ≤ 5 % | lower highlight opacity or finish |
| `halo` | pixels | Straight colour where 0 < alpha < 16/255 differs from the nearest opaque colour; composite on black vs white at edges | low | `alpha_bleed` on |
| `deterministic` | pixels | Two fresh renders identical | equal | the layer with the unseeded element |
| `fallback_parity` | pixels | RGBA8 path vs float path within §6.3's tolerance | §6.3 | |
| `golden` | pixels | Premultiplied comparison with `baseline` (`ImageDiff`) | max 48, mean 0.25, 0.1 % over 8 | |

Thresholds are starting points (S84 calibrates them on the library); the result echoes each one used.
Failures carry crops of where to look (a 512 px crop costs about 175 tokens). Checks are rows of one
registry (`editor/titleart/TitleArtChecks.{h,cpp}`: `registerCheck(CheckDef{id, kind, owner, group,
run})`), so later specs add checks without touching the tool; `checks` takes ids or a group name.
The program's check ids, by owner: S81 `glyph_atlas`, `characters`; S83 `fx_follow_missing`,
`fx_bake_only_realtime`, `fx_live_budget`, `fx_realtime_budget`, `fx_hdr_on_8bit`; S84
`style_lint` (results `style_lint.<rule>`), `color_scheme_match`, `metal_range`,
`color_scheme_options_empty`; S85 the `anim_*` checks of its §6.7.2 (group `anim`); S86
`3d_tier_budget`, `3d_reflection_pose`, `3d_z_fighting`, `3d_environment_missing`,
`3d_glint_dead`, `3d_mesh_fallback`, `3d_light_limit`; S87 `languages_complete`,
`translation_stale`, `text_overflow`, `fit_scale_low`, `project_language_missing`,
`translation_unused` (and S80's `glyphs_present` and `bake_fresh` run per language). No two specs
register the same id.

### 6.13 Editor surface in S80

- **The preview queue** (`editor/titleart/TitlePreviewQueue.{h,cpp}`, owned here so that every
  title preview of the program shares one budget from wave 6 on): `request(Priority, Key,
  PreviewJob) -> id`, `cancel(id)`, `cancelAll(owner)`, `runFrame(budgetMs)` (main thread, from
  `App::engineRender`, default 4 ms), `texture(id)`, `stats()`; priorities `Hover`, `Layers`,
  `Tiles`, `Thumbnails`, `Prefetch` in that order, visible items first within one; an item whose
  predicted cost exceeds the budget runs alone in a frame; jobs are `TitleRenderJob`s sliced by
  `step`; device loss drops and re-requests. Submitters: S80 (Project window, Inspector), S82
  (canvas layer thumbnails, hovers, Add palette), S84 (`TitleLibraryPreviews` tiles), S86 (material
  and environment pickers), S87 (Compare Languages).
- **Project window:** Title Art icon (a "T" over a rim), type colour, the thumbnail (R50) through
  `editor/render/preview/TitleArtPreviewRender` submitted to `TitlePreviewQueue` (visible
  items first, cancelled when the window closes, redrawn after device loss), disk-cached in the
  thumbnail directory (`Project::getThumbnailPath`) under a key of GUID, size, the document's source
  hash and `TITLE_ART_BAKE_VERSION` (content + engine version, contract decision 11); fresh main
  output PNGs are thumbnailed on a worker like images.
- **Inspector (one `.titleart` selected):** a 256 px preview on a checker (a backdrop dropdown:
  Checker, Black, White); **Open in Title Designer** (primary button; double-click does the same);
  Canvas `1920 x 1080`; Layers (counts by type); Fonts (each a clickable asset; missing ones red with
  "Missing" and a Locate button that opens the Missing Assets panel); Exposed Parameters (name, type,
  targets); Outputs table (File, Size, Status Fresh / Stale / Missing with the reason as a tooltip,
  **Bake** per row, **Bake All**, Add Output / Remove Output as undo steps); Problems (warnings with
  their message); Auto Bake (shows the project setting, links to it). Several selected: the count and
  Bake All. Later specs add summary rows through `InspectorWindowTitleArt::addSummarySection` (S85's
  animations with cost per mode, S87's languages); Open in Title Designer opens S82's window once it
  exists (before it, the button is hidden and double-click selects the asset).
- **Project Settings > Title Art:** Auto Bake, Stale Bakes On Export.
- Strings use "Color"; every action is in the command palette (S25) as "Title Art: Bake All".

### 6.14 Lua and CLI

No runtime Lua in S80. Editor scripts: `Editor.call("edit_title", {path = "...", ops = {...}})`.
CLI: `jasperengine-editor call <tool> --project <p> ...` (a one-shot headless editor, GPU required
for render, check pixels and export) and `jasperengine-editor check --titles` (no GPU).

### 6.15 Program rules: hooks, module, folders, libraries

The eight title specs are implemented together and land in three waves (§8). This section is the
one statement of what they share; every other title spec refers to it rather than restating it.

**Hooks S80 provides for later specs** (each a requirement and AC here, so wave 6B never waits):

| Hook | Where | Used by |
|---|---|---|
| Reserved document members and member additions | §6.1 | S81, S84, S85, S86, S87 |
| `TitleArtSchema` member table with `FxAnim` per member, sparse kinds, registration | R9a, §6.5 | S82 rows, S85 classes, S86/S87 members |
| Registry members (cost, fallback, live, glyph safety, `anim`, aliases, inputs, `light`, spectrum, `estimate`, `analyse`) | §6.5 | S81, S82, S83, S84, S85, S86 |
| `LIGHT_PREMULTIPLIED`, `MAX_PREMULTIPLIED`, `BlendOperation::MAX` | §6.4 | S83 |
| Material `bevel.direction` | §6.1 | S83, S87 |
| Render job: `progress`, `passesDone/Total`, `nextStepCostUs`, `splitOversizedSteps`, `shaderKeys`, `nodeCosts`, `error`, splits | R18, §6.3 | S81 scheduler, S82 progress, S85 |
| `RenderRequest::overrides` (exposed values, poses, languages), `keepSplits`, `glyphCell`, `tier`, `assets` | R18a, §6.3 | S81, S82, S85, S86, S87 |
| `TitleTargetPool` with depth format and sample count in the key | §6.3 | S86 |
| Layout, outline, rim-band and bounds queries | §6.6 | S81, S82, S86, S87 |
| `add_layer {from}`; `TitleArtDocument::commitText`, `stepText` | §6.2, §6.8 | S82 |
| Bake sidecar `<file>.bake.json` with optional `anchors`, `languages`; bake extensibility; `bleedAlpha(reachPx)` | §6.9 | S81, S83, S85, S86, S87 |
| `TitlePreviewQueue` | §6.13 | S82, S84, S86, S87 |
| Check registry | §6.12 | S81, S83, S84, S85, S86, S87 |
| Tool hooks: `edit_title.create.from`, `render_title.options`, `fallback: "compare"`, `export_title.format` and `options`, the program `kind` enum | §6.11 | S81, S83, S84, S85, S86, S87 |
| Inspector summary sections | §6.13 | S85, S87 |

**Reserved op names** (one owner each; S80's `TitleArtOps` dispatches them to the owner's code and
answers `not_available` naming the owner until it lands): S83 `apply_recipe`; S84 `apply_look`,
`apply_color_scheme`, `apply_template`, `save_look`, `save_color_scheme`, `save_template`,
`copy_library_item`, `delete_library_item`; S85 `add_animation`, `set_animation`,
`remove_animation`, `duplicate_animation`, `set_default_animation`, `set_track`, `set_key`,
`remove_track`, `remove_keys`, `retime`, `apply_animation_preset`, `set_marker`, `remove_marker`;
S86 `set_dimension`, `add_band`, `remove_band`, `move_band`, `apply_material`, `fit_to_tier`; S87
`add_language`, `remove_language`, `set_source_language`, `set_translation`,
`revert_translation`, `mark_reviewed`, `import_strings`, `apply_ps_style`, `import_ps_styles`,
`import_svg`, `reimport_pachislop_3d`.

**One module.** `JASPERENGINE_TITLE_ART` (S80: the option, default ON, required ON in the editor's
shared engine; `engine/core/titleart/TitleArtSources.cmake` lists every module source of every
title spec; HarfBuzz without `HB_NO_DRAW`, ThorVG and Clipper2 with it). OFF compiles nothing under
`engine/core/titleart/`. Two sub-options, `cmake_dependent_option`s ON when the module is ON, are
defined and wired into `ModuleUsage`, Compile builds and the prebuilt variants by **S81**, with
S86's code behind them: `JASPERENGINE_TITLE_ART_3D` (`engine/core/titleart/three_d/`, S83's
`three_d/Glint3D.*` included) and `JASPERENGINE_TITLE_ART_MESHER` (S86's mesher and earcut).
Prebuilt variants that carry `titleart` carry both. The player feature for notices is `titleart`.
**The always-built part** (core, outside `engine/core/titleart/`, so a Baked title needs no module):
S81's component, `TitleArtSystem`, `engine/core/object/TitleArt.*`, the Lua binding,
`engine/core/util/TitleArtBakeInfo.*` and the module-off stub
`engine/core/subsystem/TitleArtRuntimeNone.cpp`; S87's `engine/core/util/TitleArtLanguage.*`;
S85's `engine/core/titleanim/` (sampler, Baked player, `<file>.anim.json` reader). Its budget is
≤ 60 KiB stripped arm64 and ≤ 40 KiB wasm gzip in total (S81 ≤ 34 / 25, S85 ≤ 20 / 12, S87 ≤ 6 /
3), measured by S81 with `tools/players/lib_size.py`.

**One set of folders.**
- Module (`engine/core/titleart/`, namespace `jasperengine::titleart`): S80 root (`TitleArtDoc.h`,
  `TitleArtIO`, `TitleArtMigrations`, `TitleArtHash`, `TitleArtSchema`, `TitleEffectRegistry`),
  `text/`, `geometry/`, `render/`, `effects/` and `generators/` (S80's and S83's definitions, one
  file per family); S81 `runtime/`; S83 `analysis/`, `gpu/`; S84 `ColorScheme.*`,
  `ColorSchemeApply.*`; S85 `anim/`; S86 `three_d/`; S87 `TitleTranslations.*`, `TitleFit.*`.
- Shaders: `engine/shaderlib/titleart.vert`, `titleart.frag` (S80); every other title shader file
  in `engine/shaderlib/titleart/`: S80's includes, S83's `fx/`, `gen/` and `tfx_*.glsl`, S86's
  programs (`title_lit.vert/.frag`, `title3d_post.frag`, `title_glint.frag`, `title_env.frag`,
  `title_glint_sprite.vert/.frag`) and includes (`title_*.glsl`, `hdr_compress.glsl`); S81's
  library shaders `engine/shaderlib/library/title_fx.*`, `title_fx_sprite.*` and their include
  `engine/shaderlib/includes/title_fx.glsl`; S86's shared `includes/pbr_lobes.glsl`.
- Editor (`editor/titleart/`): S80 root (`TitleArtDocument`, `TitleArtOps`, `TitleArtBake`,
  `TitleArtChecks`, `TitlePreviewQueue`); S81 `runtime/`; S82 `designer/`, `fonts/`; S83 `fx/`;
  S84 `library/`; S85 `anim/`; S86 `three_d/`; S87 `localisation/`, `interchange/`. Windows:
  `editor/window/TitleDesigner*.cpp` (S82; S87 adds `TitleDesignerLanguages.cpp`,
  `TitleDesignerTranslations.cpp`), `TitleArtImportDialog.cpp`, `TitleArtExportDialog.cpp` (S87),
  `InspectorWindowTitleArt.cpp` (S80), `InspectorTitleArtRenderer.cpp` (S81). MCP:
  `editor/mcp/tools/TitleArtTools*.cpp` (S80's files; later specs add handlers there). CLI:
  `editor/cli/` (S84 `title-library`, S87 `TitleArtCli.*`).
- Built-in data (`editor/resources/titleart/`): `fx-presets/`, `pachislop-fx-map.yaml` (S83);
  `fonts/`, `thumbnails/`, `provenance/`, `pachislop-map.json`, `manifest.json` (S84); `hdri/`
  (S86, with S84's four entries), `environments/` (S86); `animation-presets/` (S85);
  `charsets/` (S81); `search-terms.json` (S82). Tools: `tools/titleart/` (every spec's scripts).
- In a project: every font Title Art copies goes under `<assets>/TitleArt/Fonts/` (S84's pack faces
  in `<pack id>/`, S82's Google Fonts in `Google/<slug>/` and system fonts in `System/`); library
  environments in `<assets>/TitleArt/Environments/` (S84); per-document imports in
  `<doc stem>_assets/` (S87); user library items in `EditorSettings/TitleLibrary/` (S84) and user
  animation presets in `EditorSettings/TitleLibrary/AnimationPresets/` (S85; personal ones in
  `<config dir>/title-library/animation-presets/`, S84's scopes); delivery exports in
  `Recordings/Title Art/` (S87); editor caches under `.jasperengine/cache/titleart/` (`renders/`
  S80, `library/` S84's tiles, `flipbooks/` S85's previews).
- Derived files beside a document `logo.titleart`: `logo.titleart.png`, `logo.titleart.<output>.png`,
  `logo.titleart.bake.json` (S80); `logo.titleart.<animation id>.flip<N>.png`,
  `logo.titleart.anim.json` (S85); a language set inserts `.<code>` after `.titleart`
  (`logo.titleart.de.png`, `logo.titleart.de.bake.json`, `logo.titleart.de.anim.json`, S87). Export
  caches only: `.titlemesh` and cooked environment KTX2s (S86).

**One list of vendored and ported third-party code** (each owned by one spec, with its `NOTICE.md`
row and, under `engine/libs/`, its `engine/notices.json` entry):

| Code | Licence | Where | Owner | Ships |
|---|---|---|---|---|
| Clipper2 (Angus Johnson) | BSL-1.0 | `engine/libs/clipper2/` | S80 | `titleart` |
| ThorVG (existing) | MIT | `engine/libs/thorvg/` | S80 (the `when` list) | `htmlui`, `titleart` |
| HarfBuzz `hb-draw` (existing sources) | Old MIT | `engine/libs/harfbuzz/` | S80 (`HB_NO_DRAW` condition) | always (code only with `titleart`) |
| HarfBuzz subsetter (`hb-subset*`, `graph/`, `hb-repacker`) | Old MIT | `engine/libs/harfbuzz/subset/` | S81 | editor |
| earcut.hpp (Mapbox) v2.2.4 | ISC | `engine/libs/earcut/` | S86 | `titleart` |
| Brotli decoder (google/brotli `c/common`, `c/dec`) | MIT | `libs/brotli/` | S82 | editor |
| WOFF2 decoder (google/woff2) | MIT | `libs/woff2/` | S82 | editor |
| Pachislop (SafEight, used with the author's permission) | permission | ported files' headers | S80 (the entry; each spec adds its file list) | per file |
| Patchy warp | MIT | `geometry/Warp.cpp` header | S80 | `titleart` |
| Hoskins "Hash without Sine", mulberry32, Inigo Quilez `sdStar5`/`sdHeart` | MIT / public domain / MIT | S83 shader and C++ headers | S83 | `titleart` |
| three.js tone maps, Khronos PBR Neutral, Inigo Quilez gradient noise | MIT / Apache-2.0 / MIT | S86 shader headers | S86 | `titleart` |
| ag-psd descriptor, EngineData and reader code | MIT | `editor/titleart/interchange/psd/` headers | S87 | editor |
| Dela Gothic One (test fixture) | OFL-1.1 | `engine/tests/core/titleart/fonts/` | S80 | tests |
| The 32-family font pack | OFL-1.1 | `editor/resources/titleart/fonts/` | S84 | editor install |
| Eight Poly Haven HDRIs (four each) | CC0 | `editor/resources/titleart/hdri/` | S86 (manifest, fetcher, size rule); S84 adds four entries | editor install |

Nothing else is vendored by the program; miniz, zstd, stb, tinyxml2, LunaSVG and meshoptimizer
are already in the tree. No ICC profile is taken from anywhere (S87 generates its sRGB profile),
and no franchise or brand name appears in any id or name (S84's denied terms).

## 7. Acceptance criteria

Fixtures: `engine/tests/core/titleart/fixtures/` (documents), the standard title (確定, Dela Gothic One,
size 200, classic style: Pachislop's `classicTitleStyle2D` converted, rim offsets negated to y-up), the
**classic fixture** (the standard title with Outer Glow and Drop Shadow), the asymmetric fixture.
Goldens are written on glcore (llvmpipe) and Vulkan (lavapipe) must match (`docs/testing.md`); GPU
suites end in `Gpu` and start with `JASPER_REQUIRE_GPU()`. Every title golden of the program (S80-S87) is
updated only with a row in the title golden ledger `docs/titleart/goldens.md` (name, metric before
and after, reason), checked by `tools/titleart/check_golden_ledger.py` in `test_docs.py` (S80 owns
both, the `tools/htmlui/check_golden_ledger.py` design).

| AC | Verifies | Test | Red control |
|---|---|---|---|
| AC1 | R1: round trip is canonical; unknown members, unknown effect/layer/mask types survive parse → op batch → write | `TitleArtIOTest.CanonicalRoundTrip`, `.UnknownDataSurvivesEdits` | drop unknown-member keeping in the writer: fails |
| AC2 | R1: lenient read warns with JSON paths and clamps; non-object, wrong format fail with line:col | `TitleArtIOTest.LenientRead`, `.Failures` | |
| AC3 | R2, R63: newer version refused (`version_newer`); test-only migration chain runs in order, idempotent ("twice equals once"); every v1 fixture loads | `TitleArtMigrationsTest.*` | register the test step out of order: fails |
| AC4 | R3: `.titleart` is a TitleArt asset with an in-file GUID; `.ttc` is a Font; `assetTypeFromName("title_art")` works | `AssetTypesTest.TitleArt`, `AssetTypeTest.NamesRoundTripEveryValue` | leave the bound at `VertexAnimation`: fails |
| AC5 | R4: edges for every AssetRef kind with fields; missing font listed in Missing Assets with document and field | `AssetIndexTest.TitleArtEdges`, `test_title_art.py::test_missing_font` | |
| AC6 | R5: renaming a font rewrites the hint in one undo step; moving the `.titleart` moves PNGs and metas and rewrites recipes; scene references to a PNG survive | `TitleArtDocumentTest.RelinkAndMove`, `test_title_art.py::test_move_document` | |
| AC7 | R6: an external edit reloads the document and rebakes with Auto Bake; a changed font rebakes its titles | `test_title_art.py::test_hot_reload_rebakes` | disable the handler: fails |
| AC8 | R7: Create Title Art makes the document, its PNG and meta; undo removes all three | `test_title_art.py::test_create_and_undo` | |
| AC9 | R8: an export ships the PNG and no `.titleart` | `test_title_art.py::test_export_excludes_document` (payload listing) | remove the skip: fails |
| AC10 | R9, R54: ops all-or-nothing; did-you-mean on a bad path; `%` echoes `px`; one undo step named after the agent; `expect_rev` refuses | `TitleArtOpsTest.*`, `test_title_art.py::test_edit_atomic_undo` | |
| AC11 | R11, R16: the compositor scenes (clipping, adjustment in and outside a group, effects-only blur, part effects 2D, nested hidden groups, mask from layer luminance, shape mask feather, group radial mask, flip 2D) match our goldens | `TitleArtCompositorGpu.<scene>` | swap clip and mask order: `clipping` and `mask-*` fail |
| AC12 | Pachislop parity: the 9 scenes of AC11 and the effects `outerGlow`, `innerGlow`, `dropShadow`, `innerShadow`, `keyline` (Outline), `gradientOverlay`, `gaussianBlur`, `adjust` on the standard title, rendered at scale 0.25 trimmed (`GOLDEN_STILL`), against `tests/golden/refs/{scene,effect}/*.png`: 4 x 4 block means, block threshold 24, ≤ 3 % of blocks over, colour histogram distance ≤ 6 % | `TitleArtParityGpu.<case>` (references copied to `engine/tests/core/golden/titleart/pachislop/` with the provenance file) | negate the bevel light's y: the effect cases fail |
| AC13 | R12, R39: asymmetric fixture identical on glcore and Vulkan, and flipped renders fail | `TitleArtGpu.OriginAsymmetric` | flip `TitleFrame`'s y: fails |
| AC14 | R13, R44: classic fixture peak target bytes ≤ 96 MB; a 20 px title in a corner allocates ROI-sized targets only | `TitleArtRendererGpu.RoiMemory` | full-canvas ROI: fails |
| AC15 | R14, R42: cached re-render pass counts (colour ≤ 8, move ≤ 2, rim width ≤ 60 % of full) and each cached result equals a fresh render byte for byte | `TitleArtRendererGpu.CacheCascade` | include colour in the mask hash: colour count fails |
| AC16 | R15: each of the 18 modes on a fixed pair of layers matches a CPU reference of `blend.glsl`'s formula within 2/255; Normal and Add record no backdrop copy | `TitleArtBlendGpu.AllModes`, `.FixedFunctionPaths` | route Normal through the shader path: second fails |
| AC17 | R12, R38: forced RGBA8 path (`TITLE_ART_TEST_NO_FLOAT_TARGETS`, `_NO_R16F`, `_NO_RGBA32F`) agrees with the float path on every S80 golden within §6.3's tolerance; both paths' goldens are kept | `TitleArtFallbackGpu.<case>` | encode JFA seeds with 8 bits per axis: fails |
| AC18 | R17: every golden case rendered twice from fresh documents with fresh ids is byte-identical | `TitleArtGpu.RenderTwiceFreshIds` | seed one pattern from the layer id: fails |
| AC19 | R18, "both render paths": the one-shot bake path and the time-sliced preview path (1-pass steps, after a sequence of 10 edits and their undo) give byte-identical images | `TitleArtRendererGpu.SlicedEqualsOneShot` | |
| AC20 | R19: a simulated context loss mid-render recovers and re-renders | `TitleArtRendererGpu.DeviceLoss` | |
| AC21 | R20-R23: shaping with a fallback font; variable `wght` 400 vs 800 outlines differ and advances follow; `.ttc` member 1; vertical 縦書き with TCY `12`; `proportional` 1 narrows kana only; per-char rotate keeps neighbours; missing glyph reported with a covering project font | `TitleTextLayoutTest.*`, `GlyphOutlinesTest.*`, goldens `TitleArtTextGpu.Vertical`, `.Variable` | turn off TTB shaping: vertical golden fails |
| AC22 | R24-R26: warp styles, distort, weight, plates and counters on fixed outlines match reference paths (point tolerance 1e−3 em) and goldens | `TitleGeometryTest.*`, `TitleArtMaskGpu.*` | round joins instead of mitre: mitre golden fails |
| AC23 | R27, R43: bounded JFA gives the same shading as unbounded within 1/255 on bevel-only elements, with flood passes ≤ ceil(log2(R + 2)) + 1; classic fixture total floods ≤ 50 % of unbounded | `TitleArtFieldsGpu.BoundedJfa` | always unbounded: pass bound fails |
| AC24 | R28-R30: every Material member and every core effect/generator at defaults on the standard title matches its golden (registry-driven list; missing reference or orphan reference fails) | `TitleArtEffectsGpu.<type>`, `TitleArtMaterialGpu.<member>` | delete one reference: the list check fails |
| AC25 | R31: inspector rows, MCP validation, `list_title_library` and the generated docs come from the table: adding a test-only definition makes it appear in all four | `TitleEffectRegistryTest.DrivesEverySurface`, `test_docs.py` (`gen_effect_docs.py --check`) | |
| AC26 | R32, R33: bake writes shared-frame PNGs with `round_to` sizes, alpha bleed, recipe; a doc edit, a font change or a `passVersion` bump makes exactly the affected outputs stale | `TitleArtBakeTest.*`, `test_title_art.py::test_bake_stale` | |
| AC27 | R34, R35: Auto Bake after edits (not an undo step); export with `rebake` bakes stale outputs, `fail` fails naming them | `test_title_art.py::test_auto_bake`, `::test_export_stale_policy` | |
| AC28 | R36: float readback round trip on GL core and Vulkan in CI; D3D11 and Metal by the same test on the Windows and macOS runners | `FrameReadbackGpu.FloatTargetsRoundTrip` | |
| AC29 | R41, R46: classic fixture full render ≤ 40 ms GPU and bake ≤ 1.5 s on the RTX lane; ≤ 8 s / ≤ 12 s on llvmpipe (median of 5 after 2 warm-ups) | `tools/perf/title_art_core_bench.py` | |
| AC30 | R45: previews and thumbnails ≤ 4 ms main-thread per frame (p95, 200 frames, 20 thumbnails queued) | `test_title_art.py::test_preview_budget` (`get_frame_times`) | |
| AC31 | R47, R48: codec and op batch, shaping budgets | `TitleArtIOTest.Budget`, `TitleTextLayoutTest.Budget` (Release only) | |
| AC32 | R40, R49: an OFF build compiles no `titleart` symbol (nm) and links no ThorVG/Clipper2 unless HTML UI needs ThorVG; ON with nothing open does 0 passes and holds 0 bytes | a new `titleart-off` selection of `tools/ci/test_player_cli.py`, `TitleArtRendererGpu.IdleIsFree` | |
| AC33 | R37: all title-art programs compile to every shader target; sampler and block counts within limits | `TitleArtShadersTest.CompileAllTargets` | |
| AC34 | R53: the group is hidden by default and listed after `tool_groups`; every tool within its cap; budget and conventions gates pass | `test_title_art.py::test_group_hidden`, `test_tool_budget.py`, `test_tool_conventions.py` | |
| AC35 | R55: `try` sheet of 3 variants leaves the file untouched; `compare: "rev:n"` gives `changed_pct` 0 for a no-op and > 0 for a colour change; `crop` and `backgrounds` alter the image only; real alpha in `save_path` | `test_title_art.py::test_render_title` | |
| AC36 | R56: seeded failures (clipped glyph, missing glyph, flooded counter, no-op effect, halo) FAIL with crops and a fix; applying the fix makes them PASS; the guidance text is always present | `test_title_art.py::test_checks_red_green` | |
| AC37 | R57, R58, R59: `export_title` dry run writes nothing; `check --titles` exit codes and codes on a fixture project; `view_asset`/`get_asset_info` on a `.titleart` | `test_title_art.py::test_export_and_cli`, `::test_check_titles_cli` | |
| AC38 | `diff_images`: identical → ok; premultiplied mode ignores colour under alpha 0 that straight mode reports; size mismatch errors; `pngdiff.py --premultiplied` agrees | `test_diff_images.py` | |
| AC39 | R50-R52: Project window thumbnail, Inspector summary (Open in Title Designer, Bake, outputs) and Project Settings, shown in screenshots from an isolated editor | `tools/editor-shot.sh` shots `titleart-project.png`, `titleart-inspector.png`, kept in the PR evidence | |
| AC40 | R60: guide stays within budget with the new entry; eval case fails without the entry | `tool_budget.py --check`, `tools/mcp/eval/cases.py` case `make_title_logo` | remove the entry: the eval case fails |

| AC41 | R9a, §6.15: every member of §6.1 resolves in `TitleArtSchema` with type, range, default and `FxAnim` (S85's §6.2 assignment); a test-registered member appears in normalize's clamps and in `TitleArtOps` did-you-mean; S87's sparse kind writes only present members | `TitleArtSchemaTest.CoversEveryMember`, `.RegisteredMemberIsValidated`, `.SparseKind` | drop one member from the table: the coverage test names it |
| AC42 | R18, R18a: a job reports `progress` rising monotonically to 1, `passesDone == passesTotal` at the end, `nextStepCostUs` > 0 for every GPU step, `splitOversizedSteps(1)` gives tiles ≥ 256 × 256 whose image equals the unsplit one byte for byte, `shaderKeys` equals the bound programs; an `overrides` render equals the render of an edited copy; a `keepSplits` render keeps the layer's composite equal to its isolated render; a `glyphCell` back + front pair composited equals the glyph's merged render within 1/255 | `TitleArtRendererGpu.JobMembers`, `.OverridesEqualEdits`, `.KeptSplit`, `.GlyphCell` | let `splitOversizedSteps` skip the scissor: the equality fails |
| AC43 | §6.3: a 3D-style request for a depth + 4× MSAA target is served from `TitleTargetPool` under its own key and freed by the same rules; the 2D fixtures request neither (pool stats) | `TitleTargetPoolTest.DepthAndSamplesInKey` | key without the sample count: a 1× target is returned and the test fails |
| AC44 | §6.4: `LIGHT_PREMULTIPLIED` and `MAX_PREMULTIPLIED` give the expected values on RGBA8 and RGBA16F 2 × 2 targets on glcore and Vulkan | `BlendModesGpu.LightAndMax` | map MAX to ADD: fails |
| AC45 | §6.9: every bake writes `<file>.bake.json` with the shared frame, origin, outputs, alpha bounds and hashes; a reader keeps unknown members and refuses a newer version; `bleedAlpha` with reach 8 and 64 fills exactly the pixels within the reach | `TitleArtBakeTest.Sidecar`, `.BleedReach` | write the frame in document space: the sidecar test fails |
| AC46 | §6.2, §6.8: `add_layer {from}` pastes a group with fresh ids and remapped inner references in one step; `commitText` restores a stored version as one step; `stepText` returns both texts of a step | `TitleArtOpsTest.AddLayerFrom`, `TitleArtDocumentTest.CommitTextAndStepText` | reuse the source ids: the duplicate-id check fails |
| AC47 | §6.13, R45: three fake submitters at different priorities never exceed 4 ms of a frame together; visible before hidden; `cancelAll(owner)` empties that owner's items | `TitlePreviewQueueTest` (fake clock and costs) | `runFrame(inf)`: the budget assertion fails |
| AC48 | §6.11, §6.15: a reserved op answers `not_available` naming its owner; `render_title {options: {x: 1}}` and `export_title {format: "psd"}` before their owners land fail with `bad_option` / `not_available` listing the valid keys and formats; the group's six tools stay within the program budget table | `test_title_art.py::test_reserved_and_options`, `test_tool_budget.py` | |

`docs/mcp/TOOLS.md` is regenerated; `tools/check-changed` is the per-commit gate.

## 8. Dependencies & conflicts

**The Title Art program graph and waves** (the same text in S80-S87; an arrow means "needs"):

| Wave | Specs | Needs |
|---|---|---|
| 6 (title art) | S80 core | merged work only |
| 6B | S81 runtime, S82 Title Designer, S83 effects and generators, S86 3D | S80 (every hook of §6.15); within the wave, by owner: S81 owns the component, scheduler, live compose, `title_fx`, the module wiring and the 3D and mesher sub-options; S82 owns the window, its §6.8 hooks, `DocRow` and the 3D gizmo; S83 owns the catalogue values of the registry members (glyph safety, live variants, classes, cost model), `Glint3D` and the light encoding; S86 owns 3D layers, `Gizmo3DTarget`, `quality`/`debug_view`, environments and the compressed-light encoding |
| 6C | S84 library, S85 animation, S87 localisation and interchange | S80 and wave 6B; within the wave, by owner: S84 owns `PachislopPort`, `pachislop-map.json`, `TitleLibraryApply` (and `lookFromStyle`), the denied terms; S85 owns sampling, `TitleAnimPlan`, flipbooks and `<file>.anim.json`; S87 owns the sidecar `languages` table and the per-language bake sets |

Edges: S81 → S80. S82 → S80, S81 (Preview As Glyph Atlas, create menus). S83 → S80, S81 (live
compose runs its kernels; sidecar anchors). S86 → S80, S81 (scheduler, sub-options, component
fields), S82 (sections, gizmo), S83 (`Glint3D`). S84 → S80, S82 (Library panel), S83, S86. S85 →
S80, S81, S82, S83 (S86 optional). S87 → S80, S81, S82, S83, S84, S85, S86. One exception crosses a
wave: S82's Library panel milestone (its M6) draws S84's data and lands with S84 in wave 6C; the
rest of S82 lands in 6B.

- **S80 depends on** merged S03, S04, S05, S06, S14, S24, S31 only.
- **Blocks** every title spec (the hooks of §6.15). S80 lands alone and first; its hooks are
  complete when it lands, so wave 6B starts from its headers.
- **Merge hotspots:** `engine/core/asset/AssetType.h` (append order with any spec adding a type:
  S80 takes 24), `editor/asset/AssetTypes.cpp`, `DepEdge.h`, `AssetIndex.cpp`, `Render.h`
  (`ShaderType` append; S86 appends `TITLE_LIT`, `TITLE_3D_POST`, `TITLE_ENV`, `TITLE_GLINT_SPRITE` after S80's `TITLE_ART`; `BlendMode` and
  `BlendOperation` appends), `engine/CMakeLists.txt` and `engine/libs/harfbuzz/CMakeLists.txt`,
  `engine/core/util/TextShaper.*` (S51 pixel fonts and S16 text work touch fonts),
  `FrameReadback.cpp`/`MetalReadback`, `editor/Exporter.cpp` `copyAssets`, `ProjectWindow.*`,
  `InspectorWindow.cpp`, `McpToolGroups.cpp`, `McpInstructions.cpp` (S80's ≤ 40 bytes are the
  program's only guide bytes), `editor/cli/CommandLine.cpp` (`check` flags), `NOTICE.md`,
  `engine/notices.json`, `tools/ci/check-notices.py`; S80's own `TitleArtOps.cpp`,
  `TitleArtTools*.cpp`, `TitleEffectRegistry.h` and `TitleArtSchema.cpp`, which every later title
  spec extends.
- **Contract with S81:** the renderer is runtime code from day one (no editor includes);
  `AssetResolver` is S81's seam for players; `TitleArtExport::isNeeded` is S81's to fill; S81 reads
  the sidecar S80 writes.

## 9. Risks & defaults

| Risk or question | Default taken |
|---|---|
| Linear vs Photoshop (sRGB) blending makes overlay and soft light look different from Photoshop | Linear, as Pachislop, so ported looks match; documented; an sRGB blend space would be a canvas member added later without a version bump (absent = linear) |
| Adjustment layers clip HDR light | Accepted (Photoshop parity); documented in `docs/title-art.md` |
| RGBA8 fallback loses light above 1 and precision | Tolerance-gated on the S80 set; heavy light effects (S83's Light Glow) declare their own fallback behaviour |
| JFA bounded by reach changes values beyond R | Consumers clamp at R; AC23 proves shading equality |
| Clipper2 adds code to Realtime players | Only with `JASPERENGINE_TITLE_ART`; measured by S81 with `lib_size.py` |
| Dropping `HB_NO_DRAW` grows players | Only with `JASPERENGINE_TITLE_ART` (a compile definition on the harfbuzz target) |
| Auto Bake rewrites committed PNGs often | Debounced 500 ms; only existing outputs; one setting turns it off |
| GPU output differs between machines, so goldens are renderer-specific | Strict goldens on llvmpipe/lavapipe; Pachislop parity and cross-GPU checks use block/histogram tolerances |
| Light cut at the canvas edge | The canvas frames every output; `not_clipped` warns; no enlarged frames |
| Guide bytes are shared with other specs | S80 claims ≤ 40 bytes; if the headroom is gone when S80 lands, its entry moves into `jasper://docs/title-art` only and the eval case is updated (never a raised limit) |
| Pachislop references were drawn by Chromium's WebGL on unknown hardware | Parity is a loose structural check (AC12), our own goldens are the strict gate |
| Solo in Pachislop's documents | View state in S82, not stored; S84's converter drops it |
| Pass-through groups | Not in v1 (Pachislop has none); S87's PSD import maps them to Normal with a note |
| Rotation sign | Clockwise on screen (Photoshop, Pachislop); the Designer shows degrees with that convention |
| Very long texts or huge canvases | Limits in §6.1; renders above `maxSide` reduce scale with a problem |

## Deviations

1. **Reconciled: S80 lands alone in wave 6, not together with S82.** The program's waves put S82 in
   6B; S80 therefore owns what its own surface needs from the window era: the shared
   `TitlePreviewQueue` (it was S82's) and the hooks of §6.15.
2. **Reconciled: depth and MSAA targets in the pool** (S86's need). The no-depth rule now covers the
   2D passes; the pool key carries depth format and sample count.
3. **Reconciled: tool caps lowered and parameters folded into `options`/`format`.** The specs'
   claims summed to about 2,300 characters on `render_title` and 1,700 on `edit_title`, over the
   1,500 tool default; §6.11's budget table replaces S80's first caps (render 1,450 → 1,300 with
   `options`, edit 1,450 → 1,350 with the program's op list, list 800 → 860 with every kind, export
   900 → 1,000 with `format` and `options`).
4. **Reconciled: `CostTier` replaced by S83's `CostClass` C0-C4**, one cost vocabulary for the
   registry, `list_title_library` and S84's derived `cost`.
5. **Reconciled: `variants` is no longer reserved** (S87 D15); `export_title` gains no `apply` (S81
   prebakes at export instead).
6. **Reconciled: the light and max blend modes are S80's** (they were S83 R12's conditional
   addition), so wave 6B has them from S80's headers.
7. **Implementation (foundation): the document is its schema-normalized JSON tree.** `TitleArtDoc`
   (`engine/core/titleart/TitleArtDoc.h`) holds the normalized `ordered_json` tree instead of one
   C++ struct per §6.1 object. `TitleArtSchema` is the only description of the members: normalize
   fills, types, clamps and orders every object by it, the writer only formats, `TitleArtOps` and
   exposed targets address the tree by the §6.2 paths, and `TitleArtHash` takes each member's
   cache scope from it (`SchemaMember::hash`). So a member a later spec registers (S81 `runtime`,
   S85 `stretch`, S86 `style_3d`, S87's sparse `translations`) gets defaults, clamping, canonical
   writing, validation, did-you-mean and hashing from one table row, with no struct, reader or
   writer to extend. Consumers read typed values through the decoders of `TitleArtDoc.h` (Color,
   Vec2, Gradient, Curve, AssetRef, Transform) and `ParamValues`.
8. **Implementation: names.** The problem type is `TitleProblem` (S85's name; §6.3 said `Problem`).
   `FieldOverride` lives in `TitleArtDoc.h` and `applyFieldOverrides` in `TitleArtIO.h`, beside the
   field API (`setTitleArtField`, `expandFieldShorthand`). `TitleTextLayout` is a class (S85 passes
   it by reference) whose `layout(layer, overrides)` returns `LaidOutText`; §6.6's public geometry
   queries (`outlines`, `rimBands`, `contentBounds`, `reachBounds`, the element masks) are on
   `geometry::TitleGeometry` (`geometry/TitleGeometry.h`); `text/GlyphOutlines.*` stays the
   module's internal hb-draw reader. The step list S86 appends to is `TitleStepList`
   (`render/TitleRenderJob.h`).
9. **Implementation: `TitlePreviewQueue`.** Keys are strings (S82's SHA-1 and S84's SHA-256 keys
   both fit; equal keys share one job), a request names an owner (`cancelAll(owner)`, S80's form;
   S82's argument-less sketch is that with the window's owner id) and a factory, so device loss
   re-requests every job from it; `result(id)` returns the finished job, whose type the submitter
   knows (its texture), instead of a `texture(id)`. An over-budget step runs alone and the queue
   then rests ceil(cost / budget) − 1 frames (S82 R67).
10. **Implementation: checks and schema metadata.** `CheckDef::kind` also has `document_pixels` and
    `estimate` (S85 §6.7.2), and a pixel check without a renderer reports `SKIP` (check --titles
    runs headless). `SchemaMember` also carries `glyphSafety` (S81 R31 sets it per style member).
11. **Implementation: the vendored libraries wire in with their first user.** The foundation adds
    the `JASPERENGINE_TITLE_ART` option, the module's globbed source lists
    (`engine/core/titleart/TitleArtSources.cmake`, `editor/titleart/TitleArtEditorSources.cmake`)
    and the compile definition; ThorVG for `JASPERENGINE_TITLE_ART`, Clipper2, dropping
    `HB_NO_DRAW` and their `engine/notices.json` rows land with the geometry and text code that
    uses them, so no build links a library nothing calls.
12. **Implementation: later specs' ops and imports register.** `TitleArtOps::registerOp(name,
    handler)` gives a reserved name (or a new one) its handler, run inside S80's batch with the
    same `TitleOpContext` helpers and all-or-nothing rule; `registerCreateImporter(extension, fn)`
    serves `edit_title {create: {from}}`. `add_effect`'s optional `layer` means the only top-level
    layer of a one-layer document.
13. **Implementation (editor stream): two interface additions for the editor's readback.**
    `TitleTarget::colorTexture()` (a virtual returning the target's colour `TextureRender*`, null
    by default) lets the editor read a finished `TitleImage` back with `FrameReadback`, and
    `RenderRequest::outputBitDepth` (8 or 16) asks the output pass for RGBA8 or straight
    sRGB-encoded RGBA16F, which a 16-bit bake reads with 8 bytes per pixel. The renderer's
    allocator overrides the first and its output pass honours the second.
14. **Implementation: one editor seam to the renderer.** `editor/titleart/TitleArtRenderHost` owns
    the editor's `TitleArtRenderer`, the project's `EditorTitleAssetResolver` (asset database, GUID
    first) and the readback (straight sRGB RGBA8 or 16-bit, a premultiplied linear image converted).
    Every bake, render, check, thumbnail and preview goes through it. While the streams were apart
    the host compiled without the renderer and answered `renderer_unavailable`; the integration
    removed that condition and its branches, so every editor renders (a check run without a GPU,
    `check --titles`, still reports its pixel checks as SKIP: it has no render function).
15. **Implementation: bake details.** The shared frame's sides are rounded to the largest
    `round_to` of the outputs (grown right and down, then left and up at the canvas edge); each
    output renders the whole canvas at its scale (capped so the canvas fits the renderer's
    `maxSide`) and is cut to the frame, and renders the frame region again only when `max_size`
    reduces its scale. `mask` content is the render's alpha as a 1-channel PNG. The sidecar's
    `alpha_bounds` are canvas pixels like `frame`, and each output entry also carries its `size`.
    The recipe's `gpu` is `SystemRender::getDeviceName()`. A first bake writes the PNG's `.meta`
    (texture import type `ui`, quality `high`, mipmaps, `maxSize` the next power of two) in the
    same all-or-nothing batch. `export_title`'s format `bake` takes `options.bleed_px` (0..64).
16. **Implementation: freshness and checks.** `bake_fresh` warns (a document may be left unbaked)
    and its fix is the `export_title` call, not ops; `check_title {save_baseline}` takes the PNG
    path to write. A rename of a referenced font changes the document's `path` hint and therefore
    its source hash: its outputs are stale until rebaked (Auto Bake rebakes them).
17. **Implementation: editor surface.** Project window thumbnails are cached at
    `Project::getThumbnailPath` (GUID, size and modification time) and drawn again whenever the
    document or an asset it reads changes (the document change hook); the preview queue's keys are
    GUID, size, source hash and bake version as §6.13 says. Several selected `.titleart` files show
    no shared Inspector: the command palette's **Title Art: Bake All** bakes every document. The
    Inspector's Open in Title Designer stays hidden until S82 sets `InspectorWindowTitleArt::setOpenInDesigner`.
    Missing Assets lists an output never baked as "(not baked)" with a **Bake** button.
18. **Implementation: MCP.** `diff_images` is registered by its own module row ("Image
    comparison", group `editor_tools`). Numeric arguments whose ranges the budget could not carry
    (`max_width`, `columns`, the `diff_images` thresholds) are clamped by the handlers.
    `render_title` keeps the canonical render of each revision it renders (the canvas at
    min(1, 768 / width)) under `.jasperengine/cache/titleart/renders/<guid>/`, emptied the first time
    a document renders in an editor session, because `rev` numbers restart with each session.
19. **Implementation (effects stream): the pass interface of `apply` and `render`.** `EffectContext`
    and `GeneratorContext` (`render/TitleEffectContext.h`) carry a `TitlePassRunner`
    (`render/TitlePasses.h`): a pass is data (`TitlePass`: program, variant bits, the 16-vec4 block
    `u_fs_titleart`, four filtered and two raw sources, output, scissor, blend), the runner lends
    pooled targets, uploads 256-entry lookups and runs passes. Definitions build their passes with
    the effect library `effects/TitleFx.h` (Pachislop's `lib.ts`: blurs, the bounded jump-flood field,
    the layer box, ramps). `TitlePassKit` (`render/TitlePassKit.*`) is the runner that draws at once
    (tests, tools); the renderer's job runs the same calls as its steps. `ShaderType::TITLE_ART`'s
    property bits 0-6 are the `TA_PROGRAM` number, bit 7 `Sfx` and bit 8 `Dfx` the RGBA8 encodings of
    seeds and distances; programs 1-17 are the effect library's, the effects' and the generators'
    (`titleart.frag`), and the renderer's own passes take the next numbers. `EffectContext::image`
    resolves an image asset to a texture (a custom Pattern Overlay).
20. **Implementation: classes before S83.** As §6.5 says, S80's definitions declare no live variant
    and only `Rerender` (numbers, colours) and `Step` (the rest) animation classes; S83 §6.12's live
    variants and classes land with S83. Glyph safety: Color Overlay, Pattern Overlay and Adjust are
    `safe`, Gradient Overlay `unsafe` (it spans the layer's box), the others `approximate`.
21. **Implementation: units.** `gaussian_blur.scale_x`/`scale_y`, `gradient_overlay.position` and
    `pattern_fill.bg_opacity` are percentages of a radius, a travel and an opacity, not of the layer's
    size: their unit is None (the description says percent), because `ParamUnit::Percent` means % of
    the layer's source size and ops echo it in pixels.
22. **Implementation: ramps and the layer box.** Gradient ramps are RGBA8 lookups of the sRGB-encoded
    stops (TitleArtCaps' GradientLut); kernels decode with `ta_toLinear` where Pachislop sampled an
    sRGB texture (filtering before the decode: under one code). The layer-box reduction writes a pair
    of texels per block (minimum, maximum), so the RGBA8 path keeps 16 bits per coordinate; it takes
    the Seeds buffer's format.
23. **Implementation: Pattern Overlay `align: layer`** anchors the pattern at the layer's position (it
    moves with the layer, it does not turn with it); `canvas` at the canvas centre.
24. **Implementation: the docs page and the golden list.** `docs/mcp/guide/title-art-effects.md` is
    generated by `titleEffectDocsMarkdown` (`TitleEffectCatalog.h`) and checked by
    `TitleEffectCatalogTest.DocsPageIsUpToDate` (`JASPER_TEST_UPDATE_GOLDEN=1` rewrites it with the goldens) instead of
    `tools/titleart/gen_effect_docs.py` over `list_title_library`: no editor is needed, and
    `list_title_library` returns the same rows and details (`titleEffectRow`, `titleEffectDetail`,
    and the generators' `titleGeneratorRow`, `titleGeneratorDetail`). The effect goldens
    (`golden/titleart/effects/`) run each definition on the standard title rendered by the
    renderer at scale 1 (the module's text and geometry, the test fonts) and cut to its pixels;
    AC12's effect parity renders the standard title with the effect on its layer at 0.25 through
    the renderer, as Pachislop's harness does.
25. **Implementation: fallback tolerance per class.** The RGBA8 path is held to the class each
    definition declares: F0 to §6.3's tolerance, F1 (the blurs, glows, shadows and outline) to S83
    §6.5's F1 (mean ≤ 1.5/255, max ≤ 64/255, ≤ 2 % of pixels over 8/255).
26. **Implementation (renderer): the 2D title's element frames are y-up.** Their textures keep
    layer-space rows bottom first (row 0 the frame's lowest row), so Pachislop's element passes
    (seeds' Sobel normal, bevel normals, reflection, satin and pattern directions) run with their
    own y-up formulas instead of having y negated in `fields.glsl` and `element.glsl`; the mask and
    picture uploads reverse the provider's rows and the place pass maps the frame into the node
    target's rows (§6.3's memory rows hold for every target; node targets stay rows-down).
    `TitleArtScenesGpu.OriginAsymmetric` checks the result on both paths.
27. **Implementation: regions of interest align to the node's anchor.** A 2D layer's ROI aligns
    to 64 px from its position's whole output pixel, not from the canvas origin, and its cached
    pixels are keyed by its content hash (`LayerHashes::content`, added: node minus its own
    placement) and the position's fraction of a pixel: a whole-pixel move re-places the cached
    node byte for byte (R42's two passes). A node clipped by the region, with a clip group,
    canvas-space masks or an effect or generator that reads canvas positions (it sets
    `EffectContext::usesCanvasPosition` / `GeneratorContext::usesCanvasPosition`) is keyed by its
    position instead.
28. **Implementation: title frames snap to a 64 mask-px grid** from the layer origin (§6.7's frame
    plus pad rounded outward), so a small rim or bevel change keeps the frame and every other
    element's cached masks and fields.
29. **Implementation: the flood's first step is 2^(k-1), k = ceil(log2(R + 2)).** Steps 2^(k-1),
    ..., 1 reach 2^k - 1 >= R + 1 px; with the extra step-1 pass that is k + 1 passes, R43's
    count (R27's "first step 2^ceil(log2(R + 2))" would take one more pass than R43 allows).
30. **Implementation: Add composes with `LIGHT_PREMULTIPLIED`** (colour added, coverage unioned),
    blend.glsl's add, so the fixed-function and shader paths of Add agree; `ADDITIVE_PREMULTIPLIED`
    adds alpha. `BlendMode::ERASE_PREMULTIPLIED` (ZERO, ONE_MINUS_SRC_ALPHA) is added for the
    knockout; blend.glsl has internal `erase` (19) and `subtract` (20) for the 2D stack and the
    glow split on devices that render RGBA16F without blending it.
31. **Implementation: scalar field blurs stay direct.** The height's soften and the Fit cover blurs
    are separable texelFetch loops sized by the radius (at most 97 taps at sigma 32): a pyramid
    would resample the RGBA8 fallback's 16-bit fixed point byte by byte. Satin, mask feathers and
    the effects' blur use the pyramid above sigma 6.
32. **Implementation: the text and geometry seam is `TitleContentProvider`** (`render/TitleContent.h`):
    elements (with a glyph-cell cluster filter), coverage at a frame, a layer's own pixels (image
    layers, `artwork` vectors), the glyph boxes of spans and shape-mask outlines. The renderer starts
    with the process default (`setDefaultTitleContentProvider`), which the module's adapter over
    `geometry::TitleGeometry` and `TitleTextLayout` registers; without one, text, shape, vector and
    image layers report `geometry_unavailable` and draw nothing. The process default is the
    module's own (`makeGeometryTitleContentProvider`, `render/TitleGeometryContent.cpp`) unless a
    factory replaces it; `TitleGeometry::elements` takes the glyph cell's cluster. The renderer
    tests install an analytic provider.
33. **Implementation: interface additions.** `TitleRenderStep::ready` (a step waits while its
    program compiles; `TitleRenderJob::waiting()`) and `passes` (an effect's kernel step runs
    several); `RenderRequest::output` (`TitleOutputFormat`: 8-bit dithered sRGB, 16-bit sRGB,
    premultiplied sRGB for displays, or the linear accumulator for S81's live compose);
    `RendererStats::floodPasses` / `floodPassesUnbounded`; `TitleArtRenderer::setContentProvider`,
    `cacheEntries`, `poolStats`, `simulateDeviceLossForTesting`; `TITLE_ART_TEST_UNBOUNDED_FIELDS`.
34. **Implementation: device loss fails the job in flight** with `device_lost` (its targets are
    gone); the renderer drops pool, caches and programs at once and the owner (the preview queue,
    the bake) begins again. A job does not restart itself: its step list was planned against caches
    that no longer exist.
35. **Implementation: `TitleImage::alphaBounds` is the drawn region** (the image's rect). Exact alpha
    bounds come from the readback the bake and the checks make; a preview never syncs the GPU.
36. **Implementation: the glow output** is the full composite minus the ink composite, colour only,
    written as light (alpha = the largest channel, straight colour) so the PNG drawn additively over
    the ink output gives the light back.
37. **Implementation: Fit cover seeds hold centres only**; a circle's radius is read back from the
    distance field at its centre (the RGBA8 fallback's seeds have no room for a radius).
38. **Implementation: glyph cells.** The back part is the cell's rims with the layer's effects and
    masks, the front part its face alone; the cell's image is front over back.
39. **Implementation: element seeds add the document's as seed + (document seed - 1)**, so the
    default document seed 1 renders Pachislop's patterns, textures and dry brush.
40. **Measured: R42's colour budget holds for the title, not its layer effects.** On the
    classic-style fixture of `TitleArtRendererGpu.CacheCascade` (four rims, a gold face, a glow
    and a drop shadow standing in for Outer Glow and Drop Shadow) a full render is 57 passes, a
    face colour change 23, a whole-pixel move 2 and an outer rim width change 20 (35 %). The title
    itself re-runs only its stack, placement, compose and output (8 passes); the two layer effects
    re-run because an effect's output is a function of its input pixels and a definition cannot
    say it reads only their alpha. A later `FxInputs` member (alpha only) would let the runner key
    such effects by the field hash and hold the colour change at 8.
41. **Implementation: one shader type, two fragment files.** The renderer's programs are
    `ShaderType::TITLE_ART` numbers 18-41 (`render/TitlePrograms.h`, `titleart/render/programs.glsl`)
    after the effects' 1-17 (`render/TitlePasses.h`); the variant compiler sends 18 and up to
    `titleart_render.frag` (includes in `engine/shaderlib/titleart/render/`) and the rest to
    `titleart.frag`, so the two program sets keep separate parameter blocks (48 vec4s and 16) and
    neither file includes the other's helpers. Both share `titleart.vert` and `titleart/patterns.glsl`.
42. **Implementation: effects and generators run through `TitleJobRunner`**, the renderer's
    `TitlePassRunner` (`render/TitleJobRunner.h`): one per kernel step, lending pool targets,
    caching lookups by content and drawing through `TitleGpu`. A pass whose program is still being
    compiled draws nothing; the step then waits for it and runs the definition again (a definition
    writes all of its output from its inputs), so a step never leaves half an effect.
43. **Implementation: what may move a cached node.** A definition says whether what it drew read
    canvas positions: its `apply` or `render` sets `usesCanvasPosition` on its context (Pattern
    Overlay with `align: canvas`, Fill's gradients), and the node's cached pixels are then kept
    at their position (whether a node may move is known after its steps ran, so the cache entry
    records it then). A following generator (`FollowMode` Optional or Required) gets the layer its
    `follow` names in `GeneratorContext::follow`: the layer source rendered alone, cut to the
    generator's target; a cycle reports `follow_cycle` and the generator draws as following
    nothing.
44. **Implementation (text and geometry): the geometry interface carries what pixel layers and
    SVG strokes need.** `geometry/TitleGeometry.h` gains `StrokeCap`, `StrokedPath` (SVG strokes
    stay strokes: `TitleElement::strokes`, widened by 2 x grow in rim masks), `ElementSource`
    (`TitleElement::source`: an Image layer's decoded texture or an `artwork` SVG with the rect it
    is drawn over) and `TitleElement::close` (a pixel layer's contour-rim closing),
    `GlyphSpan` / `LayerElements::spans` (the boxes of the `line` / `char` span map, after the
    outline operations), `elementBounds()` (§6.7's frame rule per element, without the 3 px
    margin), `rasterizeSource()` (the `fill.kind: source` colour of a pixel layer at a mask frame)
    and `TitleGeometry::clear()`. `rasterizeElement` applies the element's offset and, for a
    `fill_counters` rim, the flood itself (`fillCounters()` is idempotent, so calling it again is
    harmless); for a pixel layer it returns the picture's alpha and the renderer grows contour
    rims by `grow` through the alpha's distance field, hull and box plates being outlines.
    `TitleTextLayout.h`'s `LaidOutGlyph` gains `transform` (font units to layer space) and
    `outline` (layer space, before warp, distort and weight); `subFont()` serves the faces of
    the object's own layouts only (a sub-font is cached by its face). The "sub-font" of a
    (face, `variations`) pair is `hb_font_create` on the face's shared `hb_face_t` with the
    variations set, not `hb_font_create_sub_font`: a HarfBuzz sub-font's default functions
    forward advances and outlines to the parent at the parent's coordinates, so its variations
    would change shaping but neither advances nor outlines (measured: Oswald `wght` 300 and 700
    gave equal advances that way).
45. **Implementation: a laid-out cluster is a HarfBuzz cluster.** `LaidOutCluster` is one shaped
    cluster: a grapheme cluster, or the code points a ligature covers; its CharAdjust is the one
    keyed by its first code point (the others of a ligature have none). A tate-chu-yoko run is one
    cluster holding its glyphs side by side. Vertical glyph forms come from top-to-bottom shaping
    while the cells are Pachislop's one-em cells (the font's vertical metrics only choose forms).
46. **Implementation: proportional spacing by difference.** With `palt` / `vpal` in the font, a
    line is shaped with and without the feature and the difference, times `proportional`, is
    applied to the characters 詰め applies to (kana, CJK punctuation, full-width symbols), which
    is Pachislop's partial palt; turning the feature on would move every glyph it lists (often
    kanji too) by the full amount. Without them, advances fit the ink plus 5 % of the em a side.
47. **Implementation: rims are elements when enabled with a width** (Pachislop's
    `elementSpecs`): a 0-width rim adds no element and no band, and `rimBands()` lists the same
    rims as `elements()`. A shape's burst / shout jitter seed is its `seed` plus the document's
    `seed` (§6.1: "added to every element seed").
48. **Implementation: Vector layers.** `shape` mode fits the ink bounds (stroke widths included)
    to `size` as Pachislop does; warp and distort map the fills and the strokes' centre lines,
    weight offsets the fills and widens strokes by 2 x the offset. `artwork` mode fits the SVG's
    own viewport (LunaSVG's document size), so the picture is placed as the file was drawn;
    its hull and `outlines()` (S86) come from a raster of it. What shape mode cannot draw is
    `svg_feature_ignored` with `detail.features`; an unreadable SVG or image is `asset_missing`
    with the reason.
49. **Implementation: bounds.** `reachBounds` grows the content by the effects' reach in canvas
    px (Px params scale with `px_scale`, so a reach is the same in document space at any scale),
    1.5 x the masks' feather and one output pixel (1 / `pxScale`) of antialiasing; a group's
    content bounds are the union of its drawn children's reach bounds; content bounds of a 2D
    title include its rims by `elementBounds()`.
50. **Implementation: missing glyphs and fonts.** `fonts_with_it` lists the project fonts that
    cover at least one missing code point, those covering most first, then by path. R48 is
    measured with the fonts already loaded by the resolver (it loads each file once for its
    lifetime); the budget covers shaping, sub-fonts, outlines and layout. Besides Dela Gothic One
    the text tests use Stick (palt / vpal), Oswald (variable glyf) and Source Sans 3 (CFF and
    CFF2), all OFL from their original sources (`NOTICE.md`); `.ttc` members are tested on a
    collection the test assembles from two of them, so no collection file is committed.
51. **Integration: one adjustment implementation.** Adjustment layers run `applyTitleAdjustment`
    (`effects/TitleAdjust.cpp`, `titleart/adjust.glsl`), the Adjust effect's code, in a kernel step
    with an `EffectContext`, then the renderer's ADJUST_MIX; the renderer's own copy
    (`render/Adjustments.*`, `titleart/render/adjust.glsl` and its program) is gone, so the
    renderer's programs after Blend moved down one number (Deviation 41).
