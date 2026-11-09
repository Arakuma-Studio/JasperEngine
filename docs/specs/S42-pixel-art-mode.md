# S42 — Pixel Art Project Mode & Validator: the Pixel Art preset, a 2D Pixel Art template, Pixel Art import, `check_pixel_art`

**Family / Depends on.** Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5B. Depends on
**S41** (the Pixel Perfect Camera, Pixels Per Unit, the pixel-snap rule and the layout function it
measures with), **S43** (palette conformance, the `palette` rule) and **S44** (Aseprite sheet pages
are pixel-art textures), all wave 5A. Uses S14 (texture import settings and cook), S27 (project hub
and templates), S25 (settings registry, command catalog), S24 (undo groups), S31 (`call`, budget).
Reads S45, S48, S49 and S51 fields when they exist (same wave); S46 and S50 (wave 5C) extend the
check when they land (§8). Owner area: `editor/pixelart/` (`PixelArtCheck`, `PixelArtRules`, `PixelArtPreset`,
`PixelArtCheckWindow`), `editor/mcp/tools/PixelArtTools.cpp`, the `check --pixel-art` CLI category,
the Pixel Art fields of `editor/texture/TextureImportSettings.*` and the nearest filter and alpha clip
of `engine/core/texture/` (`MipChain`, `TexturePrepare`), `editor/resources/templates/2d-pixel-art/`,
`docs/mcp/guide/pixel-art.md` (`jasper://docs/pixel-art`, the family's guide page).

## 1. Goal

One click makes a project render pixel art correctly, and one check proves it stays correct. **Pixel
Art Mode** is a project switch: new textures import as pixel art (point filtering, no mipmaps,
lossless, a nearest resize, binary alpha), new 2D scenes get a Pixel Perfect Camera, the Scene view
uses pixel zoom, and the validator runs on import and on Play. The **Pixel Art preset** converts an
existing project in one undoable step after a preview. A **2D Pixel Art** template starts a new one
already correct. The validator, `check_pixel_art` for agents, the **Pixel Art Check** window for
people and `check --pixel-art` for CI, returns PASS/WARN/FAIL per rule with the measured value, the
expected value, a verdict (`clean`, `fixable`, `needs_person`) and a one-click undoable fix for every
finding it can fix. It turns the rules people learn the hard way (blur from linear filtering, columns
5 and 6 px wide at 5.45×, a character with bigger pixels than the scene) into numbers an agent can act
on.

## 2. Scope

**In scope**
- Project Settings › Pixel Art (`pixel_art.*`): Pixel Art Mode and what it changes; the runtime
  project setting **Default Filter Mode** (`graphics.default_filter_mode`).
- The Pixel Art preset: a planned list of changes (preview), applied as one undo step; offered in
  Project Settings, by the `pixel_art_mode` rule's fix and in the new-project flow.
- Texture import: a per-texture **Pixel Art** switch (`pixelArt`) whose defaults are point filtering,
  no mipmaps, uncompressed on every platform, the new **Nearest** resize and mip filter, binary alpha
  (**Alpha Clip** at the cutoff) and no streaming; a **Filter Mode** and **Pixels Per Unit** import
  default copied onto references when a texture is assigned.
- The validator: thirteen rules (§5.3; ten of S42's own, `palette` from S43's hand-off,
  `sprite_variants` from S48's, and `cleanup_outputs`, which S46 registers), the measurement of
  each, a fix per finding, the verdict; one core shared by MCP (the live editor) and the CLI (files
  on disk). It is the family's one validator: the checks other specs hand to it are listed in R10.
- `check_pixel_art` (group `pixel_art`), the Pixel Art Check window, Console summaries, `check
  --pixel-art` (and its place in `--hook`).
- The **2D Pixel Art** template in the project hub (S27) and `create_project {template}`.
- `docs/mcp/guide/pixel-art.md`: the rendering rules in our own words, the agent loop, the rule ids;
  `docs/pixel-art.md` user docs (shared with S41).

**Out of scope**
- The camera, snapping, PPU and views themselves: **S41**.
- Detecting and repairing upscaled or AI-made art (grid recovery, palette snapping, specks): **S46**;
  S42 reports a texture whose art is not one texel per pixel only through the texel size of its use,
  and points to S46 when S46 is present (§5.3, `texel_size`).
- Pixel fonts (S51), pixel particle modes (S49) and sprite atlases (S45), same wave: S42's rules
  read their fields when they exist and degrade to a WARN with `needs_person` otherwise. Palettes
  (S43) and Aseprite sheets (S44) have landed before S42 and are read directly.
- Moving sampler state from references into the importer (S14 §2 keeps it out): the import Filter
  Mode is a default copied at assignment, never a second source of truth.
- Do-not-copy list (`docs/specs/README.md`): nothing applies.

## 3. Current state (read on `specs/pixel-art` 1b1c34fbd)

- **Texture import** (S14). `TextureImportSettings` (`editor/texture/TextureImportSettings.h:121-150`)
  has type, sRGB, alpha source, alpha is transparency, readable, max size, resize filter, mipmaps,
  mip filter, mip limit, preserve coverage, alpha cutoff, quality, effort, streamable, ship source and
  platform overrides; version 1, unknown keys kept. The filters are `MipFilter { Box, Triangle,
  Mitchell, CatmullRom }` (`engine/core/texture/MipChain.h:20`): **no nearest**, so a pixel-art sheet
  larger than its max size is resampled with Mitchell (blurred). Sprite and UI types turn mipmaps off
  and alpha-is-transparency on (`TextureImportSettings.cpp:199-205`), but the R5 format table cooks
  them to BC7, ASTC, ETC2 or Basis (S14 §5 R5), all **lossy**; only `quality: none` gives RGBA8.
  `alphaCutoff` exists only for coverage preservation; nothing binarises alpha.
  `TextureProcessing` (`engine/core/texture/TexturePrepare.h:37-58`) is what the cook and the raw
  load both run, so a new processing step must live there.
- **Filtering lives on references.** Every texture property carries its own min/mag filter, default
  LINEAR (`engine/core/texture/Texture.cpp:44-45`), always written to scenes
  (`editor/Stream.cpp:1485-1486`). MCP `set_texture_settings` edits one property's filters
  (`editor/mcp/tools/AssetTools.cpp:4148-4169`; its description already says "mag_filter nearest
  for pixel art"). A sprite dropped into a scene, a Lua `Sprite:setTexture` and an MCP
  `create_entity` all start bilinear. There is no project default filter.
- **Templates** (S27). Built-ins `empty`, `3d`, `2d`, `ui` in `editor/resources/templates/<id>/`
  with `template.json` (`editor/hub/TemplateCatalog.h:24-37`, schema version 1), listed by
  `list_project_templates` (`editor/mcp/tools/EditorToolsTools.cpp:676`) for `create_project
  {template}`. The `2d` template is a 1280×720 letterboxed canvas, fixed resolution off
  (`editor/resources/templates/2d/Main.scene`: `fixedResolutionEnabled: false`), and its
  `sprites/player.png.meta` resolved to `type: default` ("inferredFrom: no references"): mipmaps on and
  BC1/BC7 compression for the only sprite of the 2D template.
- **Project settings** go through the S25 registry (`editor/settings/SettingsRows.cpp`, keys
  `graphics.*`, `player.*`, `audio.*` …) and MCP `get/set_project_settings`; there is no `pixel_art.*`
  page.
- **Checks.** `jasperengine-editor check` (`editor/cli/CommandLine.cpp:2116-2190`) runs categories
  `--lua --shaders --cpp --ui --textures --anims --vat --normals --videos`, prints
  `path:line:col: error|warning: message (fix: …) [code]`, exits 0/1/2 and has an agent `--hook`
  mode. Findings are `ProjectChecks::Finding` (`editor/util/ProjectChecks.h:42-51`). The closest
  precedent for an offline asset check over scenes is `check --normals`
  (`editor/spritenormals/SpriteNormalsCheck.h`: pure parsing of PNG headers, scene/prefab YAML and a
  cache). Nothing checks pixel-art rules.
- **Fonts.** Text is stb_truetype coverage rasterised (`engine/core/util/STBText.cpp:216`), with an
  SDF option; `TextComponent` has `fontSize` (`engine/core/component/TextComponent.h:29`) and no
  antialiasing switch (S51 adds pixel fonts).
- **MCP.** No `pixel_art` group yet (`editor/mcp/McpToolGroups.cpp:18-36`). The guide's `## 2D and
  3D` section (`editor/mcp/McpInstructions.cpp:144-148`) states units are canvas points. No guide page
  covers pixel art; `docs/mcp/guide/textures.md` documents the import fields.

## 4. Reference

| Source | Files | What we take | What we change |
|---|---|---|---|
| @asynkimo post | `/tmp/pixelart-research/x-post/fx.json` (post text; video `vid.mp4`) | The rules (nearest everywhere, no mips, lossless; one base resolution scaled by whole numbers with bars; snap camera and sprites; one pixel size; 90° steps; pixel fonts at native size without antialiasing; particles on the grid; check at 1×) and the Godot/Unity checklists | Our own wording (§6.5) and a measurable rule per item (§5.3); "check at 1×" becomes S41's Pixel Scale 1× and `check_pixel_art {image}` |
| ritmo (MIT) | `packages/tools/src/pixel/core/report.ts:1-9`, `:22` (verdicts `clean`/`fixable`/`needs-person`, "never a confident bad fix"), `:183-228`; `packages/tools/src/pixel/cli/commands.ts:39` (exit codes 0/3/4) | Verdicts for agents first, with the rule that low confidence means a person looks | Our verdict is computed from findings and their fixes (D5); exit codes stay `check`'s 0/1/2 (D6) with the verdict in the JSON |
| SpriteCook skills (MIT) | `x-post/spritecook-skills/skills/spritecook-polish-sprites/scripts/compare_alpha.py:11-15` (`255 if value >= cutoff else 0`) | Binary alpha at a cutoff | An import step (`alphaClip`) in `TexturePrepare`, so the cook and the raw path agree; cutoff in 0–1 like our `alphaCutoff` |
| S35 (ours) | `docs/specs/S35-agent-audio.md` R21, R23 | Check entries with `measured`, `threshold`, `limited_by`, `fix_hint`; "never loosen thresholds" | Rules here have no tunable thresholds (D7); every finding names its fix |
| `check --normals` (ours) | `editor/spritenormals/SpriteNormalsCheck.h` | Offline scene/meta parsing shared with the editor | One core with two inputs (D3) |
| Unity, Godot (behaviour) | — | Unity: Filter Mode Point, Compression None, the same Pixels Per Unit on every sprite, Presets; Godot: Default Texture Filter Nearest, Lossless import, fonts without antialiasing | Unity names in the importer (Filter Mode: Point / Bilinear / Trilinear); one switch instead of a preset asset |

## 5. Requirements

### 5.1 Functional — Pixel Art Mode and the preset

- **R1. Pixel Art Mode** (`pixel_art.enabled`, default false). While on:
  - a texture imported for the first time whose type infers to `sprite` or `ui` (S14 R2) gets
    `pixelArt: true` written into its import settings with the inference (written once, as S14
    writes the type; turning the mode off later changes no file);
  - a new 2D scene (File › New Scene, `create_scene` with a 2D type, the hub) gets a Pixel Perfect
    Camera on its main camera (S41, reference = project canvas);
  - the Scene view's Pixel Zoom and Pixel Move Snap default on (S41 R21);
  - the validator runs on import (`pixel_art.check_on_import`, default true) and at Play start
    (`pixel_art.check_on_play`, default true) and reports to the Console (R25);
  - `check --hook` includes the pixel-art category (R24).
  Turning the mode on or off is one setting change; it never edits other settings (that is R2's job).
- **R2. The Pixel Art preset** (`editor/pixelart/PixelArtPreset.{h,cpp}`). `plan(options)` lists every
  change with its current and new value; `apply(plan)` performs them as one undo step
  ("Apply Pixel Art Preset"). Options and defaults:

  | Change | Default | Option |
  |---|---|---|
  | `pixel_art.enabled` | true | — |
  | `graphics.canvas_width/height` (the base resolution) | 320×180 | 256×144, 320×180, 384×216, 480×270, 640×360, custom |
  | `graphics.scaling_mode` | `integer` | `integer`, `integer_expand` |
  | `graphics.pixels_per_unit` | 1 | 1–256 |
  | `graphics.default_filter_mode` | `point` | — |
  | `player.window_width/height` | 4× the base (1280×720 for 320×180: a window that fits a 1366×768 laptop) | 2×…8× |
  | Pixel Perfect Camera on the main camera of every 2D scene in the build list | on | on/off |
  | `pixelArt: true` on every texture of type sprite/ui or referenced by a Sprite, Tilemap, UI Image or particle renderer | on | on/off |
  | references to those textures set to point filtering | on | on/off |

  Settings go through the registry's undoable path, metas through `WriteFilesCmd`, components through
  the component commands, all inside one `UndoGroupScope` (S24); the textures recook afterwards (S14
  R8). The plan is deterministic (sorted by kind, then path).
- **R3. Default Filter Mode** (`graphics.default_filter_mode`: `bilinear` default, `point`). A runtime
  project setting generated into the game's main (`Engine::setDefaultTextureFilter`): every `Texture`
  constructed at runtime starts with it (replacing the fixed LINEAR of `Texture.cpp:44-45`), so a Lua
  `Sprite:setTexture("hero.png")` in a pixel-art game is crisp. Saved references keep their own
  filters.

### 5.2 Functional — texture import

- **R4. Pixel Art import switch.** `textureImporter.pixelArt` (optional bool). When true, absent
  fields resolve to the Pixel Art defaults instead of the type's: type `sprite` (if not set and not
  inferred otherwise), `mipmaps: false`, `mipFilter: nearest`, `resizeFilter: nearest`, `quality: none`
  on every family (uncompressed RGBA8 in KTX2 with zstd: lossless), `alphaIsTransparency: true`,
  `alphaClip: true` at `alphaCutoff 0.5`, `streamable: false`, `filterMode: point`. Explicit fields
  and platform overrides still win (and the `mipmaps_compression` rule reports a lossy override). In
  the Inspector: a **Pixel Art** checkbox under Texture Type, and the Presets menu's **Pixel Art**
  entry, which sets the checkbox and clears the fields that contradict it (one undo step).
- **R5. Nearest filter.** `MipFilter::Nearest` (appended; YAML `nearest`, Inspector "Nearest (Point)")
  for `resizeFilter` and `mipFilter`. Destination texel i reads source texel `⌊(i + 0.5) · s/d⌋` per
  axis (texel centres; a 2× downscale keeps the top-left texel of each 2×2 block), no blending, alpha
  kept. `MipChain` implements it for both the cook and the raw path.
- **R6. Alpha Clip.** `alphaClip` (optional bool, default false; true under `pixelArt`). After the alpha
  source and before resizing, every texel's alpha becomes 255 when `a ≥ alphaCutoff · 255` and 0
  otherwise; RGB of cleared texels then follows `alphaIsTransparency` bleeding. In `TextureProcessing`
  (`TexturePrepare.h`), so the cook and the raw load give the same bytes.
- **R7. Filter Mode and Pixels Per Unit import defaults.** `filterMode` (`point`, `bilinear`,
  `trilinear`; absent: `point` under `pixelArt`, else the project's Default Filter Mode) and
  `pixelsPerUnit` (absent: the project's). When a texture is assigned to a property in the editor
  (Inspector field or drop, drag from the Project window creating a Sprite or Tilemap, MCP
  `create_entity`/`set_properties`/`add_component` with a texture), the property's min/mag filters
  become `NEAREST/NEAREST`, `LINEAR/LINEAR` or `LINEAR_MIPMAP_LINEAR/LINEAR`, and a Sprite's or
  Tilemap's `pixelsPerUnit` becomes the import's when it differs from the project's — inside the
  assignment's undo step. Changing a texture's Filter Mode in the importer offers "Apply to N
  references" (one undo step); references left behind are `filtering` findings.
- **R8. Cache stability.** `ResolvedTextureSettings::canonical()` appends the new fields only when they
  differ from today's resolution, so every texture without them keeps its cook key (S14 R7) and no
  existing project recooks on upgrade.

### 5.3 Functional — the validator

- **R9. Scope.** A **pixel-art drawable** is a Sprite, Tilemap, UI Image, Text, particle system or
  Sprite Batch (S52) in a scene whose main camera (or a layer scene's camera) has an enabled Pixel
  Perfect Camera, or that draws a texture with resolved `pixelArt`. S50 adds Tilemap Layers to the
  input when it lands (§8). A **pixel-art texture** is one with resolved `pixelArt`, one a pixel-art
  drawable references, or a generated page of a sheet such a drawable uses (an `.aseprite` sheet's
  pages, S44; an atlas page, S45). A rule with nothing in scope is PASS with `checked: 0`.
  Inputs: `scenes` (default: the build list plus open scenes), `paths` (textures and fonts; default:
  every pixel-art texture), `rules` (default all).
- **R10. The rules,** in this order (ids are stable API; each finding has `measured` and `expected`).
  The rows marked "hand-off" are checks other specs give to this one validator; a spec that lands
  after S42 registers its rule through `PixelArtCheck::registerRule` (§6.1) and adds its inputs to
  `PixelArtCheckInput`, so S42 never needs code from a later wave:

  | id | Checks | Measured | PASS / WARN / FAIL | Fix |
  |---|---|---|---|---|
  | `pixel_art_mode` | the project | the preset's settings that differ (R2) | PASS when Pixel Art Mode is on and the base resolution, scaling, PPU and default filter match a pixel-art layout; WARN when pixel-art content exists but the mode is off or one of them differs | apply the preset's project part (R2 rows 1–5) |
  | `integer_scale` | each scene's main camera | per screen of `pixel_art.check_screens`: `{scale, integer, bars}` from S41's `computePixelPerfectLayout` (or `computeViewLayout` without a component) | PASS when integer on every screen; WARN for Stretch Fill (soft seams by design); FAIL when any screen scales by a fraction (no component and a non-integer Scaling, or the scene's fixed resolution with Fit) | add/enable a Pixel Perfect Camera (reference = the fixed resolution if set, else the canvas) |
  | `filtering` | every reference to a pixel-art texture | `{min_filter, mag_filter}` | PASS nearest/nearest; FAIL otherwise | both filters `nearest` on that reference |
  | `mipmaps_compression` | every pixel-art texture, per family, and every texture a Palette Swap material draws (S43 R15, `palette_swap_lossy_texture`) | `{mipmaps, format, resize_filter, resized_from}` from the resolved import (for an `.aseprite` sheet, its `texture.compression`, S44 §6.4) | FAIL with mipmaps or any block/Basis format (BC*, ASTC*, ETC*, EAC*, ETC1S, UASTC); WARN when the max size clamps it (uneven pixels) or its resize filter is not nearest; PASS otherwise | `pixelArt: true` and remove the contradicting fields/overrides (recook); an `.aseprite` file: `texture.compression: none` |
  | `palette` (S43's hand-off) | every pixel-art texture, when the project has a Default Palette (S43 R4b) or the texture's material names a Palette Swap source (that palette then) | S43's `apply_palette {measure_only: true}` core: `{palette, verdict, off_palette_pixels, colors, max_delta_e, lost_groups}` | PASS `clean`; WARN `fixable` (off-palette pixels a remap fixes); FAIL `needs_person` (lost colour groups of ≥ 16 px) | `apply_palette {path, palette, output: true}` (one change `{target: <texture>, field: "pixels", from: <sha256>, to: "palette <path>"}`); `needs_person` keeps S43's lost groups as the reason |
  | `texel_size` | every pixel-art drawable | `{x, y}` texel size in base pixels = world scale × PPU_project / PPU_drawable (UI: rect size / texture size × scale) | PASS 1×1 (±1e-4); WARN a whole number > 1 on both axes, equal (deliberate big pixels); FAIL anything else (fractional, < 1, x ≠ y) | scale to ±1 (sign kept) when that makes it 1; else drawable PPU to 0 (the project's); UI Image size to the texture's; otherwise `needs_person` (with S46 present: its detected cell size and S46's fix named) |
  | `off_grid` | every pixel-art drawable's authored position (edit mode) | the snap anchor (S41 §6.2: the drawn quad's min corner) in base pixels and its fraction | PASS integer (±1e-3); WARN otherwise (drawn snapped, so authored ≠ drawn and ties flicker in motion) | move by the S41 snap delta (`snapWorld`), children kept |
  | `rotation` | pixel-art drawables (Text included, S51 R8) and each pixel-art scene's main camera | world rotation (Z, and X/Y), the effective Pixel Rotation, the camera's roll | PASS on 90° steps (X/Y 0 or 180) or Rotate On Grid with the render texture; WARN otherwise (drawn rounded to 90°, or rotated at screen resolution), and WARN a rolled camera (its draws are not grid-aligned, so they snap pivots only, S41 D8) | round the Transform rotation to the step S41 draws (default) or set Pixel Rotation `on_grid` (second fix option); camera roll to 0 |
  | `font_antialiasing` (S51's hand-off) | Text in pixel-art scenes, and fonts in `paths` | S51's `analyzeFontForPixelArt` (S51 D2): `{font, kind: vector|bitmap, size, native_size, pixel_scale, antialiased, partial_alpha_share, vector_fallbacks[], pixel_grid_detected}` | PASS a bitmap font at a whole multiple of its native size with `partial_alpha_share` 0 and no vector fallback; WARN a vector font (antialiased coverage), a bitmap font at another size, partial alpha, or a vector font in its fallback list (S51 R14); FAIL a bitmap font that does not load (S51's typed error) | font size to the nearest whole multiple of the native size; `alpha_cutoff: 128` on the `.bitmapfont`; remove the vector fallback; a vector font whose pixel grid S51 detects: "create a .bitmapfont with source truetype" (S51 R27); other vector fonts: `needs_person` ("use a pixel font"). Before S51 lands every font reads as `vector`: WARN, `needs_person` |
  | `particles_grid` (S49's hand-off) | particle systems in pixel-art scenes | `{texture_filter, render_mode, sizes_px, snapping, pixel_art_module: {snap, pixel_sizes, fade}, camera: ortho|perspective}` | FAIL a linear texture filter; WARN stretched or velocity-aligned render modes, sizes that are not whole pixels, no Pixel Art module or its `snap`/`pixelSizes` off, `fade: alpha` (partial alpha), Pixel Snapping off on the camera, or a perspective camera (no world grid, S49 R2) | nearest filter; add S49's Pixel Art module (`pixelArt {snap, pixelSizes, fade: dither}`) when S49 is present |
  | `anti_aliasing` | each pixel-art scene's main camera | `{fxaa, msaa_samples, smoothing_effects[]}` | PASS none; WARN FXAA, MSAA > 1 on the base target, or a blur pass over the whole frame | FXAA off, MSAA 1 |
  | `sprite_variants` (S48's hand-off, S48 D2) | the project's `library:sprite` materials, from the variant collector (`ShaderVariantCollector`) | `{variants, by_material: [{material, variants}]}` (per blend mode and light state) | PASS ≤ 24 distinct variants; WARN above, naming the materials that add the most | `needs_person` ("merge feature sets or record the warm-up list"); no automatic fix |
  | `cleanup_outputs` (S46's hand-off; S46 registers it when it lands, S46 R49) | outputs of S46's cleanup (a `jasper.pixelcleanup` recipe or `import.pixelCleanup`) | `{stale, forced, fail_lints[]}` | FAIL a FAIL lint on the output; WARN `stale` (source changed) or `forced` | `fix_pixel_art {source, rerun: true}` for `stale`; `needs_person` for `forced` and FAIL lints |

- **R11. Findings.** Each finding: `id` (`F1`…, in rule order then target order), `key` (stable:
  `<rule>:<target>[:<field>]`, e.g. `filtering:scenes/Main.scene#/Player:sprite.texture`), `rule`,
  `status`, `target` (`{kind: project|scene|entity|texture|reference|font, path, scene, entity, field}`
  with S31's entity paths), `measured`, `expected`, `message` (one sentence), `fix`
  (`{options: [{id, label, changes: [{target, field, from, to}]}]}`, the first option the default) or
  `null` with `needs_person` (the reason). Rules: `{id, status, checked, findings}`.
- **R12. Verdict.** `clean`: no WARN and no FAIL. `fixable`: every WARN/FAIL finding has a fix.
  `needs_person`: at least one has none. Ordered as ritmo's (`clean` > `fixable` > `needs_person`).
- **R13. Fixes.** `fix` selects findings by `key`, by `F<n>` (resolved on the current state), by rule
  id, or `"all"`, each optionally with `@<option>` (`rotation:...@on_grid`). Without `output` the call
  returns the plan (`fix_plan`: every change with from/to) and modifies nothing. With `output: true`
  it applies the plan as one undo step ("Fix Pixel Art (n)"), queues the recooks, re-runs the check
  on the same scope and returns `applied` (`{undo_step, changed[], recooking[]}`) and `after` (verdict
  and counts). A selected key that no longer matches a finding is a typed error, not a silent skip.
- **R14. Determinism.** The same project state and arguments give the same structured result, byte
  for byte (sorted keys and lists, no timestamps, floats printed by `textureCanonicalNumber`'s rule).

### Platform & backends

- **R15.** The validator, preset and window are editor-only. The runtime additions are the Default
  Filter Mode (every backend: a sampler choice), the Nearest filter and Alpha Clip (CPU, cook and
  raw path, every platform); uncompressed RGBA8 sprites load on every backend and family (S14 R11
  needs no fallback for them).

### Performance

- **R16.** `check_pixel_art` on `tools/fixtures/pixel-art-large` (12 scenes, 600 textures, 6,000
  entities): ≤ 1.0 s warm and ≤ 3 s cold (metas parsed) on one S06 Background worker; over 1 s it is
  an `McpJobs` job. The CLI: same core, same budgets. The import hook (R1): ≤ 2 ms of main-thread
  time per imported texture (the check itself runs on the worker, debounced 1 s). With Pixel Art Mode
  off nothing is registered: zero cost on import, Play and frames.
- **R17.** Uncompressed sprites cost VRAM: the importer's info line shows it, and the check's
  `mipmaps_compression` measured value includes `vram_bytes`; a pixel-art texture above 2048² gets a
  WARN-level note in the importer (not a rule), since such a sheet is better split (S45).

### Editor UX (human)

- **R18. Project Settings › Pixel Art.** Pixel Art Mode; Base Resolution (the canvas, with the R2
  presets); Upscale Mode default for new Pixel Perfect Cameras; Pixels Per Unit (the
  `graphics.pixels_per_unit` row shown here too); Default Filter Mode; Check on Import; Check on
  Play; Check Screens (a list editor); and **Apply Pixel Art Preset…**, which opens the preview dialog
  (R2's table as rows with current → new, the options as checkboxes, a count of textures and scenes
  touched) with Apply and Cancel.
- **R19. Pixel Art Check window** (Window › 2D › Pixel Art Check, `window.pixelArtCheck`):
  - toolbar: **Run**, Scope (Open Scenes / Build Scenes / Selection / Project), a Rules filter, the
    verdict badge, **Fix All (n)…** (preview dialog listing every change, Apply = one undo step);
  - a table grouped by rule (collapsible, status icons in `Theme::Colors`), one row per finding:
    target (click selects the GameObject or pings the asset, double-click frames it), measured,
    expected, and a **Fix** button (tooltip: the change) or "Needs a person: <reason>"; a second fix
    option in the row's dropdown;
  - after a fix or an asset import the window re-runs on its scope (debounced, on the worker);
  - **Copy Report** copies the JSON of R11; commands `pixelArtCheck.run` (F5 in the window),
    `pixelArtCheck.fixSelected`, `pixelArtCheck.fixAll` in S25's catalog.
- **R20. Importer.** Texture Import Settings shows the Pixel Art checkbox, the Presets entry, Filter
  Mode, Pixels Per Unit and Alpha Clip; the preview shows the texture at whole-number zoom with
  nearest sampling and a 1× toggle; a pixel-art texture with a lossy platform override shows the
  `mipmaps_compression` message inline.
- **R21. Project hub.** The template grid shows **2D Pixel Art** (category Core, after 2D). Choosing it
  shows a Base Resolution choice (R2's list, default 320×180) in the New Project form, written into
  the project canvas and window size before the first open (templates that declare `"pixelArt": true`
  in `template.json` get the field).

### Agent UX (MCP, CLI, docs)

- **R22. `check_pixel_art`** (group `pixel_art`; §6.3). Arguments `scenes`, `paths`, `rules`, `fix`,
  `output`, `image`, `max_width`; result R11–R13 plus `project` (mode, base resolution, scaling, PPU,
  default filter), `camera[]` (per scene: has component, reference, mode, per-screen layout),
  `guidance` (the next call, e.g. "check_pixel_art {fix:[\"all\"]} previews 7 fixes; add output:true to
  apply"), typed `errors[]` (`{kind, message, fix, field}`). With `image: true`: the first scene's
  base-resolution frame (rendered through its layout at k = 1, as S41's Pixel Scale 1×), upscaled by
  the largest whole number that fits `max_width`, with each finding on screen boxed and labelled
  `F<n>`, as an MCP image. Allowed while playing for checks; `output: true` while playing is refused
  (`kind: not_while_playing`). Annotations: not read-only, not destructive, idempotent.
- **R23. Preset and template for agents.** The preset is the `pixel_art_mode` fix (project part) plus
  the per-finding fixes (`fix: ["all"]` does all of R2); `create_project {template: "2d-pixel-art"}`
  creates the template; `set_project_settings {"graphics.canvas_width": 384, ...}` changes the base
  resolution; `set_texture_import_settings {settings: {pixel_art: true}}` (free-form, no schema
  change) marks textures.
- **R24. CLI.** `jasperengine-editor check --pixel-art` (also in `--all`; in the default set and in
  `--hook` when the project's Pixel Art Mode is on): one line per finding,
  `path[#entity]: error|warning: [<rule>] <message> (fix: <first option's label>) [PIXELART-<RULE>]`,
  FAIL → error, WARN → warning; `--format json` adds `pixel_art` with the exact structured result of
  R22 (no image). Exit codes stay `check`'s: 0 no errors, 1 errors, 2 usage. The CLI does not fix; its
  text ends with the call that does:
  `jasperengine-editor call check_pixel_art '{"fix":["all"],"output":true}' --project <p>` (S31).
- **R25. Console.** With Pixel Art Mode on, an import or Play start that finds WARN/FAIL findings logs
  one warning line ("Pixel Art Check: 2 FAIL, 1 WARN — hero.png is filtered bilinear on Player"),
  clickable to open the window on that finding; never more than one line per import batch or Play.
- **R26. Guide page.** `docs/mcp/guide/pixel-art.md` (`jasper://docs/pixel-art`, listed in
  `docs/mcp/guide/README.md`; S41 creates it with its "Camera and snapping" section in wave 5A):
  §6.5's outline, written in our own words. The agent guide's `## Pixel art` digest section (S41
  creates it, S42 owns its wording; S41 §6.4) gets `check_pixel_art` on S41's line: "Pixel art:
  Pixel Perfect Camera (PPU, snapping, Pixel Scale); check_pixel_art (group pixel_art) checks and
  fixes: jasper://docs/pixel-art." (≈ 150 bytes). S42 keeps the section within the family's share
  (ledger "MCP budget": ≤ 1,300 bytes, one line per spec).

### Data & compatibility

- **R27.** No version bump (S03 rule): new optional keys only.
  - `textureImporter` (version stays 1): `pixelArt`, `filterMode`, `pixelsPerUnit`, `alphaClip`; the
    `nearest` value of `resizeFilter`/`mipFilter`. An older editor keeps them as unknown keys.
  - project.yaml: `pixelArt: {enabled, checkOnImport, checkOnPlay, checkScreens}` and
    `graphics.defaultFilterMode`, written only when not default; `StreamSchema`'s project keys gain
    `pixelArt`.
  - `template.json`: optional `"pixelArt": true` (schema version stays 1, unknown keys ignored by
    older editors).
- **R28.** Old projects load and save byte-identically; no texture recooks (R8); nothing turns on by
  itself.

## 6. Design constraints & decisions

- **D1. A mode and a preset, not one switch with side effects.** The mode decides what happens to
  *new* things and when checks run; the preset changes *existing* things once, after a preview. A
  setting whose setter rewrote a project would be impossible to undo cleanly and surprising for an
  agent calling `set_project_settings`.
- **D2. `pixelArt` is written per file, explicitly.** A texture's look never depends on a project
  switch that someone may flip later; the mode only decides what new imports get, as S14 writes the
  inferred type once.
- **D3. One checker, two inputs.** `PixelArtCheck::run(const PixelArtCheckInput&, const Options&)` in
  `editor/pixelart/`. The MCP side builds the input from the live project (unsaved edits included);
  the CLI builds it from files (project.yaml, scene and prefab YAML as `Stream.cpp` writes them,
  meta sidecars) like `check --normals`. For a saved project both produce the same JSON (AC7). Rules
  are pure functions of the input plus S41's `computePixelPerfectLayout` and `PixelSnap`.
- **D4. Assignment copies import defaults.** References stay the truth for sampling (S14 §2); the
  import Filter Mode and PPU are what an assignment starts from, so the common path is right without
  a second source of truth, and the `filtering` rule catches what drifted.
- **D5. Verdicts from fixes.** A finding with a fix is `fixable` even when it is a FAIL; only a
  finding no automatic change can resolve (AI-upscaled art, vector fonts) needs a person — ritmo's
  "never a confident bad fix". Fix options are listed so an agent can choose the second one.
- **D6. `check`'s exit codes.** `check` combines categories in one run; one code scheme for all of
  them beats ritmo's per-verdict codes. The verdict is in the JSON for those who need it.
- **D7. No tunable thresholds.** The rules' limits are exact (texel size 1, whole numbers, nearest,
  lossless); only the list of screens to test is a setting. Nothing for an agent to loosen.
- **D8. Group budget.** One tool in the shared `pixel_art` group (§6.4); everything else goes through
  existing tools with free-form arguments.
- **D9. One validator for the family.** Checks other specs hand over become rules here (R10's
  hand-off rows), not new tools: a spec that lands before S42 (S43, S44) is read directly; one in
  the same wave (S45, S48, S49, S51) is read when present; one that lands later (S46, S50) registers
  its rule or input through `registerRule` / `PixelArtCheckInput` extensions, with its own tests.

### 6.1 API sketch

```cpp
// editor/pixelart/PixelArtCheck.h
namespace jasperengine::editor::pixelart {
    enum class RuleStatus { Pass, Warn, Fail };
    // Verdict { Clean, Fixable, NeedsPerson } is the family's, from editor/pixelart/PixelProblem.h (S44)

    struct FixChange { std::string target, field; nlohmann::json from, to; };
    struct FixOption { std::string id, label; std::vector<FixChange> changes; };
    struct Finding {
        std::string id, key, rule; RuleStatus status;
        nlohmann::json target, measured, expected;
        std::string message, needsPerson;          // needsPerson set when options is empty
        std::vector<FixOption> options;
    };
    struct RuleResult { std::string id; RuleStatus status; int checked = 0; int findings = 0; };
    struct CheckResult {
        Verdict verdict; std::vector<RuleResult> rules; std::vector<Finding> findings;
        nlohmann::json project, cameras;
        nlohmann::json toJson() const;              // R11, deterministic (R14)
    };

    struct PixelArtCheckInput;                      // project settings, scenes (entities, components),
                                                    // texture import settings, font info (S51), particles (S49)
    PixelArtCheckInput inputFromProject(const Project& project, const Scope& scope);      // MCP, window
    PixelArtCheckInput inputFromFiles(const std::filesystem::path& root, const Scope& scope); // CLI
    CheckResult run(const PixelArtCheckInput& input, const std::vector<std::string>& rules);
    // the selected fixes as a plan; apply() is one UndoGroupScope
    struct FixPlan { std::vector<std::pair<std::string, FixOption>> fixes; std::vector<std::string> unknown; };
    FixPlan plan(const CheckResult& result, const std::vector<std::string>& selection);
    bool apply(Project& project, const FixPlan& plan, std::string& undoStepName, std::string& error);
    // rules of specs that land after S42 (S46's cleanup_outputs); id order = R10's table order
    using RuleFn = std::function<RuleResult(const PixelArtCheckInput&, std::vector<Finding>&)>;
    void registerRule(std::string id, RuleFn rule);
}

// editor/pixelart/PixelArtPreset.h
struct PixelArtPresetOptions { int baseWidth = 320, baseHeight = 180; Scaling scaling = Scaling::INTEGER;
    int pixelsPerUnit = 1; int windowScale = 4; bool addCameras = true, convertTextures = true, pointReferences = true; };
std::vector<FixChange> planPixelArtPreset(const Project&, const PixelArtPresetOptions&);

// engine/core/texture: MipFilter gains Nearest; TextureProcessing gains bool alphaClip
// engine/core/Engine.h: static void setDefaultTextureFilter(TextureFilter); static TextureFilter getDefaultTextureFilter();
```

### 6.2 Settings (S25 registry; Project scope)

| Key | Type | Default | Page |
|---|---|---|---|
| `pixel_art.enabled` | bool | false | Pixel Art |
| `pixel_art.check_on_import` | bool | true | Pixel Art |
| `pixel_art.check_on_play` | bool | true | Pixel Art |
| `pixel_art.check_screens` | list of `WxH` | 1280x720, 1366x768, 1920x1080, 2560x1440, 3840x2160, 2400x1080, 1280x800 | Pixel Art |
| `graphics.default_filter_mode` | enum `bilinear`, `point` | `bilinear` | Graphics (shown on Pixel Art) |
| `graphics.pixels_per_unit` | S41 | 1 | Graphics (shown on Pixel Art) |
| `pixel_art.backdrops` | list of `#rrggbb` (first = dark, second = light) | `["#1a1c2c", "#f4f4f4"]` | Pixel Art ("Backdrops"): the project's pixel-art backdrops, read by S46's `contrast` lint, S47's `render_sprite` and S48's contact sheets (`dark`, `light`) |
| `palettes.default` | S43 R4b (a Palette asset) | — | Pixel Art ("Default Palette"; S43 shows it on the 2D page until S42 lands) |

### 6.3 `check_pixel_art`

```text
check_pixel_art {
  scenes?:  [sceneRef]            default: build list + open scenes
  paths?:   [string]              textures/fonts; default: every pixel-art texture
  rules?:   [pixel_art_mode|integer_scale|filtering|mipmaps_compression|palette|texel_size|
             off_grid|rotation|font_antialiasing|particles_grid|anti_aliasing|sprite_variants|
             cleanup_outputs]
  fix?:     [string]              finding keys, F<n>, rule ids or "all"; "<key>@<option>"
  output?:  boolean = false       with fix: apply as one undo step (else: the plan only)
  image?:   boolean = false       the 1x frame of the first scene with findings boxed
  max_width?: integer 256..2048
}
```

Description (≤ 400 characters): "Pixel-art rules on scenes and textures (filtering, texel size, integer
scale, grid, rotation, fonts, palette, mipmaps/compression, particles, anti-aliasing, project mode):
PASS/WARN/FAIL with measured and expected values, a verdict and fixes. fix previews the changes; the same call
with output:true applies them as one undo step and re-checks. Rules and loop: jasper://docs/pixel-art."

### 6.4 MCP tools and the budget (S31)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `check_pixel_art` | `pixel_art` | new | ≈ 1,250 (name 15, description ≈ 395, schema ≈ 840 with the 13-id enum) |
| `set_texture_import_settings` | `assets_pipeline` | none in the schema (`settings` is free-form): new keys `pixel_art`, `filter_mode`, `pixels_per_unit`, `alpha_clip`, value `nearest`; its description gains ", pixel_art" | +12 |
| `get_texture_import_settings` | `assets_pipeline` | result only (resolved Pixel Art fields) | 0 |
| `set_project_settings` | core | registry keys of §6.2 (data) | 0 |
| `create_project`, `list_project_templates` | (their group) | data only: the `2d-pixel-art` template | 0 |

- `pixel_art` group (tools from S42, S43, S46 and S48; ≤ 12 tools, ≤ 15,000 chars,
  `group_default`): S42 1 tool ≈ 1,250, S43 2 (≤ 2,800), S46 2 (≤ 2,550), S48 1 (≤ 1,450): 6 tools,
  ≈ 8,050 characters (the ledger's "MCP budget" table). No `budget.json` entry is needed. S43 adds
  the group row in wave 5A (S43 §6.1 gives its text).
- The test asserts `check_pixel_art` is hidden until `tool_groups {enable: ["pixel_art"]}` and
  callable by name before (conventions: hiding is only about context).

### 6.5 The guide page (`docs/mcp/guide/pixel-art.md`), outline

1. **The rules, in our words.**
   - Pick one base resolution and draw everything into it; 320×180 divides every common 16:9 screen
     exactly (4× at 720p, 6× at 1080p, 8× at 1440p, 12× at 4K).
   - Scale the base image to the screen by a whole number only; fill what is left with bars, or let
     Integer Expand show more of the world instead.
   - Sample pixel art with point filtering, keep it without mipmaps and store it uncompressed: block
     compression shifts colours and smears edges.
   - Give every piece of art the same pixel size: one texel is one base pixel, so every sprite uses the
     project's Pixels Per Unit and a scale of 1.
   - Keep the camera and every sprite on whole base pixels; smooth scrolling moves the finished picture
     by screen pixels, never the art inside it.
   - Rotate in 90° steps, or draw the rotations; let only things that must spin rotate on the grid.
   - Draw text with pixel fonts at their native size (or a whole multiple), never antialiased.
   - Put particles and effects on the same grid: whole-pixel sizes, point-filtered textures, snapped.
   - Leave smoothing filters (FXAA, MSAA) off the pixel-art image.
   - Look at every new asset at 1× next to a tile before it goes in.
2. **Set up**: `create_project {template:"2d-pixel-art"}`, or `check_pixel_art {fix:["pixel_art_mode"]}`
   then the same with `output:true`; the base resolution is the project canvas.
3. **The loop**: import → `check_pixel_art {paths}` → fix (preview, then `output:true`) → Play →
   `configure_game_view {pixel_scale:1}` + `screenshot` (the 1× view) → `check_pixel_art {image:true}`
   → report the verdict and counts; stop and hand over on `needs_person`.
4. **Rule reference**: each id, what it measures, its fix options.
5. **Camera and snapping** (S41): the component's fields, PPU, the snap rule in one paragraph, Pixel
   Scale and Pixel Zoom.
6. **Pitfalls**: PPU > 1 with in-scene UI; a HUD belongs in a layer scene; never change the scale
   to hide a `texel_size` finding (fix the art or the PPU).

## 7. Acceptance criteria

Fixtures: `tools/fixtures/pixel-art/` (S41's, clean) and `tools/fixtures/pixel-art-bad/` (one seeded
failure per rule: a bilinear reference, a BC7 + mipmapped sprite, a 1.5× scaled sprite, a PPU-32
sprite in a PPU-16 project, a sprite at x = 10.3, a 37° rotation, a vector font, a stretched
particle system, FXAA on, a scene with fixed resolution 640×360 and Fit scaling), CC0 art made for
them; for the hand-off rules, an off-palette sprite in a project with a Default Palette, a Palette
Swap material on a BC7 texture, and 30 `library:sprite` variants. Unit tests in
`editor/tests/pixelart/` and `editor/tests/texture/`; MCP tests in
`tools/mcp/tests/` (`CI = True`).

| AC | Verification |
|---|---|
| AC1 (R4, R7, R8) | `PixelArtImportTest.Resolve`: `pixelArt: true` resolves to the R4 defaults on all four families; an explicit `mipmaps: true` or a `mobile` override to ASTC wins; `canonical()` of every S14 corpus texture (no new fields) is byte-identical to before (cook keys unchanged). |
| AC2 (R5, R6) | `MipChainTest.NearestResize`: 256→128 keeps the top-left texel of every 2×2 block; 256→100 follows `⌊(i + 0.5)·s/d⌋`; alpha untouched; raw path equals cook path. `TexturePrepareTest.AlphaClip`: cutoff 0.5 maps 127 → 0, 128 → 255, cleared texels bled. **Red control:** with `Mitchell` the 2×2 test produces blended values. |
| AC3 (R4, lossless) | `TextureCookTest.PixelArtLossless`: the fixture's 256×128 sprite with `pixelArt: true` cooks to RGBA8 KTX2 with one level for desktop, mobile, android_etc2 and web, and each decodes to the source bytes exactly. |
| AC4 (R3) | `tools/mcp/tests/test_pixel_art_runtime.py`: in a project with `graphics.default_filter_mode: point`, a Lua `Sprite:setTexture` in Play reads `minFilter`/`magFilter` NEAREST; the default project reads LINEAR (red control). The generated main contains `Engine::setDefaultTextureFilter`. |
| AC5 (R7) | `test_pixel_art_import.py`: `create_entity` a sprite with a `pixelArt` texture → its texture property is nearest/nearest and its PPU copied (PPU 32 import in a PPU 16 project); one undo step removes the sprite; the importer's "Apply to N references" changes 3 references in one undo step. |
| AC6 (R9, R10) | `PixelArtCheckTest.<rule>` for each of S42's twelve rules on `pixel-art-bad` (`cleanup_outputs` is tested by S46, S46 AC15): the seeded finding has the listed status, `measured` and `expected` (e.g. `texel_size` measured `{x:1.5, y:1.5}` FAIL; `integer_scale` at 1366×768 measured `{scale: 2.134, integer:false}`), and the same rule on `pixel-art` is PASS. **Red control per rule:** removing the seeded defect from `pixel-art-bad` turns that rule PASS. |
| AC7 (R14, D3) | `PixelArtCheckTest.Deterministic`: two runs give byte-identical JSON; `tools/mcp/tests/test_check_pixel_art.py::test_cli_matches_mcp`: for the saved `pixel-art-bad` project, `check --pixel-art --format json`'s `pixel_art` equals `check_pixel_art`'s structuredContent (minus `image`). |
| AC8 (R11–R13, R22) | `test_check_pixel_art.py`: `fix:["all"]` without output changes no file (hashes of scenes and metas identical) and lists every change; with `output:true` the result's `after.verdict` is `clean` except the `needs_person` findings (vector font), `get_undo_history` shows one step "Fix Pixel Art (n)", undo restores every file hash; `rotation:...@on_grid` applies the second option; an unknown key is an `{kind:"not_found", field:"fix"}` error; `output:true` during Play is `not_while_playing`; `image:true` returns a PNG whose width is a whole multiple of 320 and ≤ `max_width`, with `F<n>` labels. The group test: hidden until `tool_groups` enables `pixel_art`. |
| AC9 (R2, R23) | `test_pixel_art_preset.py`: on a copy of the `2d` template, `fix:["pixel_art_mode"]` then the full `fix:["all"]` with output give: canvas 320×180, scaling `integer`, PPU 1, default filter point, window 1280×720, a Pixel Perfect Camera on `Main`'s camera, `player.png.meta` with `pixelArt: true`, the sprite reference nearest; the check is `clean`; undo restores the project byte-identically. The Project Settings dialog's preview lists the same changes (`PixelArtPresetTest.PlanMatchesFixes`). |
| AC10 (R21, template) | `editor/tests/hub/HubTest` lists `2d-pixel-art` with `pixelArt: true`; `test_project_templates.py`: `create_project {template:"2d-pixel-art"}` → `check_pixel_art` verdict `clean` with 0 WARN; Play, `configure_game_view {pixel_scale:1}`, `screenshot` equals the template's stored 320×180 golden pixel for pixel (S41 AC4 method). |
| AC11 (R24) | `tools/mcp/tests/test_cli_check.py::test_pixel_art`: `check --pixel-art` on `pixel-art-bad` exits 1 and prints one line per finding in the R24 format with `[PIXELART-TEXEL_SIZE]` style codes; on `pixel-art` exits 0; `--format json` validates against `tools/mcp/schemas/pixel_art_check.json`; `--hook` with a Write of a new PNG in a Pixel Art Mode project reports its findings on stderr and exits 2 on a FAIL. |
| AC12 (R1, R25) | `test_pixel_art_mode.py`: with the mode on, importing a PNG writes `pixelArt: true`, a new 2D scene has a Pixel Perfect Camera, one Console warning names the finding (`read_console`), and Play start logs one summary line; with the mode off neither happens (red control). |
| AC13 (R18–R20) | `tools/mcp/tests/test_pixel_art_check_window.py` (xvfb, isolated editor): Window › 2D › Pixel Art Check lists the `pixel-art-bad` findings grouped by rule; clicking a row selects the GameObject; **Fix** on the `filtering` row changes that reference only (undo one step); Fix All's preview count equals `fix:["all"]`'s plan. Screenshots of the window, the preset dialog and the importer's Pixel Art section in the evidence. `check-hardcoded-shortcuts.py` passes. |
| AC14 (R16) | `test_check_pixel_art.py::test_cost` on `pixel-art-large`: warm ≤ 1.0 s, cold ≤ 3 s; > 1 s returns a `job_id`. With the mode off, `PixelArtModeTest.OffRegistersNothing` finds no import or Play hook. |
| AC15 (R27, R28) | `test_runtime_regressions.py` and `StreamRoundTripTest`: every existing fixture project, scene and meta re-saves byte-identically; a meta with the new keys round-trips through an editor without S42 (unknown keys kept). |
| AC16 (§6.4, R26) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`, `test_docs.py` (`jasper://docs/pixel-art` served and listed), `test_autosync.py` (tool names in the guide exist), `test_protocol.py` (guide size). |

## 8. Dependencies & conflicts

- **Depends on:** S41 (layout, PPU, rule, Pixel Perfect Camera, Pixel Scale; must land first), S14
  (import settings, cook, recook queue), S27 (hub, templates), S25 (settings, commands), S24 (undo
  groups, `WriteFilesCmd`), S31 (`call`, budget, groups), S06 (Background worker).
- **Depends on (wave 5A, landed first):** S43 (`apply_palette`'s measure core and `palettes.default`
  for the `palette` rule, R15's lossy-texture check), S44 (sheet pages, `texture.compression` of
  `.aseprite` metas, the family `Verdict` in `PixelProblem.h`), S52 (Sprite Batch drawables).
- **Reads when present (same wave 5B):** S51 (`analyzeFontForPixelArt` for `font_antialiasing`),
  S49 (the Pixel Art module and Pixel mode for `particles_grid`), S45 (atlases: a pixel-art atlas
  page is a pixel-art texture), S48 (more `library:sprite` features for `sprite_variants`). Each is
  read through a small interface in `PixelArtCheckInput`; absent, the rule degrades as R10 says.
- **Extended later (wave 5C):** S46 registers `cleanup_outputs` and names its grid detection in
  `texel_size`'s `needs_person`; S50 adds Tilemap Layers to the input. S42 needs neither.
- **Blocks:** S46 and S47 (the `pixel_art.backdrops` setting, the shared verdict vocabulary, the
  rule registration), S50 (drawable input), the family's guide page structure.
- **Merge hotspots:** `editor/texture/TextureImportSettings.*`, `TextureImporterView.cpp` (S14
  owners), `engine/core/texture/MipChain.*`, `TexturePrepare.*`, `Texture.cpp`, `Engine.h`,
  `Generator.cpp`, `SettingsRows.cpp`, `StreamSchema.cpp`, `editor/cli/CommandLine.cpp` (check),
  `editor/hub/*`, `McpToolGroups.cpp`, `McpInstructions.cpp`, `docs/mcp/guide/README.md`.

## 9. Risks & defaults

- **Uncompressed sprites and VRAM on mobile.** RGBA8 is 4 bytes per texel; a 2048² sheet is 16 MB.
  Default: lossless is the rule (pixel art is small); R17 warns on large sheets; a project may override
  per platform and accept the FAIL.
- **Alpha Clip on intentional translucency** (shadows, glass). Default: on in the Pixel Art defaults
  (AI-made and hand-drawn edges are the common case); one checkbox turns it off per texture.
- **Assign-time copies can drift** from the import Filter Mode later. Default: the `filtering` rule and
  the importer's "Apply to N references" cover it; no hidden link.
- **Vector fonts before S51** stay `needs_person`. Default: accepted; the verdict says so.
- **Off-grid authored positions are only WARN** because S41 draws them snapped. Default: WARN; the fix
  is cheap and Pixel Move Snap prevents new ones.
- **Window size 4×** may not fit small laptops (1280×720 fits 1366×768). Default 4×; R2's option.
- **The CLI cannot see unsaved edits.** Default: documented; MCP is the live view, and AC7 holds for
  saved projects.

## Milestones

- **M1** Import: `MipFilter::Nearest`, Alpha Clip, the `pixelArt`/`filterMode`/`pixelsPerUnit`
  fields, resolution and cache stability, Default Filter Mode, assign-time copies (R3–R8); AC1–AC5.
- **M2** The checker core with both inputs, the twelve rules and `registerRule`, deterministic JSON, `check --pixel-art`
  (R9–R12, R14, R24); AC6, AC7, AC11.
- **M3** `check_pixel_art`: fixes with preview and output, re-check, image, the `pixel_art` group row
  if not there (R13, R22); AC8.
- **M4** The preset, Project Settings › Pixel Art, Pixel Art Mode behaviours, Console lines (R1, R2,
  R18, R25); AC9, AC12.
- **M5** The 2D Pixel Art template and the hub's Base Resolution field (R21); AC10.
- **M6** The Pixel Art Check window and the importer UI (R19, R20); AC13.
- **M7** Guide page, docs, budgets, performance, compatibility (R16, R26–R28); AC14–AC16.

## Deviations

Recorded at implementation (`feature/s42-pixel-art-mode`); the code is what is true now.

- **D3, R24: one input builder.** `inputFromFiles` is not a second parser of raw YAML: `check
  --pixel-art` loads the project headless, as `check`'s other engine categories do, and calls the
  same `inputFromProject`; closed scenes of the build list are read from their files into scenes of
  their own (`Project::loadSceneProbe`) in both. A saved project gives the same input both ways by
  construction (AC7).
- **R5: ties.** "A 2× downscale keeps the top-left texel" and "`⌊(i + 0.5)·s/d⌋`" disagree when a
  destination centre lands exactly on a source texel edge; the edge goes to the lower texel
  (`⌈(i + 0.5)·s/d⌉ − 1`, integer maths), which keeps both statements everywhere else.
- **R9, R10: what counts as pixel art.** In Pixel Art Mode every 2D scene is a pixel-art scene (the
  mode declares the project pixel art). `pixel_art_mode`'s "pixel-art content" also counts a 2D scene
  with sprites or tilemaps, so a plain 2D project (the 2d template, AC9) can be converted by its fix.
- **R9: layer scenes, Sprite Batch.** "A layer scene's camera": a layer (child) scene without a
  camera of its own is drawn under the main camera of the scene it is a layer of, so it is a
  pixel-art scene when that camera has an enabled orthographic Pixel Perfect Camera; a layer with
  its own camera is judged by it. S52 (Sprite Batch) is not in this base: its drawables come in
  through `pixelart::registerDrawableCollector` (PixelArtInput.h), called for every scene read, as
  S46's rule comes in through `registerRule`.
- **R2: fixed-resolution scenes.** The preset gives a fixed-resolution scene's new Pixel Perfect
  Camera the preset's base resolution as its reference (the scene's own fixed size was the old
  base); `integer_scale`'s fix on its own keeps R10's "reference = the fixed resolution".
- **R13: stale fixes.** A plan made from an earlier check (the window's last result, an MCP job's
  snapshot, the preset dialog's preview) is checked again before it applies (`staleChanges`: the
  scope is checked again as Pixel Art Mode sees it and every change must still be a fix with the same
  `from`; project settings are compared with their values). One that no longer holds applies nothing
  and is the typed error `stale` (MCP) or the window's notice, which checks again.
- **R16: cost.** What a closed scene's file holds and a texture's meta and header say are kept by
  file stamp between checks (an asset batch that adds, moves or removes files or changes a prefab
  forgets them), so a warm check decodes no scene file. `check_pixel_art` runs the rules inline when
  the project's last check took at most 0.25 s (before the first, at most 2,000 drawables and
  textures), else on an S06 Background job the call waits a second for. AC14's cold run is a fresh
  editor on the saved project; the warm run follows it.
- **R19: Project scope.** Every scene file of the project, whether or not the build lists it
  (`Project::loadSceneFileProbe`), every pixel-art texture and every Aseprite sheet. Prefabs are
  checked where their instances are.
- **R10 `integer_scale`:** a pixel-art scene with integer scaling but no enabled Pixel Perfect Camera
  is a WARN (nothing snaps without one, so `off_grid`'s "drawn snapped" would not hold); its fix adds
  the camera, which is how `fix: ["all"]` does R2's camera row. A scene without a saved camera gets a
  "Main Camera" (set as main) with the component. A Stretch Fill camera's fix is Upscale Mode Integer.
- **R10 `pixel_art_mode` fix:** R2 rows 1–5 keep the project's Pixels Per Unit (any PPU is a valid
  pixel-art layout) and add row 6 (the window at 4× the base) when the window is not already a whole
  multiple of the base, so `fix: ["pixel_art_mode"]` then `fix: ["all"]` equals the preset (AC9).
- **R10 `filtering`, R11 keys:** the field is the property path `set_properties` takes
  (`mesh.submeshes[0].material.baseColorTexture`, `ui.texture`), not the spec's illustrative
  `sprite.texture`.
- **R10 `mipmaps_compression`:** one finding per texture with every family's format in `measured`
  (and `vram_bytes`, R17). An `.aseprite` sheet is judged by `texture.compression`; a `.spriteatlas`
  by its `compression` (S45's Auto is now lossless in Pixel Art Mode too, R16 of S45). Generated pages
  are judged through their source.
- **R10 `particles_grid`:** particle textures are made at runtime, so their filter is the project's
  Default Filter Mode (fix: Default Filter Mode point). No enabled Pixel Perfect Camera is fixed with
  integer_scale's own change (`rules::cameraFixChanges`; identical changes are applied once), and
  Pixel Snapping off with the camera's switch. Render mode, sizes and a perspective camera have no
  automatic fix (`needs_person` names what to change). The Pixel Art module criteria wait for S49
  (not in this base). A particle renderer's texture is a pixel-art texture of a pixel-art scene
  (`mipmaps_compression`, so R2's preset converts it).
- **R10 `font_antialiasing`:** S51 plugs in through `pixelart::setFontAnalyzer`; until then every font
  reads as a vector font (WARN, `needs_person`). The `.bitmapfont` `alpha_cutoff` fix is S51's to add
  (its writer), so that case is `needs_person` meanwhile.
- **R10 `sprite_variants`:** the collector runs on one GameObject per library:sprite material identity
  (shader features and blend), and `by_material` names those identities.
- **R10 `cleanup_outputs`:** not listed until S46 registers it (`registerRule`); asking for it before
  is `unknown_rule`.
- **R22:** `guidance` is `next` in structuredContent (AGENTS.md "MCP text"); typed errors are
  `errors[]`. The image is the Game view at Pixel Scale 1× of the first pixel-art scene with
  findings, cropped to the base image, scaled by whole numbers within `max_width` (default 1024).
- **R24:** the closing call is `jasperengine-editor call --project <p> check_pixel_art '{...}'`
  (options before the tool name, as `call` takes them). Given files (`--hook`, `--files`), the
  category checks the changed scenes (`Scope::scenes`) and the changed images that resolve to pixel
  art (an image's meta counts as the image; `Scope::pixelArtPathsOnly`), and reports only the findings
  of those files and, when project.yaml changed, the project's; old findings elsewhere neither print
  nor set the exit code. A changed prefab is checked where its scenes are.
- **R1:** only an image *added* while the mode is on gets `pixelArt` (written at once with S14's
  inference, `writeImportSettingsNow`); the cook queue's own first import of an image the project
  already had is S14's, so turning the mode on (or the preset, undone) leaves no file changed (D2).
  An added 2D image nothing references yet (S14 infers `default`, "no references") is pixel art
  only when it looks like it (`looksLikePixelArt`): a folder of its path named sprites, sprite,
  tiles, tileset(s) or pixelart, or at most 512x512 with 256 colours or fewer among its visible
  texels; it then imports as `type: sprite` ("no references (Pixel Art Mode)"). Photos and painted
  backgrounds keep the usual (lossy) import. The colour count decodes images up to 512x512 on the
  main thread at import (a few ms for the largest). The validator judges a texture with no import
  settings yet as S14 will import it.
- **R3:** the editor applies the Default Filter Mode at Play start (and restores Bilinear at stop);
  edit mode keeps Bilinear, since assignments copy their filters explicitly (R7). Lua gets
  `Engine.setDefaultTextureFilter` / `getDefaultTextureFilter`.
- **R18:** the Upscale Mode default has a key of its own, `pixel_art.upscale_mode`. Base Resolution,
  Pixels Per Unit and Default Filter Mode are rows of the Pixel Art page under the Graphics keys
  (`graphics.canvas_width`/`height`, `graphics.pixels_per_unit`, `graphics.default_filter_mode`: one
  name per concept; no alias key was ever stored, so nothing migrates). `edit.projectSettings` takes a page as payload (run_command opens Pixel Art directly).
- **R19:** the Console line is clickable through a small `ConsoleWindow::setMessageAction` (lines that
  start with "Pixel Art Check:"), registered only while Pixel Art Mode's hooks are. The window's
  double-click frames through `edit.frameSelected`.
- **R21:** the hub's Base Resolution is a field of the New Project form; `create_project` takes the
  template as it is (320×180). The `create_project` / `list_project_templates` descriptions were not
  changed (`list_project_templates` lists the new template).
- **R13:** fixing a GameObject of a closed scene opens the scene; scene edits are unsaved until
  `save_scene` (the result's `next` says so), as every MCP scene edit.
- **R26:** no line in the agent guide: it has 6,689 of 8,192 bytes with the 1,500 bytes of headroom
  kept, so even the ledger's "Other areas" entry (29 bytes) breaks the budget. The `pixel_art` group's
  description, which `tool_groups` shows, names it instead ("Pixel art: checks and palettes
  (jasper://docs/pixel-art)"); the rules, loop and rule reference are in `docs/mcp/guide/pixel-art.md`.
- **AC1-AC3, AC6, AC9, AC14 test names** follow docs/testing.md's naming (`MipChain.NearestResize`,
  `TexturePrepare.AlphaClip`, `PixelArtImport.*`, `TextureCook.PixelArtLossless`, `PixelArtCheck.*`,
  `PixelArtPreset.PlanMatchesFixes`, `PixelArtMode.OffRegistersNothing`). The rule unit tests run on
  the fixture's data written in code (the rules are pure functions of the input, D3);
  `test_check_pixel_art.py` checks the same seeded findings on the fixture's files.
- **AC10:** the 1× frame is compared with a CPU reference composed from the template's own sprite
  (S41 AC4's method) rather than a stored golden image.
- **AC14:** `pixel-art-large` (12 scenes, 600 textures, 6,000 sprites) is generated by `test_cost`
  instead of being committed (megabytes of scene YAML).
