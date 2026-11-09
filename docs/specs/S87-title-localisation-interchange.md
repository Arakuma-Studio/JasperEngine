# S87 — Title localisation and interchange: languages, translations and per-language bakes; Pachislop, Photoshop and SVG import; PNG, PSD, SVG and sheet export

**Wave / Depends on.** Title Art program (spec-first), **wave 6C** (with S84, S85; program graph in
§8, program rules in S80 §6.15). Depends on:
- **S80**: the `.titleart` document v1 (§6.1), `TitleArtOps` and the path grammar (§6.2), the
  renderer, `TitleFrame`, Outputs and bakes, float readback, the `title_art` MCP tools;
- **S81**: the Title Art Renderer, Render Mode, the Lua `TitleArt` API, the bake sidecar
  `logo.titleart.bake.json` and its always-in-core reader `TitleArtBakeInfo`, Realtime re-render,
  Glyph Atlas, export (font subsetting, Characters set);
- **S82**: the Title Designer (toolbar, panels, Library panel, font picker);
- **S83**: the effect and generator ids, the `material` effect, and the Pachislop map
  (`editor/titleart/fx/PachislopFxMap.cpp`, data `editor/resources/titleart/pachislop-fx-map.yaml`);
- **S84**: user Looks (`.titlelook`, the library ops), the font pack ids, the library maps;
- **S85** (same wave; S85 owns them): `TitleAnimPlan` and frame sampling, its baked flipbook pages
  and `<file>.anim.json`;
- **S86**: `TitleDocument3D::fromPachislop` for Pachislop `title3d` layers.

The milestones (end of the spec) let parts land as their dependencies do: localisation needs S80
and S81, export needs S80, Pachislop import needs S83 and S84.

Owner area:
- `engine/core/util/TitleArtLanguage.*` (always in core, beside S81's `TitleArtBakeInfo`);
  `engine/core/titleart/TitleTranslations.*`, `TitleFit.*`;
- `editor/titleart/localisation/`: strings CSV/XLIFF, the language checks, the Translations panel
  model;
- `editor/titleart/interchange/`: `pachislop/`, `psd/`, `svg/`, `png/`, `sheet/`, `IccSrgb.*`;
- `editor/window/TitleDesignerLanguages.cpp`, `TitleDesignerTranslations.cpp`,
  `TitleArtImportDialog.cpp`, `TitleArtExportDialog.cpp`;
- the `import-title-art` and `export-title-art` subcommands (`editor/cli/TitleArtCli.*`);
- `tools/mcp/psdread.py`, `tools/titleart/make_pachislop_fixtures.py`, `make_psd_fixtures.py`;
- the Languages and Import/Export sections of `docs/mcp/guide/title-art.md` (`jasper://docs/title-art`)
  and of the Title Art manual.

## 1. Goal

A title made once ships in every language of the game. A German title that is twice as long as the
English one still fits its box. A Japanese title can switch to vertical text and a Japanese font.
The game picks the language from the device, the project default, or a Lua override. Baked titles
switch to their per-language PNG with no frame left blank. Realtime and Glyph Atlas titles re-shape
the text. Translators work in CSV or XLIFF in their own tools, and `check_title` reports every
missing translation and every overflow, per language.

The same documents move in and out of other tools without losing work:
- existing Pachislop projects (v4–v11) open as `.titleart`;
- Photoshop layer styles (`.asl`, `.psd`) become Looks;
- layered PSDs and SVGs become layers.

Exports go to whatever the next tool needs:
- PNG (8/16-bit, trimmed, with no dark fringes);
- layered PSD;
- SVG outlines;
- sprite sheets, atlases and sequences of S85 animations;
- KTX2 through the existing cook.

All exports are sRGB, tagged as sRGB.

## 2. Scope

**In scope**
- Per-document languages; per-layer translations with per-language font, size, layout and fit
  overrides; per-layer language visibility; auto-fit to a box (shrink, condense, wrap).
- Language resolution at runtime (RFC 4647 lookup with script rules), the project default, the
  global and per-component Lua override, events.
- Per-language bakes (a full bake set per language, listed in the source's bake sidecar) and the
  Baked switch.
  Realtime and Glyph Atlas re-shaping on a language change.
- Language checks in `check_title`, per-language sheets in `render_title`, and the language ops of
  `edit_title`.
- Strings exchange: CSV (RFC 4180, Unity Localization-style headers accepted) and XLIFF 1.2
  (export, import) and 2.0 (import).
- Import:
  - Pachislop `.pslot.json` v4–v11 (its migrations ported);
  - Photoshop layer styles from `.asl` and `.psd` (port of `src/io/psStyle.ts`, with fixes);
  - layered PSD into a document;
  - SVG into Vector layers, optionally split.
- Export:
  - PNG 8/16-bit (trim, padding, rounding, alpha threshold, straight with bleed or
    premultiplied), with per-layer parts and a placement manifest;
  - layered PSD 8/16-bit (a C++ writer);
  - SVG for vector-representable content;
  - grid sheets, packed atlases and PNG sequences of S85 animations;
  - KTX2 through S14's cooker;
  - ZIP bundles.
- Colour management of exports: sRGB with an sRGB tag (PNG `sRGB` chunk, PSD embedded ICC
  profile), and a generated sRGB ICC profile.
- The `import-title-art` and `export-title-art` CLI subcommands, the editor UI, Lua, MCP
  parameters, docs and tests.

**Out of scope**
- A project-wide localisation system: string tables for UI text, plurals, smart strings,
  localised audio. D3 fixes the keys and files such a system will reuse.
- Machine translation inside the engine. Agents translate with their own model and write through
  ops (R60).
- Live PSD text layers, editable Photoshop layer styles in exported PSDs, CMYK, print and bleed,
  soft proofing, wide-gamut (Display P3) exports. The Japan Color 2011 ICC profile is never taken
  (contract).
- `.psb` export, 32-bit PSD, WebP/AVIF/EXR exports, GIF/APNG/video exports (the Movie Recorder,
  S27, records Play), Lottie.
- Pachislop v1–v3 files (Pachislop itself refuses them), its autosave records, its user preset
  files (`src/presets/user.ts`; S84 converts looks).
- Runtime parsing of CSV, XLIFF, PSD, SVG or Pachislop files. Everything here except language
  resolution, translations, fit and reading the bake sidecar is editor-only.

## 3. Current state (`feature/title-art` 3ffafdfaa, read)

**Language queries, no localisation.**
- `System::getPreferredLocales()`, `getLocale()`, `getLanguageCode()`, `getRegionCode()` and
  `getSystemLanguage()` (`engine/core/System.h:299-311`, `System.cpp:178-205`) are bound to Lua
  (`engine/core/script/binding/CoreClassesLua.cpp:3779`) and documented
  (`docs/mcp/guide/platform-services.md:27-43`).
- `LocaleRules` (`engine/core/util/PlatformRules.h:22-45`) normalises platform names to BCP 47
  (`"zh_hant_tw"` → `"zh-Hant-TW"`) and splits language, script and region.
- The platforms fill the preferred list:
  - Linux: `LC_ALL`/`LC_MESSAGES`/`LANG`/`LANGUAGE`, `engine/platform/linux/HostServicesLinux.cpp:108-115`;
  - Android: `JasperEngineAndroid.cpp:505`;
  - the web, Apple and Windows: their host services.
- The editor simulates a language in Play: the Game view's Device > Language, or
  `configure_game_view {language}` (`editor/mcp/tools/ViewTools.cpp:1753`), returned by
  `editor::Platform::getPreferredLocales` (`editor/Platform.cpp:275-279`).
- Nothing translates anything. No string table, no `.po`/`.csv`/`.xliff` reader, no
  `localization` setting. `docs/openengine-analysis/06-ui-framework.md:147` lists "no
  localization" as a gap. The only text-related language input is `ShapeOptions::language`
  (`engine/core/util/TextShaper.h:28`), the BCP 47 tag HarfBuzz uses for `locl` forms. S80's
  text layer `language` member feeds it.
- S80 reserves the top-level `.titleart` members `languages` and `variants` for this spec
  (S80 §6.1, "Reserved top-level names").

**Images out.**
- `editor/util/PngWriter.h` has `writePng` (8-bit via `stb_image_write`) and `encodePng16` (16-bit,
  1–4 channels, stb's zlib, adaptive filters, single-threaded). Neither writes colour chunks.
- `editor/texsynth/SynthesisRecipe.h:104` inserts `jasper.*` iTXt recipe chunks.
  `editor/util/PngEditorChunks.h` strips every `jasper.*` text chunk when a game is exported, so a
  game can never read them.
- miniz is vendored (`libs/miniz`, `editor/util/ZipArchive.h`: reader and writer).
  `stb_rect_pack.h` is vendored (`engine/libs/stb/`) and unused by the editor.
- No PSD writer and no PSD layer reader. `stb_image` reads a PSD's composite only, so `.psd` is a
  Texture type (`editor/asset/AssetTypes.cpp:29`). No `.asl` reader. `8BPS`/`lfx2` appear nowhere
  in `editor/` or `engine/core/`.
- SVG: LunaSVG with plutovg is vendored (`engine/libs/lunasvg`). Its `Element::getAttribute`,
  `getGlobalMatrix` (`lunasvg.h:593`, :628) and `plutovg_path_parse` (`plutovg.h:730`) give path
  geometry. ThorVG's SVG loader is off (`engine/libs/thorvg/config/config.h:1-3`). No SVG writer
  exists.
- Alpha:
  - `bleedAlphaBorder` (`engine/core/texture/MipChain.h:65`, 4 rings) runs in the cook when
    `alphaIsTransparency` is set (`engine/core/texture/TexturePrepare.cpp:104`), so cooked
    sprites have no fringes;
  - a standalone exported PNG has nothing similar;
  - KTX2 carries a premultiplied flag (`engine/core/texture/Ktx2.h:50`), and materials have
    `MaterialBlendMode::PREMULTIPLIED` (`engine/core/texture/Material.h:39`).
- `TextureCooker::cook` (`editor/texture/TextureCooker.h:61`) turns a source plus resolved settings
  into KTX2 for a `TextureFamily`.
- Editor outputs that are not assets go to `Recordings/`, one of the project's non-asset
  directories (`editor/Project.cpp:5214`, S27 R31).

**Tests.**
- `tools/mcp/pngdiff.py` is a standard-library PNG decoder and comparer for 8-bit only. It compares
  straight channels, so the colour of invisible pixels counts.
- `engine/tests/support/ImageCompare.h` compares against goldens with thresholds.

**Settings and export.**
- Project settings are rows of `editor/settings/SettingsRows.cpp` (keys like
  `player.target_frame_rate`, read and written by `get/set_project_settings`,
  `editor/mcp/tools/ProjectTools.cpp`).
- A start value reaches a game through the generated main (`editor/Generator.cpp:2622`) or the
  prebuilt player package (`editor/ExporterPrebuilt.cpp:1106`).
- `copyAssets` decides what ships (`editor/Exporter.cpp:2806-2810`).

**Pachislop** (`/tmp/pachislop-analysis/repo`, `1c32502`; report p6 §3–4):
- `src/io/project.ts` reads v4–v11 `.pslot.json` (base64 fonts and assets) and migrates in one
  function (`migrateProject`) plus four steps (`src/io/migrate/`) and an asynchronous v4 glyph-scale
  compensation (`src/io/migrateText.ts`).
- `src/io/psStyle.ts` maps Photoshop styles to 2D presets through ag-psd internals (with a
  monkey-patch for Shape Burst gradients). It ignores gradient transparency, approximates
  midpoints with extra stops, draws strokes opaque and skips bitmap patterns.
- `src/io/psd.ts` writes layered 8-bit PSDs through ag-psd, with styles baked.
- `src/io/export.ts`:
  - writes PNGs with RGB zeroed under alpha 0 (dark fringes under bilinear filtering and mips,
    report p6 §3.7);
  - writes an `sRGB` chunk and no profile;
  - writes face-only SVG without the layer transform.
- It has no localisation. Its fit is manual.

## 4. Reference

### 4.1 Pachislop: what is ported and what is improved

The owner has the author's permission to port (contract). Ported files keep a header naming
Pachislop (SafEight) and the source file. `NOTICE.md` gets the program's Pachislop entry (S80)
and the ag-psd entry below.

| Pachislop | Take | Improve |
|---|---|---|
| `src/io/project.ts` (`parseProjectFile`, `migrateProject`, `migrateLayer`, `migrateStyle2D/3D`, `migrateEffects`, `migrateDeepGlow`, `referencedIds`, `safeFileName`) | Format checks, version gates and messages; every fill-only migration, ported literally onto `nlohmann::json` | Run as one pass to v11, kept as Pachislop structures it (its unit tests define the behaviour), followed by one mapper v11 → `.titleart` (§6.8). Fonts and assets become project files, not base64 |
| `src/io/migrate/{generatorParams,effectDedupe,materialEffects,contentEffects,bands}.ts` | The four ordered v7 steps and the v11 bands conversion, literally | None: their maths is documented in their headers and is kept |
| `src/io/migrateText.ts` | The v4 → v5 per-character scale compensation | Font metrics from `TextShaper` with the imported fonts, synchronously inside the import job |
| `tests/unit/project.test.ts`, `contentEffects.test.ts`, `effectDedupe.test.ts`, `generatorParams.test.ts`, `materialEffects.test.ts`, `outwardBands.test.ts` | Their cases, as gtests over constructed JSON | Plus golden imports into `.titleart` (AC15) |
| `src/io/psStyle.ts` | The whole mapping (§6.9), the EN/JA gloss and pattern name tables, colour conversions (Lab D50 → sRGB, HSB, naive CMYK), stroke reach ordering, the summary/notes report | Gradient opacity stops kept as stop `alpha`; midpoints kept as `mid`; stroke opacity kept as rim material `opacity`; bitmap patterns extracted as `custom` patterns; multi-effect (`lmfx`) arrays; Shape Burst decoded natively (no monkey-patch) |
| `src/io/psd.ts` (`titleParts`, `siblingsPlan`, `nodePlan`, `stackPlan`, `exportPsd`, `resolutionInfo`, `PSD_BLEND`, `psdAdjustment` for the types below) | The layer plan: every layer rendered alone on the shared frame; groups with section dividers; clipping masked by the base's pre-effect alpha; raster layer masks; split titles (rim N, face, fx); adjustment layers native or "(baked)"; the warnings | A C++ writer (ag-psd is JavaScript); 16-bit (`Lr16`, ZIP with prediction); an embedded sRGB ICC profile; byte-deterministic output |
| `src/io/export.ts` (`Frame`, `alphaBounds`, `roundFrame`, `contentFrame`, trim on an enlarged canvas, full-bleed detection, `DEFAULT_TRIM_ALPHA` 8, the band-parallel PNG with Sub filter and combined Adler-32, `exportLayerParts`, `exportSvg`) | Frame model, trim rules, PNG band design, parts naming | Colour bleed under transparent pixels (or premultiplied); `gAMA`/`cHRM` beside `sRGB`; no Huffman patching (miniz `TDEFL_SYNC_FLUSH`); SVG with transforms, rims as strokes, gradients, masks and clips |
| `src/io/pngChunk.ts` | `writeChunk`, `adler32Combine` | None |

### 4.2 Third-party code and format references

| Source | Licence | Use |
|---|---|---|
| ag-psd 33.0.0 (Agamnentzar; Pachislop's `package-lock.json:858-862` says MIT) `src/descriptor.ts` (`readVersionAndDescriptor`, `parseEffects`, the OSType item readers, the `GrdT` enum), `src/engineData.ts` (the text EngineData parser), `src/psdReader.ts` (`readLayerInfo`, section dividers, RLE/ZIP channel decode) | MIT | **Ported** to `editor/titleart/interchange/psd/PsdDescriptor.cpp`, `PsdEngineData.cpp` and `PsdReader.cpp`, keeping the ag-psd copyright header. One `NOTICE.md` entry. The implementer checks the LICENSE file at the pinned tag before porting |
| Adobe Photoshop File Formats Specification (public document) | — | Section layout, `Lr16`, `lsct`, `luni`, `lyid`, `iOpa`, `knko`, adjustment records, image resources 0x03ED, 0x040D, 0x040F, 0x0419, 0x0421, patterns (`Patt`) |
| PNG Specification, 3rd edition (W3C) | — | `sRGB`, `gAMA`, `cHRM` values for sRGB writers; iCCP and sRGB not both present |
| ICC.1:2022 and IEC 61966-2-1 | — | The generated sRGB v2 profile (§6.10) |
| XLIFF 1.2 (OASIS) and XLIFF 2.0 core | — | §6.6 |
| RFC 4180 (CSV), BCP 47 / RFC 4647 (language tags, lookup) | — | §6.6, D4 |

No library is added. tinyxml2 (`engine/libs/tinyxml2`) parses and writes XLIFF. nlohmann JSON
(`engine/libs/json`) parses Pachislop files. LunaSVG/plutovg read SVG. miniz deflates.
`stb_rect_pack` packs atlases.

### 4.3 Engine precedents copied

- `LocaleRules` for normalisation, and the editor's simulated language for Play.
- `SynthesisRecipe` chunks for export recipes.
- `PngEditorChunks` stripping for shipped PNGs.
- The VAT sibling-file pattern and S80's output naming (`logo.titleart.png`) for per-language
  bakes.
- S81's bake sidecar and S85's `<file>.anim.json`, reused unchanged per language.
- `WriteFilesCmd` for imports that create files.
- `bake-vat` / `generate-sprite-normals` (`editor/main.cpp:101-109`) for subcommand shape.
- `call --project` (S31) for one-shot headless editors.

## 5. Requirements

### Functional — languages and translations

- **R1. Languages of a document.**
  - `source_language` is the language of the base text, BCP 47. Empty means the document is
    monolingual.
  - `languages` is a list of translation languages. Each has a `code`, a display `name`, a default
    `font` and `fallback_fonts` for every text layer of that language, `characters` (extra
    characters for runtime text, added to S81's Characters set), and `bake` (true).
  - Codes are stored in BCP 47 canonical case (`zh-Hant-TW`, `pt-BR`) after
    `LocaleRules::normalize`. They are unique and never equal `source_language`.
  - At most 64 languages.
  - A document with an empty `source_language` and no `languages` is monolingual. It renders in
    every language exactly as S80 v1 renders it, and every R2–R27 code path is skipped (R50).
- **R2. Translations of a text layer** (`translations: {code: Translation}`).
  - A Translation always has `text`, `status` (`translated` | `needs_review`) and `source_hash`.
  - Optionally it overrides `font`, `face_index`, `fallback_fonts`, `language` (shaping), `size_scale`,
    `tracking`, `line_height`, `h_scale`, `v_scale`, `vertical`, `align`, `features`,
    `variations`, `chars`, `lines`, `offset` and `fit`.
  - An absent override inherits. A present one wins (§6.1).
  - A Translation with no `text` member does not exist: the language falls back to the source text
    (R4) and R18 reports it.
- **R3. Text layer strings metadata.**
  - `text_key` is the layer's stable id in string files. `[a-z0-9_.-]{1,64}`, unique in the
    document, made from the layer name on the first `add_language` or `text_key` read, and kept
    when the layer is renamed.
  - `translate` (true). False marks a brand word that is never translated; checks skip it.
  - `max_chars` (0 = none) and `translator_note` are hints for translators.
  - The document has a `string_namespace` (made from the file stem on first use, kept on rename).
    The global key is `<string_namespace>.<text_key>`.
- **R4. Resolution of a layer in a language** (`resolveTranslation(layer, doc, code) ->
  ResolvedText`):
  - Each member comes from the first of:
    1. the layer's Translation for `code`;
    2. for `font` and `fallback_fonts` only, the document's Language entry for `code`;
    3. the layer's own member.
  - `size` is the layer's `size` × `size_scale`.
  - The shaping `language` is the Translation's override, else `code`. HarfBuzz then picks
    language-specific forms (`locl`: Japanese against Chinese CJK glyphs, Turkish i, Serbian
    italics).
  - Rendering the source language uses the layer as stored, with shaping language = the layer's
    `language` or else `source_language`.
- **R5. Image and Vector layers** may override their asset per language
  (`translations: {code: {image}}` or `{svg}`), for a localised logo picture. The frame, size and
  style stay shared.
- **R6. Per-layer language visibility.**
  - Every layer has `only_languages: [codes]` (default `[]` = every language).
  - A non-empty list hides the layer in every other language, the source included unless its code
    is listed. An example is a katakana reading shown only in Japanese.
  - Hidden layers are excluded from fit, frames, checks and bakes of those languages.
- **R7. Exposed text parameters** (S80 `exposed`, S81 instances).
  - An instance's text override replaces the text in every language. The active language still
    supplies font, layout and fit, and the shaping language.
  - `setLanguage` never clears an instance override.

### Functional — fit

- **R8. Fit members** of a text layer (also overridable per Translation):
  - `fit {mode: none | shrink | condense | wrap, box: [w, h] px (layer space, centred on the
    text block's anchor, moves, turns and scales with the layer), min_scale: 0.6, min_h_scale:
    0.8, max_lines: 2}`;
  - `box: [0, 0]` turns fit off whatever the mode.
  - Default `mode` is `none`. `add_language` sets `shrink` on text layers whose `fit.box` is
    `[0, 0]` and fills `box` with the source's measured extent + 10 % (R9), so a new translation
    fits by default.
- **R9. What fits.** The measured extent is:
  - the ink box of the glyph outlines after warp, distort and weight;
  - grown on each side by the sum of the widths of the enabled rims plus each rim's `offset`;
  - effects (glow, shadow) are excluded and may bleed outside the box.

  Growing the ink box by the rim widths is exact for round and bevel joins. Miter spikes may
  exceed it; a 2 % tolerance absorbs them. The extent is computed from shaping and outline bounds
  alone, without rendering.
- **R10. Algorithm** (`engine/core/titleart/TitleFit.{h,cpp}`, deterministic):
  - `shrink`: the largest uniform scale s ∈ [min_scale, 1] with extent(s) ≤ box. Binary search
    to 1/256, at most 9 measures. Style units scale with it (S80 `%` units), so the look is kept.
  - `condense`: first `h_scale` × c with c ∈ [min_h_scale, 1], then `shrink`.
  - `wrap`: line breaks are chosen so the longest line is as short as possible, for 1 to
    `max_lines` lines, with the fewest lines that fit; then `shrink`. Break opportunities:
    - spaces (U+0020, U+3000) and existing `\n`;
    - between two CJK ideographs or kana, except before `、。，．・：；？！ー）」』】〕〉》”’`
      and small kana (`ぁぃぅぇぉっゃゅょゎァィゥェォッャュョヮヵヶ`), and except after
      `（「『【〔〈《“‘`.
  - Vertical text measures the column length against `box[1]`.
  - The result `{scale, h_scale, lines, fits, overflow_px}` is cached per (layer, language,
    resolved-text hash) and reported by `get_title` and the checks.
- **R11. No fit without a box.** `fit.mode` other than `none` with `box: [0, 0]` is a warning
  (`fit_without_box`), not an error. The text renders unfitted.

### Functional — selecting a language at runtime

- **R12. Candidates, in order.**
  1. The component's `language` (Inspector **Language**, `""` = Auto).
  2. The global language set by `TitleArt.setLanguage(code)` (runtime only, not persisted).
  3. The device's preferred locales, the whole `System::getPreferredLocales()` list in order.
  4. The project's `localization.default_language`.
  5. The document's `source_language`.

  The first candidate that matches (R13) wins. In editor Play, step 3 is the Game view's simulated
  language when one is set.
- **R13. Matching a candidate** (`TitleArtLanguage::match(candidate, available) -> code | ""`):
  1. the same tag, case-insensitive;
  2. the same language and script, ignoring region. A missing script is inferred: `zh` with
     region TW, HK or MO is `Hant`, other `zh` is `Hans`, other languages have no script.
     `zh-Hant` is preferred to the first `zh-Hant-*`;
  3. the same language with either script unknown, ignoring region: `pt` first, then the first
     `pt-*` in `available` order (`pt-PT` matches `pt-BR`). `zh-Hans` never matches `zh-Hant`, and
     `sr-Latn` never matches `sr-Cyrl`;
  4. no match: the next candidate.

  Before matching, legacy codes are canonicalised (`iw` → `he`, `in` → `id`, `ji` → `yi`) on both
  sides. `available` is `[source_language] + languages[*].code` minus languages the export
  dropped (R25).
- **R14. Global default.**
  - `TitleArt.setLanguage(code)` re-resolves every Title Art Renderer whose Language is Auto, in
    every loaded scene, and future ones.
  - `TitleArt.setLanguage("")` returns to the device.
  - An invalid tag is refused with an error naming `LocaleRules::normalize`'s rule. Nothing changes.
  - `TitleArt.onLanguageChanged(code)` fires once per call that changes the value.
- **R15. The switch never shows a blank frame.**
  - The previous language's image stays on screen until the new one is ready (contract decision 2).
  - `renderer.onLanguageReady(code)` fires on the first frame the new language is drawn.
  - `renderer:isLanguageReady()` is false while it loads.

### Functional — bakes and modes

- **R16. Per-language bakes (Baked mode).** S80's bake runs once for the source and once for each
  language with `bake: true` whose resolved document differs from the source:
  - **A language bake is a full bake set.** Every derived file of the source set (S80's output
    PNGs and `.bake.json` sidecar, S85's flipbook pages and `.anim.json`) is written again for the
    language through S80's bake writer. The infix `.<code lowercase>` goes right after the
    document's file name, before everything else (S80 §6.15): `logo.titleart.de.png`,
    `logo.titleart.de.glow.png`, `logo.titleart.de.bake.json`, `logo.titleart.de.anim.json`,
    `logo.titleart.de.7c1e0a42.flip0.png`. S81's and S85's readers then read a language set
    unchanged. (Delivery exports, R40, name their files `<stem>[_<part>][.<code>].<ext>` instead:
    they are not bake sets.)
  - Each language has its own trimmed frame, shared by that language's outputs (S80 §6.9).
  - For animations, S85's `TitleAnimPlan` is rebuilt per language, because a different glyph
    count changes stagger delays and glyph parts.
  - A language that does not differ (no translated text, no override, no `only_languages`
    difference) writes no file and reuses the source set.
  - The source sidecar gains `languages` (§6.6): per code, the language sidecar's file name, or
    `null` for "same as the source".
  - Each PNG's S80 recipe gains `language` and `resolved_hash`, so a translation change stales
    only that language's set.
  - An output id equal to a language code is refused by normalize (`output_id_is_language`), so
    file names stay unambiguous.
- **R17. Realtime and Glyph Atlas.**
  - A language change resolves the document for the new language and runs S81's re-render path
    (time-sliced, double-buffered), with no extra code path.
  - Glyph Atlas keys its atlas by (document, language). It renders the new language's glyphs and
    Characters set once, and drops the old atlas when the new one is ready.
  - Fonts of every shipped language ship. S81's "Characters used by the document" (S81 R53)
    covers every shipped language's resolved text; each Language's `characters` joins the set.

### Functional — checks and reports

- **R18. `check_title` language checks**, per language in `languages` (`"all"` default).
  - S80's `assets_present` already covers Language and Translation fonts (they are AssetRefs).
  - Each check returns PASS/FAIL/WARN with the language, the layer path and ready-to-apply fix
    ops (contract decision 12).
  - A fix op that needs content (a missing translation) is marked `needs_input: true` and carries
    the source text as a placeholder.
  - The checks:

  | Check | Level | When | Fix ops |
  |---|---|---|---|
  | `languages_complete` | FAIL | A layer with `translate`, non-empty source text, visible in that language, has no Translation `text` (the id S80 §6.12 reserves) | `set_translation {layer, language, text: "<source>"}` (`needs_input`); `set {layer, values: {translate: false}}` |
  | `translation_stale` | WARN | `source_hash` ≠ hash of the current source text, or `status: needs_review` | `mark_reviewed {language, layers}` |
  | `text_overflow` | FAIL | The resolved extent exceeds `fit.box` by more than 2 % after fit, or no fit and a box is set | Each candidate is offered only after the check re-runs fit with it and it fits: `fit.mode: condense`, `wrap` with `max_lines` 2 or 3, the `min_scale` that fits (≥ 0.4), a box widened to the extent |
  | `fit_scale_low` | WARN | Fit scale (× condense) below 0.75 | Same candidates as above, except lowering `min_scale` |
  | `glyphs_present` (S80's, run per language) | FAIL | Code points of the resolved text that no face of the chain has | Adding a project font or an S84 pack font that covers them to the Language's `fallback_fonts` (copied in on apply) |
  | `bake_fresh` (S80's, run per language) | FAIL | Baked mode: a language set is missing, or its recipe `resolved_hash` differs | `export_title {path, languages: [code]}` |
  | `project_language_missing` | WARN | `localization.languages` lists a language that the document lacks, and the document has translatable text | `add_language {code}` |
  | `translation_unused` | WARN | A Translation of a layer whose `translate` is false, or of a language not in `languages` (kept, unused) | `revert_translation` |

- **R19. `get_title` language view.**
  - `get_title {language}` returns the document resolved for that language: text, font,
    fit result and frame per layer.
  - `get_title` always includes `languages` as `{source, list: [{code, name, translated,
    missing, stale, overflow, bake}]}` and, per text layer, `text_key`, `translate`, `max_chars`
    and the list of translated codes.

### Functional — strings exchange

- **R20. CSV export.**
  - Writes one file of rows (§6.6): `key`, `context`, `max_chars`, the source column, then one
    column per language.
  - A `file` column comes first when several documents are exported together.
  - UTF-8 with BOM, comma, CRLF, RFC 4180 quoting. Fields that contain a comma, a quote, CR/LF,
    or leading or trailing spaces are quoted. Newlines stay inside the quotes.
  - Content is never altered: no formula escaping (§9).
- **R21. CSV import.** Accepts UTF-8 with or without BOM. The delimiter (`,` `;` or tab) is
  detected from the header row, and quoted newlines are read. Headers may be codes (`de`) or
  Unity Localization style (`German (de)`): the code in parentheses wins. Rows match by key:
  - an unknown key is reported as `unknown_key` and not applied;
  - an unknown language column is reported as `unknown_language`, unless `add_languages: true`;
  - an empty cell, or a cell equal to the current value, changes nothing;
  - when the row's source cell differs from the current source text, its translations import with
    `status: needs_review` and the row is reported `source_changed`;
  - otherwise the status is `translated` (or the `status` argument) and `source_hash` is set to the
    current source.

  One import is one undo step.
- **R22. XLIFF.**
  - Export writes XLIFF 1.2, one file per target language (`<stem>.<code>.xlf`; one `<file>` per
    document): `<trans-unit id=key resname=key maxwidth=max_chars size-unit="char"
    xml:space="preserve">`, `<source>`, `<target state>`, `<note>` from `translator_note`.
  - Import reads XLIFF 1.2 and 2.0. Target states are mapped:
    - `final`, `signed-off`, `translated` (1.2) and `translated`, `reviewed`, `final` (2.0) →
      `translated`;
    - `needs-*` (1.2) and `initial` with a target (2.0) → `needs_review`;
    - an absent or empty target is skipped.
  - A unit whose target has inline markup (`<g>`, `<x/>`, `<ph>`, `<pc>`, …) is refused per unit
    (`inline_markup`): titles are plain text.
- **R23. Project-wide exchange.** Several documents (a list, a folder or every `.titleart`)
  export to one CSV with a `file` column, or to one XLIFF per language with one `<file>` per
  document. Import routes rows by `file` (project-relative path) else by the key's namespace.

### Functional — export of languages to games

- **R24. Languages in the shipped sidecar.**
  - A Baked title ships its source set, its language sets and the source sidecar's `languages`
    table (§6.6). Baked titles still need no `.titleart` in the build (S81 R8, R52).
  - The table also holds `source` and `default` (the project default when the document has it,
    else the source), so the runtime resolves without the document.
- **R25. Which languages ship.**
  - When the project's `localization.languages` is non-empty, only those languages, plus each
    document's source, ship. Their sets and fonts go in. The others are dropped with the build
    warning `W-TITLE-LANGUAGE-DROPPED` naming document and code, and the shipped copy of each
    source sidecar lists only the shipped languages. The exporter writes that filtered copy; the
    project's file is untouched.
  - An empty list ships every language of every document.
- **R26. Runtime footprint.**
  - Language matching, the global language and the sidecar `languages` table belong to S81's
    always-in-core part: `engine/core/util/TitleArtLanguage.{h,cpp}` beside `TitleArtBakeInfo`,
    ≤ 6 KiB stripped arm64 (≤ 3 KiB wasm gzip) of the program's 60 KiB core-part target (S80
    §6.15, measured by S81).
  - Translation resolution and fit (`engine/core/titleart/TitleTranslations.*`, `TitleFit.*`)
    are needed only to re-shape, so they sit with the renderer behind `JASPERENGINE_TITLE_ART`
    (Realtime and Glyph Atlas; contract decision 4) and in the editor.
- **R27. The Baked switch.**
  - On a language change the component reads the language's sidecar (S81's reader) and requests
    its texture through the texture pool. The current texture is drawn until the new one is
    loaded.
  - The new texture is positioned by its frame, so a wider German bake stays centred on the same
    anchor.
  - Textures of languages not on screen are not loaded. A texture the pool has evicted reloads
    the same way.

### Functional — import

- **R28. Pachislop projects** (`.pslot.json`):
  - The file is checked as `parseProjectFile` does, with its messages:
    - `format: "pslot"`, project layers and comp present;
    - `version` 4–11 and equal to `project.version`;
    - versions 1–3 get Pachislop's own message;
    - newer versions get `pachislop_version_newer`.
  - Size caps: 256 MiB of JSON, 64 MiB per decoded asset, 4,096 layers.
  - It is migrated to v11 (§4.1, literally) and then mapped to `.titleart` through `TitleArtOps`
    (`add_layer`, `set`, `add_effect`, ...), so S80 validates and normalises the result (§6.8).
  - The import never fails because one field cannot be mapped (R31).
- **R29. Pachislop assets.**
  - Embedded fonts and assets are written to `<stem>_assets/` beside the new `.titleart`.
    Names are sanitised (port of `safeFileName`) and suffixed `_2`, `_3` on clashes.
  - A file whose SHA-256 equals a file already in that folder is reused, so re-importing writes
    nothing new.
  - The 12 Pachislop builtin font ids map to S84 font pack ids by S84's `pachislop-map.json` fonts
    table (`dela-gothic-one` → `dela-gothic-one`, ...). The pack font is imported as S84 R29 does
    (`assets/TitleArt/Fonts/<dir>/`, once per project). A font the pack lacks leaves the built-in
    face, and the report says `font_unavailable` with the family name; the S82 font picker finds
    it on Google Fonts.
  - GIF assets become PNG (first frame).
  - WebP assets cannot be decoded (no WebP decoder is vendored): the layer keeps an empty
    image with note `image_format_unsupported`.
  - SVG, GLB and HDR assets are copied as they are.
- **R30. 3D layers.** `title3d` layers map through S86's `TitleDocument3D::fromPachislop` (S86
  R67). Before S86 lands:
  - they import with `dimension: "3d"` (S80 draws them 2D with `dimension_unavailable`);
  - the face colour is taken from the 3D face material's base colour;
  - the Pachislop `style3d` JSON is kept verbatim in the unknown member `pachislop_style3d`.

  **Reimport 3D** (Designer, op `reimport_pachislop_3d`) re-maps that member once S86 is present.
- **R31. Import report.**
  - Every unmapped, approximated or dropped item is a note `{layer (new path), field, what,
    action: dropped | approximated | kept_unknown}`. Examples: `solo` true, `companionOf`, an
    effect type with no S83 id, a non-default comp camera without 3D layers.
  - The report is returned (MCP, CLI `--json`) and shown in the **Import Report** window. Clicking a
    note selects its layer.
  - The document's `import_notes: [string]` keeps the report's text lines. The Inspector shows
    their count, with **Clear**.
- **R32. Photoshop styles** (`.asl` style libraries, and layers with effects in `.psd`), mapped
  by §6.9:
  - **to Looks:** library op `import_ps_styles {file, styles?, scope: "project", reference_px?}`
    (no document `path`, like S84's library ops, S84 R44) writes one `.titlelook` per style
    through S84's capture code. The id is the style name's slug, the family `custom`, the tags
    `["photoshop", "<file stem>"]`, and `ref_size` the reference size;
  - **to a layer:** op `apply_ps_style {layer, file, style, reference_px}`.

  The reference size, the font size of the PS text layer (`TySh`, point size × transform ×
  ppi/72), is taken from the file, else from the layer height, else 100 px. The Designer asks to
  confirm it when the file has no font size, as Pachislop's dialog does. Every conversion returns
  `summary` and `notes` (Pachislop's pattern), unique by content.
- **R33. Layered PSD into a document** (`import {file: "x.psd", kind: "psd"}`):
  - Accepted inputs: PSD v1 and PSB v2; RGB or Grayscale; 8 or 16 bits. CMYK, Lab, indexed,
    bitmap and 32-bit files are refused for layer import (`psd_mode_unsupported`); their styles
    can still be imported (R32).
  - Pixel layers and smart objects become Image layers (pixels to `<stem>_assets/<layer>.png`,
    trimmed, placed at their bounds). They keep their styles, mapped as R32.
  - Text layers become Text layers:
    - text, font PostScript name (matched to project fonts, then S84's pack, then system fonts
      by family; unmatched → built-in face + note), size, fill colour, tracking, leading
      (`line_height`) and paragraph alignment come from EngineData;
    - multiple style runs use the first run, with a note;
    - warp text becomes a note.
  - Groups become groups. Pass-through becomes Normal with a note (S80 §6.1 Group).
  - Blend modes and opacity carry over. Fill opacity goes to the face. Clipping, visibility and
    user layer masks carry over (a mask becomes an `image` mask on `<stem>_assets/<layer>_mask.png`).
    Vector masks are rasterised to the same, with a note.
  - Adjustment layers of the 13 S80 types become Adjustment layers: `levl`, `curv`, `hue2`,
    `brit`/`CgEd`, `expA`, `vibA`, `blnc`, `blwh`, `phfl`, `grdm`, `nvrt`, `post`, `thrs`. Others
    (selective colour, channel mixer, colour lookup) are dropped with a note.
  - Solid and gradient fill layers become S80's `fill` generator.
  - The canvas is the PSD size. `dpi` comes from resource 0x03ED. `global_light` comes from
    0x040D/0x0419.
- **R34. SVG into Vector layers** (`import_svg {file, split, mode, parent, name}`; drop on the
  Designer; **Add Layer > Vector (SVG)...**):
  - A file outside the assets is copied to `<doc stem>_assets/`.
  - `split: none` (default) makes one Vector layer.
  - `groups` makes one layer per top-level `<g>` or drawable element. `paths` makes one per
    drawable element.
  - Each split part is written as its own small `.svg` beside the original: the original root
    `viewBox` and `width`/`height`, plus that element with its ancestors' transforms, `<defs>`
    and referenced `<use>` targets. The layers share size and position, so they register exactly
    with no new S80 member.
  - `mode: auto` (default) picks S80 `shape` when every fill and stroke is one colour or
    none/black, else `artwork`.
  - Elements that `shape` mode cannot use are listed as notes: `<text>` (convert to outlines in
    the authoring tool), `<image>`, `clipPath`, `mask`, `filter`.

### Functional — export

- **R35. `export_title` formats** (S80's tool bakes; S87 adds `format`, `languages`, `options`
  and `out`, §6.3):
  - `format: "bake"` (default) is S80's bake, run for the source and the languages of R16
    (`languages` restricts it);
  - delivery formats: `png`, `psd`, `svg`, `sheet`, `atlas`, `sequence`, `ktx2`, `csv`, `xliff`
    (values of S80's `format` enum, beside S86's `glb`).
    Their options (R36–R43) go in `options`, a free-form object the handler checks against a
    closed table per format before anything renders (`bad_option`, listing that format's keys).
    The table is in `jasper://docs/title-art`. Every option named in R36–R43 is an `options` key,
    and so are `overwrite` and `zip`;
  - every image format takes `languages` and writes one result per language, with `.<code>`
    before the extension;
  - destination: `out`, default `Recordings/Title Art/<stem>/` (a non-asset directory, D9);
  - MCP may write only inside the project. The CLI may write anywhere;
  - existing files fail with `output_exists` unless `overwrite`;
  - work over 1 s is an `McpJobs` job with progress `done/total` renders;
  - the result lists `files`, `frame` per language, and `warnings`.
- **R36. Frames.** Every image export uses S80's `TitleFrame`:
  - rendered on an enlarged canvas so glows never clip;
  - the opaque background is excluded from measuring;
  - full-bleed generators are measured clipped to the canvas.

  The options:
  - `scale` (0.1–8);
  - `trim` (true);
  - `padding` (px at output scale, default 4);
  - `round_to` 1/2/4/8 (default 4);
  - `pot` (pad to a power of two, false);
  - `alpha_threshold`: the 1–255 alpha a pixel needs to count as content when trimming. Default
    8, Pachislop's: faint haze does not grow the box, but it is still drawn inside it;
  - `max_size` (16,384; above it the scale is reduced with `output_downscaled`).

  All languages and outputs of one call share one frame when `registered: true` (default
  false: each language has its own trimmed frame).
- **R37. PNG.**
  - Bit depth uses S80 §6.9's readback:
    - 8-bit: the output pass writes straight sRGB with dither into RGBA8. `dither: false` asks
      that pass for none;
    - 16-bit: straight sRGB-encoded half floats, rounded to 0..65535 on the CPU, undithered;
    - on the RGBA8 fallback path (no float readback, contract decision 3) 16-bit is refused
      (`bit_depth_unsupported`).
  - `alpha: straight` (default) uses `alpha_bleed: true`, S80 §6.9's routine. Every fully
    transparent pixel takes the colour of the nearest pixel with alpha > 0 (two-pass distance on
    the CPU) within `bleed_px` (default 8, S80's cap; up to 64 for textures mipped further).
    Farther pixels are written black. There are no dark fringes under bilinear filtering or mips.
  - `alpha_bleed: false` writes zeros (Pachislop's output).
  - `alpha: premultiplied` multiplies colour by alpha in the encoded domain (what image editors and
    premultiplied engine textures expect). When the destination is inside the assets, a warning
    says a material needs Blend Mode Premultiplied.
  - Chunks, in order: IHDR, `sRGB` (perceptual), `gAMA` 45455, `cHRM` (sRGB primaries and D65),
    `pHYs` (document `dpi`), the `jasper.titleart.export` iTXt recipe (source hash, bake
    version, language, options; `metadata: false` omits it), IDAT, IEND. No iCCP (D7).
  - Encoder (`editor/titleart/interchange/png/PngBandEncoder`):
    - Sub filter on every row;
    - 128-row bands deflated in parallel on the job system with miniz `tdefl` at level 6, each
      band but the last ending with `TDEFL_SYNC_FLUSH`;
    - Adler-32s combined (port of `pngChunk.ts` `adler32Combine`);
    - the bytes are identical for any thread count.
- **R38. PNG parts.** `parts: true` writes each layer rendered alone on the shared frame, plus
  `<stem>.parts.json` (§6.6): file, layer id and path, part, blend, opacity, order, hidden.
  `split_titles: true` splits each 2D title into its rims, its face and `fx`, as Pachislop's
  `titleParts` does (rim N alone with the face hidden and its width cumulative; each part with its
  own part effects only).
- **R39. PSD** (`editor/titleart/interchange/psd/PsdWriter`, plan ported from `psd.ts`):
  - PSD version 1, RGB, 8 or 16 bits, at most 30,000 px per side (`psd_too_large`);
  - the merged image is our own composite (raw for 16-bit, RLE for 8-bit);
  - image resources: 0x03ED resolution (`dpi`), 0x040F the generated sRGB profile (§6.10),
    0x0421 version info (`hasRealMergedData` 1);
  - one record per layer, each rendered alone on the shared frame:
    - name: Pascal ASCII with `?` for other characters, plus `luni` UTF-16;
    - `lyid` sequential;
    - opacity, visibility, the clipping flag;
    - blend keys: normal `norm`, add `lddg`, screen `scrn`, multiply `mul `, overlay `over`,
      and the other S80 §6.4 modes by a table. A mode Photoshop lacks is written as `norm`, with
      a warning;
  - groups as `lsct` section dividers (open folder 1, end marker 3);
  - clipped layers masked by the base's alpha before the base's effects, as Pachislop does;
  - layer masks as raster user masks covering the document;
  - adjustment layers native for `levels`, `curves`, `hue_saturation`, `brightness_contrast`,
    `invert`, `posterize` and `threshold`. Every other type is baked into a pixel layer
    "<name> (baked)" holding everything below it in its parent, adjusted, with what it covers
    hidden, plus a warning;
  - `split_titles` as R38;
  - text is written as pixels, and styles are baked;
  - layer channel data: 8-bit PackBits RLE with row byte counts; 16-bit in an `Lr16` tagged
    block, ZIP with prediction (compression 3) through miniz;
  - transparent pixels' colour is written as 0 (smaller RLE; Photoshop ignores it);
  - the bytes are identical for the same input (no time, no random ids);
  - one result warning says once that Photoshop blends in sRGB and the engine in linear light, so
    a recombination in Photoshop differs where layers overlap with partial alpha; the merged image
    is the true render.
- **R40. Multi-file results.** A call that writes more than one file (languages, parts, pages,
  sequences, XLIFF per language) names them `<stem>[_<part>][.<code>].<ext>` and lists them in
  `files` in write order. `zip: true` bundles them into `<stem>_<format>.zip` (miniz
  `ZipWriter`, stored entries for PNGs, deflated for JSON/SVG/CSV/XLIFF) and lists only the ZIP.
- **R41. SVG** (`editor/titleart/interchange/svg/SvgWriter`):
  - one file per language; `viewBox` = the export frame in document px (converted to y-down);
    `width`/`height` in px; one `<g>` per layer with its name as `id` (slugged, unique), its
    opacity, and CSS `mix-blend-mode` for blend modes;
  - text, shape and `shape`-mode Vector layers become `<path>` outlines after warp, distort and
    weight, with the layer and group transforms baked into the coordinates (Pachislop leaves them
    out);
  - face fill:
    - `solid` → fill;
    - `linear` and `radial` → `userSpaceOnUse` gradients, with stop `alpha` as `stop-opacity`
      and `mid` as an extra stop at the midpoint colour;
    - `reflected` → linear with `spreadMethod="reflect"`;
    - other kinds → their first stop's colour, with a warning;
  - rims become copies of the outline under the face, outermost first: stroke width 2 × the
    cumulative width, `stroke-linejoin` = the rim's join, `stroke-miterlimit` 4, filled and stroked
    in the rim colour, offset by a translate. `fill_counters` strokes and fills only the outer
    contours. `hull`/`box` plates are their own paths;
  - shape and gradient masks become `<mask>`. Clipping to a vector base becomes `<clipPath>`;
  - effects, bevels, textures, patterns, generators, adjustment layers, image layers, `artwork`
    Vector layers, layer and image masks and 3D layers are left out, each with a warning naming
    the layer;
  - `embed_raster: true` embeds non-vector layers as registered PNG `<image>`s;
  - `strict: true` fails with the list instead.
- **R42. Sheets, atlases, sequences** (S85 animations):
  - `animation` (name, default the first), `fps` (default S85's), `frames` (`[from, to]`
    indexes or a count; at most 1,024);
  - frame i is S85's sampling (`TitleAnimSampling`) at t = i / fps, with `TitleAnimPlan` built
    for the export's language.

  The three formats:
  - **`sheet`**: a uniform grid of the frames on their union frame, `columns` (default
    ⌈√n⌉), file `<stem>@<W>x<H>.png` (S45's grid convention, so the engine slices it) plus
    `<stem>.sheet.json` (Aseprite/TexturePacker JSON-array shape, §6.6);
  - **`atlas`**: each frame trimmed by `alpha_threshold` and packed with `stb_rect_pack`
    (order: height descending, then frame index; `padding` 2, `extrude` 1, pages up to
    `max_atlas` 4,096, at most 16), plus `<stem>.atlas.json` (JSON-hash shape with `trimmed`,
    `spriteSourceSize`, `sourceSize`, `duration`, `frameTags`);
  - **`sequence`**: `<stem>_<NNNN>.png` on a shared frame.

  PNG options apply to each image.
- **R43. KTX2.** `format: "ktx2"` with `options {family: desktop | mobile | web, quality, mipmaps}`:
  - renders the PNG in memory;
  - resolves texture settings as an S14 `ui` texture (sRGB, `alphaIsTransparency`, no mips unless
    asked);
  - runs `TextureCooker::cook` for that family;
  - writes `<stem>[.<code>].ktx2`.

  For delivery outside the project. Bakes inside the project are cooked by S14 as usual.
- **R44. Colour management** (D7):
  - exports are sRGB-encoded and tagged sRGB;
  - imports read colours as sRGB. A PSD with another embedded profile (Adobe RGB, Display P3)
    imports with its values unconverted and the note `color_profile_ignored` naming the profile
    description;
  - Lab and HSB colours in descriptors convert as `psStyle.ts` does;
  - no CMYK conversion, no Japan Color, no soft proof.

### Platform & backends

- **R45. Runtime parts on every target.** R12–R17 and R24–R27 run on GL 3.3, GLES3/WebGL2,
  Metal, D3D11 and Vulkan, on Windows, Linux, macOS, Android, iOS and HTML5. They add no shader,
  no GPU feature and no thread: the Baked switch is a texture swap, and Realtime uses S81's
  path.
- **R46. Editor parts** (import, export, strings, checks) run in the Linux, Windows and macOS
  editors. Their GPU work is S80's renderer on the editor's backend. 16-bit outputs need float
  readback (S80), else `bit_depth_unsupported`.
- **R47. Portable code.** C++20 within `tools/check-cxx-subset.py`. Parsers of untrusted files
  (PSD, ASL, CSV, XLIFF, `.pslot.json`, SVG) bound every length against the remaining bytes, cap
  descriptor nesting at 64, and fail with a message, never a crash. They are run under
  `SANITIZE=address,undefined` with a corpus of truncated and mutated files (AC31).

### Performance (Release; editor on build server A; runtime also on the Mali-G57 / Adreno 610 class device of S13 AC23)

- **R48. Runtime.**
  - `TitleArtLanguage::match`: ≤ 2 µs.
  - `TitleArt.setLanguage` with 100 Title Art Renderers loaded: ≤ 0.3 ms on the main thread. It
    only marks them; the work is time-sliced by S81.
  - Reading a language sidecar (≤ 4 KB, S81 R8): ≤ 0.1 ms on the loader thread.
  - Fit of one text layer of ≤ 64 code points: ≤ 0.3 ms desktop, ≤ 1.5 ms phone. It runs only
    when text, language or a fit member changes.
  - A Realtime switch costs what S81's text change costs.
  - A Baked switch adds one texture load (KTX2: a 2048² BC7 loads in ≈ 3.3 ms on the loader
    thread, `docs/textures.md:239`).
- **R49. Editor.**
  - CSV import or export of 1,000 rows: ≤ 50 ms. XLIFF of 1,000 units: ≤ 80 ms.
  - Pachislop import (job):
    - a 5 MiB v11 file with 3 fonts and 4 images: ≤ 1.5 s;
    - a v4 file with glyph-scale compensation: ≤ 2.5 s.
  - `.asl` with 200 styles: ≤ 0.5 s.
  - PSD 4,096² with 40 layers into a document: ≤ 6 s.
  - PNG encode, render excluded, 8 threads: 4,096 × 2,048 8-bit ≤ 250 ms; 16-bit ≤ 600 ms.
  - PSD 2,048 × 1,024, 20 layers, 8-bit, renders included (desktop GPU): ≤ 3 s.
  - Sheet of 64 frames of 512²: ≤ 6 s.
  - Peak export memory ≤ 3 × the frame's RGBA16F size: the PSD writer streams each layer's
    channels after its render.
- **R50. Disabled costs nothing.**
  - A monolingual document does no resolution, fit or language check, writes no `languages`
    table in its sidecar, and renders byte-identical bakes to S80 without S87.
  - A project without titles compiles none of R26.
  - Nothing polls: no CSV is watched, and imports run only when asked.

### Editor UX (human)

- **R51. Language switcher.**
  - The Title Designer's toolbar gets **Language: English (en) · Source ▾**. It lists the source,
    then each language with a status icon: complete, missing N, stale N, overflow N.
  - Its items include **Edit Languages...** and **Compare Languages**.
  - Commands of the S25 catalog: "Title Designer: Next Language" and "Previous Language"
    (rebindable, no default keys).
  - The canvas shows the resolved document for the selected language, its fit box (dashed) and
    overflow (red outline), and a corner badge with the code.
- **R52. Editing in a language.** While a translation language is selected:
  - edits of overridable members (R2) write that language's Translation;
  - those fields show as overrides, as Unity shows prefab overrides: bold label and a blue bar,
    with right-click **Revert to Source** and **Apply to Source**;
  - non-overridable fields (colours, effects) edit the shared document, under the strip "Shared
    property: changes every language".
  - Typing the text of a layer that has no translation creates it.
  - Each gesture is one undo step: "Set Translation (Title: de)", "Revert Translation (Title: de)".
- **R53. Edit Languages dialog.**
  - Add from a searchable list of the 60 most used game languages (code and English and native
    names), or a free BCP 47 tag validated live.
  - Per language: name, default font and fallbacks (the S82 picker), Bake, and **Copy From**
    another language.
  - **Add Project Languages** adds every language of `localization.languages`. **Source
    Language** has a swap option (D6).
  - Removing a language asks for confirmation with the count of translations lost (undoable).
- **R54. Translations panel** (dockable, next to Layers):
  - one row per text layer: Key, Layer, Source, Max, then one column per language;
  - cells are red when missing, orange when stale, with an overflow icon and the fit scale;
  - filters: All, Missing, Stale, Overflow;
  - inline editing, where one committed cell is one undo step;
  - cell context menu: Copy Source, Mark Reviewed, Revert;
  - **Import...** and **Export...** (CSV, XLIFF);
  - double-click selects the layer and switches the canvas to that language.
- **R55. Compare Languages.** A canvas mode with every language side by side:
  - thumbnails rendered through S80's `TitlePreviewQueue` within its budget (contract decision 11),
    visible ones first;
  - labels with code, fit scale and overflow state;
  - a click switches to that language.
- **R56. Project Settings > Localization.**
  - **Languages** (an ordered list) and **Default Language** (`localization.languages`,
    `localization.default_language`), searchable (S25).
  - The **Title Art Renderer** Inspector (S81) gets **Language: Auto (Device) | <codes>**, and the
    Title Art Inspector summary "Languages: en (source), de, ja · 1 missing".
- **R57. Import UI.**
  - **Assets > Import Title Art...** (`.pslot.json`, `.psd`, `.svg`) and drag-drop onto the
    Project window open the import dialog: destination folder, kind, the summary of what will be
    created. Then the **Import Report** window (R31) opens, and the document opens in the Title
    Designer.
  - **Layer > Import Photoshop Style...** applies a style to the selected layer: a list of the
    file's styles with thumbnails, and reference size confirmation.
  - The Library panel's **Import Photoshop Styles...** saves them as Looks.
  - "Import Pachislop Project (logo.pslot.json)" and "Import SVG (badge.svg)" are one undo step
    each, removing every file they wrote.
- **R58. Export dialog.**
  - **File > Export...** in the Title Designer has tabs PNG, PSD, SVG, Sprite Sheet, Atlas,
    Sequence, KTX2, Strings.
  - The options are those of R36–R43 with Unity-like labels (Trim, Padding, Round To, Alpha
    Threshold, Alpha: Straight/Premultiplied, Bleed Color, Bit Depth).
  - It has language checkboxes, the output folder, and a live size readout ("1,236 × 412 px,
    3 languages").
  - **Export** runs as a background job, with progress in the status bar. A toast offers **Show
    in Folder**.
  - The last options are kept per document in the project's editor-local settings, not in the
    `.titleart`.

### Agent UX (MCP, Lua, CLI)

- **R59. MCP** (group `title_art`, no new tool; §6.3):
  - `edit_title` gets the ops of §6.2 and `create.from` (a Pachislop, PSD or SVG file to import);
  - `export_title` gets `format`, `languages`, `options` and `out` (R35–R43);
  - `render_title` gets `options.languages` (a list or `"all"`, a key of S80's `options`): one sheet
    row per language and one column per backdrop, labelled with code, fit scale and overflow;
  - `check_title` gets `languages`;
  - `get_title` gets `language`.

  The `allowedWhilePlaying` decisions:
  - `edit_title` keeps S80's;
  - `export_title` with an image format renders off-screen and is allowed;
  - imports through `create.from` and the library op `import_ps_styles` are allowed (they write
    files and open no scene).
- **R60. The agent translation loop** (in `jasper://docs/title-art`, section Languages):
  1. `get_title` (strings, `max_chars`, notes).
  2. `edit_title` with `add_language` and one `set_translation` per layer.
  3. `check_title {languages: "all"}`, then apply its fix ops.
  4. `render_title {options: {languages: "all"}}` to look.

  Agents translate with their own model; the engine never calls a translation service.
- **R61. Lua** (runtime, §6.4): `TitleArt.setLanguage`, `getLanguage`, `onLanguageChanged`, the
  component's `language`, `getResolvedLanguage`, `getLanguages`, `isLanguageReady`,
  `onLanguageReady`. Editor scripts reach imports and exports through `Editor.call("edit_title",
  ...)` and `Editor.call("export_title", ...)`.
- **R62. CLI** (§6.5): `import-title-art` and `export-title-art`, batch over files and globs:
  - offline (no editor process) for Pachislop, PSD, ASL and SVG import and for strings;
  - one one-shot headless editor for the whole batch when rendering is needed;
  - exit codes 0 (all done), 1 (some failed, each listed), 2 (usage); `--json` prints each
    result's `structuredContent`.

### Data & compatibility

- **R63. Document members** (§6.1) are optional and added to v1 without a version bump (S80 §6.1
  migration rule 1).
  - Missing members mean monolingual, no fit and every language visible, which is exactly S80 v1's
    rendering.
  - Translation objects are sparse (D5): the writer emits `text`, `status` and `source_hash`,
    then only the overrides that are present, in table order.
  - Unknown members inside Translations and Languages survive a save (S80).
- **R64. Project settings.** `localization.languages` (list, `[]`) and
  `localization.default_language` (`""`, which means the first of `localization.languages`, else
  `"en"`). Both are project-wide names a later localisation system reuses (D3). They reach the game
  with the other start values (`Generator.cpp:2622` pattern; the prebuilt package beside
  `package.presentation`, `ExporterPrebuilt.cpp:1106`).
- **R65. Formats** are versioned:
  - the sidecar `languages` table is an optional member of S81's sidecar v1. A sidecar that
    lacks it, or a language sidecar that fails to read, makes the runtime use the source set,
    with one warning;
  - `.parts.json`, `.sheet.json` and `.atlas.json` carry `format`/`version` keys as well;
  - CSV and XLIFF are fixed by §6.6.

  Pachislop v4–v11 import forever. A Pachislop v12 is refused until a later change ports its
  migration.

## 6. Design constraints & decisions

- **D1. Translations live in the title, not in a string table.** A title's per-language font,
  size, vertical layout and fit are part of the art, and an artist tunes them looking at the
  canvas. The engine has no string tables (§3). Inline translations work the day this lands and
  stay next to the art they change. D3 keeps the door open.
- **D2. Overrides, not copies.** A Translation is a sparse override of the source layer. Effects,
  colours and rims are shared, so restyling the title restyles every language. Copying the layer
  per language (Pachislop's only option) would let languages drift apart. `only_languages` covers
  the rare layer that exists in one language only.
- **D3. Coexisting with a future project localisation.** S87 fixes three things such a system
  must reuse unchanged:
  - the key format `<string_namespace>.<text_key>` (dots, lowercase);
  - the CSV and XLIFF files of §6.6, which a String Table importer reads as they are;
  - the project settings `localization.languages` and `localization.default_language` with R12–R13's
    resolution.

  A later spec may let a text layer read its text from a table by key. It adds its own member
  (unknown members are preserved), and table text then wins over `translations`. No member is
  reserved here.
- **D4. RFC 4647 lookup with a script rule.** Plain prefix matching gives Hong Kong users
  Simplified Chinese and Portuguese users nothing. R13's rules are the ones platforms use, and a
  table test pins them (AC1). The whole preferred-locales list is used, not only the first entry,
  because users list fallbacks.
- **D5. Sparse Translations are the one exception to S80's "every member written".** A written
  default would freeze today's source value into every language, and a later edit of the source
  `size` would then not reach them. The exception is local and tested (AC3).
- **D6. Changing the source language swaps.** `set_source_language {code, swap: true}`:
  - when `code` is a translation language, its texts become the base;
  - the old base becomes a Translation of the old source code, with the old overrides moved;
  - `source_hash` values are recomputed.

  This lets a team that wrote the title in Japanese switch the source to English without
  retyping. `swap: false` only relabels.
- **D7. sRGB only, tagged the standard way per format.** Titles are authored and rendered in sRGB
  primaries (S80). The tags:
  - PNG: the `sRGB` chunk, plus the `gAMA`/`cHRM` that the PNG spec asks writers to add. The PNG
    spec says not to write iCCP beside sRGB, and the chunk is understood by every decoder;
  - PSD: an embedded profile, the only tag Photoshop reads. It is our own generated sRGB v2
    profile (§6.10) from IEC 61966-2-1 constants, so no third-party profile and no licence
    question;
  - SVG: sRGB by definition.

  Wide gamut, CMYK and print are not game needs (report p6 §3.5–3.6).
- **D8. Straight alpha with colour bleed by default.** The engine's Image and Sprite blend straight
  alpha. Bleeding colour under transparent pixels removes the dark fringe Pachislop's zeroed RGB
  causes under bilinear filtering and mips, without asking every consumer to switch blend modes.
  Premultiplied stays available for tools that want it. S80's 8 px reach covers three mip
  levels. `bleed_px` goes to 64 for textures mipped further. Beyond the reach, black keeps the
  file small.
- **D9. Exports default to `Recordings/Title Art/`.**
  - Delivery files (PSD, SVG, ZIP, 16-bit PNG) inside the assets would become textures: a PSD
    is a Texture type (`AssetTypes.cpp:29`).
  - `Recordings/` is already a non-asset directory (S27 R31).
  - A new reserved name would hide an existing project's assets of that name.
  - `out` can still point into the assets for sheets meant for the game.
- **D10. A C++ PSD writer and reader of our own, with ag-psd's descriptor and EngineData parsers
  ported.**
  - The write side is small and documented (records, RLE, ZIP, sections).
  - Descriptors and EngineData are where ag-psd encodes years of real-file quirks, so they are
    ported, not rewritten.
  - No JavaScript runs anywhere.
- **D11. Port Pachislop's migrations literally, then map once.** Its migration code is a single
  cross-version function with flags (report p6 §4.3). Re-deriving it as v→v+1 steps would risk
  renders changing. Its unit tests are the specification, so the port keeps its structure, and
  only the v11 → `.titleart` mapper is new code.
- **D12. Split SVG parts are files, not element references.** They give exact registration with
  no S80 renderer change. Each part is an ordinary asset that a person can open and edit.
- **D13. Fit measures geometry, not pixels.** Rendering to measure would cost a bake per
  iteration and could not run on a phone for Realtime titles. Outline bounds grown by rim widths
  are cheap, deterministic and exact for round joins.
- **D14. No new MCP tool and no guide bytes.** Every capability is an op or a parameter of the six
  `title_art` tools (contract). The detail lives in `jasper://docs/title-art`, whose Languages and
  Import/Export sections S87 writes.
- **D15. `variants` stays unused.** S80 reserved it for S87. Per-language bakes are derived files
  listed by the source sidecar, not document data, so S87 needs no `variants` member. S80 may
  release the reservation.

### 6.1 Document members (S87 adds; all optional)

Top level:

| Member | Type | Default | Notes |
|---|---|---|---|
| `source_language` | string | `""` | BCP 47; `""` = monolingual. `add_language` sets it from its `source` argument, else `localization.default_language`, else `"en"` |
| `languages` | [Language] | `[]` | ≤ 64 |
| `string_namespace` | string | `""` | `[a-z0-9_]{1,48}`; made from the file stem on first use, then kept |
| `dpi` | number | 72 | 1..2400; PNG `pHYs`, PSD resolution; Pachislop `comp.dpi`, PSD 0x03ED |
| `import_notes` | [string] | `[]` | R31 |

Language: `code` (string), `name` (`""` = English name from the built-in table), `font`
(AssetRef or `null`), `fallback_fonts` ([AssetRef], `[]`), `characters` (string, `""`), `bake`
(bool, true).

Text layer:

| Member | Type | Default |
|---|---|---|
| `text_key` | string | `""` (made on first use, R3) |
| `translate` | bool | true |
| `max_chars` | int | 0 |
| `translator_note` | string | `""` |
| `fit` | `{mode, box, min_scale, min_h_scale, max_lines}` | `{none, [0, 0], 0.6, 0.8, 2}` |
| `translations` | map code → Translation | `{}` |

Translation (sparse, D5): `text` (string), `status` (`translated` \| `needs_review`),
`source_hash` (8 hex: FNV-1a 32 of the source text's UTF-8), then any of `font`, `face_index`,
`fallback_fonts`, `language`, `size_scale` (0.1..10), `tracking`, `line_height`, `h_scale`,
`v_scale`, `vertical`, `align`, `features`, `variations`, `chars`, `lines`, `offset` (Vec2 px,
added to `transform.position`), `fit` (partial: only the members given).

Image layer: `translations: {code: {image: AssetRef}}`. Vector layer: `translations: {code: {svg:
AssetRef}}`. Every layer: `only_languages` ([string], `[]`).

Field paths (S80 §6.2) reach them as `translations.de.text`, `translations.pt-BR.fit.mode`, and
`languages[1].font`.

### 6.2 Ops (`edit_title {ops}`; S87 adds)

| Op | Members | Effect |
|---|---|---|
| `add_language` | `code`, `name?`, `font?`, `fallback_fonts?`, `copy_from?` (a code: its Translations copied, status `needs_review`), `source?` | Adds a Language. Sets `source_language` when empty. Fills missing `text_key`s and the R8 default fit |
| `remove_language` | `code` | Removes it and every Translation of it; `changes` counts them |
| `set_source_language` | `code`, `swap?` (true) | D6 |
| `set_translation` | `layer`, `language`, `text?`, `values?` (Translation paths: `font`, `size_scale`, `fit.mode`, ...), `status?` | Creates or updates. Setting `text` stamps `source_hash` |
| `revert_translation` | `layer`, `language`, `fields?` (default every override but `text`; `["all"]` removes the Translation) | |
| `mark_reviewed` | `language?`, `layers?` (default all) | `status: translated`, `source_hash` refreshed |
| `import_strings` | `file`, `format?` (`auto` \| `csv` \| `xliff`), `languages?`, `add_languages?` (false), `status?` | R21–R23; returns applied counts and the report |
| `apply_ps_style` | `layer`, `file`, `style?` (name or index, 0), `reference_px?` | R32; returns `summary`, `notes` |
| `import_ps_styles` | `file`, `styles?` (names or indexes; default all), `scope?` (`project`), `reference_px?`, `overwrite?` | Library op (no `path`, S84 R44): one `.titlelook` per style; returns per style `id`, `summary`, `notes` |
| `import_svg` | `file`, `split?`, `mode?`, `parent?`, `name?` | R34 |
| `reimport_pachislop_3d` | `layer?` (default all with `pachislop_style3d`) | R30 |

**Importing into a new document** is S80's `create` with one more key: `edit_title {path:
"assets/titles/logo.titleart", create: {from: "/home/me/logo.pslot.json", overwrite?}}`.
- `from` may be outside the project, because it is only read. The kind is detected by content:
  `8BPS` (PSD/PSB), `<svg` (SVG), or a JSON object with `format: "pslot"` (Pachislop).
- An `.asl` given to `from` fails with `ps_styles_not_a_document`, whose fix is the
  `import_ps_styles` op.
- `create.from` excludes `canvas`, `text` and `template` (`bad_option`).
- The result is `edit_title`'s plus `import: {kind, files, notes}`.

### 6.3 MCP parameters and the budget (S31)

S87's row of S80 §6.11's program budget table (≤ 195 characters; S80 already carries the hooks
`export_title.format` with every S87 format, `export_title.options`, `render_title.options` and the
`create` object), with schemas that carry no description (each parameter is explained in
`jasper://docs/title-art`):

| Tool | S87 adds | Listing chars (cap) |
|---|---|---|
| `export_title` | `languages` (`{"type":["string","array"]}`); `out` (string); `path` also accepts an array (several documents, strings only). The formats `png`, `psd`, `svg`, `sheet`, `atlas`, `sequence`, `ktx2`, `csv`, `xliff` are values of S80's `format` enum, and their options (R35-R43) keys of S80's `options`, checked per format by the handler (the S47 D4 pattern) | ≤ 90 |
| `edit_title` | `create.from` (string). The 11 ops are `ops` items, which S80 validates in `TitleArtOps`, so they cost no schema characters | ≤ 30 |
| `render_title` | `options.languages` (a key of S80's `options`) | 0 |
| `check_title` | `languages` (`{"type":["string","array"]}`, no description) | ≤ 45 |
| `get_title` | `language` (string) | ≤ 30 |

- ≤ 195 characters in total, no tool, and no `budget.json` entry; the group total is S80's table's
  (7,735 of 15,000, 6 of 12 tools).
- If a tool would pass 1,500, descriptions move to `jasper://docs/title-art`. Limits are never
  raised (conventions: `exceptions` are for tools that predate the rule).
- Guide: +0 bytes. The Title Art digest line (S80) already points to `jasper://docs/title-art`.
- Docs: `jasper://docs/title-art` gains `### Languages` (the R60 loop, the resolution rules, fit,
  the checks) and `### Import and export` (kinds, formats, parameters, the colour rules).
- Gates: `tool_budget.py --check`, `test_tool_conventions.py` (snake_case, closed objects,
  `anyOf` only), `gen_tool_docs.py --check`, `test_tool_coverage.py`.

### 6.4 Lua API (runtime; S81 owns the component object and how it is obtained)

```lua
TitleArt.setLanguage("de")              -- global default for every renderer whose Language is Auto
TitleArt.setLanguage("")                -- back to the device's languages
local code = TitleArt.getLanguage()     -- the value set ("" = device)
RegisterEvent(self, TitleArt.onLanguageChanged, "languageChanged")   -- self:languageChanged(code)

local title = self.gameObject:getTitleArt()   -- S81's component object
title.language = "ja"                   -- this renderer only; "" = Auto
title:getResolvedLanguage()             -- "ja" (what R12-R13 picked)
title:getLanguages()                    -- {"en", "de", "ja"}: source first, then shipped languages
title:isLanguageReady()                 -- false while the new bake or render is on its way
RegisterEvent(self, title.onLanguageReady, "showTitle")         -- self:showTitle(code), first frame drawn

-- a language menu that persists its choice (the game's job)
TitleArt.setLanguage(PlayerPrefs.getString("language", ""))
```

- Bindings go in S81's `engine/core/script/binding/TitleArtClassesLua.cpp`, with the stubs
  `---@field language string` (rw), `---@field onLanguageReady FunctionSubscribe` (code: string),
  and the statics `TitleArt.setLanguage(code)`, `TitleArt.getLanguage()` and
  `TitleArt.onLanguageChanged` (a static `FunctionSubscribe`, code: string).
- Errors, one sentence each, as S81 §6.6 words them:
  - `TitleArt.setLanguage("xx_!")`: "TitleArt: 'xx_!' is not a language tag (BCP 47, for example
    "de", "pt-BR", "zh-Hant").";
  - `title.language = "fr"` on a title without French: no error. The renderer resolves to its
    best match (R13), and `getResolvedLanguage()` tells which.
- C++: `jasperengine::TitleArtLanguage::setGlobal(const std::string&)`, `global()`, `match(...)`,
  `resolve(candidates, available)`.
- Events use the event-object protocol (`docs/mcp/guide/lua-api.md` §Events), `FunctionSubscribe`
  as S81's `onReady`.
- `docs/unity-glossary.md` gets the row: Unity Localization `LocalizationSettings.SelectedLocale`
  maps to `TitleArt.setLanguage` (titles only).

### 6.5 CLI

```text
jasperengine-editor import-title-art <file|glob>... --project <dir> [--into <assets dir>]
    [--kind auto|pachislop|psd|ps_styles|svg|strings] [--title <x.titleart>] [--add-languages]
    [--overwrite] [--json]
jasperengine-editor export-title-art <x.titleart|glob|dir>... --project <dir>
    --format png|psd|svg|sheet|atlas|sequence|ktx2|csv|xliff [--languages all|en,de]
    [--out <dir>] [--bit-depth 8|16] [--scale <s>] [--alpha straight|premultiplied]
    [--no-bleed] [--alpha-threshold <n>] [--split-titles] [--parts] [--animation <name>]
    [--fps <n>] [--family desktop|mobile|web] [--zip] [--overwrite] [--json]
```

- Each flag is the MCP parameter or `options` key of the same name, dashes for underscores
  (`SchemaArgs` converts them as `call` does).
- `--kind strings` imports CSV/XLIFF into the documents its rows name.
- The subcommands are dispatched in `editor/main.cpp` beside `bake-vat`, implemented in
  `editor/cli/TitleArtCli.{h,cpp}`, and listed by `runHelpCommand`.

### 6.6 File formats

**CSV** (one row per text layer, `translate: true`):

```text
file,key,context,max_chars,en,de,ja
assets/titles/logo.titleart,logo.headline,"Main title, max one line",12,BIG WIN,GROSSER GEWINN,大当たり
```

- `file` is present only for multi-document exports.
- The source column is the source code. When documents have different sources, the column is
  named `source` and a `source_language` column follows it.

**XLIFF 1.2** (`logo.de.xlf`):

```xml
<xliff version="1.2" xmlns="urn:oasis:names:tc:xliff:document:1.2">
  <file original="assets/titles/logo.titleart" source-language="en" target-language="de" datatype="plaintext">
    <body>
      <trans-unit id="logo.headline" resname="logo.headline" maxwidth="12" size-unit="char" xml:space="preserve">
        <source>BIG WIN</source><target state="translated">GROSSER GEWINN</target>
        <note>Main title, max one line</note>
      </trans-unit>
    </body>
  </file>
</xliff>
```

**Sidecar `languages` table** (an optional member S87 adds to S81's `logo.titleart.bake.json` v1;
only the source sidecar has it):

```json
"languages": {"source": "en", "default": "en",
              "sets": {"de": "logo.titleart.de.bake.json", "ja": null, "pt-BR": "logo.titleart.pt-br.bake.json"}}
```

- `null` means the language reuses the source set.
- A language sidecar is an ordinary S81 sidecar (its own `frame`, `origin`, `outputs`, `anchors`,
  `alpha_bounds`) plus `"language": "<code>"`.
- The shipped copy lists only shipped languages (R25).

**Parts manifest** (`<stem>.parts.json`):
- `{format: "jasperengine.titleart.parts", version: 1, frame, language, parts: [...]}`;
- each part is `{file, name, layer, path, part, blend, opacity, order, hidden}`.

**Sheet / atlas JSON.** The Aseprite/TexturePacker shapes most importers read:
- `frames` as an array (sheet) or a hash (atlas) of `{filename, frame {x, y, w, h}, rotated:
  false, trimmed, spriteSourceSize, sourceSize, duration}` (ms);
- `meta {app: "JasperEngine", version, image, format: "RGBA8888", size, scale: "1", frameTags:
  [{name, from, to, direction: "forward"}]}`.

### 6.7 Settings

| Key | Type | Default | UI |
|---|---|---|---|
| `localization.languages` | list of BCP 47 | `[]` | Project Settings > Localization > Languages |
| `localization.default_language` | string | `""` | Default Language |

Rows in `editor/settings/SettingsRows.cpp` with placements and keywords "language locale
translation", validated by `LocaleRules::normalize`.

### 6.8 Pachislop v11 → `.titleart` mapping (`editor/titleart/interchange/pachislop/PachislopMapper.cpp`)

`PachislopMapper` maps the project's structure (tree, ids, groups, comp); every style, effect,
generator and template member goes through S84's `PachislopPort` member mapping (data
`editor/resources/titleart/pachislop-map.json`) and S83's `PachislopFxMap`, so the look converter
and the project importer give the same result for the same Pachislop data.

| Pachislop | `.titleart` | Notes |
|---|---|---|
| `project.name`; `comp.width/height`; `comp.background`; `comp.globalLight`; `comp.dpi` | `name`; `canvas`; `canvas.background`; `global_light`; `dpi` | |
| `comp.camera`, `environment`, `lights`, `toneMapping`, `exposure`, `supersample` | S86's `scene_3d` when a `title3d` layer exists | Else a note if non-default |
| flat `layers` with `parentId` | the tree, `children` bottom first | Subtrees are contiguous below their group, as Pachislop guarantees |
| `id` | new S80 ids; a map old → new for `follow`, layer masks, `materialLinks`, part keys | |
| `name`, `visible`, `locked`, `guide`, `blend` (5 modes), `transform` (`x, y` → `position`; `rotZ` → `rotation`, both clockwise; `scale`; `flipX/Y`; `opacity`); `clip`; `masks` (`shape`/`gradient`/`layer` types 1:1) | the same S80 members | `solo` dropped with a note; `z`, `rotX`, `rotY` go to S86 |
| `title2d` with `text` / `shape` / `svg` / `image` source | Text / Shape / Vector (`mode: shape`) / Image layer, `dimension: 2d` | Text: `fontId` → `font`, `tracking`, `lineHeight`, `align`, `vertical`, `shear`, `hScale/vScale`, `tsume` → `proportional`, `tateChuYoko` → `tate_chu_yoko`, `chars` (`dx, dy` → `offset`, `rotate` → `rotation`, `face`), `lines`; `warp`, `distort`, `weight` |
| `style` (TitleStyle2D: `face`, `rims`, `knockout`, `erode`; Material2D camelCase) | `style` (snake_case members, S80 §6.1), by the member rules of S84's `pachislop-map.json` (the camelCase to snake_case rule and its exceptions) | `reflection.map` presets → S84's library maps of the same names, copied into the project |
| `title3d` | `dimension: 3d` via S86's `TitleDocument3D::fromPachislop` (R30) | |
| `background` | generator `fill` (S80) | |
| `group` (`collapsed`) | Group | `companionGroupOf` dropped |
| `adjustment` (14 types, camelCase) | Adjustment (snake_case types, S80) | |
| `generator` (`type`, `params`) | Generator with the S83 id and params from S83's `PachislopFxMap` (S83 R51), units and angle conventions converted there | `companionOf` dropped with a note |
| `effects`, `partEffects` (`face`, `rim:<id>`, `band:<id>`, `side`) | `effects`, `part_effects` with S83 ids and params from `PachislopFxMap` | A type the map lacks is kept as an S80 unknown effect (`unknown_type`) with a note; `fromPreset` dropped |
| `materialLinks` | `material_links` | |

### 6.9 Photoshop style mapping (`editor/titleart/interchange/psd/PsStyleConvert.cpp`, port of `psStyle.ts`)

| Photoshop | Title Art | Change from Pachislop |
|---|---|---|
| Stroke(s) (`FrFX`, `lmfx` arrays) | Rims sorted by reach (outside = size, center = size/2, inside drawn outside with a note); a stroke reaching no further than one above it is dropped; colour, gradient (Shape Burst → `shape_burst`) or pattern fill | Stroke opacity → rim material `opacity` (was drawn opaque) |
| Gradient overlay (`GrFl`) | Face fill gradient (`linear`, `radial`, `angle`, `reflected`, `diamond`); PS angle (counter-clockwise) → S80 clockwise; scale; offset along the axis; reverse | Opacity stops → stop `alpha`; midpoints → `mid` (were extra mixed stops; transparency ignored) |
| Colour overlay (`SoFi`) | Face fill solid mixed over the base by opacity | |
| Pattern overlay (`patternFill`) | Procedural pattern by name (EN/JA table) | Unmatched bitmap patterns: the `Patt` image is extracted to `<stem>_assets/patterns/<name>.png`, giving `pattern.kind: custom` (was skipped) |
| Bevel & Emboss (`ebbl`), Inner and Outer Bevel | Face or rim bevel: technique → profile (chisel soft → chamfer, note); contour → custom profile; gloss contour → named or custom; highlight/shadow colours and opacities; `use_global_light`; stroke emboss → every rim; outer bevel → outermost rim, else the face with a note | Direction Down → S80's Material `bevel.direction: down` (was light + 180°, which also moved the highlight colour) |
| Bevel & Emboss, Emboss and Pillow Emboss styles | An S83 `material` effect (layer colours) with the bevel above; Pillow Emboss with `bevel.direction: down` (an approximation, noted), as S83 asks | Were face bevels, which cannot raise outside the outline |
| Satin (`ChFX`) | `satin` (blend mapped as Pachislop's table) | |
| Inner Glow (`IrGl`) | S80's `inner_glow` effect (`source: edge` or `center` as the style says) | |
| Outer Glow, Drop Shadow, Inner Shadow (`OrGl`, `DrSh`, `IrSh`, `lmfx` arrays) | S80's `outer_glow`, `drop_shadow`, `inner_shadow` (px params; an Outer Glow with a light blend → `mode: add`; the shadows have no mode, a non-Normal shadow blend is a note) | Every instance of a CC multi-effect array is kept |
| Fill opacity (`iOpa`), Knockout (`knko`) | face `opacity`, `knockout` | |
| Scale Effects (`Scl `) | Multiplies every size | |
| Units (points, picas, mm, cm, inches) | px at the file's ppi, then % of the reference size for style members | |

### 6.10 The generated sRGB profile (`editor/titleart/interchange/IccSrgb.{h,cpp}`)

- ICC v2.1 display class, RGB/XYZ, D50 PCS.
- Tags:
  - `desc` "sRGB (JasperEngine)";
  - `cprt` "No copyright, use freely";
  - `wtpt` D50;
  - `rXYZ`/`gXYZ`/`bXYZ`: the Bradford-adapted sRGB primaries, s15Fixed16;
  - `rTRC`, `gTRC`, `bTRC`: one shared 1,024-entry `curv` of the IEC 61966-2-1 transfer, a single
    tag data block that all three offsets point to.
- About 2.6 KB, built once at startup from constants, byte-identical on every platform (integer
  rounding of the computed values).

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R13, D4) | `engine/tests/core/util/TitleArtLanguageTest.cpp` `MatchTable`: 32 rows. For example: `zh-TW` → `zh-Hant`; `zh-HK` → `zh-Hant-TW` when only that exists; `zh-CN` never → `zh-Hant`; `pt-PT` → `pt-BR`; `en-GB` → `en`; `sr-Latn` ≠ `sr-Cyrl`; `iw` → `he`; case and `_` variants. **Red control:** the same table against a prefix matcher fails at least 6 rows (recorded) |
| AC2 (R12, R14) | `TitleArtLanguageTest.CandidateOrder`: component > global > each preferred locale in order > project default > source; `setLanguage("")` returns to the device; an invalid tag changes nothing |
| AC3 (R2, R4–R7, D5) | `TitleTranslationsTest`: per-member inheritance; Language font default; `size_scale`; shaping language = code; `only_languages`; an exposed text override wins in every language; a sparse Translation writes only present overrides, and a later source `size` change reaches it. **Red control:** writing every Translation member (a test-only flag) breaks the last assertion |
| AC4 (R8–R11, D13) | `TitleFitTest`: shrink, condense then shrink, wrap balanced (Latin and the kinsoku table), vertical, the `min_scale` floor then `fits: false`, determinism, ≤ 9 measures; the extent equals the bounds of the rasterised face + rims within 1 px for round joins, and within 2 % for miter |
| AC5 (R18) | `tools/mcp/tests/test_title_language_checks.py`: each of the 7 S87 checks and the per-language runs of `glyphs_present` and `bake_fresh` fire on their seeded defects; every offered fix op applied through `edit_title` makes it PASS (except `needs_input` ones, which are checked for shape); a clean three-language document passes all; a monolingual document runs none |
| AC6 (R16, R50) | `test_title_language_bakes.py`: language sets are written only for differing languages (PNGs, sidecar, and for an animated fixture the `.de.anim.json` and flipbook pages with the German glyph count); the source sidecar's `languages` table; recipe `language`/`resolved_hash`; per-language frames; editing the German text stales only `.de` bakes; a monolingual document's bakes are byte-identical to the same document baked with S87 disabled (S80's bake on a build without the S87 members) |
| AC7 (R15, R27) | `engine/tests/core/titleart/TitleBakedLanguageTest.cpp` (headless GPU, S80's harness): `setLanguage("de")` swaps the texture after it loads; a frame capture of every frame in between shows the English title (no blank frame); `onLanguageReady` fires on the first German frame; the Japanese texture is never requested |
| AC8 (R17, contract decision 1) | `TitleRealtimeLanguageTest`: Realtime and Glyph Atlas titles switched to `de` and `ja` match their Baked goldens within S80's tolerance; the previous image stays until the new one is ready |
| AC9 (R14, R61) | `tools/mcp/tests/test_title_lua_language.py` (Play): `TitleArt.setLanguage`, `renderer.language`, `onLanguageChanged` once per change, `onLanguageReady` after it; `configure_game_view {language: "de-DE"}` selects German with no script code |
| AC10 (R24–R26, R64) | `test_title_export_languages.py` (`JASPER_SLOW_BUILD=1`, prebuilt Linux player): the shipped source sidecar's `languages` table matches the expected JSON and the language sets are in the build; `localization.languages: [en, de]` drops `ja` (its set absent, the shipped table without it, the project's sidecar unchanged) with `W-TITLE-LANGUAGE-DROPPED`; the player run with `LANGUAGE=de` (headless, `HostServicesLinux.cpp:115`) shows the German bake (screenshot equal to the bake within 2/255); a project without titles has no `TitleArtLanguage` symbol (`nm`) |
| AC11 (R20, R21, R23) | `editor/tests/titleart/StringsCsvTest.cpp`: export then import is an identity; BOM/no BOM, `;` and tab, CRLF/LF, quoted newlines and quotes; Unity headers `German (de)`; `unknown_key`, `unknown_language`, `add_languages`; `source_changed` → `needs_review`; empty cell unchanged; multi-document `file` routing; one undo step |
| AC12 (R22) | `StringsXliffTest`: the 1.2 export parses with tinyxml2 and has the fixed structure; 1.2 and 2.0 fixtures import with the state mapping; inline markup refused per unit; round trip |
| AC13 (R28, D11) | `editor/tests/titleart/PachislopMigrationTest.cpp`: every case of the six Pachislop test files of §4.1, ported, passes on constructed JSON (case names kept); "migrating twice equals once" for each step |
| AC14 (R28–R31, §6.8) | `PachislopImportTest.MatchesGolden` over `editor/tests/titleart/fixtures/pachislop/` (`v4_text_rims`, `v6_groups_masks_clip`, `v7_content_effects`, `v8_lighting`, `v10_rings`, `v11_all_kinds`, made by `tools/titleart/make_pachislop_fixtures.py` from the documented shapes, with an OFL font subset and an arithmetic PNG); each import equals its committed `.titleart` golden; the v4 file and its v11 twin import identically; each fixture's report lists exactly its seeded unmapped items. `tools/mcp/tests/test_title_import.py` renders them (a regression golden, not a claim of Pachislop parity: Pachislop is never run) |
| AC15 (R29, R57) | `test_title_import.py`: fonts and images extracted; re-import writes nothing new; builtin font ids map to the S84 pack; a WebP asset gives `image_format_unsupported`; one undo removes every file; a v3 file gives Pachislop's message; a v12 file gives `pachislop_version_newer` |
| AC16 (R32, §6.9) | `editor/tests/titleart/PsStyleConvertTest.cpp`, one case per §6.9 row. Fixtures come from `tools/titleart/make_psd_fixtures.py` (an independent Python writer of `lfx2`/`Lefx` descriptors, ASL and PSD from the Adobe document) plus Photoshop-made samples (the PSD/ASL test files of ag-psd 33.0.0's MIT repository at the pinned tag, or files the owner provides; recorded in Deviations if neither exists). The improvements are asserted: alpha stops, `mid`, rim opacity, custom pattern image, `lmfx` arrays. **Red control:** a Pachislop-literal mode (test-only) fails the alpha and `mid` assertions |
| AC17 (R33) | `PsdImportTest`: groups, pass-through → Normal + note, clipping, masks, text layer → Text (font name, size, colour, tracking, leading), the 13 adjustment records, fill layers → `fill`, 16-bit, PSB, CMYK refused for layers but styles read |
| AC18 (R34, D12) | `SvgImportTest`: `split: groups` and `paths` render, composited, equal the unsplit render within 1/255; `auto` mode picks `shape` for a one-colour icon and `artwork` for a multi-colour logo; text/image/filter elements reported |
| AC19 (R36–R38, R40, D8) | `editor/tests/titleart/TitlePngExportTest.cpp`: trim, padding, round_to, pot and alpha_threshold boxes; bleed: every alpha-0 pixel within 64 px holds its nearest visible pixel's colour; a white title on transparent, 4× box-downsampled and composited over mid grey, dips ≤ 1/255 in luminance at the edge. **Red control:** `alpha_bleed: false` dips ≥ 40/255. Also: premultiplied values; 16-bit equals 8-bit within 1 LSB; chunks (`sRGB`, `gAMA` 45455, `cHRM`, `pHYs`) and no `iCCP`; two exports byte-identical; 1 and 8 threads byte-identical; the decode (pngdiff extended to 16-bit) equals the pre-encode pixels; parts recombine to the composite within 2/255; a 3-language parts export with `zip: true` lists only the ZIP, whose entries are the R40 names |
| AC20 (R39, D10) | `PsdWriterTest.RoundTrip8/16` (our reader: layers, groups, clipping, masks, blend keys, merged image). `tools/mcp/tests/test_title_export_psd.py` reads every export with `tools/mcp/psdread.py`, an independent standard-library reader written from the Adobe document (not from our C++). It checks the header, section lengths, layer count, names (`luni`), bounds, blend, clipping, visibility, group nesting, RLE and ZIP-with-prediction decode equal to the per-layer renders, the merged image equal to the PNG export, the ICC resource parsing as §6.10, and byte determinism. **Red control:** one flipped length byte → psdread fails with the offset. Manual (evidence screenshots): the 8- and 16-bit files open in GIMP 2.10 and Krita 5 with the layer tree shown; the owner opens them in Photoshop |
| AC21 (R41) | `test_title_export_svg.py`: the SVG of a document with text, rims (round and miter), a linear gradient and a shape mask, rendered by LunaSVG through the editor's test helper, matches `export_title png` of the same document within max channel 48 and ≤ 0.5 % of pixels over 8; unrepresentable items are listed by layer; `strict` fails; transforms are applied |
| AC22 (R42) | `TitleSheetExportTest`: sheet cells registered and named `@WxH`; atlas packing deterministic, with padding and extrude; JSON valid against the shapes of §6.6; `duration`; per-language sheets; 1,024-frame cap |
| AC23 (R43) | `test_title_export_ktx2.py`: the KTX2 decodes with `cook-textures`' decode path (`TextureCli::decode`) and matches the PNG within that family's codec tolerance (S14) |
| AC24 (R44, §6.10) | `IccSrgbTest.ProfileIsValid`: the header, tag table and D50; matrix columns sum to D50 within 1e-4; the TRC matches IEC 61966-2-1 within 1/65535; bytes identical on Linux, Windows and macOS (CI); a PSD with an Adobe RGB profile imports with `color_profile_ignored` |
| AC25 (R59, §6.3) | `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py`, `gen_tool_docs.py --check`; `title_art` stays hidden until `tool_groups` enables it (S80's test, extended with the new parameters) |
| AC26 (R62) | `tools/mcp/tests/test_title_cli.py`: batch import of 3 files and batch export to 2 formats; exit codes 0/1/2; `--json`; strings and Pachislop import spawn no editor process (the harness checks the child list); render formats start exactly one headless editor |
| AC27 (R51–R58) | `tools/mcp/tests/test_title_designer_languages.py` (xvfb, isolated editor, `find_ui_item`): the switcher lists status; editing in German shows the override bar and Revert works; a Translations cell edit is one undo step; the Edit Languages dialog adds `pt-BR`; the import dialog and Import Report; the export dialog writes the files. Screenshots in `docs/specs/evidence/S87/` |
| AC28 (R48, R49) | `tools/perf/title_localisation_bench` on server A and the S13 phone class; thresholds as R48–R49, reported in this spec with the measured table |
| AC29 (R50) | Monolingual documents: no sidecar `languages` table, no check output, identical bakes (AC6); a Compile export of a project with no titles is byte-identical to the base (S34 size tools) |
| AC30 (R60) | `tools/mcp/eval` case "translate this title to German and Japanese": an agent with only the guide doc finishes in one `edit_title` batch, plus fixes, with `check_title` all PASS (this proves the doc is enough, not the guide) |
| AC31 (R47) | `editor/tests/titleart/InterchangeFuzzTest.cpp` under `SANITIZE=address,undefined`: 2,000 truncated and mutated PSD, ASL, CSV, XLIFF, `.pslot.json` and SVG inputs; no crash or leak, each fails with a message |

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

- **Depends on.**
  - S80 (§6.1 members, §6.2 grammar, `TitleArtOps`, `TitleFrame`, Outputs, bakes, float readback,
    missing-glyph report, tools); S81 (component, Lua object, re-render, Glyph Atlas, subsetting,
    the sidecar and `TitleArtBakeInfo`); S82 (Designer toolbar overflow and `addPanel`, S82
    §6.8; font picker); S83 (`PachislopFxMap`, the `material` effect); S84
    (`.titlelook` capture, library ops, font pack, `pachislop-map.json`, reflection maps); S85
    (`TitleAnimPlan`, sampling, `<file>.anim.json`); S86 (`TitleDocument3D::fromPachislop`).
  - S03 (atomic writes), S04 (GUIDs, asset refresh), S14 (cooker), S24 (undo), S25 (commands,
    settings search), S27 (Recordings), S31 (`call`, `SchemaArgs`), S45 (`@WxH` slicing, soft:
    sheets work as plain textures before it lands).
- **What the other specs provide (all in their text now).**
  - S80 (§6.15's hook table): sparse Translation objects in the canonical writer (a sparse kind of
    `TitleArtSchema`, D5); the reserved members `source_language`, `languages`,
    `string_namespace`, `dpi`, `import_notes`; `create.from`; `RenderRequest::overrides` for a
    resolved language, `TitleFrame` and the bake writer's per-language sets; `bleedAlpha(reachPx)`
    (8 for bakes, up to 64 here, R37); the check registry with S87's ids listed; `export_title`'s
    `format` and `options` and `render_title.options.languages`; `variants` released (D15). S87
    adds the `output_id_is_language` rule to normalize itself.
  - S81:
    - the sidecar gets the optional `languages` table (§6.6), and the Baked binding switches sets
      (R27);
    - `TitleArtLanguage` sits in its always-in-core part (R26);
    - "Characters used by the document" uses shipped languages only (R25);
    - Glyph Atlas is keyed by language;
    - the component's `language` field and `onLanguageReady`; the static `TitleArt.setLanguage`
      and `onLanguageChanged` (§6.4).
  - S85: `TitleAnimPlan` and frame sampling callable per language. S85's flipbook writer and
    S87's atlas writer share `stb_rect_pack` order rules (R42).
  - S84 (same wave, S84 owns them): `pachislop-map.json`, embedded in the editor, read by the same
    `PachislopPort` member mapping S87's `PachislopMapper` calls (S84 R20; `port-pachislop --check`
    keeps it in sync); `TitleLibraryApply::lookFromStyle` (capture from a style instead of a layer)
    for `import_ps_styles`.
- **Merge hotspots.**
  - `editor/main.cpp` and `editor/cli/CommandLine.cpp` (subcommands, help);
  - `editor/settings/SettingsRows.cpp`;
  - `editor/Exporter.cpp` and `ExporterPrebuilt.cpp` (filtered sidecars, dropped languages, start
    values);
  - `editor/Generator.cpp`;
  - `NOTICE.md`;
  - `tools/mcp/pngdiff.py` (16-bit decode, premultiplied compare);
  - the `title_art` tool files of S80 (`editor/mcp/tools/TitleArtTools*.cpp`);
  - `docs/mcp/guide/title-art.md`;
  - `docs/unity-glossary.md`.

## 9. Risks & defaults

- **No Photoshop on the build servers.** Default:
  - generated fixtures from an independent Python writer;
  - ag-psd's MIT sample files at the pinned tag;
  - an owner check in Photoshop as manual evidence (AC16, AC20).
- **EngineData variety in real PSDs.** Default: font size, name, colour, tracking and leading of
  the first style run, plus paragraph alignment. Anything else is a note.
- **Linear vs sRGB blending in recombined PSDs.** Default: the true composite is embedded, with one
  warning (R39). Writing sRGB-blended layers would make the layers lie.
- **Miter spikes and fit.** Default: 2 % tolerance. A miter-joined rim that still overflows is
  caught by `text_overflow` with fixes.
- **Kinsoku is a short table, not UAX #14.** Default: R10's table. Titles are short. A wrong
  break is visible on the canvas and fixed with a manual `\n` in that language.
- **CSV formula injection in translators' spreadsheets.** Default: content is never altered (an
  apostrophe prefix would corrupt "-50%" titles). The guide warns that CSVs from untrusted sources
  should be opened with formulas disabled. Import never evaluates anything.
- **WebP in Pachislop projects.** Default: a note; the user re-saves the image as PNG. No WebP
  decoder is added for one importer.
- **Device language differs from a game's own language menu.** Default: `TitleArt.setLanguage`
  wins over the device. Persisting is the game's job (`PlayerPrefs`, §6.4).
- **Hundreds of languages × outputs bloat bakes.** Default: languages identical to the source share
  its PNG (R16), `bake: false` per language, and `localization.languages` limits what ships.
- **S86 lands after S87's Pachislop import.** Default: R30's verbatim member and Reimport 3D.
- **ag-psd licence or files differ at the pinned tag.** Default: stop and record it in Deviations.
  The descriptor parser is then written from the Adobe document.

## Milestones

- **M1** Document members, `TitleTranslations`, `TitleFit`, `TitleArtLanguage`, the ops, `get_title`
  language view, the language checks, `render_title` sheets (R1–R13, R18, R19); AC1–AC5.
- **M2** Per-language bakes, the sidecar `languages` table, the Baked switch, Realtime/Glyph Atlas re-shaping, Lua,
  settings, export of languages (R14–R17, R24–R27, R61, R64); AC6–AC10. Needs S81.
- **M3** Strings CSV/XLIFF and the Translations panel (R20–R23, R54); AC11, AC12.
- **M4** PNG export (encoder, bleed, chunks, ICC), parts, sheets, atlases, sequences, KTX2, ZIP
  (R35–R38, R42–R44); AC19, AC22–AC24.
- **M5** PSD reader and writer, `psdread.py` (R39); AC20.
- **M6** Photoshop styles and layered PSD import (R32, R33); AC16, AC17. Needs S83, S84.
- **M7** Pachislop import (R28–R31); AC13–AC15. Needs S83, S84 (S86 for 3D).
- **M8** SVG import and export (R34, R41); AC18, AC21.
- **M9** Designer UI not covered above, CLI, docs, budgets, benches, fuzzing (R51–R53, R55–R60,
  R62); AC25–AC31.

## Deviations

1. **Reconciled: S85's files are `<file>.anim.json` and `<file>.<animation id>.flip<N>.png`** (S87
   said `.titlebake` and "parts atlas", which no spec defines); a language set inserts `.<code>`
   after `.titleart` for every derived file.
2. **Reconciled: S84's map is `pachislop-map.json`** read through S84's `PachislopPort`;
   `PachislopMapper` keeps only the structure mapping.
3. **Reconciled: `bevel.direction` is S80's Material member** (S83 asked for it, S80 owns it); the
   Photoshop Outer Glow, Drop Shadow, Inner Shadow and Inner Glow map to S80's types.
4. **Reconciled: `export_title.format` and `options` are S80's hooks** (S86's `glb` is in the same
   enum), and `render_title`'s `languages` is `options.languages`; S87's budget row is ≤ 195
   characters (was about 410).
5. **Reconciled: the core-part share is 6 KiB of the program's 60 KiB** (S80 §6.15), not of
   S81's former 40.
6. **Implementation: `create.from` importers register by kind and are chosen by content.** S80's
   `registerCreateImporter(extension, fn)` became `registerCreateImporter(kind, sniffer, importer)`
   with `TitleCreateRequest {sourcePath, documentPath, projectPath, assetsPath}` and
   `TitleCreateResult {doc, kind, files, notes, warnings}`: an import needs its destination (for
   `<stem>_assets/`) and returns the files it writes, and §6.2 detects the kind by content, not by
   extension. `TitleArtDocument::create` and `commit` take those files and write them with the
   document in one `WriteFilesCmd`, so one undo removes every file (R57). Ops that write files
   (`import_svg`, `apply_ps_style`, `import_ps_styles`) put them in the new `TitleOpsResult::files`;
   ops that report counts use `TitleOpsResult::reports` (by op index). `TitleOpsOptions` carries
   the document's paths, the project default language and the font resolver
   (`TitleArtDocument::edit` fills them).
7. **Implementation: the renderer draws the resolved language.** `RenderRequest::language` makes
   the snapshot copy the document resolved for it (`resolveTitleLanguage`), then the overrides,
   then fit (`applyTitleFits`); a monolingual document without fit takes none of these steps. So
   bakes, the Designer's canvas, previews, Realtime and Glyph Atlas share one path, and the S87
   members feed no content hash themselves (`HashScope::Identity`): the resolved copy's members do.
   A resolved copy has `languages: []` and `source_language` = the resolved code, so resolving it
   again changes nothing and its canonical text is that language's alone: the bake's
   `resolved_hash` is the SHA-256 of it.
8. **Implementation: a Translation always holds `text` ("" = no translation).** The sparse kind
   writes `text`, `status`, `source_hash` always (R63); an empty `text` is a Translation without
   text (R2's "no `text` member"), which the checks report and the source shows.
9. **Implementation: translation keys are canonical BCP 47 with `-`.** S80's map-key rule for
   language codes takes letters, digits and `-`; ops and the strings importers canonicalise
   (`pt_br` -> `pt-BR`), and normalize canonicalises keys and drops a duplicate after
   canonicalising with a warning.
10. **Decision: a Language's font applies to translatable text layers only.** A brand word
    (`translate: false`) keeps its font and text in every language.
11. **Decision: sheets, atlases and sequences take S85's poses through a registered frame source.**
    S85 implements its sampling in parallel; `registerTitleAnimFrameSource {animations, pose}`
    (`editor/titleart/interchange/sheet/TitleSheetExport.h`) is the seam S85 registers (its
    animations with fps, duration and loop, and the document posed at a time for the export's
    language). Until it does, a document with an `animation` member answers `not_available`
    naming S85. The grid, packing, JSON shapes, names and per-language runs are S87's.
12. **Deviation (Designer):** the override marker of R52 is drawn in a **Language** section of the
    text layer's page (each override with the blue bar, Revert to Source and Apply to Source in its
    menu), not on each Inspector row; edits in a language are routed to its Translation by the
    designer state (`routeTitleLanguageOps`, after S85's edit router). The Designer's Export... and
    Photoshop style imports are toolbar items (S82's toolbar hook) rather than File and Layer menu
    entries; the Edit Languages font field takes an assets-relative path (the S82 picker is not
    reachable from a toolbar item's popup). Drag-and-drop onto the Project window is not wired: the
    import dialog is Assets > Import Title Art....
13. **Deviation (Inspector):** the Title Art Renderer's Language is a text field (empty = Auto)
    whose help lists the languages the title offers and the one it shows, not a dropdown.
14. **Implementation: `dither: false`** renders 16-bit and rounds to 8 bits on the CPU (an
    undithered output); the output pass has no dither switch.
15. **Implementation: the KTX2 format** writes the PNG to the project's
    `.jasperengine/cache/titleart/ktx2/` and cooks it there with its own cache (S14's cooker reads
    files).
16. **Implementation: the CLI's render formats** run through `CallCommand::runCalls` (one session,
    one one-shot headless editor for the batch). An `--out` outside the project is written inside
    `Recordings/Title Art/.cli/` by the editor (MCP writes only inside the project) and moved.
17. **Implementation: the project default reaches games through the generated main**
    (`TitleArtLanguage::setProjectDefault`), and Play through `Project::applyPresentationSettings`
    (which also resets `TitleArt.setLanguage` to the device). The prebuilt Android package's
    `presentation` is manifest data and needs no language.
18. **Implementation: S84's member mapping.** S84's `PachislopPort` and `pachislop-map.json` are
    not in this tree (S84 works in parallel); see the Pachislop import's own deviation below.
19. **Implementation (Pachislop import): `PachislopMembers` applies S84 §6.7.1's member rules from
    the schema** and reads `pachislop-map.json` (fields, fonts) from the library directory
    (`JASPERENGINE_TITLEART_LIBRARY_DIR`, else `<editor exe>/titleart`) when it exists, because S84's
    `PachislopPort` is not in this tree. It moves to `PachislopPort` when S84 lands; S84 must stage
    the font pack in that directory.
20. **Implementation (Pachislop import): the mapper writes the document tree** and then runs
    `TitleArtOps::applyCommitInvariants`, rather than one op per field. Effects created by the
    migration steps carry only the parameters they set, the rest coming from S83's defaults on
    mapping; their ids are deterministic.
21. **Not ported from Pachislop:** `outwardBands.test.ts` (it tests S86's mesher),
    `contentEffectsOfLook` (S84's look conversion) and the generator-preset case. The comp's 3D
    scene and the 2D reflection-map presets each become one "approximated" note; trace smooth and
    despeckle are multiplied by 100 (fraction to percent).
22. **Implementation: the v4 glyph-scale compensation uses `TitleTextLayout`**, not Pachislop's
    layout, so the v4/v11-twin fixture has no scaled characters and the compensation has its own
    test. `v11_all_kinds` embeds the whole Oswald OFL font (272 KB), not a subset.
23. **Implementation: test names.** AC5 is `tools/mcp/tests/test_title_language_checks.py`; AC6 is
    `test_title_languages.py::test_language_bakes`; AC11 and AC12 are
    `editor/tests/titleart/TitleStringsTest.cpp`; AC8 is
    `engine/tests/core/titleart/runtime/TitleRealtimeLanguageTest.cpp`.
24. **Not done: AC7's per-frame capture.** `TitleBakedLanguageTest` does not exist. The Baked switch
    is covered in Play by `test_title_lua_language.py` (not ready, then ready; `onLanguageReady`
    once), which cannot see every frame or prove the Japanese texture is never requested.
25. **Partly done: AC10.** `TitleLanguageShippingTest` covers the plan (dropped files, the shipped
    table, `W-TITLE-LANGUAGE-DROPPED`, the project's sidecar unchanged);
    `test_title_export_languages.py` against a prebuilt player was not written.
26. **Partly done: AC22 until S85 lands.** `TitleSheetExportTest` covers the writers and the frame
    times over frames made by the test; rendering frames needs S85's `registerTitleAnimFrameSource`,
    so the job end to end is unverified here.
27. **Deviation: AC28's bench is `TitleLocalisationBenchTest`,** a unit test, not
    `tools/perf/title_localisation_bench`. Measured on build box 2 (15 workers), never on a phone:

    | What | Result |
    |---|---|
    | `TitleArtLanguage::match` | 0.515 us |
    | `TitleArt.setLanguage`, 100 listeners | 0.0103 ms |
    | CSV 1,000 rows, export / parse | 2.22 / 0.79 ms |
    | XLIFF 1,000 units, export / parse | 2.19 / 2.71 ms |
    | PNG 4,096 x 2,048, 8-bit / 16-bit | 48.0 / 61.6 ms |
28. **Decision: `isLanguageReady` answers for the language just asked.** The system sees a change
    at its next update, so the call resolves the candidates itself when one is pending and reads
    false right after `setLanguage`; `getResolvedLanguage` still changes at that update.
