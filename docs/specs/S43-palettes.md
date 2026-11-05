# S43 — Palettes: the Palette asset, Lospec, the Palette window, ramps, colour maths, Palette Swap, Palette Quantize

**Wave / Depends on.** Pixel-art family (ledger `PIXEL-ART-LEDGER.md`), wave 5A (with S41, S44 and
S52). No hard dependency: every milestone but M2's `.aseprite` reader runs on today's `main`. That
reader uses S44's codec (`editor/pixelart/aseprite/`, same wave): it is registered when S44 is
present, and whichever of S43 and S44 lands second adds the Aseprite row of R5 and its test.
Owner area: `engine/core/pixelart/` (colour maths, `Palette`, the swap cache, the quantize LUT),
`engine/shaderlib/includes/palette.glsl` and `includes/bayer.glsl` (the family's Bayer table),
`library/sprite.frag` (the new features only), `uber.frag` (`UBER_PALETTE` only),
`editor/pixelart/` (formats, Lospec, ramp generator, Apply Palette / Reduce Colours),
`editor/window/PaletteWindow.*`, `editor/mcp/tools/PaletteTools.cpp`,
`docs/mcp/guide/palettes.md`, `docs/palettes.md`.

## 1. Goal

A pixel-art game is built on a palette. Today JasperEngine has no idea what a palette is. This
spec makes the palette a first-class asset that people and agents share:

- **The asset.** One `Palette` asset (`.palette`) holds the colours, named ramps, and aligned
  variants (team colours, damage tints). It imports and exports every common palette format, and
  it can be fetched from Lospec by slug.
- **The editor.** It is edited in a Palette window with ramps, a hue-shift ramp generator, sorting
  and per-colour usage counts. One colour library, built on OKLab and CIEDE2000, is shared with
  S46, S47 and S48.
- **In the game.** A palette recolours sprites through **Palette Swap**. Swap is exact, needs no
  re-authoring of the art, and supports up to 15 target palettes per material plus
  DPaint-style colour cycling. **Palette Quantize** forces the whole frame onto a palette, with
  an optional ordered dither, inside the existing uber pass.
- **Textures.** **Apply Palette / Reduce Colours** rewrites a texture onto a palette, as a preview
  first and then an undoable write.

Every piece costs nothing when it is off.

## 2. Scope

**In scope**
- The `Palette` asset type: a versioned JSON `.palette` file with its GUID in `.meta` (S04). Its
  codec and runtime loading live in `engine/core/pixelart/`.
- Import: `.gpl` (GIMP), `.hex` (Lospec), `.pal` (both JASC-PAL text and Microsoft RIFF PAL),
  `.txt` (Paint.NET), `.ase` (Adobe Swatch Exchange, sniffed by its `ASEF` magic), Aseprite
  `.ase`/`.aseprite` (sniffed by the `0xA5E0` magic, read with S44's codec), PNG/GIF swatch
  strips and any image (its unique colours), and the editor's own `.colors` swatch libraries.
- Export to every one of those formats except Aseprite. A palette never writes into an
  `.aseprite` file; that is S47's job.
- Lospec fetch by slug, in the editor only, with a clear offline failure (R8).
- 37 built-in palettes: pixelfx's 27 hue-shifted ramps and 10 classic palettes (R9).
- The **Palette window**:
  - a grid of swatches and a ramps strip;
  - variants;
  - drag to reorder, sort, and the hue-shift ramp generator;
  - usage counts for a selected texture;
  - colour-picker integration with the SwatchLibrary.
- Shared colour maths in `engine/core/pixelart/PixelColor.h`, used by the editor too:
  - sRGB (the piecewise curve), OKLab, CIELAB (D65), CIEDE2000 and HSV;
  - a cached nearest-colour matcher;
  - the ordered pair-mix dither.
- **Palette Swap** and **Palette Cycling**, new features of `library:sprite` (so Sprite, Tilemap
  and MeshPolygon get them). They work through a material palette binding (`@palette`) and come
  with a Lua API.
- **Palette Quantize** (`UBER_PALETTE`), a post-processing setting in the uber pass.
- **Apply Palette / Reduce Colours** on texture assets (preview, then an undoable write).
- Two MCP tools in the `pixel_art` group: `edit_palette` and `apply_palette`.

**Out of scope (decided)**
- **A runtime indexed-texture mode** (an R8 index texture plus a palette LUT as the sprite's main
  texture). This is decision D1, and §6.3 gives the reasons. Palette Swap's lookup is designed so
  that an index source can be added later without changing the asset, the material binding or
  the Lua API.
- **Painting.** Owner decision: Aseprite is the drawing tool. The Palette window edits palettes,
  never pixels. Apply Palette is a whole-texture operation.
- **Browsing or searching Lospec.** We only fetch by slug. "Browse Lospec…" opens
  `https://lospec.com/palette-list` in the system browser.
- **Any network access at runtime.** Exported games never fetch.
- **Error-diffusion dithering** (Floyd–Steinberg and similar) in the runtime pass. It cannot be
  done in one fragment pass. Apply Palette offers it in the editor (R23).
- **Removing COLOR_REPLACE, `library:gameboy` or `library:posterize`.** Old scenes keep them
  (R44). New work is steered to Palette Swap and Palette Quantize.
- **S46's cleanup tools** (grid recovery, speck cleanup, lost-colour repair). S46 builds them on
  this spec's maths and asset.

## 3. Current state (read on `specs/pixel-art` 1b1c34fbd)

- **No palette concept.**
  - `engine/core/asset/AssetType.h:15-40` ends at `VertexAnimation = 23`. It is append-only
    (the values are stored in `assetmap.bin`).
  - The editor type table, `editor/asset/AssetTypes.cpp:23-54`, has no palette row.
  - Nothing parses `.gpl`, `.pal`, `.hex` or `.ase`.
- **Swatches.** `editor/widget/SwatchLibrary.h:22-49`:
  - Holds `Swatch{name, linear, hdr}` and `SwatchLibrary` (`kVersion 1`, `kRowLength 19`,
    `kMaxSwatches 76`), stored as `.colors` JSON.
  - User libraries live under `<settings>/swatches/`, project ones under
    `<project>/EditorSettings/Swatches/` (`SwatchLibrary.cpp:38-44`).
  - The built-in "Default" library has 19 swatches and 6 gradients (`SwatchLibrary.cpp:46-73`).
  - The colour picker shows the libraries (`editor/widget/ColorPickerWindow.cpp:175-185`, grid
    and menu at `:616-686`). It writes the files directly, with no undo (`:297`).
  - MCP `edit_swatches` (`editor/mcp/tools/EditorToolsTools.cpp:620-637`, handler `:261-350`,
    group `editor_tools`) writes without undo too (`:321`).
  - The swatches are linear floats: right for HDR material colours, wrong for exact sRGB8 pixel
    art.
- **Colour maths is scattered.**
  - `editor/widget/ColorMath.h:17-34`: sRGB↔linear and HSV.
  - `engine/core/util/Color.h:25-28`: `sRGBToLinear`.
  - The OKLab lightness of the sprite-normals wand (`editor/spritenormals/SpriteNormalsField.cpp:139-145`).
  - The CSS OKLab of the HTML UI (`engine/core/htmlui/compat/css/ColorValue.cpp:69-78`).
  - GLSL `jfx_rgb_to_oklab` for the HTML UI effects (`engine/shaderlib/effects/lib/jfx_color.glsl:55-86`).
  - There is no CIELAB, no CIEDE2000 and no nearest-colour matcher.
  - `includes/srgb.glsl` is the gamma-2.2 approximation.
- **Sprite recolouring.** `engine/shaderlib/library/sprite.frag`:
  - Features at `:9-15`: OUTLINE, HUE_SHIFT, FLASH, GRAYSCALE, COLOR_REPLACE, OVERLAY, GLOW.
  - COLOR_REPLACE (`:23-25`, `:32`, logic `:183-190`) mixes three fixed colours, scaled by the
    texel's luminance, wherever an extra RGB mask texture says so.
  - Its weaknesses:
    - It needs a hand-painted mask per sprite.
    - It handles only three colour groups, and they are shaded by luminance, so the output is
      never an exact palette colour.
    - It runs after the tint (`:179-181`).
    - It cannot hold several looks per material.
  - The texture is read as raw sRGB-encoded bytes and linearised in the shader (`:119-128`,
    `:181`). That is what makes an exact colour lookup possible.
- **Post passes.**
  - `library/gameboy.frag`: four fixed shades by luma with a 4×4 Bayer dither (`:7-13`,
    `:43-58`).
  - `library/posterize.frag`: N levels per channel with Bayer 4×4 (`:7-9`, `:32-43`).
  - Both are T3 passes. They run before the uber pass (`SceneSettings.h:301-306`), so the grade,
    vignette, grain and anti-banding dither are applied *after* them and push pixels off the
    palette.
  - The uber features end at `UBER_GRAIN_FILMIC = 1u << 17`, `UBER_FEATURE_COUNT = 18`
    (`engine/core/subsystem/RenderSystem.h:412-432`, append-only).
  - `UBER_DITHER` (`ditherEnabled`, `SceneSettings.h:169`) is ±0.5/255 noise against banding
    (`uber.frag:318-320`), not palette dithering.
  - With fixed resolution the uber pass is the upscale blit itself, reading the low-resolution
    source (`docs/mcp/guide/post-processing.md:19`). So a per-art-pixel dither must index by
    *source* texel (`uber.sourceTexel`, `uber.frag:84`).
  - The tiers are chosen in `RenderSystem::getPostProcessingTiers` (`RenderSystem.cpp:4218-4275`).
  - The Bayer helpers `jfx_bayer2/4/8` exist for the HTML UI (`effects/lib/jfx_tone.glsl:10-20`).
- **Undoable file writes.**
  - `WriteFilesCmd` (`editor/command/type/WriteFilesCmd.h:31`) and
    `ProjectFileWrites::writeUndoable` (`editor/util/ProjectFileWrites.h:74`).
  - The sprite-normals session shows the preview → `output` pattern
    (`editor/mcp/tools/SpriteNormalsTools.cpp:358-396`, `:856`, `:878`).
- **Runtime textures from code.** `TextureRender::createDynamicTexture` / `updateTexture`
  (`engine/core/render/TextureRender.h:62-73`) is the path the post colour LUT uses
  (`RenderSystem.cpp:4281-4302`). `texelFetch` is already used across backends
  (`includes/lattice.glsl:86`, `includes/vat.glsl:104`).
- **The editor HTTP client.**
  - `editor/htmlui/HttpClient.h` is curl through `CommandRunner`, blocking, run on a worker.
  - `JASPERENGINE_IMPORT_MIRROR` replays recorded responses with no network (for tests). The
    Google Fonts importer uses it.
- **Per-instance sprite data.** `InstancedMeshComponent::InstanceData`
  (`engine/core/component/InstancedMeshComponent.h:69-81`) carries a colour and a texture rect,
  but no palette row. Sprite entities are not batched; each is one Mesh draw (`surface_vs.glsl:133-136`).
- **Lua.**
  - Sprite is `deriveClass<Sprite, Mesh>` (`engine/core/script/binding/GameObjectClassesLua.cpp:2712`).
  - Material features are set with `m:setShaderFeature`, `setShaderParam` and
    `setShaderTexture` on a material copy (`CoreClassesLua.cpp:3010-3066`).
  - `PostProcessingSettings` is bound at `CoreClassesLua.cpp:2295`.

## 4. Reference

| Source (clone under `/tmp/pixelart-research/`, read-only) | What we take | Change |
|---|---|---|
| Pixelorama `src/Autoload/Palettes.gd:548-790` | `.gpl` reading (GIMP's "living spec", `gimppalette-load.c`), JASC-PAL, image → unique colours, the Lospec URL `https://lospec.com/palette-list/<slug>.json` with `{name, author, colors[]}` | Ported to C++. Also: RIFF PAL, `.hex`, Paint.NET `.txt`, Adobe ASE, Aseprite, `.colors`. Empty slots kept as `#00000000`, so indices stay aligned. |
| Pixelorama `src/Palette/Palette.gd:6-300` | A grid palette model (`width`, insert/move/swap/reverse) | The column count is a display hint only; colours are dense. |
| Pixelorama `src/Palette/PalettePanel.gd:45-61` | The sort menu (hue, saturation, value, lightness, reverse, "create a new palette") | Luma means OKLab L. Sorting remaps ramps and variants. |
| Pixelorama `src/Tools/DesignTools/Shading.gd:24-119` | HUE_SHIFTING: lighten pulls the hue toward 60° (`hue_lighten_limit`) and lowers saturation down to a 10 % floor; darken pulls the hue toward 240° and raises saturation, with value floored at 10 %; the yellow-to-blue range reverses the direction (`hue_range`, `hue_limit_lighten/darken`) | Ported exactly as the "Hue Shift" ramp mode (R10). Pixelorama's amounts (10°, 10 %, 10 %) are the defaults. |
| pixelfx `src/core/color.js:37-80` | `fit(r, n)` (resample, keeping the ends), `at(r, t)` (a step with no blending), and the 27 `PAL` ramps | Copied as data and as `fitRamp`/`rampAt`. S48 and S49 use them. |
| ritmo `packages/tools/src/pixel/core/color.ts:33-203` | sRGB tables, OKLab (Ottosson's matrices), CIELAB D65, CIEDE2000, the per-colour cached matcher | Ported to C++. The cache is an open-addressed 24-bit-key table. CIEDE2000 is checked against Sharma, Wu and Dalal's 34 test pairs (2005). |
| Joel Yliluoma, "Arbitrary-palette positional dithering algorithm" (article; algorithm only, no code) | Ordered dithering with an arbitrary palette: per colour, mix two palette colours A and B with a ratio, then choose between them by a Bayer threshold | Baked per LUT cell on the CPU (R19). The shader does one lookup and one comparison. |
| Lospec format pages (`.hex`, `.pal`, `.gpl`, `.txt`, `.ase`) | The file layouts | — |

Licences: Pixelorama, pixelfx and ritmo are MIT.
- Copied code keeps its header; the data and the ported code get `NOTICE.md` entries.
- The classic palettes are data, credited in the `.palette`'s `author` field and in `NOTICE.md`
  (owner decision, ledger 2026-11-04).

## 5. Requirements

### Functional — the Palette asset

- **R1. The asset.** A `Palette` (`AssetType::Palette`, appended; `.palette`) is UTF-8 JSON (§6.2).
  - **Fields:** `format: "jasperengine.palette"`, `version: 1`, `name`, `author`, `description`,
    `source` (where it came from), `columns` (a display hint, 1–64, default 8), and `colors`.
  - **Colours:** 1–256 entries `"#rrggbb"` or `"#rrggbbaa"`, exact sRGB 8-bit.
  - **Names:** `names`, sparse, by index.
  - **Locked colours:** `locked`, a list of indices. S46 never remaps them, and Reduce Colours
    keeps them.
  - **Transparent index:** `transparent`, an index treated as see-through (Aseprite's), or −1
    for none.
  - **Ramps:** `ramps[]` (≤ 64), each `{name, colors: [indices], cycle: steps per second (0 = no
    cycling)}`.
  - **Variants:** `variants[]` (≤ 15), each `{name, colors: [...]}`, aligned index by index to
    `colors`. `null` keeps the base colour; a shorter list keeps the rest.
  - **Identity:** the GUID is in `<file>.meta` (`AssetIdentity::Sidecar`).
- **R2. Validation.** Load reports typed diagnostics `{kind, message, fix, field}`. These are
  errors:
  - a bad hex;
  - more than 256 colours;
  - a ramp or locked index out of range;
  - duplicate ramp or variant names;
  - a variant longer than `colors`.

  An exact duplicate colour is a warning (it makes Palette Swap ambiguous, R14). Unknown keys are
  kept on rewrite (S03 unknown-data preservation).
- **R3. Runtime use.**
  - Palettes load at runtime through the asset system: the JSON is parsed by the engine's `json`
    library; there is no editor dependency.
  - An exported game ships the palettes its scenes, materials and post settings reference (S04
    dependency graph), and none it does not.
  - Hot reload (S05) of a palette rebuilds the swap and quantize textures that use it within one
    frame of the reload.
- **R4. Ramp helpers** (runtime and editor):
  - `fitRamp(colors, n)` is pixelfx `fit`: round `i·(len−1)/(n−1)`, keeping both ends.
  - `rampAt(colors, t)` is pixelfx `at`: `colors[clamp(floor(t·len), 0, len−1)]`, never blended.
  - `rampColors(name, steps)` resolves a ramp's indices to colours, fitted when `steps > 0`.
  - S48 and S49 use exactly these.
- **R4a. Ramp strips.**
  - `<palette>.palette#ramp:<name>` is a sub-asset reference (S04 sub-asset keys,
    `SubAssetKind::PaletteRamp` appended). It resolves at runtime to an N×1 RGBA8 texture:
    Nearest, clamp, no mips, never compressed, built from the ramp and rebuilt on reload.
  - S48 R9 binds it as a ramp texture; any texture field that accepts a strip accepts it.
  - A missing ramp binds the 1×1 white texture and raises the warning `palette_ramp_missing`.
- **R4b. Default Palette.**
  - A project setting, `palettes.default` (Project Settings ▸ Pixel Art ▸ Default Palette): a
    Palette asset, empty by default.
  - It is the palette used when one is not named:
    - `apply_palette` without `palette`;
    - the colour picker's Snap to Palette;
    - S46's cleanup and S47's new sprites (S47 falls back to Sweetie 16 when the setting is
      empty);
    - the Palette window's first open.
  - When S42 lands, its Pixel Art page hosts the field; until then, the field sits on the 2D
    page of Project Settings.

### Functional — import, export, Lospec, built-ins

- **R5. Import.** Import File… (and dropping a file onto the Palette window or the Project window)
  sniffs the content first and the extension second.

  | Format | Detection | Notes |
  |---|---|---|
  | GIMP `.gpl` | `GIMP Palette` first line | `Name:`, `Columns:`, `#` comments to `description`, per-colour names (the 4th field) |
  | Lospec `.hex` | lines of 6 or 8 hex digits | `#` optional |
  | JASC `.pal` | `JASC-PAL`, `0100`, count | RGB per line |
  | Microsoft `.pal` | `RIFF`…`PAL ` `data` chunk, version 0x0300 | `PALETTEENTRY` r, g, b, flags |
  | Paint.NET `.txt` | `;` comments, `AARRGGBB` lines | alpha kept |
  | Adobe `.ase` | `ASEF` magic, version 1.0 | RGB, Gray and CMYK colour blocks (CMYK uncalibrated naive conversion, flagged in a warning), group names kept as ramps |
  | Aseprite `.ase`/`.aseprite` | header magic `0xA5E0` | palette chunk 0x2019 (names, alpha), old 0x0004/0x0011, transparent index; through S44's codec. Inside `Assets/`, S44 also exposes it as the read-only sub-asset `<file>.aseprite#palette#0` (S44 R19), usable as a Palette anywhere, for example as a Palette Swap source; "Extract Palette" makes an editable copy |
  | Images (`.png`, `.gif`, …) | image | unique colours in scan order (≤ 256), or one per cell of a swatch strip (an image whose width is a multiple of its height: each square is one colour) |
  | `.colors` | the swatch-library JSON | linear → sRGB8; HDR swatches clamped, with a warning |

  - The result is a new `.palette` beside the source, or at a chosen path. The source file itself
    is not imported as an asset.
  - A foreign palette file already under `Assets/` offers "Import as Palette" in the Project
    window's context menu.
  - Failures are typed:
    - `format_unsupported` (it lists the formats);
    - `too_many_colors` (with the count, and fix "Reduce Colours… or Import the first 256");
    - `parse_error` (with the line or offset).
- **R6. Export.** Export ▸ GIMP (.gpl), Hex (.hex), JASC (.pal), RIFF (.pal), Adobe (.ase),
  Paint.NET (.txt), PNG strip (1 px per colour, or N×N swatches), Swatch Library (.colors,
  the first 76 colours, as SwatchLibrary allows).
  - Each writer round-trips through its reader to an identical colour list (R5, AC2).
  - Alpha is written only by formats that hold it; a format that cannot carries a warning.
- **R7. Built-ins as templates.** The Palette window's "Built-in" section and every Palette asset
  field list the built-ins.
  - Using one, in a field, `edit_palette` or the swap or quantize setup, copies it once to
    `Assets/Palettes/<slug>.palette`, then references the copy. A second use reuses the copy
    (matched by `source.kind = builtin` and its id).
  - The runtime never references editor resources, and an export contains only what is used.
  - **The `builtin:<id>` alias.** Wherever a palette is named (asset fields, MCP `palette`
    arguments, presets, S46 and S47 "preset names", S48's `builtin:fire`), the alias resolves
    through this copy. What gets stored is always the project reference. `builtin:fire#ramp:fire`
    resolves the same way (R4a).
- **R8. Lospec fetch.**
  - **Entry:** Import ▸ From Lospec… opens a small dialog with one field. It accepts a slug
    (`endesga-32`), a palette page URL (`https://lospec.com/palette-list/endesga-32`) or a
    `lospec-palette://endesga-32` link, normalised to the slug, which must match
    `[a-z0-9-]{1,64}`.
  - **Fetch:** pressing Fetch requests `https://lospec.com/palette-list/<slug>.json` through
    `editor/htmlui/HttpClient`. It runs on a worker with a 10 s timeout and shows a spinner and a
    Cancel button.
  - **Success:** the dialog shows the swatches, name, author and count. Create writes
    `Assets/Palettes/<slug>.palette` with `source {kind: lospec, id, url, fetched (ISO date)}` and
    `author`, and opens it in the Palette window.
  - **Failure:** the dialog shows one line plus a fix, and never a modal error or a hang:
    - `network_unavailable`: no curl, DNS or connect failure, or timeout. "Can't reach lospec.com
      (offline, or blocked by a proxy). Download the palette's .hex from its Lospec page and drop
      it on this window."
    - `not_found` (HTTP 404): "No Lospec palette called '<slug>'. Copy the slug from the
      palette's address on lospec.com."
    - `bad_response`: unexpected JSON.
  - **Offline at once:** with the editor setting `network.offline` (or no curl,
    `HttpClient::available()`), the dialog says so before any request and offers Import File….
  - **Tests:** `JASPERENGINE_IMPORT_MIRROR` serves recorded responses.
- **R9. The built-in library.** 37 palettes, embedded in the editor.
  - **pixelfx's 27 ramps** (`color.js:52-80`), each a palette with one full ramp named after it:
    fire, ember, lava, smoke, dust, ice, water, electric, volt, poison, holy, heal, arcane,
    shadow, blood, earth, stone, gold, leaf, wood, metal, bone, neon, cyber, matrix, candy and
    night.
  - **10 classic palettes:** Game Boy (DMG), PICO-8, CGA, EGA, Commodore 64 (Pepto), ZX
    Spectrum, Sweetie 16, Endesga 32, Resurrect 64 and AAP-64, each with its author and source.
  - **One extra palette, "pixelfx: Effects"** (`builtin:pixelfx-effects`), holds all 27 ramps
    as ramps of one palette. It also has the three ramps of `sim.js:13-16`: `hot` (the spark
    colours, 4), `fire_cooling` (white-hot to smouldering, 9) and `smoke_thin` (5). That is
    220 colours; exact duplicates share one index, and the palette stays ≤ 256. S48's and
    S49's presets point at it.
  - The list is data: adding a palette is one file plus a `NOTICE.md` line.

### Functional — ramps and the generator

- **R10. Generate Ramp.** A dialog, also reachable through `edit_palette` op `generate_ramp`.
  - **Inputs:** a base colour (picker, or the selected swatch), Darker steps (0–15) and Lighter
    steps (0–15), and Mode:
    - **Hue Shift** (the default): Pixelorama's rule (§4), applied once per step from the base,
      with Hue (°, default 10), Saturation (%, 10) and Value (%, 10) per step. The limits are
      60° (lighten), 240° (darken), a saturation floor of 10 % and a value floor of 10 %, all
      editable under Advanced.
    - **OKLCh**: even OKLab L steps between the chosen darkest and lightest L. The hue rotates by
      Hue° per step toward 60° (lighter) and 240° (darker), and chroma falls toward both ends
      (a parabola with an editable peak).
  - **Output:** the preview strip updates live. Add to Palette appends the new colours (skipping
    exact duplicates, which it reuses) and a ramp of them. Replace Ramp rewrites a selected
    ramp's colours in place.
  - The output is deterministic, and AC6 checks it against a golden list.
- **R11. Ramps in the window.**
  - Each ramp shows as a strip under the grid.
  - Dragging swatches from the grid onto a strip appends them; dragging inside a strip
    reorders it.
  - A ramp's context menu: Rename, Reverse, Fit to N Steps (R4), Cycle Speed, Delete.
  - The grid highlights the selected ramp's swatches.

### Functional — colour maths (shared; S46, S47 and S48 depend on it)

- **R12. `engine/core/pixelart/PixelColor.h`** (§6.4) provides:
  - sRGB8↔linear through the piecewise IEC 61966-2-1 curve (8-bit tables);
  - OKLab (Ottosson, to 1e-6 against his reference values);
  - CIELAB D65;
  - CIEDE2000 (k_L = k_C = k_H = 1; all 34 Sharma pairs within 1e-4);
  - HSV and OKLCh;
  - `ColorMetric {OkLab, Ciede2000, Rgb}`;
  - `PaletteMatcher` (nearest index, and distance, cached per 24-bit colour);
  - `ditherPair` (R19);
  - `medianCut` + `kMeansRefine` in OKLab (R23).

  The code is deterministic (no threads inside; the callers parallelise by rows) and identical
  on every platform for the same inputs. It uses no `std::pow`/`cbrt` approximations that differ
  per libc: `cbrt` is a fixed Newton iteration, so the CPU results are bit-identical across the
  platforms we test. The editor's `ColorMath.h` keeps its API and forwards sRGB and HSV to it.
  The GLSL twin is `includes/palette.glsl` (OKLab only, for diagnostics; the runtime passes need
  no colour space in the shader, R19).

### Functional — Palette Swap (runtime)

- **R13. The feature.** `library:sprite` gains `PALETTE_SWAP` "Palette Swap" and `PALETTE_CYCLE`
  "Palette Cycling".
  - **Binding:** both use the material's palette binding `u_palette`, declared in the shader by a
    new manifest directive `// @palette u_palette "Palette" feature=PALETTE_SWAP,PALETTE_CYCLE`
    (§6.5).
  - **What the binding holds:** `{source: Palette, targets: [Palette | "<palette>#<variant>"] (≤ 15),
    useVariants: true}`. Rows are numbered:
    - row 0 is the source's own colours;
    - rows 1…n are the source's variants (when `useVariants`), then the extra targets, in order.
  - **Choosing a row:** the `paletteRow` property (int, 0–15) picks the active row, set from the
    inspector, Lua (`sprite.palette`), animation (`paletteRow` is keyable like any shader
    parameter) or MCP.
- **R14. Exact colour match.** Swap runs on the raw texel, before the tint, before COLOR_REPLACE
  and before every other effect (the effect order comment in `sprite.frag:37-39` gains it first).
  - **The lookup:**
    1. The texel's sRGB8 colour (`round(texel·255)`) picks a cell of a 32×32×32 grid (5 bits per
       channel), read with `texelFetch` from a 256×128 RGBA8 texture.
    2. That cell holds up to 4 candidate source indices (index + 1; 0 means none).
    3. The shader compares each candidate against the source row's exact colour and keeps the
       first match.
  - **No match:** the texel is left unchanged. That covers anti-aliased, off-palette and
    filtered pixels.
  - **A match:** the output is the target row's colour at that index. The output's alpha is the
    texel's alpha times the target's alpha, so a target alpha of 0 hides a part (a hat, say).
  - **Limits:** a source has at most 255 colours. A cell with more than 4 source colours (four
    colours within 8/255 of each other on every channel) is the typed error
    `palette_cell_overflow` at bind, naming the colours, with fix "merge these colours or use a
    smaller source palette". Exact duplicates in the source map to their first index (warning).
  - **Same image:** the swapped image equals Apply Palette's index-preserving remap of the same
    texture, byte for byte (AC9).
- **R15. Lossless textures.** Swap needs the exact colours.
  - When `PALETTE_SWAP` is on and the sprite texture's import settings are lossy (compression,
    mipmaps, or a resize that is not Nearest), the material inspector shows a warning: "Palette
    Swap needs lossless textures: <texture> is compressed". It offers Fix, which applies S14
    `set_texture_settings` lossless/Nearest/no mips in one undo step.
  - `get_entity`/`check` report the same thing as the warning `palette_swap_lossy_texture`.
  - S42's validator adopts it: its `mipmaps_compression` rule covers every texture a Palette Swap
    material draws (S42 R10), and its `palette` rule checks palette conformance through
    `apply_palette`'s measure core (R40).
- **R16. Palette Cycling** (DPaint-style colour cycling).
  - Every ramp of the source with `cycle ≠ 0` rotates: an index at position p of a ramp of
    length L shows the colour of ramp index `(p + floor(time · cycle · cycleSpeed)) mod L`.
  - `cycleSpeed` is a material multiplier, default 1. It is negative to reverse and 0 to freeze.
  - At most 8 cycling ramps per source; ramps never overlap (validated).
  - Cycling composes with swap: the cycled index is looked up in the active row.
  - Time is the material's engine time (`customParams.time`), so pausing the game pauses it, and
    a fixed timestep makes it deterministic.
- **R17. Per instance.**
  - **Shared row:** sprites, tilemaps and polygons that share a material share its row unless
    they set their own. `sprite.palette` writes the sprite's own submesh material, which
    already is per renderer.
  - **Instanced drawing:** the property is declared
    `// @property paletteRow int 0 "Active palette (0: original)" feature=PALETTE_SWAP instance=fx.x`,
    following S52 R22/R23. Slot `fx.x` of S52's per-instance FX stream (a half that stores the
    integer exactly) carries the row. Sprites that differ only in their row stay in one batch.
    S52 owns the stream and the `instance=` directive; this spec owns the slot's meaning:
    `round(fx.x)` clamped to 0…rows−1.
- **R18. Team colours and tints workflow.**
  - Create Variant (in the Palette window) copies the base into a new variant column; the user
    edits only the colours that differ.
  - In the sprite's material, ticking Palette Swap fills Source from the sprite's texture when
    exactly one project palette covers all its opaque colours ("Detect").
  - The Active Palette dropdown lists "Original" plus each row's name.

### Functional — Palette Quantize (post)

- **R19. The setting.** `PostProcessingSettings` gains a group.

  | Field | Default |
  |---|---|
  | `paletteEnabled` | false |
  | `palette` (AssetRef) | — |
  | `paletteDither` {None, Bayer 2×2, Bayer 4×4, Bayer 8×8} | Bayer 4×4 |
  | `paletteDitherStrength` (0–1) | 1 |
  | `paletteDistance` {OKLab, RGB} | OKLab |
  | `palettePrecision` {Normal (32³), High (64³)} | Normal |

  When enabled with a palette, `getPostProcessingTiers` sets `UBER_PALETTE`, which turns on the
  uber pass. `UBER_PALETTE` is the next free `UberFeature` bit at landing (18 when it is the first;
  S41's `UBER_SHARP_BILINEAR` lands in the same wave and S48's `UBER_GLITCH` later, each appended
  in landing order with a fixed name), and `UBER_FEATURE_COUNT` grows by one. With fixed resolution that
  pass is the upscale blit anyway, so no pass is added.
- **R20. The LUT.**
  - **Bake:** the CPU bakes, per grid cell (the display-encoded sRGB colour at the cell centre),
    `(A, B, ratio)`.
    - With no dither: A is the nearest palette colour by `paletteDistance` and the ratio is 0.
    - With a dither: Yliluoma's mix among the 8 nearest. Choose `(A, B, r)`, with r in 1/16
      steps up to `paletteDitherStrength`, minimising
      `|mix(A,B,r) − c|² + 0.1·r(1−r)·|A − B|²`.
  - **Identity:** a cell that holds an exact palette colour bakes `(that colour, —, 0)`, so an
    image already in the palette passes through unchanged even with dithering on (AC12).
  - **Storage:** cells go into a 256×128 (Normal) or 512×512 (High) RGBA8 `texelFetch` texture,
    and the palette colours into a 256×1 row.
  - **Async:** the bake runs as a job (S06, Background lane). Until it finishes, the previous LUT
    stays (or the feature waits, on its first use). The result does not depend on the thread
    count.
- **R21. The shader stage.**
  - **Position in the uber pass:** after the grade, edge and vignette, before CRT, film damage
    and grain. The display emulations sit on top of a palette image, the way a real screen
    would. `UBER_DITHER` (anti-banding) is not compiled while `UBER_PALETTE` is on.
  - **The pick:** the shader reads the cell from the display colour, then compares the Bayer
    threshold of the dither cell with the ratio and picks A or B.
  - **The dither cell:**
    - normally the *source* texel, `floor(uv · uber.sourceTexel.zw)`, which is one art pixel at
      fixed resolution;
    - the pixelate block when `UBER_PIXELATE` is on.

    So the pattern never smears across an upscaled art pixel.
  - **Bayer matrices:** the standard recursive 2/4/8 matrices, the same as the existing
    `jfx_bayer*` and pixelfx's `(v + 0.5)/16` table (`painter.js:10`). They live in
    `includes/bayer.glsl`, which S43 owns and writes in wave 5A: the family's one Bayer table.
    S48 (DITHER_FADE, RAMP_RECOLOR, the fade transition) and S49 (dithered particle fades) include
    it and add no table of their own.
  - **Outside the result:** the UI layers drawn after the uber pass are not quantized. This is
    inferred from the main-camera-only post stack, `SceneSettings.h:428-433`, and M4 verifies
    it. No family spec quantizes the UI: a HUD stays on the palette when its art does (S51's
    bitmap fonts draw their glyph colours exactly).
- **R22. Presets.** The post-processing presets (the Inspector's Preset menu and
  `apply_post_processing_preset`; `docs/mcp/guide/post-processing.md:40-49` lists them) gain:
  - `handheld_4`: Palette Quantize with Game Boy (DMG) and Bayer 4×4;
  - `pico_8`: PICO-8, no dither.

  Applying either copies its built-in palette into the project (R7). `library:gameboy` and
  `library:posterize` stay. Their descriptions gain "for an exact palette
  use Post-processing ▸ Palette Quantize".

### Functional — Apply Palette / Reduce Colours (editor)

- **R23. The operation**, on one texture asset (PNG, BMP or TGA; S44's Aseprite files are out of
  scope here):
  - **Apply Palette:** every pixel goes to its nearest palette colour (`ColorMetric`, default
    OKLab; CIEDE2000 is offered).
    - Dither: None, Bayer 2/4/8 (the same pair mix as R20, but exact per colour, with no cells),
      or Floyd–Steinberg (editor only), with Strength.
    - Alpha: Keep, or Binary at a threshold (default 128), the pixel-art default.
    - Locked palette colours are always used.
  - **Reduce Colours:** builds an N-colour palette (2–256) from the image: median cut in OKLab,
    then up to 8 k-means iterations, deterministic, with locked colours kept. Then it applies
    that palette. Save Palette As writes the palette found.
  - **Remap Indices** (internal; used by AC9): maps a texture painted in palette P to palette Q
    by index, with exact matches only.
- **R24. Preview, then write.** The Apply Palette dialog (Project window ▸ texture ▸ Apply
  Palette… / Reduce Colours…, or the Palette window's "Apply to Texture…") shows:
  - a split before/after view with integer zoom and Nearest filtering, flicker-compare on Space;
  - the statistics: colours before and after, pixels changed, mean and max ΔE (OKLab ×100);
  - **Lost Colours**: groups of 8-connected pixels whose mapped colour is more than 0.06 OKLab
    from the original (ritmo `lost.ts`). Each group is listed with its count and bounding box,
    and hovering one highlights it.

  Apply writes the texture, or Save As a new file, as one undo step (`writeUndoable`). Undo
  restores the original bytes. The texture's `.meta` is untouched, so its GUID is kept.

### Platform & backends

- **R25.** Everything runtime runs on GL 3.3, GLES3/WebGL2, Metal, D3D11 and Vulkan:
  - RGBA8 textures read only with `texelFetch` (no filtering, no float textures);
  - integer maths in the fragment shader limited to what GLSL ES 3.0 has (`ivec2`, `int` `%`
    on non-negative values);
  - no compute, no SSBOs, no texture arrays.
- **R26.** The palette textures use clamp and Nearest, have no mips and are sRGB-agnostic
  (UNORM; bytes are compared as bytes).
- **R27.** Lospec, the importers and Apply Palette are editor-only. The runtime palette code
  builds on every export target, including web, where the quantize bake runs on the main thread
  in slices (R30).

### Performance (reference machines as in S20 §5: CI, mid desktop, mid mobile, web)

- **R28. Off costs nothing.**
  - With `PALETTE_SWAP`/`PALETTE_CYCLE` off, no variant is compiled, no texture is created and
    no CPU work is done per frame.
  - With `paletteEnabled` false, or no palette, the uber bit is clear, no LUT exists and nothing
    is baked.
  - `PostProcessingTiersTest` and a GPU allocation probe check this (AC13).
- **R29. Palette Swap.**
  - **Build:** per (source, targets) set, the swap textures are built once and shared through a
    content-hash cache (`PaletteSwapCache`), then freed 2 s after the last user goes.
    - The build costs ≤ 0.3 ms CPU on mid desktop and ≤ 1 ms on mid mobile.
    - GPU memory: 128 KB of cells plus ≤ 26 KB of rows.
  - **Shader:** typically 3 texture fetches (cell, verify, target) and at most 6. A 1920×1080
    field of sprites with overdraw 2 costs ≤ 1.25× the GPU time with the feature off, on
    CI's GPU, and ≤ +0.5 ms on mid mobile (S15 GPU timings).
  - **Changing the row:** a uniform write, with no rebuild.
- **R30. Palette Quantize.**
  - **Shader:** ≤ +2 fetches per output pixel; ≤ 0.15 ms at 1080p on mid desktop and ≤ 0.6 ms
    on mid mobile.
  - **Bake, Normal 32³, 64 colours, dithered:**
    - ≤ 15 ms on mid desktop (jobs);
    - ≤ 60 ms on web, single thread, in 8 ms slices across frames.
  - **Bake, High 64³:** ≤ 120 ms on mid desktop, and the inspector says it is slower to bake.
  - A palette change at runtime (Lua) never stalls a frame for more than one slice.
- **R31. Editor.**
  - Usage counts on a 1024² texture: ≤ 30 ms on a worker, cached by the texture's content hash.
  - Apply Palette, 1024², 64 colours: ≤ 150 ms in OKLab, ≤ 600 ms in CIEDE2000. Over 1 s it is
    an MCP job.
  - The Palette window draws 256 swatches in ≤ 0.5 ms.
  - Import and export of 256 colours: ≤ 5 ms.

### Editor UX (human)

- **R32. The Palette window** (Window ▸ 2D ▸ Palette; it opens on double-clicking a `.palette`).
  - **Toolbar:** a palette selector (project and built-in), New, Import ▾ (File…, From Lospec…,
    From Texture…, From Swatch Library…), Export ▾, Sort ▾ (Hue, Saturation, Luma, Value,
    Reverse; Sort into a New Palette, checked by default, as in Pixelorama), Generate Ramp…,
    Create Variant.
  - **Grid:** `columns` wide; swatch size 12–48 px (a slider); index numbers on hover, or always
    (a toggle).
    - Selection: click, Shift-click for a range, Ctrl-click to add.
    - Drag to reorder, with an insertion marker.
    - Delete removes; Ctrl+D duplicates; Alt+←/→ moves.
    - Double-click opens the colour picker (R34).
    - The context menu: Rename, Lock/Unlock, Set as Transparent, Copy Hex, Paste Hex.
  - **Below the grid:** ramps (R11), then a Variants table (rows = base indices, columns =
    variants; a `null` cell is hatched and means "same as base"), then Usage (R33).
  - **Undo:** every edit is one undo step on the `.palette`, with the same undoable asset-edit
    path as `.particles` assets (`editor/util/ParticleEffectEdit.h`). Picker drags merge into one
    step (`InteractiveEdit`, `mergeWith`).
- **R33. Usage counts.**
  - The Usage section follows the Project window's selected texture, or a pinned Texture field.
  - Each swatch gets a small count badge (exact sRGB8 matches).
  - The section lists "Off-palette: N pixels in M colours", with each colour's nearest palette
    index and ΔE. Hovering an off-palette colour highlights its pixels in a thumbnail (integer
    zoom).
  - Apply Palette… opens R24 with this palette preselected.
- **R34. The colour picker** (`ColorPickerWindow`).
  - Project palettes appear as libraries, under a "Palettes" heading, below the swatch
    libraries. Clicking a palette colour picks it.
  - "Add to Palette" adds the current colour to the palette shown.
  - A "Snap to Palette" toggle (off by default) snaps picked and dragged colours to the nearest
    colour of a chosen palette (OKLab). It is useful when editing particle or tint colours for
    pixel art.
  - Edits made through a palette library are undoable palette commands, unlike the `.colors`
    libraries, whose behaviour is unchanged.
  - The SwatchLibrary API is untouched apart from a read-only adapter that presents a Palette as
    a `SwatchLibrary` (linear values from exact sRGB8).
- **R35. The material inspector**, for `library:sprite` with Palette Swap.
  - Fields: Source Palette (asset field plus "Detect"), Use Variants (checkbox), Extra Targets
    (a reorderable list), Active Palette (a dropdown of the row names, previewing live in the
    Scene view), Cycling Speed (with PALETTE_CYCLE).
  - Warnings: R14 cell overflow, R15 lossy texture, and "N opaque colours of this sprite are not
    in the source palette (they will not swap)" with a "Show" button that opens the Palette
    window's Usage.
- **R36. Post-processing inspector:** a "Palette Quantize" section with the R19 fields, a small
    swatch strip of the palette, and a "Baking…" line while the bake runs.
- **R37. The Project window.**
  - `.palette` thumbnails show the swatches (up to 64, then "+N").
  - Context menus: Create ▸ Palette; on a palette, Export ▸; on a texture, Apply Palette… /
    Reduce Colours… / Extract Palette (that is, Import from Texture).
- **R38. Docs.** `docs/palettes.md`: the asset, formats, Lospec, ramps, Swap, Cycling, Quantize,
  Apply Palette, and a team-colours walkthrough with screenshots.

### Agent UX (MCP, Lua, CLI)

- **R39. `edit_palette`** (group `pixel_art`, §6.6): list, read, create, import, Lospec, edit ops
  and export.
  - **Preview first:** without `output` it returns the resulting palette (and an optional swatch
    image) and writes nothing. The same arguments plus `output: true` write it, as one undo step.
  - The result echoes the palette in compact form, with the changed indices, typed errors and
    an `undo` entry.
- **R40. `apply_palette`** (group `pixel_art`): Apply Palette / Reduce Colours on a texture.
  - **Preview:** a before/after image, the statistics, the per-colour usage, the lost colours and
    a verdict:
    - `clean`: the texture already uses only palette colours (0 pixels change);
    - `fixable`: mapped with max ΔE ≤ 0.15 OKLab and no lost-colour group of 16 px or more;
    - `needs_person`: otherwise, with the groups listed.
  - **Writing:** `output: true` writes over the texture; `output: "<path>"` writes a new one.
  - **Read-only checks:** `measure_only: true` makes it a read-only check that never writes, for
    S42's validator.
- **R41. No new swap or quantize tools.**
  - Palette Swap is set through the existing material paths: `set_properties`
    `submeshes[0].material.shaderFeatures`/`shaderParams.paletteRow`/`shaderPalettes.u_palette`,
    and `create_material`/`update_material` values.
  - Palette Quantize goes through `set_scene_settings` `post_processing.palette_*` (the field
    table, `SceneSettings.h:283-300`).
  - `get_entity` returns the resolved row names.
  - The guide (`docs/mcp/guide/palettes.md`) shows each path.
- **R42. Lua (§6.7).**
  - `Palette.load(path)`, `Palette.new(colors)`, `p.count`, `p.name`, `p:getColor(i)` (linear,
    like other Lua colours), `p:getHex(i)`, `p:nearest(color[, distance])`, `p:getRamp(name)`,
    `p:rampColor(name, t)` (stepped), `p.variants`.
  - `sprite.palette` (get/set the active row by name, or by number; "" or 0 is the original),
    `sprite.paletteRow`, `sprite:setPaletteTarget(row, palette)` (replace a row at runtime with
    any palette, a runtime-created one included, e.g. a character creator),
    `sprite.paletteCycleSpeed`. Tilemap gets the same.
  - `scene.postProcessing.paletteEnabled`, `.palette` (path), `.paletteDither`, and the rest.
  - A wrong name raises a Lua error that lists the valid names.
- **R43. CLI.** `jasperengine-editor call edit_palette` and `call apply_palette` work offline on
  `--project` (S31). `apply_palette` with `measure_only` exits 0 on `clean`, 3 on `fixable` and
  4 on `needs_person` (ritmo's exit codes, the family convention), and 2 on usage errors.

### Data & compatibility

- **R44. Old content is untouched.**
  - COLOR_REPLACE, `library:gameboy`, `library:posterize` and every existing uber bit keep their
    values and behaviour.
  - The new sprite features are appended defines.
  - `UBER_PALETTE` is appended (the bits are part of saved shader keys).
  - No existing scene, material or particle file changes on load.
- **R45. Material data.** `Material` gains `shaderPalettes` (uniform → `{source, targets[],
  useVariants}`), optional and empty by default.
  - It is encoded in `Stream::encodeMaterial`/`decodeMaterial` (`editor/Stream.cpp:5067`,
    `:5117-5122`) and in `.material` files.
  - It is an optional new field, so the scene format is not bumped (`StreamMigrations.h:18-26`).
  - `AssetRefSchema` gets rows for it and for `post_processing.palette` (S04 rename and
    redirect safety).
- **R46. The palette format** is version 1.
  - A file with a newer version loads read-only with a warning, as `.colors` files do. A later
    version bump comes with a migration in `PaletteCodec`.
  - `AssetType::Palette = 24`: the family's reserved value (ledger: Palette 24, AsepriteSprite 25,
    SpriteAtlas 26, Tileset 27). The number is the reserved one whatever order the specs land in:
    the entry is written with its explicit value, `assetTypeFromName`'s bound becomes the highest
    value present, and its loop skips values without a name (a gap a sibling fills later). It is
    added to `assetTypeName` and the `AssetTypes.cpp` row
    `{Palette, Sidecar, Assets, false, false}, {".palette"}`.
- **R47. Determinism.** For the same inputs on one build:
  - every importer, exporter, the generator, the quantize bake, Apply Palette and Reduce Colours
    are byte-deterministic;
  - Palette Swap and Quantize output are identical across runs.

## 6. Design constraints & decisions

### 6.1 MCP tools and the budget (S31; group `pixel_art`, shared by S41, S42, S43, S46 and S48, ≤ 12 tools and 15,000 characters)

| Tool | Group | Change | Estimated listing chars |
|---|---|---|---|
| `edit_palette` | `pixel_art` | new: `path`, `from`, `ops[]`, `export`, `texture`, `image`, `output`, `overwrite`; the op vocabulary is in `jasper://docs/palettes` | ≤ 1,450 |
| `apply_palette` | `pixel_art` | new: `path`, `palette`, `reduce`, `distance`, `dither`, `dither_strength`, `alpha_threshold`, `measure_only`, `output`, `save_palette`, `image`, `max_width` | ≤ 1,350 |
| `set_properties`, `create_material`, `update_material`, `set_scene_settings`, `get_entity` | core | none in the schema (the new material and post fields are values; descriptions unchanged) | 0 |

S43 takes 2 of the group's 12 tools and ≤ 2,800 of its 15,000 characters (the family's total is in
the ledger, "MCP budget": 6 tools, ≈ 8,050 characters). S43 is the first spec with a tool in
`pixel_art` (wave 5A), so it adds the group's row in `McpToolGroups.cpp`, with the description
"Pixel art: palettes, validator, cleanup, effects (jasper://docs/pixel-art)". The agent guide gets
one line in the `## Pixel art` digest section (S41 creates it, S41 §6.4): "Palettes:
edit_palette/apply_palette preview first; output: true writes; jasper://docs/palettes" (≈ 90
bytes).

### 6.2 The `.palette` file

```json
{
  "format": "jasperengine.palette",
  "version": 1,
  "name": "Hero",
  "author": "",
  "description": "",
  "source": {"kind": "lospec", "id": "endesga-32", "url": "https://lospec.com/palette-list/endesga-32", "fetched": "2026-11-04"},
  "columns": 8,
  "colors": ["#00000000", "#1a1c2c", "#5d275d", "#b13e53", "#ef7d57", "#ffcd75", "#a7f070", "#38b764"],
  "names": {"3": "cape mid", "4": "cape light"},
  "locked": [0, 1],
  "transparent": 0,
  "ramps": [
    {"name": "cape", "colors": [2, 3, 4, 5]},
    {"name": "water", "colors": [6, 7], "cycle": 4}
  ],
  "variants": [
    {"name": "Team Blue", "colors": [null, null, "#29366f", "#3b5dc9", "#41a6f6", "#73eff7"]},
    {"name": "Hurt", "colors": [null, "#ffffff", "#ffffff", "#ffffff", "#ffffff", "#ffffff", "#ffffff", "#ffffff"]}
  ]
}
```

- **Keys** are camelCase, like `.particles` (S20). MCP shows them snake_case only at the tool
  surface.
- **Writing:** the writer leaves out keys at their defaults, sorts nothing, and keeps unknown keys.
- **`source.kind`** is one of `user`, `builtin`, `lospec`, `file` (with the original file name),
  `texture` (Reduce Colours) or `aseprite`.

### 6.3 Decisions

- **D1. No runtime indexed-texture mode in S43.** Reasons:
  1. Colour Match (R14) gives the same pixels from the ordinary lossless RGBA8 texture, with no
     authoring step, no new texture format and no change to how sprites are imported.
  2. An R8 main texture would fork every sprite consumer: the OUTLINE and OVERLAY taps, 2D
     light normal maps, tilemaps, S45's atlas packer, S52's instancing, the Sprite Preview, the
     thumbnails, and every effect that reads the texture. Each would need an index-decode path,
     and all of them would need golden tests on five backends.
  3. The 4× memory saving is real but small for pixel art. A 2048² atlas is 16 MB as RGBA8 and
     4 MB as R8, and S14's lossless formats already keep 1024² atlases at 4 MB.
  4. Palette cycling, the classic reason for indexed colour, is delivered by R16 without it.

  Its trigger for a follow-up spec: a shipped game whose palette-swapped atlases exceed the
  mobile texture budget (S15's VRAM panel). The follow-up adds an Index Source "Indexed Texture"
  to `@palette`. The same LUT rows, row numbers and Lua API apply; only the index lookup
  changes from the cell grid to an R8 fetch.
- **D2. Swap before the tint.** The match needs the authored bytes. The tint (`baseColorFactor`
  × vertex × instance colour) then multiplies the swapped colour, so a white tint keeps the
  exact palette.
- **D3. One cell grid with 4 candidates, not 6-bit cells.**
  - 5 bits × 4 candidates is 128 KB and needs a 4-way collision before it fails. That is
    impossible for any real palette: the 37 built-ins and 400 sampled Lospec palettes are checked
    in AC8.
  - 6-bit cells with 1 candidate would be 256 KB and could still collide.
- **D4. Quantize is an uber feature, not a T3 library pass.** It must be the last colour
  operation before the display emulations. T3 passes run before the grade (§3), and an extra
  pass would cost a full-screen read and write on mobile. The library `gameboy`/`posterize`
  passes stay for compatibility and for stylised chains.
- **D5. The dither pattern is per art pixel.** It is anchored to source texels, or to pixelate
  blocks, so it is stable under the S41 integer upscale and never shimmers when the camera
  moves by whole pixels.
- **D6. Built-ins are copied into the project when used** (R7). There is no `library:` asset
  reference kind, nothing for the runtime to embed, and exports stay minimal.
- **D7. One maths library in `engine/core/pixelart/`.** The runtime needs the matcher
  (`Palette:nearest`), the bake and the ramp helpers; the editor reuses them. That is the single
  place S46, S47, S48 and S49 include.
- **D8. Sorting renumbers.** Ramps, variants, locked indices and the transparent index are
  remapped with it. Textures painted by index (Aseprite indexed) are not, and the Sort menu says
  so ("Sorting changes colour numbers; indexed Aseprite files keep their old numbers").

### 6.4 C++ sketch

```cpp
// engine/core/pixelart/PixelColor.h
namespace jasperengine::pixelart {
    // engine/core/pixelart/Rgba8.h: the family's one 8-bit colour, also PixelImage's pixel (S44);
    // S43 or S44, whichever lands first, adds it with exactly this content
    struct Rgba8 { uint8_t r = 0, g = 0, b = 0, a = 255; auto operator<=>(const Rgba8&) const = default; };
    struct Lab { float l = 0.0f, a = 0.0f, b = 0.0f; };
    enum class ColorMetric : uint8_t { OkLab, Ciede2000, Rgb };   // S46 R8 uses this name

    float srgbToLinear(uint8_t c);              // IEC 61966-2-1, table
    uint8_t linearToSrgb(float linear);
    Lab toOklab(Rgba8 c);  Rgba8 fromOklab(const Lab& lab, uint8_t alpha = 255);
    Lab toCielab(Rgba8 c);                      // D65
    float ciede2000(const Lab& x, const Lab& y);
    float distance(Rgba8 x, Rgba8 y, ColorMetric metric);  // OkLab: Euclidean; Rgb: on sRGB8/255

    class PaletteMatcher {                      // not thread-safe: one per worker
    public:
        PaletteMatcher(std::span<const Rgba8> palette, ColorMetric metric, std::span<const uint16_t> only = {});
        int nearest(Rgba8 c);                   // cached by the 24-bit colour
        std::pair<int, float> nearestWithDistance(Rgba8 c);
        struct Mix { uint16_t a, b; uint8_t ratio16; };   // ratio in 1/16
        Mix ditherPair(Rgba8 c, float strength); // Yliluoma #1 over the 8 nearest
    };
    std::vector<Rgba8> medianCut(std::span<const Rgba8> pixels, int count, std::span<const Rgba8> keep);
    std::vector<Rgba8> fitRamp(std::span<const Rgba8> ramp, int steps);
    Rgba8 rampAt(std::span<const Rgba8> ramp, float t);
}

// engine/core/pixelart/Palette.h
namespace jasperengine::pixelart {
    struct PaletteRamp { std::string name; std::vector<uint16_t> colors; float cycle = 0.0f; };
    struct PaletteVariant { std::string name; std::vector<std::optional<Rgba8>> colors; };
    struct Palette {
        static constexpr int kVersion = 1, kMaxColors = 256, kMaxRamps = 64, kMaxVariants = 15;
        std::string name, author, description;
        Json source;                            // kept as read
        int columns = 8;
        std::vector<Rgba8> colors;
        std::map<int, std::string> names;
        std::vector<uint16_t> locked;
        int transparent = -1;
        std::vector<PaletteRamp> ramps;
        std::vector<PaletteVariant> variants;
        Json unknown;                           // preserved keys

        const PaletteRamp* findRamp(std::string_view name) const;
        std::vector<Rgba8> rampColors(std::string_view name, int steps = 0) const;
        Rgba8 variantColor(int variant, int index) const;   // falls back to the base
        // uncached convenience for one-off lookups (S46 R8); hot loops build a PaletteMatcher
        int nearest(Rgba8 c, ColorMetric metric = ColorMetric::OkLab) const;
    };
    namespace PaletteCodec {
        bool read(std::string_view json, Palette& out, std::vector<Diagnostic>& diagnostics);
        std::string write(const Palette& palette);
    }
    // the swap textures of one (source, targets) set, shared and reference counted
    struct PaletteSwapTextures { TextureRender cells; TextureRender rows; int rowCount; std::vector<std::string> rowNames; };
    class PaletteSwapCache {
    public:
        std::shared_ptr<PaletteSwapTextures> acquire(const PaletteBinding& binding, std::vector<Diagnostic>& diagnostics);
    };
    // Palette Quantize: cells of (A, B, ratio) and the colour row; sliced for single-thread web
    struct QuantizeLutJob;
    std::unique_ptr<QuantizeLutJob> startQuantizeBake(const Palette&, ColorMetric, int ditherSize, float strength, int cellsPerAxis);
}

// editor/pixelart/PaletteFormats.h
namespace jasperengine::editor::pixelart {
    enum class PaletteFormat { Gpl, Hex, JascPal, RiffPal, PaintNetTxt, AdobeAse, Aseprite, Image, SwatchLibrary, PngStrip };
    using PaletteError = PixelProblem;   // editor/pixelart/PixelProblem.h (S44 §6.3; added by S43 if it lands first)
    std::variant<Palette, PaletteError> importPalette(const std::filesystem::path& path, std::span<const uint8_t> bytes);
    std::variant<std::vector<uint8_t>, PaletteError> exportPalette(const Palette&, PaletteFormat, std::vector<std::string>& warnings);
    // editor/pixelart/Lospec.h: normaliseSlug, fetchLospec(slug, HttpClient&) -> Palette | PaletteError
    // editor/pixelart/RampGenerator.h: struct RampRecipe {...}; std::vector<Rgba8> generateRamp(const RampRecipe&)
    // editor/pixelart/ApplyPalette.h: struct ApplyRequest {...}; ApplyResult apply(const PixelImage& image, const ApplyRequest&)
}
```

`ApplyPalette` takes S44's `PixelImage` (RGBA8 plus an optional index plane). If S44 has not
merged when M5 starts, it takes a plain RGBA8 buffer, and adopting `PixelImage` is the one-line
change S44 makes.

### 6.5 Shader sketch (`library:sprite`, `includes/palette.glsl`)

```glsl
// @feature PALETTE_SWAP "Palette Swap"
// @feature PALETTE_CYCLE "Palette Cycling"
// @palette u_palette "Palette" feature=PALETTE_SWAP,PALETTE_CYCLE
// @property paletteRow int 0 "Active palette (0: original)" feature=PALETTE_SWAP instance=fx.x   // S52 R22/R23
// @property paletteCycleSpeed float 1 "Cycling speed" feature=PALETTE_CYCLE

// the binding declares: texture2D u_paletteCells (256x128: 32^3 cells, 4 candidates + 1 each),
// texture2D u_paletteRows (256 x rows: 0 source, 1..15 targets, 16 cycle meta, 17.. cycle orders),
// sampler u_palette_smp (nearest, clamp); rows texel (i, 16): r position + 1 in its ramp, g length,
// b/a the ramp's signed 8.8 steps per second
vec4 paletteSwap(vec4 texel /* raw sRGB-encoded, straight alpha */) {
    ivec3 c = ivec3(texel.rgb * 255.0 + 0.5);
    ivec3 cell = c >> 3;
    ivec4 cand = ivec4(texelFetch(sampler2D(u_paletteCells, u_palette_smp),
                       ivec2(cell.r + 32 * (cell.b & 7), cell.g + 32 * (cell.b >> 3)), 0) * 255.0 + 0.5);
    int index = -1;
    for (int k = 0; k < 4; k++) {               // unrolled; the first match wins
        int i = cand[k] - 1;
        if (i >= 0 && index < 0 && all(equal(ivec3(texelFetch(sampler2D(u_paletteRows, u_palette_smp), ivec2(i, 0), 0).rgb * 255.0 + 0.5), c)))
            index = i;
    }
    if (index < 0) return texel;                // not a palette colour: untouched
    #ifdef PALETTE_CYCLE
        index = paletteCycled(index);           // meta row, floor(time * speed), mod length
    #endif
    vec4 target = texelFetch(sampler2D(u_paletteRows, u_palette_smp), ivec2(index, paletteRowOf()), 0);
    return vec4(target.rgb, texel.a * target.a);
}
```

- **`@palette`** is a new `ShaderManifest` directive, parsed next to `@texture`
  (`engine/core/shader/ShaderManifest.cpp:644`). It tells the editor and RenderSystem to bind a
  `PaletteSwapCache` entry, so forks of `library:sprite` and custom shaders can use palettes too.
- **No binding:** with the feature on but no source, the binding is a 1×1 identity, the sprite
  draws unchanged, and the inspector warns.
- **`paletteRowOf()`** is `customParams.paletteRow`, or the instance's `fx.x` when S52 batches
  the sprite (R17).
- **Uber (`UBER_PALETTE`):**
  - `u_paletteQuantize` (cells) and `u_paletteQuantizeRow`, plus `uber.palette`
    (`x` cells per axis, `y` dither size, `z` 1 when pixelate sets the cell);
  - the Bayer threshold comes from `includes/bayer.glsl` (`bayer2/4/8(ivec2)`, values
    `(m + 0.5) / n²`).

### 6.6 MCP schemas (sketch; final text within §6.1's limits)

```text
edit_palette
  path?        project .palette (absent with no from: list project + built-in palettes)
  from?        {builtin: name} | {file: path} | {lospec: slug_or_url} | {texture: path, max_colors?} | {colors: [hex]}
  ops?         [{op: set|insert|remove|move|sort|rename_color|lock|set_transparent|set_info|
                     add_ramp|set_ramp|remove_ramp|generate_ramp|add_variant|set_variant|remove_variant, ...}]
               (anyObject items; the fields per op are in jasper://docs/palettes)
  export?      {path, format: gpl|hex|pal|pal_riff|txt|ase|png|colors}
  texture?     path: also return usage counts and off-palette colours for this texture
  image?       bool (false): a swatch image (ramps as rows, variants beside)
  output?      bool (false): write; overwrite?: bool (false) for an existing path on create/import
  -> {palette: {name, count, colors: [hex], ramps, variants, locked, transparent},
      changed: [indices], warnings: [...], usage?: {...}, written?: path, undo?: [{scope, name}]}
     errors {kind, message, fix, field}: invalid_value, not_found, exists, too_many_colors,
     format_unsupported, parse_error, network_unavailable, lospec_not_found, bad_response

apply_palette
  path         texture asset
  palette?     .palette path | built-in name | [hex]      (required unless reduce)
  reduce?      2..256: Reduce Colours (with palette: its locked colours are kept)
  distance?    oklab|ciede2000|rgb (oklab); dither?: none|bayer2|bayer4|bayer8|floyd_steinberg (none)
  dither_strength? 0..1 (1); alpha_threshold? 0..255 (0 keeps alpha)
  measure_only? bool: check, never write (exit codes in the CLI, R43)
  output?      false | true (overwrite the texture) | "path" (new file); save_palette?: path (with reduce)
  image?       bool (true): before | after, integer zoom; max_width? (768)
  -> {verdict: clean|fixable|needs_person, stats: {colors_before, colors_after, pixels_changed,
      mean_delta_e, max_delta_e}, usage: [{index, hex, count}], lost: [{hex, mapped, count, box}],
      written?, palette_written?, undo?}
```

Both tools: `allowedWhilePlaying` is true for previews and `measure_only`, and writes are
refused while playing (`play_mode` error), as for other asset writes. Annotations: neither is
`readOnly` (both can write); `destructive` is false (undoable); `idempotent` is true. A Lospec
fetch, or an apply estimated at more than 1 s, runs as a job (`job_id`).

### 6.7 Lua (S23 `tools/lua_api`)

```lua
local p = Palette.load("palettes/hero.palette")
print(p.name, p.count, p:getHex(3))            -- "Hero" 8 "#b13e53"
local i = p:nearest(Color(0.9, 0.3, 0.3))      -- OKLab; p:nearest(c, "ciede2000")
local hot = p:rampColor("cape", 0.8)           -- a step, never blended

hero.palette = "Team Blue"                      -- by row name; hero.palette = 0 restores
hero.paletteRow = 2
hero:setPaletteTarget(3, Palette.new({ "#000000", "#ff00ff" }))  -- runtime row (missing indices keep the source)
water.paletteCycleSpeed = -0.5

scene.postProcessing.paletteEnabled = true
scene.postProcessing.palette = "palettes/gameboy.palette"
scene.postProcessing.paletteDither = PaletteDither.BAYER4
```

`Palette.new` palettes live as long as a Lua reference or a row uses them. Their swap textures
come from the same cache (content hash), within the R29 budgets.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1, R2, R46) | `engine/tests/core/pixelart/PaletteCodecTest.cpp`: the §6.2 example reads and writes byte-identically. Each R2 error gives its `kind` and `field`. An unknown key survives a rewrite. Version 2 loads read-only with the warning. 257 colours is `too_many_colors`. |
| AC2 (R5, R6) | `editor/tests/pixelart/PaletteFormatsTest.cpp`: fixtures for every format (hand-made, plus files exported by GIMP, Aseprite, Paint.NET and Photoshop under `editor/tests/fixtures/palettes/`) import to the expected colour lists. Adobe ASE vs Aseprite `.ase` are told apart by magic. Every writer → reader round-trips identically. RIFF PAL from Windows' own sample matches. A GPL with empty-slot comments keeps the indices. |
| AC3 (R8) | `tools/mcp/tests/test_palettes.py::test_lospec` with `JASPERENGINE_IMPORT_MIRROR`: `endesga-32` creates the palette with its author and `source`; an unknown slug gives `lospec_not_found` with the fix; the mirror with no file and `network.offline` give `network_unavailable` within 1 s; URL and `lospec-palette://` forms normalise. Red control: the same test with the mirror unset and the network blocked by the harness gives the typed error, never a hang (10 s timeout asserted). |
| AC4 (R7, R9) | `test_palettes.py::test_builtins`: the listing has 37 + 1 built-ins; `from: {builtin: "pico-8"}` twice writes one copy and reuses it; every built-in validates; the pixelfx ramps equal `color.js:52-80` value for value (a checked-in JSON extracted once, compared in the test). |
| AC5 (R12) | `engine/tests/core/pixelart/PixelColorTest.cpp`: the 34 CIEDE2000 pairs within 1e-4; OKLab against Ottosson's reference table within 1e-6; sRGB round trip of all 256 bytes is exact; `PaletteMatcher` cached and uncached give the same answers on 2²⁴ colours sampled 1/64; `cbrt` gives bit-identical results under `-O0` and `-O2` and on the web build (a golden hash of 10⁵ conversions). |
| AC6 (R4, R10) | `editor/tests/pixelart/RampGeneratorTest.cpp`: Hue Shift on `#b13e53`, 3 darker and 3 lighter, equals the golden list computed by a direct transcription of `Shading.gd:35-119` in the test. The yellow-to-blue reversal case is covered. `fitRamp`/`rampAt` match pixelfx on 20 cases (both ends kept, no blending). |
| AC7 (R13, R14, D2) | `engine/tests/core/render/PaletteSwapGpuTest.cpp` on glcore, gles3 and vulkan: a 16×16 sprite in 8 palette colours plus 2 off-palette colours, rows 0, 1 and 2. The read-back pixels equal the expected bytes exactly (row 0 identical to the source texture; off-palette pixels untouched; target alpha 0 hides). A tint of white keeps exact bytes. Feature off gives the same pixels as today's golden. |
| AC8 (D3, R14) | `PaletteSwapCacheTest`: every built-in and a checked-in set of 400 Lospec palettes (`editor/tests/fixtures/palettes/lospec-400.json`) bind with no `palette_cell_overflow`; a crafted 5-colour cluster gives the error naming the 5 colours. |
| AC9 (R14, R23) | `PaletteSwapGpuTest.MatchesRemap`: the GPU swap of a 64×64 sprite equals `ApplyPalette` Remap Indices to the target palette, byte for byte. |
| AC10 (R16) | `PaletteSwapGpuTest.Cycling`: a 4-colour ramp with `cycle 4`, at fixed time steps of 0.25 s, shows the 4 rotations in order; speed −1 reverses; 0 freezes; pause (time scale 0) holds. |
| AC11 (R17) | Once S52 lands, `PaletteSwapGpuTest.Batched`: 64 batched sprites with rows 0–3 in one S52 batch (one draw, read from S52's stats) give the same pixels as 64 unbatched sprites. Until then the test is not registered, and S52's spec carries the AC; it is not a disabled test. |
| AC12 (R19–R21, D5) | `engine/tests/core/render/PaletteQuantizeGpuTest.cpp` (glcore, gles3, vulkan): (a) the identity: an image made only of palette colours is unchanged with each dither size; (b) a gradient at fixed resolution 320×180 upscaled 4× has every output pixel in the palette, and the dither pattern is constant within each 4×4 output block; (c) CRT on top still runs (output not in the palette, by design); (d) golden images for None, Bayer 2, 4 and 8 on a test card; (e) `UBER_DITHER` is not compiled with `UBER_PALETTE` (variant reflection). |
| AC13 (R28) | `PostProcessingTiersTest`: no palette → no `UBER_PALETTE`; enabled with no asset → off and a warning. `RenderGpuTest.PaletteOffAllocatesNothing`: GPU allocation counters (S15) are unchanged between a scene before the change and the same scene with the features present but off; no `PaletteSwapCache` entry exists. |
| AC14 (R29, R30, R31) | `tools/perf/palette_bench` on CI: swap ≤ 1.25× GPU time on the 1080p overdraw-2 field; quantize ≤ 0.15 ms; bakes within R30 (32³, 64 colours, dithered) on CI and the web build in headless Chrome (S02 web gate); results recorded in this spec's evidence table. Mid-mobile numbers are measured once on the reference phone and recorded (manual, M7). |
| AC15 (R23, R24, R40) | `tools/mcp/tests/test_apply_palette.py`: preview writes nothing (file hash unchanged); `output: true` writes; `undo` restores the hash; a texture already in the palette gives `clean` and 0 changes; a photo patch gives `needs_person` with lost groups; `reduce: 16` gives ≤ 16 colours and is deterministic over two runs; `save_palette` writes a valid palette; `measure_only` from the CLI exits 0/3/4 on the three fixtures. |
| AC16 (R39) | `test_palettes.py::test_edit_ops`: each op on a scratch palette; preview vs `output`; one undo step per call; a sort remaps ramps and variants (checked by colour, not by index); `generate_ramp` matches AC6; `export` for every format; `texture` usage counts equal a numpy count of the fixture PNG. |
| AC17 (R41, R42) | `tools/mcp/tests/test_palettes_lua.py`: Play a scene whose script sets `hero.palette = "Team Blue"`, then `screenshot`; the hero's pixels equal the variant colours (pixel probe through `sample_view_color`); a wrong name logs the Lua error listing the names; `setPaletteTarget` with `Palette.new`; `scene.postProcessing.paletteEnabled` turns quantize on (probe). `tools/lua_api/tests` find every new binding; the doc's Lua block runs. |
| AC18 (R32–R37) | `tools/mcp/tests/test_palette_window.py` (xvfb, isolated editor): open a palette; drag-reorder (`find_ui_item`), undo; generate a ramp; create a variant; usage badges appear for a selected texture; colour picker Snap to Palette snaps; material inspector Detect fills Source, and the lossy-texture warning and its Fix work. Screenshots in the evidence. |
| AC19 (R44, R45) | `test_runtime_regressions.py`: the fixtures with COLOR_REPLACE, `library:gameboy` and `library:posterize` re-save byte-identically and render as their goldens; a scene with `shaderPalettes` round-trips; renaming the palette asset keeps the reference (S04 redirect). |
| AC20 (§6.1) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`; the `pixel_art` group is hidden by default and listed after `tool_groups` enables it; guide ≤ its limit (`test_protocol.py`). |
| AC21 (R27) | `tools/build.sh web` and `android` build with the runtime palette code; `tools/smoke-test.sh` on a fixture with a swapped sprite and quantize on: the exported game's screenshot equals the editor Play screenshot. |

## 8. Dependencies & conflicts

- **Depends on.**
  - S44's codec for reading Aseprite palettes (M2's last item only).
  - S04 (GUIDs, dependency graph), S05 (hot reload), S06 (jobs), S14 (texture import settings
    for R15), S15 (GPU timings and allocation counters), S24 (undo), S27 (colour picker).
  - None of these is unmerged except S44.
- **Blocks.**
  - S46: palette snapping uses `PaletteMatcher`, `Palette`, `locked` and the lost-colour
    grouping.
  - S47: agents draw with palette indices from `Palette`.
  - S48: ramp recolour uses `rampColors`, `fitRamp`, `rampAt` and `includes/bayer.glsl`.
  - S49: colour over life on a ramp, and the dither include.
  - S52: carries the palette row in FX slot `fx.x` (S52 R23).
  - S42 (wave 5B): its `palette` rule runs `apply_palette`'s measure core, and its
    `mipmaps_compression` rule covers R15's lossy-texture check (S42 R10).
- **Interfaces other specs must honour.**
  - The `.palette` format (§6.2) and `AssetType::Palette`.
  - `engine/core/pixelart/PixelColor.h` and `Palette.h` (§6.4).
  - `includes/bayer.glsl` (`bayer2/4/8(ivec2) → (m+0.5)/n²`).
  - `paletteRow` in S52's `fx.x` (an integer row; 0 is the original colours).
  - Ramp sub-assets `<palette>.palette#ramp:<name>`: an N×1 RGBA8 strip, Nearest (R4a, used by
    S48 R9).
  - `builtin:<id>` names (R7) and the project's Default Palette (R4b, used by S46 and S47).
  - The Aseprite `palette#0` sub-asset as a read-only Palette (S44 R19).
  - `ColorMetric` and `Palette::nearest` (S46 R8).
  - The verdict and exit-code convention of `apply_palette measure_only` (S42).
- **Merge hotspots.**
  - `AssetType.h` and `AssetTypes.cpp` (S44, S45 and S50 add their reserved values 25, 26 and 27;
    never renumber at merge).
  - `RenderSystem.h` (uber bits; S48 may append effects).
  - `uber.frag`, `sprite.frag` (S48 may add sprite effects; keep the effect-order comment
    authoritative).
  - `ShaderManifest.cpp` (`@palette`).
  - `SceneSettings.h` and its field table.
  - `Material.h`/`Stream.cpp`.
  - `McpToolGroups.cpp` (the `pixel_art` row).
  - `McpInstructions.cpp`.
  - `ColorPickerWindow.cpp`.

## 9. Risks & defaults

- **Lossy textures defeat Palette Swap.** Default: the warning plus its Fix (R15) and S42's
  validator. The pixels that do not match stay untouched instead of turning wrong.
- **Lospec's JSON endpoint changes or rate-limits.** It is an undocumented endpoint (Pixelorama
  relies on it too). Default: a `bad_response` error that names the downloadable `.hex`
  workaround. The importers do not depend on the endpoint.
- **The UI is not quantized** (R21). Default: documented. S51's pixel fonts draw in palette
  colours.
- **High precision costs 1 MB and a slower bake.** Default: Normal. High is offered for
  palettes over 64 colours with close shades.
- **CMYK in Adobe ASE** is converted naively, with a warning. Default: accepted (rare for
  pixel-art palettes).
- **Palette size per swap source is capped at 255** (one index is reserved for "none"). Default:
  accepted. Pixel-art palettes are ≤ 64 colours in practice.
- **Exact duplicate colours** make a swap ambiguous. Default: the first index wins, with a
  warning in the inspector and the codec.
- **Licensing of classic palettes.** Owner decision: copy freely, with credit (`author`,
  `NOTICE.md`).

## Milestones

- **M1** `PixelColor`, `Palette`, `PaletteCodec`, `AssetType::Palette`, the ramp helpers (R1–R4,
  R12, R46, R47); AC1, AC5, AC6 (helpers).
- **M2** Formats, built-ins, Lospec (R5–R9); AC2–AC4. The Aseprite reader is added with S44's codec
  (same wave: by S43 if S44 has merged, else by S44).
- **M3** The Palette window, ramp generator, usage counts, colour picker, Project window (R10,
  R11, R32–R34, R37); AC6, AC18 (part).
- **M4** Palette Swap and Cycling, `@palette`, the material inspector, Lua (R13–R18, R35, R42,
  R45); AC7–AC11, AC17 (swap), AC19.
- **M5** Apply Palette / Reduce Colours and the dialog (R23, R24); AC15.
- **M6** Palette Quantize, presets, the post inspector (R19–R22, R36); AC12, AC13, AC17 (post).
- **M7** MCP tools, guide, docs, budgets, performance, export (R38–R41, R43, R28–R31, R27);
  AC14, AC16, AC20, AC21.

## Deviations

Recorded while implementing (`feature/s43-palettes`).

- **Two linear conventions.** The colour science (OKLab, CIELAB) decodes sRGB with the piecewise
  IEC curve, as R12 says. The engine's linear colours are gamma 2.2 (`util/Color.h`,
  `includes/srgb.glsl`), so everything that hands a palette colour to the renderer or to Lua as
  "linear" (`p:getColor`, `p:rampColor`, the SwatchLibrary adapter, `Palette.new` from a
  `Vector4`) uses that convention through `pixelart::toEngineLinear`/`fromEngineLinear`: a palette
  colour then draws as its exact byte. `ColorMath.h` keeps the picker's gamma-2.2 sRGB (the
  piecewise curve there would make the picker's hex disagree with the drawn byte); its HSV is the
  library's.
- **Determinism.** Powers and roots are fixed Newton iterations and the pixel-art sources build
  without floating-point contraction. CIEDE2000 and OKLCh hues use the C library's trigonometry, so
  only those may differ in the last bit between C libraries (the default OKLab metric does not).
  AC5's `-O0` build is checked against the optimised one and a golden hash on Linux x86-64.
- **CIEDE2000 at half a turn.** Sharma's pair 10 lies exactly on the formula's hue discontinuity;
  with `Lab` in floats the hue difference lands a few ulps past 180°. The boundary tests allow
  1e-6°, which gives all 34 published values.
- **Ramp sub-assets.** A palette is a sub-asset container (`hasSubAssets`, its ramps listed in its
  `.meta` as `ramp:<name>`), so `hero.palette#ramp:fire` resolves by GUID like a model's parts.
- **Palette Swap textures.** The rows texture is always 256 x (16 + 1 + cycling ramps): rows past
  the last defined one repeat it, so the shader's clamp to 0-15 is the clamp to rows - 1; row 16
  is the cycle table (an index's position + 1, its ramp's length, its order row); each cycling
  ramp's speed is a signed 8.8 number at x = 255 of its order row. A source slot of alpha 0 is never
  matched (an opaque black must not take a `#00000000` slot's place); Remap Indices follows the
  same rule. Without a source the binding is a shared full-size empty table, not 1 x 1 (a 1 x 1
  texture would be fetched out of bounds).
- **Palette targets** are stored as `{palette: AssetRef, variant}` (YAML `targets: [{palette,
  paletteGuid, variant}]`) rather than one `"<palette>#<variant>"` path, so a rename keeps the
  variant; the text form stays for Lua, MCP and the inspector.
- **Material layout.** `Material::shaderPalettes` is held out of line (`OutOfLineValue`, one
  pointer): S11's size budget of a Material (16 of them inline per MeshComponent) leaves room for
  one pointer only.
- **`paletteRow`** is declared `@property paletteRow int 0` over a `float` member (the
  convention of every `int` property: a GL upload of mixed int and float members is wrong).
  `instance=fx.x` (S52 R22/R23) is not written: the directive is S52's, and S52 adds it to this
  line when it lands.
- **Palette Cycling's clock** is `customParams.time`, the frame's shader clock (as HUE_SHIFT's): it
  is the wall clock, pinned or fixed-step for captures, and does not stop at a time scale of 0.
  Freezing is `paletteCycleSpeed = 0`. AC10's "pause holds" is checked as "a held clock holds".
- **Palette Quantize** draws nothing different until its first table is baked (`uber.palette.w`
  0: the pass leaves the colour as it is); later changes keep the previous table until the new one
  is done. Two palette colours in one cell of the grid keep only the first one's identity, with the
  warning `palette_cell_shared` (High precision separates colours 4/255 apart).
- **What an export ships.** The exporter copies every file under `Assets/` (as for particle
  effects and LUTs); a palette referenced only from Lua (`Palette.load("...")`) could not be found
  by the dependency graph. Built-ins never ship unless used: using one copies it into the project
  (R7), and the runtime never reads editor resources.
- **Error kinds.** A Lospec 404 is `lospec_not_found` everywhere (R8 said `not_found`, §6.6
  `lospec_not_found`). With `JASPERENGINE_IMPORT_MIRROR` naming a directory that does not exist the
  fetch is `network_unavailable` (AC3's offline control); a missing file in an existing mirror is a
  404.
- **Built-in data.** Commodore 64 is Pepto's 2001 palette (VICE's "pepto"); ZX Spectrum has 15
  colours (bright black is black); EGA is the full 64-colour palette; "pixelfx: Effects" holds 203
  colours (the 220 entries before exact duplicates share an index).
- **AC2 fixtures** reproduce the writers' layouts (GIMP 2.10, Aseprite and Pixelorama `.gpl`,
  Paint.NET's `.txt` header, a Photoshop-style ASE with a group, Windows' RIFF PAL) and were built
  by hand from the published formats: no GIMP, Paint.NET, Photoshop or Windows sample was
  available. The Lospec downloads of Endesga 32 in every format are real files.
- **Formats.** GPL export writes no alpha (a fully transparent colour becomes Pixelorama's empty
  slot); ASE export keeps a ramp as a group only when it is a run of consecutive colours (others are
  dropped with a warning); ASE's Lab colours are read too (D50 through Bradford, with a warning).
- **Hue Shift** writes each step to 8 bits as Godot's `Image::set_pixel` does (truncation) and
  keeps the base's alpha. OKLCh (left open by R10): L falls evenly to Darkest L and rises evenly to
  Lightest L, the hue turns toward 60°/240° the short way and stops there, the chroma follows the
  parabola relative to the base's, and an out-of-gamut colour loses chroma at constant L and hue.
- **Lost colours** follow ritmo's grouping (a lost pixel joins an 8-connected group when it is
  within 0.06 OKLab of the group's mean; groups under 2 px are not listed), not "the same original
  colour", which gives no groups on a photo. ΔE and lost colours are measured per pixel also when
  dithering.
- **Aseprite palette import** (`.ase`/`.aseprite`, magic 0xA5E0) is recognised and refused with
  `format_unsupported` ("Export the palette from Aseprite as .gpl or .hex, then import that"): S44's
  codec is not on `main` yet, and S43 does not write a second Aseprite parser. The R5 Aseprite row
  and its test go to whichever of S43 and S44 lands second.
- **The guide.** Per the upstream MCP rules (ledger): no `## Pixel art` digest line; the palette
  guidance lives in `docs/mcp/guide/palettes.md`, and the area gets its "Other areas" entry.
- **Shortcuts.** The Palette window's Delete, Ctrl+D, Alt+←/→ and the Apply dialog's Space are catalog
  commands (`palette.delete`, `palette.duplicate`, `palette.moveLeft`, `palette.moveRight`,
  `palette.flickerCompare`) in a new `ShortcutContext::Palette` (in the Editor context they would collide
  with Edit ▸ Delete / Duplicate).
- **Undo merging** of picker drags uses `WriteFilesCmd` merge keys (as the physics material Inspector) rather
  than `InteractiveEdit`; Esc restores the starting value under the same key.
- **The Apply Palette dialog** is one window with an Apply Palette / Reduce Colours switch. Apply rewrites a
  `.png` in place; a BMP, TGA or GIF texture is written with Save As as a new PNG (the encoder writes PNG), and
  `apply_palette` `output: true` on one is `format_unsupported` with the fix `output: "<stem>.png"`.
- **Sort into a New Palette** writes `<name> Sorted.palette` beside the palette and opens it.
- **Default Palette** lives on a new Project Settings page, **Pixel Art** (R4b's "Pixel Art ▸ Default Palette";
  there is no 2D page); S42's rows join it. A built-in chosen there is copied into the project.
- **The offline setting** is the preference `preferences.network.offline` (Preferences ▸ General ▸ Network ▸
  Offline): every preference key starts with `preferences.`.
- **Detect** on ticking Palette Swap fills an empty Source as its own undo step (the texture is read on a
  worker). The lossy-texture check (R15) also counts a sampler that is not Nearest; its Fix writes the import
  settings and the sampler as two steps linked into one undo.
- **Presets** `handheld_4` ("Handheld (4 Shades)") and `pico_8` hold `builtin:<id>`; applying one stores the
  project copy's path, and a scene holding the copy still reads as that preset.
- **The `pixel_art` group row** says "Pixel art: palettes and Apply Palette (jasper://docs/palettes).": no spec
  has written `jasper://docs/pixel-art` and the validator, cleanup and effect tools do not exist yet; the first
  of S42/S46/S48 to land rewrites it.
- **CLI exit codes (R43).** A successful result's integer `structuredContent.cli_exit` (1-125) becomes the exit
  code of `jasperengine-editor call`; `apply_palette` sets it with `measure_only` (3 fixable, 4 needs_person).
  `exit_code` was taken (`build_project`'s game exit code). 3 and 4 are also the CLI's `connection` and
  `internal` codes: the `--json` envelope says `code: "tool_exit"` for a tool's own code (`docs/mcp/cli.md`).
- **`apply_palette`**: `image` defaults to false with `measure_only` (a check wants the verdict);
  `save_palette` needs `reduce` and is written only with `output`; built-ins are read without copying them
  (nothing stores the reference). `edit_palette`'s `written` is a list (the palette and an export).
- **`undo` in the results** of `edit_palette` and `apply_palette` is the server's `{id, name}` of the one project step
  (S24's attribution names an edit's step so, and replaces a tool's own `undo`), not §6.6's `[{scope, name}]`; the
  tools write the same shape so that a job's result (`get_job`) agrees with the reply. The text says `scope project`.
- **`shaderPalettes` through `set_properties`** takes the material's scene-file encoding: assets-relative paths
  (both tools return `asset_path`), no `builtin:` alias there.
- **Lospec waits** up to 30 s for its job (the request is bounded by 10 s); AC3's red control blocks the
  network with an unroutable proxy (`https_proxy=http://127.0.0.1:9`), the harness having no network block.
- **`get_entity`** returns `palette_rows: [{submesh, rows, row}]` and the R15 warning in `warnings`
  (`palette_swap_lossy_texture`).
- **The dither ratio's steps.** R20 bakes the ratio in 1/16 steps; with that, Bayer 8×8 draws exactly what
  4×4 draws (the third level of an 8×8 threshold never changes a comparison against a multiple of 1/16) and
  2×2 can only show 5 of the 17 levels. The ratio is in steps of the chosen matrix's cells instead (4, 16 or
  64): `PaletteMatcher::ditherPair(color, strength, steps = 16)` (16 by default, the spec's API), Palette
  Quantize's table and Apply Palette's Bayer dithers. `PaletteQuantizeGpu.GradientAtFixedResolution`
  checks the four dithers draw four different images.

- **AC14** is met for the CPU side only: `PaletteSwapCache.BuildTime` and `PaletteQuantize.BakeTimes` print the
  swap-texture build and the quantize bake (one thread and the job system). This host has no GPU (Mesa
  llvmpipe/lavapipe), so the GPU budgets (swap ≤ 1.25×, quantize ≤ 0.15 ms) and the headless-Chrome bake time
  are not measured and no `palette` case is in `tools/perf/gpu_campaign.py` yet: a follow-up for a GPU host.
- **AC21** ran as `SMOKE_METHOD=compile tools/smoke-test.sh` on a scratch project an MCP script made (three
  library:sprite heroes, Palette Swap rows chosen by a Lua script in Play, Palette Quantize on PICO-8 with
  Bayer 4×4), not on a committed fixture. The exported game's 1280×720 frame equals the editor's Play frame
  at a 1280×720 Game view pixel for pixel. Without the scene's fixed resolution: in the editor's Play Game
  view the scene's Fixed Resolution does not reduce the render size (a 320×180 fixed resolution still dithers
  per screen pixel, where the exported game dithers per 4×4 art pixel), which is the editor's fixed-resolution
  path, not this spec's; recorded for S41/S42.
- **AC19** has no golden test of its own: goldens rendered by this branch would not show that old content
  renders as before. R44 rests on the code: every sprite.frag addition sits under `PALETTE_SWAP` /
  `PALETTE_CYCLE` (and `includes/palette.glsl` declares nothing without them), the gameboy and posterize
  changes are description text, `UBER_PALETTE` is the appended bit 18; the existing Shader*, PostProcessing*,
  Uber* and Stream* suites pass. The `shaderPalettes` round trip and the rename redirect are
  `test_palettes_lua.py::test_scene_keeps_the_binding`.
