# S84 — Title Library: Looks, Color Schemes and Templates for any style of title

**Wave / Depends on.** Title Art program (spec-first), **wave 6C** (with S85, S87; program graph in
§8, program rules in S80 §6.15). Depends on **S80** (the `.titleart` document
v1, `TitleArtOps`, the renderer and its readback, `check_title`, the `title_art` MCP group, the
JSON migration framework); **S83** for the effects and generators most looks use (a look whose
effect or generator types are not registered yet is listed as unavailable, R6, so S84 can land
before S83 is complete); **S86** for the 3D looks and 3D templates (same rule). S82 draws the
Library panel on this spec's APIs; S81 binds the runtime Color Scheme switch to Lua; S87 adds
per-language samples to templates. Owner area:
- `editor/titleart/library/` (library store, apply, capture, search, previews, lint, calibration,
  baker, CLI);
- `engine/core/titleart/ColorScheme*.{h,cpp}` (the scheme data and the recolouring algorithm, needed
  by Realtime titles in players);
- `editor/resources/titleart/` (the built-in library data, fonts, environments, thumbnails);
- `tools/titleart/` (the Pachislop converter, asset fetcher, docs generator, review records);
- the Library section of `docs/mcp/guide/title-art.md` and the generated
  `docs/mcp/guide/title-library.md`.

## 1. Goal

A title of any style starts from a library: 195 **Looks** (a complete restyle of one layer),
33 **Color Schemes** (a recolour by element role that keeps the structure) and 73 **Templates**
(whole starting documents with text placeholders), in fourteen style families from pachislot and
casino to fantasy, horror, cartoon, retro arcade, anime, sports and minimal. The three are
independent axes with a written keep-and-replace contract, so "I like this shape, try other colours"
never destroys work. Pachislop's 104 looks, 11 colourways and 37 templates are ported by a committed
converter with per-item provenance and review records, with every franchise and brand name removed;
the rest are new. Fonts (32 OFL families, Latin and Japanese display faces, from `google/fonts`) and
the reflection environments (S86's four Poly Haven CC0 HDRIs plus four S84 adds for the new
families) come from their upstream sources and are copied into a project only when a document uses
them. Users save their own looks, schemes and templates per project or in
a personal library shared across projects. Every built-in item passes `check_title` and the ported
style lint on Latin, kanji and digit sample texts in CI, and the checks' thresholds are calibrated
against the library. Agents read the library in tiers (a 150-token catalogue, rows, one item, or
looks rendered on their own text) and apply items with the same operations the Title Designer uses.
None of it adds a byte to a player that does not ship a Realtime title with an exposed Color Scheme.

## 2. Scope

**In scope**
- File formats: `.titlelook` (Look), `.titlecolors` (Color Scheme), `.titletemplate` (Template: a
  `.titleart` document plus a `template` member), version-tagged JSON with migrations.
- The ops `apply_look`, `apply_color_scheme`, `apply_template` (names reserved by S80 §6.2) and the
  library ops `save_look`, `save_color_scheme`, `save_template`, `copy_library_item`,
  `delete_library_item`; the keep-and-replace contract; capture of a layer into a look.
- The runtime recolour (`engine/core/titleart/ColorSchemeApply`) and the document member
  `color_schemes` that carries exposed schemes into players.
- The Pachislop port: converter (`tools/titleart/pachislop-port/`), mapping, renames, family
  assignment, review records, the port's red controls.
- Fourteen families, 91 new looks, 22 new color schemes, 36 new templates, each specified below.
- The font pack (32 families, OFL, from `google/fonts` at a pinned commit), four Poly Haven CC0 HDRIs
  added to S86's environment set, their notices, staging, import on use.
- User library: project and personal scopes, favorites, recents, sharing.
- Thumbnails: the baker CLI, committed baked thumbnails with recipes, the live preview service.
- Search (bilingual names, synonyms, families, effect tags), `list_title_library` tiers, the
  generated catalogue doc.
- The quality gate: the ported style lint as `check_title`'s `style_lint` check, the CI library
  gate, threshold calibration, denied terms.

**Out of scope**
- The Library panel's drawing, layout, hover intent, tile sizes and the Save As dialogs' widgets
  (S82; this spec defines what they call and what they show).
- The effects, generators and 3D materials themselves (S83, S86). This spec lists what the looks
  need from them (§6.8.1).
- Font import of `.woff`/`.woff2`, the Google Fonts browser and system fonts (S82); font subsetting
  and export of fonts and environments into games (S81, S86).
- Reveal and animation presets (S85), per-language template variants beyond the en/ja samples (S87),
  Photoshop `.asl` import into looks (S87).
- A Search palette (S25) provider for looks: `SearchKind` is a closed enum
  (`editor/search/SearchEngine.h`); the Title Designer's own search covers the library (R33).
- Never taken from Pachislop: the Japan Color 2011 ICC profile (`public/icc/`), its font and HDRI
  binaries and reflection JPGs (fonts and HDRIs come from upstream, §6.11, §6.12), its golden
  reference PNGs as goldens (they are review aids only, §6.7.4; S86 AC21 commits the 40 3D ones as
  its parity fixtures), and any franchise, brand, product
  or creator name (§6.7.3).

## 3. Current state (`feature/title-art` 3ffafdfaa, read)

- **No title code yet.** `engine/core/titleart/` and `editor/titleart/` do not exist. S80 is in
  draft (`docs/specs/S80-title-art-core.md` §6.1, §6.2 written): the document is canonical JSON with
  every member written; Text, Shape, Vector and Image layers carry `style` (face, `rims` innermost
  first, `knockout`, `erode`) and `part_effects`; part keys are `face`, `rim:<id>` with positional
  aliases `rim#<n>` "which Looks store (S84)"; every layer has an informational `look {id,
  color_scheme}` "S84 writes"; library fonts and reflection maps "are copied into the project on
  first use (S84)"; exposed parameters have a `color_scheme` type that "applies a Color Scheme to the
  document (S84)"; `apply_look`, `apply_color_scheme`, `apply_template` are reserved op names.
- **Fonts.** `.ttf`/`.otf` are Font assets with a sidecar `.meta` (`editor/asset/AssetTypes.cpp:32`).
  The engine's built-in faces are Roboto and Noto Sans Arabic (`engine/core/util/FontRegistry.cpp`
  `ensureBuiltIns`); the editor UI loads Roboto, JetBrains Mono and Font Awesome
  (`editor/resources/fonts/`). No CJK font exists anywhere, and `NOTICE.md` has no font rows.
- **Environments.** `.hdr` is a Texture extension (`AssetTypes.cpp:29`) with import type HDR
  (`editor/texture/TextureTypeInference.cpp:257`). The material preview's environment is Poly Haven's
  `lythwood_room`, tone-mapped and embedded (`editor/render/preview/PreviewEnvironment.h:1-3`), with no
  `NOTICE.md` row. `IBLEnvironment` prefilters LDR cubemaps (`engine/core/texture/IBLEnvironment.h`).
- **Library precedents.** Pose templates: built-ins embedded at configure time
  (`CMakeLists.txt:403-422`), user templates in `<project>/pose-templates/`, saving is a
  `WriteFilesCmd` and deleting a `RemoveFilesCmd` on the project history (`editor/pose/PoseTemplates.h`).
  Curve presets and swatches have built-in, user (`<settings dir>/curves/`) and project
  (`<project>/EditorSettings/Curves/`) scopes (`editor/widget/CurvePresets.cpp:55-61`), the settings
  directory being `AppSettings::getConfigDirectory()` (`editor/AppSettings.h:369`). Project templates
  embed every file of `editor/resources/templates/<id>/` (`editor/EditorToolsSources.cmake:55-61`).
- **Thumbnails.** `PoseThumbnails` renders lazily, `MAX_IN_FLIGHT = 2`, LRU of `MAX_ENTRIES = 160`
  (`editor/pose/PoseThumbnails.h:86-87`); Project window thumbnails are 128 px
  (`editor/Configs.h:7`) cached by `Project::getThumbnailPath` (`editor/Project.h:2257`).
- **Search and tiers.** `search::normalize` and `fuzzyScore` (`editor/search/SearchScorer.h:24,36`);
  `list_ui_effects` omits parameters above `FULL_ENTRIES_MAX = 12` matches
  (`editor/mcp/tools/UIEffectTools.cpp:43`); embedded guide docs split at 16 KB / 6 KB
  (`editor/mcp/tools/DocsTools.cpp:53-55`); `docs/mcp/guide/html-ui-effects.md` is generated by
  `tools/htmlui/gen_effects_docs.py`.
- **Node tooling precedent.** `tools/htmlui/package.json` and `render-reference.mjs`: pinned
  dependencies, "host only: run on purpose, never in CI or Docker".
- **Notices.** `tools/ci/check-notices.py` checks only directories under `libs/`, `engine/libs/` and
  the shader compiler's `libs/` (`VENDOR_ROOTS`), and its licence-name pattern does not match
  `OFL.txt`; the install copies `NOTICE.md`, `LICENSE` and `licenses/<library>-LICENSE`
  (`CMakeLists.txt:1730-1752`).
- **Comparison.** `engine/tests/support/ImageCompare.h` (pixel, block and histogram modes);
  `tools/mcp/pngdiff.py` compares straight channels; the MCP suite's goldens are drawn by llvmpipe
  (`tools/mcp/gpu_lane.json`); golden changes need a ledger row in the HTML UI
  (`tools/htmlui/check_golden_ledger.py`).
- **Provenance in `.meta`.** S36 R8 specifies a `provenance` section; it is not implemented
  (`editor/asset/AssetMeta.h` `isKnown` has no such section).

## 4. Reference

### 4.1 Pachislop (`/tmp/pachislop-analysis/repo`, commit `1c32502dc9fe`, 2026-11-02; by SafEight, used with the author's permission)

| File | What we take | What we change |
|---|---|---|
| `src/presets/types.ts` | `StylePreset` (look), `PresetPartKey` positional parts, `refSize`, `Template`, `Colourway` | Bilingual names, families, tags, font pairs, `applies`, `requires`, lint and check allowlists, provenance; companions become generator layers |
| `src/presets/factory.ts`, `factory3dMore.ts`, `factory3dSetA.ts`, `factory3dSetB.ts` | All 104 looks (`FACTORY_STYLES`), by evaluating them | Renamed and refamilied (§6.7); sparse JSON |
| `src/presets/colourways.ts` | The 11 colourway tables, role classification (`ringPaints`, `DARK_LUMA = 0.18`), paint mapping (`paint2D`, `paint3D`, `keepAlpha`), light ramps (`LIGHT`, `onRamp`, `WHITE_CORE`), `spectrum`, `tintOrb`, `recolorLight` | One C++ implementation in the engine (players need it for exposed schemes); generalised to any scheme file |
| `src/presets/apply.ts` | `applyPreset`, `placeLook`, `transferEffects` (px × k, handles relative), `presetFromLayer` | Ops on the S80 document; a recorded owner instead of symbol markers; "keep color scheme" |
| `src/presets/templates.ts` | The 37 templates (`TEMPLATES`), by evaluating `build()` | Flattened into `.titletemplate` documents; texts as exposed parameters with en/ja samples |
| `src/presets/lint.ts` | The seven rules (its no-op rule split into two checks here) and their thresholds | Rule texts rewritten without third-party names; a `check_title` check with fix ops; run over the library in CI |
| `src/presets/user.ts` | Shape validation before migration, fresh ids on import | Lenient read with warnings (S80 codec rules) instead of dropping entries |
| `src/ui/library/thumbs.ts`, `LibraryPanel.tsx` | Content-keyed thumbnails, isolated renderer, 300 ms settle, keep-old-only-if-same-layer | Baked first tier, disk cache, per-frame budget |
| `tests/harness/cases.ts`, `tests/golden/compare.ts` | Registry-driven cases, render twice, premultiplied compare (max 48, mean 0.25, 0.1 % over 8) | CI on known renderers, red controls, ledger |
| `tests/golden/refs/look/*.png`, `refs/template/*.png` | Visual reference beside our renders in the port review (§6.7.4) | Never committed, never goldens |

Not taken: `public/icc/**`, `public/fonts/**`, `public/hdri/**`, `public/reflections/**` (fonts and
HDRIs come from upstream; Playfair from `google/fonts`' variable originals, not Pachislop's modified
instance), `public/favicon.svg`, `docs/*.png`. Pachislop's third-party parts keep their own notices
(Patchy warp, three.js tone map: S80).

### 4.2 Engine precedents to copy

- `editor/pose/PoseTemplates.{h,cpp}` and `PoseTemplatesBuiltin.cpp`: embedded built-ins, a user
  folder, undoable save and delete.
- `editor/widget/CurvePresets.{h,cpp}`: built-in, user and project scopes, `loadAll`.
- `editor/pose/PoseThumbnails.{h,cpp}`: lazy GPU thumbnails with an LRU.
- `editor/mcp/tools/UIEffectTools.cpp` and `tools/htmlui/gen_effects_docs.py`: tiered listing and a
  generated catalogue doc with aliases.
- S36 (`docs/specs/S36-sound-library.md`): editor-only content staged with the install, `library:`
  references with no GUID until import, a licence gate with denied terms, one squashed commit.

## 5. Requirements

### Functional — library files and references

- **R1. Look** (`.titlelook`, §6.1): one JSON object, `"format": "jasperengine.titlelook"`,
  `"version": 1`; identity (`id`, `revision`), bilingual `name` and `description`, `family`, `tags`,
  `synonyms`, `dimension` (`2d`, `3d`), `applies` (`style` or `effects`), `ref_size`, an optional font
  pair, then the restyle: `style` (2D) or `style_3d` (3D, S86's member), `effects`, `part_effects`
  keyed by positional aliases (`face`, `rim#<n>`, `band#<n>`, `bevel`, `side`), `generators`,
  `group_effects`, `lighting` (3D), `links` (material link names), plus `lint_allow`, `check_allow`
  and `provenance`. A look holds no layer or element ids and no document data: it applies to any
  document.
- **R2. Color Scheme** (`.titlecolors`, §6.2): `"format": "jasperengine.titlecolors"`, `"version": 1`;
  identity and search members as R1; `paints` by role (`face`, `dark`, `accents` inner to outer,
  `base`, `bevel_rim_3d`, `side_3d`), each a 2D fill, an optional metal ramp, a 3D material and a
  bevel shade; `face_glow`, `glow`, `glow_gradient`, `shadow`, `glint`, `light_ramp` (4 to 7 colours,
  luma ascending), `orb`, optional `pattern` marks and `pattern_ground`, `spectrum` (the rainbow
  behaviour), `swatch` (UI colours).
- **R3. Template** (`.titletemplate`, §6.3): a complete S80 `.titleart` document (every member
  written) plus a top-level `template` member: identity and search members as R1, `placeholders`
  (the document's exposed `text` parameters with `sample.en` and `sample.ja`, `role`, `max_chars`),
  `canvas_note`. A template's layers carry `look {id, color_scheme}` of the items used to build it.
- **R4. References.** A built-in item is `<family>/<slug>` (`metal/classic-gold`); a project item
  `project/<slug>`; a personal item `user/<slug>` (`[a-z0-9-]{1,48}` slugs; `project` and `user` are
  not family names). Schemes have flat built-in ids (`gold`, `neon-pink`). Unknown references fail
  with `library_item_not_found`, the three closest ids (`fuzzyScore` over id and names) and the
  `list_title_library` call that finds more. Built-in ids never change once released; a renamed or
  removed item keeps an `aliases` entry in `manifest.json` that resolves with a `deprecated` warning.
- **R5. Versions.** Looks and schemes use S80's JSON migration framework with their own kinds
  (`TitleLookMigrations`, `TitleColorsMigrations`, v1 tables empty) and S80's rules (fill the old
  behaviour, idempotent steps, a fixture per version). They are **sparse**: a member equal to the
  default of the file's format version is omitted, arrays are written whole with sparse elements.
  Therefore **changing the default of any member a look or scheme can carry (S80, S83, S86 members)
  bumps `TITLE_LOOK_VERSION` / `TITLE_COLORS_VERSION` with a step that writes the old default into
  files that omit it** (`TitleLookMigrationsTest.DefaultChangeKeepsOldLooks`). Unknown members and
  unknown effect, generator or pattern types survive a rewrite. Templates follow `TITLE_ART_VERSION`;
  their `template` member is versioned by `template.version` (1).
- **R6. Availability.** When the library index loads, each item's `requires` is computed (not
  stored): every effect, generator, pattern, texture and 3D material type it uses, and `s59` for
  `dimension: 3d` or 3D template layers. An item whose requirement this build lacks is listed with
  `available: false` and a reason (`needs effect drips (S83)`), cannot be applied (`library_item_unavailable`),
  and is excluded from the gate with its count reported (R36).

### Functional — applying

- **R7. `apply_look`** `{op, layer, look, apply_font: false, keep_color_scheme: true}` applies a look
  to one layer by the contract of §6.4 and the algorithm of §6.5.1: the restyle members are
  replaced; source, text, transform, blend, masks and tree position are kept; px parameters are
  scaled by `layer size / ref_size`; positional part keys resolve to the new style's parts; the
  look's generators become generator layers that follow the layer (owned, in an auto group when the
  look has group effects or outer generators); a 3D look on a 2D layer switches its Dimension (and
  back); `look.id` is recorded; the layer's previous Color Scheme is re-applied when
  `keep_color_scheme` and one is recorded. Multi-layer application is one op per layer in one batch:
  one undo step named `Apply Look <name>` (`Apply Look <name> (3 layers)`).
- **R8. `apply_color_scheme`** `{op, layer?, color_scheme}` recolours a layer, a group (recursively)
  or, with no `layer`, the whole document, by role (§6.5.2): structure is kept exactly (rim and band
  count, widths, joins, offsets, bevels, textures, gloss, depths, effects, generators' non-colour
  parameters, alpha profiles); `look.color_scheme` is recorded on each recoloured layer. Applying
  the same scheme twice changes nothing the second time. One undo step `Apply Color Scheme <name>`.
- **R9. `apply_template`** `{op, template, keep_text: true, language: "en"}` replaces the document's
  content with the template's (§6.4, §6.5.3), keeping its file identity, name and outputs; with
  `keep_text` the values of the document's exposed `text` parameters carry into the template's
  placeholders of the same name, otherwise the placeholders take `sample.<language>`. Creating a
  document from a template is S80's create path with `template` (one undo step that writes the file,
  `Create Title Art from Template <name>`). Fonts and maps the template uses are imported (R29, R30)
  in the same undo step.
- **R10. Determinism.** Applying an item gives the same document whatever ids the library or the
  layer had (new ids are drawn from the document's id generator, rendering never depends on them,
  S80); the result of `apply_look` does not depend on the layer's previous look except through the
  kept members of §6.4; every random element a look brings is seeded from its own data plus the
  document `seed` (S80 decision 8).
- **R11. Preview = apply.** The Library's hover preview and every thumbnail run the same apply
  functions on a copy of the document (`TitleLibraryApply::preview(doc, op) -> TitleArtDoc`), never
  through the history; the canvas shows the copy until the pointer leaves (S82 draws it). A preview
  never imports files: a font or map not yet in the project is read from the pack for the preview.
- **R12. Runtime Color Schemes.** `ColorSchemeApply` lives in `engine/core/titleart/` (inside the
  `JASPERENGINE_TITLE_ART` module), is pure CPU and deterministic, and is the one implementation the
  editor, MCP and players call. When a document exposes a `color_scheme` parameter (S80 `expose`),
  the data of each scheme in its `options` is copied into the document's top-level `color_schemes`
  map (scheme id to scheme data; written by `expose` and refreshed by `apply_color_scheme`), so a
  player switches schemes without the library (S81's `TitleArt:setColorScheme(id)` re-renders from
  the authored document with the scheme applied). Empty `options` means any scheme (S81's
  `colorSchemes`): `color_schemes` then holds every built-in and project scheme (about 100 KB of JSON
  in the document) and `check_title` warns `color_scheme_options_empty` with that size and the
  `set` op that lists the used ones. A document without the member costs nothing.

### Functional — the user library

- **R13. `save_look`** `{op, layer, name, id?, scope: "project", family: "custom", tags: [],
  include_font: true, overwrite: false}` captures the layer into a look (§6.5.4): style (ids to
  positional keys), effects, part effects, the generator layers it owns and the user's generator
  layers that follow it inside its auto group, the auto group's effects, the effective 3D lighting,
  material link names, `ref_size` = the layer's `size`, the font pair when `include_font`. Referenced
  textures (pattern and image fills, reflection maps) are copied into `<slug>.files/` beside the look;
  user fonts not in the pack likewise. The id defaults to the slug of `name.en` (or of the name);
  an existing id needs `overwrite`.
- **R14. `save_color_scheme`** `{op, layer?, name, id?, scope, overwrite}` extracts a scheme from a
  layer (or the document's first styled layer) by the same role classification as R8 run in
  reverse: face, dark, accents, base and side paints, glow and shadow colours of its effects; the
  rest is derived (§6.9.1). Applying the saved scheme to the source layer changes no pixel by more
  than 2/255 (AC9).
- **R15. `save_template`** `{op, name, id?, scope, placeholders?: [exposed names], overwrite}`
  writes the current document as a template; placeholders default to every exposed `text` parameter
  (required when the document has a text layer: `template_without_placeholder` otherwise, with the
  `expose` op to fix it); samples are the current values (as `en`, or `ja` when the text is in Japanese script).
- **R16. Folders and history.** Project items live in `<project>/EditorSettings/TitleLibrary/`
  `Looks/`, `ColorSchemes/`, `Templates/` (versioned with the project, outside `assets/`, never
  exported); personal items in `<config dir>/title-library/looks|color-schemes|templates/`
  (`AppSettings::getConfigDirectory()`). File name = `<slug>.<ext>`. Saving and deleting a project
  item is one project undo step (`WriteFilesCmd`, `RemoveFilesCmd`, as `PoseTemplates`); personal
  items are outside the project: a save is immediate and a delete moves the file into
  `<config dir>/title-library/.trash/` (kept 30 days, restorable from the Library's context menu).
  The library rescans a folder on S05 file-watcher events for it.
- **R17. Sharing.** `copy_library_item {op, ref, to: "project" | "user", id?}` copies an item (and its
  `.files/` folder) between scopes, so a personal look reaches every project and a project look can
  be promoted. Dropping `.titlelook`, `.titlecolors` or `.titletemplate` files onto the Library panel
  copies them into the project scope (S82 handles the drop, this spec the copy: validation, id
  clash refused unless `overwrite`). `delete_library_item {op, ref}` deletes a project or personal
  item; built-ins cannot be deleted (`library_item_builtin`).
- **R18. Favorites and recents** are personal editor settings (S25 settings registry):
  `title_library.favorites` (refs, any kind, ordered by when starred) and
  `title_library.recents.<kind>` (the 8 last applied per kind, newest first, recorded by Title
  Designer applies only, never by MCP). A favorite whose item no longer resolves is shown greyed with
  "Missing" and removed on the next star toggle. `list_title_library {favorites: true}` reads them.

### Functional — the Pachislop port

- **R19. Converter, stage 1 (evaluate).** `tools/titleart/pachislop-port/` is committed: `package.json`
  (pinned `esbuild`, `three@0.186.1`, `opentype.js@2.0.0`, `typescript@6.0.3`, `npm ci` with the
  committed lockfile), `dump.mjs` and `entry.ts`. `node dump.mjs --pachislop <clone> --scratch <dir>`
  refuses a scratch directory inside the repository or the clone, copies the clone's `src/` into
  the scratch directory, bundles `entry.ts` against that copy with esbuild (Node target;
  `import.meta.env.BASE_URL` defined as `"/"`; `document`, `window`, `navigator` and `fetch` shimmed to
  throw if touched; `Math.random` and `crypto.randomUUID` replaced by a seeded counter), runs the
  bundle, and writes `pachislop-dump.json`: `FACTORY_STYLES`, the colourway definitions and light
  ramps, every `TEMPLATES[i].build()` result, the effect and generator registries' `ParamDef`
  lists (key, type, unit, default), the Material3D presets, and two oracle sets computed by
  Pachislop's own code: `colourway_cases` (`applyColourway` of 21 ported looks, the 11 outlines and
  ten others, in each of the 11 colourways) and `lint_cases` (`lintStyle` of every factory look and of
  the eight red-control layers of §6.14.4). It never runs the clone's own
  scripts, `npm install` or tests. Host only, like `tools/htmlui` (never in CI or Docker).
- **R20. Converter, stage 2 (map).** `jasperengine-editor title-library port-pachislop --dump <json>
  --out <dir>` (no GPU) maps the dump with `PachislopPort` (`editor/titleart/library/PachislopPort.{h,cpp}`),
  the C++ code S87's Pachislop project importer shares: style and template members by
  `editor/resources/titleart/pachislop-map.json` (embedded in the editor, so S87 reads the same file:
  ids, families, bilingual names, renames, the camelCase to snake_case rule and its exceptions, part
  keys, companion to generator layer, fonts, environments, fields dropped with reasons, template
  texts), effects and generators by S83's `PachislopFxMap` (`editor/resources/titleart/pachislop-fx-map.yaml`),
  3D styles by S86's `TitleDocument3D::fromPachislop`; then the per-item JSON merge patches of
  `tools/titleart/pachislop-port/overrides.json` (the tuning done after the port, so a re-run keeps
  it). A Pachislop member with no mapping and no `dropped` entry fails the run naming it; nothing is
  dropped silently. It writes `editor/resources/titleart/provenance/pachislop.json` (per item: new id,
  Pachislop source file, line and commit, the source id unless it contains a denied term,
  mapped/approximated/dropped notes) and checks the denied terms (R38) on its output.
- **R21. Reproducible.** The dump of commit `1c32502` is committed
  (`tools/titleart/pachislop-port/dump/pachislop-1c32502.json`). `port-pachislop` followed by
  `title-library normalize` (canonical sparse form, R5) reproduces the committed ported files byte
  for byte; `port-pachislop --check` does both into a temporary directory and compares (AC11). A
  later Pachislop commit is ported by a new dump and a reviewed diff.
- **R22. Review.** `tools/titleart/pachislop-port/review.json` holds one record per ported item
  `{id, verdict: "match" | "close" | "tuned" | "differs", note, reviewer, date}`; `close`, `tuned` and
  `differs` need a note, `tuned` needs an `overrides.json` entry. The review sheets come from
  `jasperengine-editor title-library review-sheet --pachislop <clone> --out <scratch>`: per item our
  render beside Pachislop's `tests/golden/refs/<kind>/<source id>.png` on the same text (確定 for 2D
  looks, BIG for 3D looks, the template's own text), written outside the repository. CI fails on a
  ported item without a record.

### Functional — content

- **R23. Families.** Fourteen families with bilingual names and the counts of §6.6: Casino, Metal &
  Chrome, Neon, Elements, Gems & Glass, Japanese Wa, Sci-fi, Fantasy, Horror, Cartoon, Retro Arcade,
  Anime, Sports, Minimal. Every family has at least 10 looks, 3 templates and 2 suggested color
  schemes, and at least 2 of its looks are 3D.
- **R24. Ported items.** All 104 Pachislop looks under the new ids of §6.7.2, the 11 colourways as
  Color Schemes with their ids (`white`, `blue`, `green`, `purple`, `red`, `gold`, `silver`, `zebra`,
  `giraffe`, `leopard`, `rainbow`), the 37 templates as §6.10.1, with the renames of §6.7.3.
- **R25. New looks**: the 91 of §6.8, each built from S80/S83/S86 members as described there, with a
  `provenance {source: "jasperengine"}`.
- **R26. New schemes**: the 22 of §6.9.
- **R27. New templates**: the 36 of §6.10.2.
- **R28. Font pack** (§6.11): 32 font families (21 Latin display, 11 Japanese display) fetched by
  `tools/titleart/fetch_fonts.py` from `google/fonts` at commit `2fe1e8c6b137` (paths under `ofl/`
  only; any other licence directory fails the fetch), byte-identical to upstream (SHA-256 recorded),
  each in `editor/resources/titleart/fonts/<dir>/` with upstream's `OFL.txt` verbatim and a
  `SOURCE.json`; `fonts.json` lists pack ids, family, style, file, default variation values,
  scripts (`latin`, `ja`) and category. Playfair is upstream's variable italic
  `Playfair-Italic[opsz,wdth,wght].ttf`, used at `wght 900` (not Pachislop's modified instance).
- **R29. Fonts in documents.** A look or template names fonts by pack id as a pair
  `{latin, ja}`. `apply_look` with `apply_font` (and `apply_template` always) sets the layer's `font`
  to the `latin` face and puts the `ja` face first in `fallback_fonts` when it differs and the latin
  face lacks the text's kana and kanji; a pack font the project lacks is imported first (§6.11.3:
  copied with its `OFL.txt` into `assets/TitleArt/Fonts/<dir>/`, its `.meta` provenance
  `licence: OFL-1.1`, `source: titleart-pack`), once per project, in the same undo step. A pack font
  that is missing (no staged pack) leaves the layer's font as it was and returns warning
  `font_unavailable` naming the face and the fix (reinstall, or `JASPERENGINE_TITLEART_LIBRARY_DIR`);
  thumbnails then render with the built-in face and say so. Nothing is ever silently substituted:
  missing glyphs remain S80's `glyphs_present` problem.
- **R30. Environments** (§6.12): S86 owns title environments (procedural presets and the four
  Pachislop HDRIs `softbox`, `highway`, `city`, `sky`, S86 R24, R66). S84 adds four Poly Haven CC0
  photo environments for the new families (`night_city`, `night_dark`, `sunset_harbour`, `warehouse`)
  to S86's `editor/resources/titleart/hdri/manifest.json`, fetched by S86's `fetch_hdris.py` (2k
  `.hdr`, S86's size rule). Looks name environments by S86's ids. 2D reflection maps (S80's
  `reflection.map`, an equirectangular Texture) use the same `.hdr` files: the first apply into a
  project imports the file into `assets/TitleArt/Environments/` as an HDR Texture (S14 import type HDR)
  and points the field at it, once per project, in the op's undo step.
- **R31. Notices.** `NOTICE.md` gains: one paragraph for the Pachislop port (looks, color schemes,
  templates, the recolouring algorithm and the style lint rules, by SafEight, used with the author's
  permission, with the files that hold them); one row per font family (§6.11); four rows in S86's
  HDRI table for S84's environments; and the missing row for `lythwood_room`
  (`PreviewEnvironment.h`). `tools/ci/check-notices.py` gains `CONTENT_ROOTS`: every directory under
  `editor/resources/titleart/fonts/` needs a NOTICE row naming it and an `OFL.txt`; the editor install
  carries them as `licenses/titleart-font-<dir>-OFL.txt` (`--install-manifest` and `--installed` check
  them; S86 R66 covers `hdri/`).

### Functional — thumbnails, previews, search

- **R32. Baked thumbnails.** `jasperengine-editor title-library bake-thumbnails [--only <glob>]
  [--check]` renders, headless with the S80 renderer, every built-in look and scheme on three sample
  texts (`latin` = `HERO`, `kanji` = `大当り`, `digits` = `1234`; a look may override them with
  `thumbnail_texts`) and every template once per sample language (en, ja): 240 × 135 RGBA8 PNGs in
  `editor/resources/titleart/thumbnails/<kind>/<id>.<text>.png`, transparent background, framed by
  text bounds plus the style's reach. Looks render with their own font pair, schemes on
  `casino/outline-gold` with its font. Each PNG carries a `tEXt` chunk `jasper.titleart.thumb`
  with the SHA-256 of (item canonical bytes, font files used, `TITLE_ART_BAKE_VERSION`, text, size).
  `--check` recomputes recipes without rendering and exits 1 listing stale or missing thumbnails.
- **R33. Live previews** (`TitleLibraryPreviews`): renders library items on the user's own layer (its
  text, font, size, warp, distort, weight, transforms) or document (templates on their own
  placeholders) for S82's tiles: key = SHA-256 of (item id, revision, the layer's content key,
  applied scheme, Dimension, `TITLE_ART_BAKE_VERSION`), excluding position and canvas; requests
  carry a priority (visible tiles first), at most 2 in flight, submitted through S80's
  `TitlePreviewQueue` (S80 §6.13, priority `Tiles`: the one 4 ms per-frame budget every title preview shares),
  time-sliced on the S80 renderer (contract decision 2), cancelled when the panel closes; GPU
  LRU of 240 tiles; disk cache `.jasperengine/cache/titleart/library/<key>.png` (contract decision
  11); a pending tile keeps its old picture only if it shows the same layer; a failed render shows
  "Render failed" and is not retried for that key. Until a live tile is ready, the baked thumbnail of
  the script that matches the layer's text (kanji/kana, digits only, else Latin) is shown. GPU device
  loss drops the GPU cache and re-requests visible tiles.
- **R34. Search** (`TitleLibrarySearch`): all words of the query must match (after S82's
  `foldForSearch`, S82 R27: full-width to ASCII, half-width to full-width katakana, katakana to
  hiragana, lower case) any of: the item's `name.en`, `name.ja`, `synonyms`
  (both languages, romaji included), `tags`, family names (both languages), `dimension`, and the
  derived effect tags (§6.13: `light glow`, `bevel`, `pattern zebra`, `glass`, `3d`...); results
  ranked by `fuzzyScore` (name first, then synonyms and tags −80, as S25's scorer). Filters: kind,
  family, dimension, tags (all), scope, favorites, available. Searching never uses the network.

### Functional — quality

- **R35. Style lint** (`editor/titleart/library/StyleLint.{h,cpp}`): the rules of Pachislop's
  `lint.ts`, re-implemented and registered as `check_title`'s `style_lint` check (S80's check
  registry) with one result per finding (§6.14.1: ids, thresholds, fix ops). Rule texts are our own
  words. The Title Designer shows them (S82); `check_title` returns them with ready-to-apply ops.
  S84 also adds the two pixel checks S80 §6.12 assigns it, `color_scheme_match` and `metal_range`
  (§6.14.1).
- **R36. Library gate.** `tools/mcp/tests/test_title_library.py` (CI) loads every built-in item
  (no load warning), and for every available look on each sample text, every scheme on its three
  texts and every template in both languages: renders twice at 0.25 × (identical bytes), renders
  not flat, passes every default `check_title` check (`check_allow` excepted) and `style_lint`
  (`lint_allow` excepted); and verifies metadata (both names, ≥ 3 tags, a valid family, font pack ids
  that exist, thumbnails fresh, a provenance, a review record for ported items, no denied term).
  Allowlist entries need a reason; at most 10 % of a kind may carry any. Unavailable items are
  counted and listed, never skipped silently. A representative subset also has golden images
  (§6.14.5).
- **R37. Calibration.** `jasperengine-editor title-library calibrate --out tools/titleart/calibration.json`
  measures every metric of every `check_title` check over the gate's renders (min, p5, median, p95,
  max per check, per kind) and over the red-control fixtures; the committed file is what the
  defaults were set from. A test fails when a default threshold no longer separates the library
  from its red controls with the margin of §6.14.3, or when a threshold changed without a
  `calibration.json` change in the same commit.
- **R38. Denied terms.** `tools/titleart/denied_terms.txt` (franchise, brand, product, company and
  creator names; §6.7.3 lists the first entries) is checked by `port-pachislop`, by `title-library verify`
  and by the gate against every id, name, synonym, tag, description, template sample, placeholder
  value and text layer of every built-in item: Latin terms as whole words, case-insensitive; CJK
  terms as substrings.

### Platform & backends

- **R39.** `ColorSchemeApply` is CPU code with no GPU or platform dependency, built on every export
  target inside `JASPERENGINE_TITLE_ART`; it is part of the module, so a player has it exactly when
  it has the module (a shipped Realtime or Glyph Atlas title, S81 R52) and a Baked-only game has
  none of it. Everything
  else in this spec is editor-only: nothing under `editor/resources/titleart/` is a player input; a
  game gets only the fonts and environments its documents imported, as ordinary assets (S81/S86
  decide what an export includes). The pack is staged next to the editor binary (`bin/titleart/`,
  macOS `Contents/Resources/titleart/`), in the build tree beside the editor target, found by
  `FileUtils::getTitleArtLibraryDir()`, overridden by `JASPERENGINE_TITLEART_LIBRARY_DIR`. A missing
  pack is a state: items still list and apply (their data is embedded, R41), fonts and environments
  report `font_unavailable` / `environment_unavailable`, thumbnails render live.

### Performance (Release editor, build server; real-GPU lane machine H where stated)

- **R40.** Budgets (benchmarks in AC21):
  - library index (every built-in item parsed to metadata, `requires` computed) ≤ 60 ms, done on the
    first Title Designer open or first library call, never at editor start (start time unchanged
    within noise);
  - `apply_look` on a 16-rim layer ≤ 3 ms CPU, `apply_color_scheme` ≤ 2 ms per layer and ≤ 25 ms for
    a 64-layer document (imports and rendering excluded); the runtime switch in a Linux Release
    player ≤ 1 ms per styled layer before the re-render;
  - a live 2D tile at 240 × 135 ≤ 15 ms GPU on H (3D ≤ 40 ms); the preview service never spends more
    than 4 ms of a frame's main thread;
  - `list_title_library` catalogue and rows ≤ 30 ms; a 9-cell sheet of 2D looks ≤ 1.5 s on H (a job
    past 1 s);
  - the baker: every thumbnail ≤ 20 min on a build server's llvmpipe; the gate ≤ 10 min wall with
    `-j 4`.
- **R41. Sizes.** Embedded in the editor binary: the item data (looks, schemes, templates,
  `manifest.json`), zstd-compressed (`engine/libs/zstd`), ≤ 1.5 MiB of binary growth. Staged with
  the install: fonts 46.2 MB (§6.11), S84's four HDRIs ≤ 30 MB (S86's rule), thumbnails ≤ 16 MiB (the baker fails
  above it). Players: zero bytes unless R39's condition holds, then `ColorSchemeApply` ≤ 30 KB
  (Linux Release, measured by S34's size tools).

### Editor UX (human; drawn by S82)

- **R42.** The Library shows three tabs **Looks**, **Color Schemes**, **Templates**; sections by
  family (folding state remembered per user), Favorites and Recent first when no filter is set;
  each tile shows the item's name in the UI language (English names, Japanese in the tooltip and
  search), a star, a `3D` badge, `→3D` / `→2D` when applying changes the layer's Dimension, and
  disabled tiles say why (`Needs Generator: Fire (S83)`, `Image layers cannot take 3D looks yet`).
  Applying is one named undo step (R7–R9); a toast after a Dimension change names it ("Converted
  Title to 3D. Ctrl+Z to undo"). An "Apply Font" toggle (off by default, remembered per user) and a
  "Keep Color Scheme" toggle (on by default) sit above the Looks grid. The Title Designer menu gains
  **Save As Look…**, **Save As Color Scheme…**, **Save As Template…** (name, ID, scope Project / My
  Library, family, tags; dialog validation from the same functions as the ops) and the layer context
  menu **Revert to Look** (re-applies `look.id` with the toggles' values; disabled when none).
  Assets > Create > Title Art > **From Template…** opens the Templates grid in a picker. The six
  featured templates of S82's empty canvas card (`manifest.json` `featured`) are
  `casino/big-win-3d`, `metal/chrome-title`, `fantasy/kingdom-title`, `scifi/hud-title`,
  `cartoon/party-title`, `minimal/clean-title`; applying a template selects the layer of its
  placeholder with `role: "title"`.

### Agent UX (MCP, CLI, Lua)

- **R43. `list_title_library`** (group `title_art`, the tool S80 registers; this spec defines its
  library kinds and tiers, §6.15): no arguments → the catalogue (counts per kind and family, ≤ 150
  tokens); `kind` with `query`/`family`/`dimension`/`tags`/`scope`/`favorites` → rows `{n, id, name,
  family, dim, tags, cost, available, use}` where `use` is the ready op (≤ 30 tokens a row, 20 rows by
  default as S80 §6.11, `offset` pages); `id` → the item's detail (structure summary, what applying keeps and
  replaces, font pair, environment, allowlists, provenance; ≤ 400 tokens); `sheet` → the chosen items
  rendered on the agent's text (or on a layer of a document), numbered cells with the same numbers
  in the JSON, ≤ 9 cells, ≤ 1024 × 576 (about 786 image tokens). Kinds: `looks`, `color_schemes`,
  `templates` and the pack rows of `fonts` (values of S80's program `kind` enum; `effects`,
  `generators`, `blend_modes` and `fonts` are S80's, `environments`, `materials_3d` and
  `bevel_profiles` S86's, which list S84's four environments, `animation_presets` S85's).
- **R44. Ops for agents.** The ops of R7–R9 and R13–R17 are `edit_title` ops (S80): a batch made only
  of library ops (`save_*`, `copy_library_item`, `delete_library_item`) needs no document `path`.
  Results list `changes` (replaced and kept members, imported files with paths, the Dimension change,
  warnings), errors list valid alternatives (R4). `edit_title` ops never touch favorites or recents.
- **R45. `check_title`** returns `style_lint` findings as checks `style_lint.<rule>` with `measured`,
  `threshold`, the element (`rim#1`), `fix` ops where the repair is mechanical, and the library
  item a layer came from (`look.id`), so an agent can tell a library look from its own edits.
- **R46. CLI** (`editor/cli/`, sharing the library code): `jasperengine-editor title-library list
  [--kind K] [--family F] [--json]`, `verify` (formats, references, fonts, environments, notices,
  denied terms, thumbnails' recipes; standard exit codes 0 / 1 / 2), `lint [<id>...]`, `normalize
  [--check] <dir>`, `port-pachislop [--check]`, `bake-thumbnails`, `review-sheet`, `calibrate`,
  `gen-docs`. All but
  `bake-thumbnails`, `review-sheet` and `calibrate` need no GPU.
- **R47. Docs.** `docs/mcp/guide/title-art.md` gets a Library section (the three axes and their
  keep-and-replace lines, refs and scopes, the tier ladder, Save As ops, font pairs); the catalogue
  is generated into `docs/mcp/guide/title-library.md` (`jasper://docs/title-library`, one `###`
  section per family so it splits into per-family resources, `also_found_as` aliases from synonyms)
  by `jasperengine-editor title-library gen-docs`, checked by `gen-docs --check` in `test_docs.py`.
  No new line in the agent guide (`McpInstructions.cpp`): the Title Art entry S80 adds points at
  `jasper://docs/title-art`. `docs/unity-glossary.md` gains rows for Preset (Look), Preset Library
  (Title Library) and "Swatches/Color Presets" (Color Scheme).
- **R48. Lua.** S84 adds no Lua API; S81's `TitleArt:setColorScheme(id)` and
  read-only `colorSchemes` property call R12's function.

### Data & compatibility

- **R49.** Old documents and old library files load forever (R5); a document's layers keep working
  when the look they name is renamed or removed (`look.id` is informational; "Revert to Look" resolves
  aliases). A personal library written by a newer editor is read-only with a notice (S80's newer
  version rule). Unknown members are preserved.
- **R50.** The binary content (fonts, S84's HDRIs, thumbnails; at most 92 MB) enters the repository
  as **one squashed commit** with `.gitattributes` (`*.ttf *.hdr *.png binary`), as S36 did; its size
  and the install growth are recorded in the Progress section on landing. The text content (looks,
  schemes, templates, mapping, provenance, review) is ordinary commits.

## 6. Design constraints & decisions

- **D1. Three axes, three file kinds, one apply module.** A Look restyles one layer, a Color Scheme
  recolours by role, a Template replaces the document. Each is its own file kind with its own
  contract (§6.4), so a user tries schemes on a look, or looks on a scheme, without losing the
  other. All three are applied by `TitleLibraryApply` (`editor/titleart/library/`), called by the ops,
  the previews, the baker and the gate; the scheme half is `ColorSchemeApply` in the engine.
- **D2. JSON like the document.** S80 chose canonical JSON for `.titleart`; library files use the
  same codec rules (member order, `%.7g`, sorted maps, lenient read, unknown members kept), but
  looks and schemes are **sparse** (R5) because they overlay any document and stay small and
  reviewable; templates are full documents.
- **D3. Positional, id-free, size-relative.** Looks store part keys as `rim#n` / `band#n`, `%` style
  units, px effect parameters with `ref_size`, handle positions relative to the layer, material
  links by name, fonts by pack id, environments by S86's environment ids: nothing ties a look to a document.
- **D4. A copy, with a record.** Applying copies the data into the document (exported games carry no
  library); the layer records `look.id` and `look.color_scheme` (S80's informational member), which
  drives Keep Color Scheme, Revert to Look and `check_title`'s attribution. There is no live link:
  editing a look never changes documents (a user who wants that re-applies).
- **D5. Built-in data embedded, heavy assets staged.** Item data (≈ 4 MB of JSON, ≤ 1.5 MiB
  compressed) is embedded in the editor, so the library always works; fonts, HDRIs and thumbnails
  (≤ 92 MB) are files staged with the install and copied into a project on first use (S80 §6.1
  decided "copied into the project on first use"). No network access, ever.
- **D6. The converter is committed, evaluates rather than parses.** Pachislop's looks are built by
  TypeScript helpers (`fx()` expands registry defaults, colourway-generated families, orbs as
  finish × colourway); evaluating the real code in a scratch Node environment is the only faithful
  way. Its output is reproducible in CI from a committed dump, and tuning lives in an overrides
  file, so the port can be re-run and reviewed as a diff.
- **D7. Families are presentation; ids are forever.** An id's family prefix is chosen at release and
  never changes (a later family move keeps the id; `family` is a separate member).
- **D8. Font pairs.** Most display fonts with character are Latin-only, Japanese titles are first
  class, and S80 has a fallback chain: a look names `{latin, ja}`, the Latin face is the font and the
  Japanese face its first fallback. One look then serves `HERO` and `大当り`.
- **D9. Quality is measured, not hoped.** Every built-in item passes the checks and the lint on the
  sample texts in CI; thresholds come from measurements over the library (calibration) and a
  red control per rule; failures that are deliberate are allowlisted with a reason.

### 6.1 Look file (`.titlelook`)

```json
{
  "format": "jasperengine.titlelook",
  "version": 1,
  "id": "metal/classic-gold",
  "revision": 1,
  "name": {"en": "Classic Gold", "ja": "クラシックゴールド"},
  "description": {"en": "Banded gold face, black keyline, white line, gold frame, dropped dark base.",
                  "ja": "帯状の金の面、黒の縁、白の線、金の枠、下に落とした影の台。"},
  "family": "metal",
  "tags": ["gold", "banded", "bevel", "outline"],
  "synonyms": {"en": ["gilded", "yellow gold"], "ja": ["金", "ゴールド", "kin"]},
  "dimension": "2d",
  "applies": "style",
  "ref_size": 200,
  "font": {"latin": "dela-gothic-one", "ja": "dela-gothic-one"},
  "style": {
    "face": {"fill": {"kind": "linear", "gradient": {"stops": [...]}}, "bevel": {"enabled": true, "size": 3, "depth": 4},
             "finish": {"enabled": true, "gradient": {...}, "amount": 0.85}, "inner_glow": {...}, "gloss": {...}},
    "rims": [{"width": 1, "material": {"fill": {"color": "#0a0604"}}},
             {"width": 1.6, "material": {...}},
             {"width": 3.2, "material": {...}},
             {"width": 2.4, "offset": [0, -2.4], "material": {"fill": {"color": "#000000"}}}]
  },
  "effects": [{"type": "drop_shadow", "params": {"angle": 90, "distance": 6, "size": 14, "opacity": 0.6, "color": "#3a1800"}},
              {"type": "outer_glow", "params": {"color": "#ffb000", "size": 22, "mode": "behind"}}],
  "part_effects": {},
  "generators": [],
  "group_effects": [],
  "links": {},
  "lint_allow": [],
  "check_allow": [],
  "provenance": {"source": "pachislop", "commit": "1c32502", "file": "src/presets/factory.ts", "line": 275,
                 "item": "f-gold-classic", "license_note": "used with the author's permission"}
}
```

| Member | Type, default | Notes |
|---|---|---|
| `id`, `revision` | string; int 1 | `revision` +1 on every change of a built-in (thumbnail and preview keys use it) |
| `name`, `description` | `{en, ja}` | Both required for built-ins; user items need one |
| `family` | string | §6.6 ids, or `custom` (user items' default) |
| `tags` | [string] | lowercase `[a-z0-9-]`, ≤ 12 |
| `synonyms` | `{en: [], ja: []}` | ja may hold kana, kanji and romaji |
| `dimension` | `2d` \| `3d` | |
| `applies` | `style` \| `effects` | `effects`: only `effects`, `generators`, `group_effects` apply (any layer but adjustment); style and part effects are absent |
| `ref_size` | number 200 | The layer `size` the px values were authored for |
| `font` | `{latin, ja, variations?}` or absent | Pack ids (§6.11); `variations` e.g. `{"wght": 900}`; user looks may name `{"file": "<slug>.files/X.ttf"}` |
| `thumbnail_texts` | `{latin, kanji, digits}` | Optional overrides of R32's samples |
| `style` / `style_3d` | sparse S80 Style2D / S86 3D style | Exactly one, matching `dimension`; absent for `applies: effects`. Rims and bands carry no `id` |
| `effects` | [`{type, enabled?, params}`] | S80 effect entries without `id`; params sparse |
| `part_effects` | map positional key → [effect] | Keys `face`, `rim#n`, `band#n`, `bevel`, `side`; a key the look's own style lacks is a load warning |
| `generators` | [`{type, preset?, place, scope, name?, blend, opacity, params, effects}` (`place`: `above` or `below`; `scope`: `layer` or `group`)] | Become generator layers following the layer (§6.5.1); `scope: group` follows the auto group (Pachislop's `outer` companions); `preset` names an S83 generator preset expanded at apply |
| `group_effects` | [effect] | Effects of the auto group (§6.5.1) |
| `lighting` | S86 lighting override, complete | 3D only |
| `links` | map name → `{name}` | Materials in `style`/`style_3d` with `link: <name>` form one link group per application |
| `lint_allow`, `check_allow` | [`{rule` or `check, reason}`] | Gate exceptions (R36) |
| `assets` | [`{key, file, sha256}`] | User looks: files in `<slug>.files/` referenced from the style as `"asset:<key>"` |
| `provenance` | `{source, commit?, file?, line?, item?, author?, license_note?}` | `source`: `pachislop`, `jasperengine`, `user` |

Units: `%` style members and `rim#n` widths as S80; effect and generator params whose definition
has `ParamUnit::Px` (S80 §6.5) are multiplied by `k = layer size / ref_size` on apply; Vec2 params
with a `point` gizmo hint (an on-canvas position: flare centre, impact point) are stored relative to
the layer origin and become `layer position + k × value`.

### 6.2 Color Scheme file (`.titlecolors`)

```json
{
  "format": "jasperengine.titlecolors",
  "version": 1,
  "id": "gold",
  "revision": 1,
  "name": {"en": "Gold", "ja": "金"},
  "family_hint": ["casino", "metal"],
  "tags": ["warm", "metal"], "synonyms": {"en": ["yellow"], "ja": ["きん", "kin", "ゴールド"]},
  "paints": {
    "face":   {"fill": {"kind": "linear", "gradient": {...}}, "ramp": {...}, "material_3d": {"preset": "gold"}, "shade": "#5a2a00"},
    "dark":   {"fill": {"color": "#0a0604"}, "ramp": null, "material_3d": {"preset": "black_gloss"}, "shade": "#000000"},
    "accents": [{...gold rim...}, {...red rim...}],
    "base":   {...},
    "bevel_rim_3d": null,
    "side_3d": {"preset": "custom", "color": "#5a2a00", "metallic": 1, "roughness": 0.35}
  },
  "face_glow": "#fff0a0",
  "glow": "#ffb000",
  "glow_gradient": {...},
  "shadow": "#3a1800",
  "glint": "#fff4d0",
  "light_ramp": ["#2a1000", "#8a3000", "#c85a00", "#ff9400", "#ffb000", "#ffd820", "#fff8b8"],
  "orb": {"ramp": {...}, "radial": true},
  "pattern": null,
  "pattern_ground": null,
  "spectrum": false,
  "swatch": ["#ffd820", "#ff9400", "#0a0604", "#c00010"],
  "provenance": {"source": "pachislop", "commit": "1c32502", "file": "src/presets/colourways.ts", "line": 206}
}
```

A paint is `{fill (S80 Fill), ramp (Gradient or null: the gradient-map finish for bevelled metal
elements; null keeps the element's colour unmapped), material_3d (S86 Material), shade (the bevel
shadow tint), pattern? (marks for patterned faces: zebra, giraffe, leopard)}`. `spectrum: true`
(the `rainbow` scheme) makes faces and gradient fills spectra and switches solid generator fills to
their palette modes (§6.5.2).

### 6.3 Template file (`.titletemplate`)

A `.titleart` document (S80 §6.1, every member written, its own `guid`, unused: a template is not an
asset) with the member `template` after `guid`:

```json
"template": {
  "version": 1,
  "id": "casino/big-win-3d",
  "revision": 1,
  "name": {"en": "Big Win (3D gold, rays)", "ja": "大当り (3D 金 + 光線)"},
  "description": {...}, "family": "casino", "tags": ["3d", "gold", "rays"], "synonyms": {...},
  "placeholders": [{"name": "text", "role": "title", "sample": {"en": "BIG WIN", "ja": "大当り"}, "max_chars": 10}],
  "canvas_note": "1920 × 1080, transparent; the backdrop is a guide layer",
  "provenance": {"source": "pachislop", "commit": "1c32502", "file": "src/presets/templates.ts", "line": 447, "item": "t-oatari-3d"}
}
```

Placeholders are the document's exposed `text` parameters (S80 `exposed`), named `text` (the
title: S80 exposes the first text layer as `text`), `subtitle`, `kicker`, `counter`, `label`, `line_2`
in that order of preference. The stored document holds the `ja` sample for Japanese-origin
templates and the `en` sample otherwise; `apply_template` sets the language's sample (or the
carried text) through the exposed targets. An optional `cjk` member after `placeholders`
(`rim_scale`, `band_scale`, `highlight_scale`; a look's `cjk` takes the same three) applies to the
template's layers that then hold Japanese text (Deviation 29). Backdrops are guide layers (never in outputs) unless they
are part of the art, as in Pachislop.

### 6.4 The keep-and-replace contract

| Document or layer member | Look (`apply_look`) | Color Scheme (`apply_color_scheme`) | Template (`apply_template`) |
|---|---|---|---|
| `format`, `version`, `guid`, `name`, the file, its outputs (`outputs[]`) | kept | kept | kept (the template's outputs are used only when the document's are the default single `main`) |
| `canvas`, `global_light`, `seed` | kept | kept | replaced |
| `exposed` | kept | kept (a `color_scheme` parameter's embedded data refreshed) | replaced; values of same-named `text` parameters carried with `keep_text` |
| `material_links` | the layer's old link groups lose this layer; the look's links get fresh ids | kept (a recoloured linked material recolours its group through S80's link sync) | replaced |
| `color_schemes` (embedded) | kept | the applied scheme added when it is an exposed option | replaced by the template's |
| layer tree | kept, except the look's own generator layers and auto group (below) | kept | replaced |
| layer `id`, `type`, `name`, `visible`, `locked`, `guide` | kept | kept | — |
| `blend`, `opacity`, `clip`, `transform`, `masks` | kept | kept | — |
| source: `text`, `size`, `tracking`, `line_height`, `kerning`, `align`, `vertical`, `tate_chu_yoko*`, `proportional`, `h_scale`, `v_scale`, `shear`, `chars` (offsets, scales, rotations), `lines`, `language`, `features`, `variations`; shape geometry; `svg`, `image`, `mode`, `filter`, `trace` | kept | kept | — |
| outline operations `warp`, `distort`, `weight` | kept (geometry, as Pachislop) | kept | — |
| `font`, `fallback_fonts` | kept; replaced by the look's pair with Apply Font | kept | — |
| `dimension` | set to the look's (`→3D` / `→2D`) | kept | — |
| `style` / `style_3d`: face, rims, bands, side, bevel, `knockout`, `erode` | replaced (fresh rim and band ids) | colours replaced by role; structure kept | — |
| a face fill of kind `source` (images, SVG artwork) | the fill kept, the rest of the face material replaced | kept | — |
| `chars.<i>.face` (per-character faces) | cleared (they are colour styling of the old look) | recoloured as the face | — |
| layer `effects`, `part_effects` | replaced (fresh ids) | colour parameters of glow, shadow and light effects recoloured; others kept | — |
| generator layers the layer's look created (`look.owner` = this layer) | removed, replaced by the new look's | light-recoloured (exceptions in §6.5.2) | — |
| other generator layers following the layer | kept | recoloured when the target is their group or the document | — |
| auto group (`look.owner` = this layer) | kept if the new look needs one, else dissolved | kept | — |
| its effects with `from_look: true` / without | replaced / kept | recoloured / recoloured | — |
| 3D lighting override of the layer | replaced with the look's complete lighting | kept | — |
| `look.id` | set | kept | from the template |
| `look.color_scheme` | kept and re-applied with Keep Color Scheme, else cleared | set | from the template |

S84 adds two optional members to S80's schema (no version bump, S80 rule 1): `look.owner` (an Id,
default `""`: the layer whose look created this generator layer or auto group) and `from_look`
(bool, default false) on effect entries of an auto group.

### 6.5 Apply semantics

#### 6.5.1 `apply_look`

1. Resolve `look` (R4) and check availability (R6). Applicability: `applies: style` → Text, Shape,
   Vector (both modes) and Image layers; `applies: effects` → every type but Adjustment; else
   `look_not_applicable` naming the layer type. A 3D look needs S86 (`dimension_unavailable`).
2. `k = layer.size / look.ref_size`.
3. Dimension: when the look's differs, set `dimension` (S86 rebuilds the 3D state from the source;
   an Image layer becomes 3D through its `trace`).
4. Style: build the full style from S80/S86 defaults overlaid with the look's sparse style; give every
   rim and band a fresh id; when the layer's face fill kind is `source`, keep that fill. Replace.
5. Part effects: map each positional key to the new style's part (`rim#2` → `rim:<id of the third
   rim>`); fresh effect ids; px params × k, handles relative (§6.1). Replace `part_effects`.
6. Effects: the same transfer; replace `effects`.
7. Generators: remove generator layers whose `look.owner` is this layer. If the look has
   `group_effects` or a `scope: group` generator, or (S83 D3's rule) a generator whose blend is not
   Normal while the layer has siblings below it, ensure the auto group: the existing one (a group
   whose `look.owner` is this layer and that contains it) or a new group named "<layer name> FX"
   (S83 D3), placed where the layer was, containing it. For each look generator, add a generator layer
   (`follow` = the layer, or the auto group for `scope: group`; `look.owner` = the layer id; S83
   preset expanded, then the look's params; px × k; name = the look's `name` or the generator's
   type name) directly above or below the layer in its order, inside the auto group when there is
   one. Replace the auto group's effects marked `from_look` with the look's `group_effects` (marked
   `from_look`), keeping the user's. A group left with no `group_effects`, no `scope: group`
   generator and only the layer (and its own generators) is dissolved: members move to its place
   with its transform folded in (S80 `ungroup`).
8. Lighting (3D): set the layer's complete lighting override from the look (S86).
9. Links: for each link name used, a new `material_links` entry (fresh id, the name) for this
   application; the layer's materials that belonged to older link groups leave them.
10. Font (only with `apply_font`): R29.
11. Environments and images the style references: imported on first use (R30); the field points at
    the project asset.
12. Record `look.id`; with `keep_color_scheme` and a recorded scheme that still resolves, run
    §6.5.2 on the layer and its owned generators in the same batch; else clear `look.color_scheme`.
13. Result `changes`: `{replaced: [...], kept_color_scheme, dimension_changed, generators_added,
    generators_removed, group: "created" | "kept" | "dissolved", imported: [paths], warnings}`.

#### 6.5.2 `apply_color_scheme` (the port of `colourways.ts`)

Targets: the layer; a group → every Text, Shape, Vector (shape mode), Image (rims only) and Generator
layer inside, recursively; no target → every layer of the document. Guide layers and Adjustment
layers are skipped; Image and SVG-artwork faces with fill `source` keep their pixels.

Roles (2D rings = rims, 3D rings = bands; inner to outer):
- **face** → `paints.face` (3D: the letter's face and bevel; bands in the face's own material take
  `bevel_rim_3d` when the scheme has one, else follow the face);
- **base** → a rim with a non-zero `offset` (the dropped base): `paints.base`;
- **dark** → any other ring whose Rec.601 luma (fill sampled at 9 points, pushed through its finish
  by its amount, as `luma2D`) is below **0.18**: `paints.dark`;
- **accent** → the remaining rings; with `n` accent rings and `A` accents, accent ring `j` (0-based,
  inner to outer) takes `accents[max(0, A − n + j)]`, so the outermost always takes the last;
- **side** → 3D side walls of their own: `paints.side_3d`.

Painting an element keeps its structure: gradient geometry (kind, angle, scale, offset; faces switch
span `layer` to `line` so every line shows the whole ramp, `char` stays), the alpha profile of a
gradient with transparent stops (`keepAlpha`), bevel (its `shadow_color` takes the paint's
`shade`), textures, gloss, satin, depths and 3D depth fades (a `depth` axis ramp becomes the new
colour fading to black). A paint with a `ramp` enables the gradient-map finish on bevelled or
finished elements (amount kept, 0.7 when it was off). Patterns: replaced by the paint's marks when
the scheme has them; a relief-only pattern (opacity 0, relief > 0) is structure and stays; other
colour patterns are cleared. The face's inner glow takes `face_glow`.

Effects and generators: glow effects take `glow` (gradient glows `glow_gradient`); drop, inner and
perspective shadows take `shadow` (never black); the 3D glint takes
`glint`. Every colour and gradient parameter marked `light` (S83's `ParamDef::light`: the colours of light
effects and of generators) is mapped by luma onto `light_ramp` closed by black and white
(`onRamp`): colours with every channel ≥ 0.92 (white cores) stay unless every visible colour of
the element is white, then they take `glow`; a colour already within 4/255 of the ramp stays (so
applying twice changes nothing). Exceptions that keep their own colours: generators `fire`,
`smoke`, `ink_splatter`, `lens_flare`, `glints` (Pachislop's rule: natural phenomena and optics).
`pattern_fill` generators take the scheme's marks when it has them. With `spectrum: true`, faces and
gradient parameters become the spectrum (after a white core stop when they start with one, the
alpha profile kept), and each definition's `SpectrumSwitch` (S83 §6.2:
`particles.color_mode solid → palette`, `speed_lines.color_mode solid → around`) is applied so it shows. Shape layers of shape `orb` recolour
by body tint only (`orb.ramp`, radial or diagonal for spectra), keeping their material model.

The function is `ColorSchemeApply::apply(TitleArtDoc&, const ColorScheme&, Target)`; it returns the
changed field paths (the op's `changes`).

#### 6.5.3 `apply_template`

1. Resolve and check availability (fonts and environments may be absent: warnings, R29).
2. Keep `format`, `version`, `guid`, `name`, and the document's `outputs` when they differ from the
   default single `main`; replace every other member with the template's, the `template` member
   excepted. Every id of the template's layers, rims, masks and effects is re-drawn from the
   document's id generator (references remapped: `follow`, masks, exposed targets, links).
3. Placeholders: for each placeholder, the value is the old document's exposed `text` parameter
   of the same name when `keep_text` and it existed (and is not longer than `max_chars × 2`; else
   warning `placeholder_text_long`), else `sample[language]`; set through the exposed targets.
   Exposed parameters of the old document with no counterpart are listed in `changes.exposed_removed`
   (instances using them warn, S81).
4. Import the fonts and environments the template references (R29, R30); its layers already name
   project-relative paths under `assets/TitleArt/`, which the import fills.
5. One undo step `Apply Template <name>`; `changes` lists the imports and carried texts.

#### 6.5.4 Capture (`save_look`)

The inverse of 6.5.1: style and part effects with ids replaced by positional keys; effects and the
owned generators' params as they are, with `ref_size` = layer `size` and handle params made relative
to the layer origin; generator layers following the layer inside its auto group (owned or not) and
the auto group's effects (all marked `from_look` in the saved look); the effective 3D lighting as a
complete override; link groups touching the layer named by their `material_links` names; members
equal to the defaults removed (sparse). Asset references are copied into `<slug>.files/` and
rewritten to `asset:<key>`. `capture(apply(look)) == look` for every built-in look (AC5), Pachislop's
`presetFromLayer` round trip.

### 6.6 Families and counts

| `family` | Name (en / ja) | Ported looks | New looks | Looks | Templates (ported + new) | Suggested schemes (`family_hint`) |
|---|---|---|---|---|---|---|
| `casino` | Casino & Pachislot / カジノ・パチスロ | 41 | 3 | 44 | 32 + 1 = 33 | gold, red, rainbow, zebra, royal, candy |
| `metal` | Metal & Chrome / メタル・クローム | 29 | 2 | 31 | 2 + 2 = 4 | gold, silver, bronze, obsidian |
| `neon` | Neon / ネオン | 5 | 5 | 10 | 1 + 2 = 3 | neon-pink, neon-cyan, synthwave |
| `elements` | Fire, Ice & Energy / 炎・氷・エネルギー | 9 | 1 | 10 | 0 + 3 = 3 | ember, ice, toxic |
| `gems` | Gems & Glass / 宝石・ガラス | 9 | 1 | 10 | 0 + 3 = 3 | emerald, blue, purple |
| `wa` | Japanese Wa / 和風 | 9 | 1 | 10 | 2 + 1 = 3 | ink, red, gold |
| `scifi` | Sci-fi / SF | 2 | 8 | 10 | 0 + 3 = 3 | neon-cyan, silver, toxic |
| `fantasy` | Fantasy / ファンタジー | 0 | 10 | 10 | 3 | royal, bronze, forest |
| `horror` | Horror / ホラー | 0 | 10 | 10 | 3 | blood, bone, toxic |
| `cartoon` | Cartoon / カートゥーン | 0 | 10 | 10 | 3 | candy, pastel, forest |
| `retro` | Retro Arcade / レトロアーケード | 0 | 10 | 10 | 3 | synthwave, sunset, neon-pink |
| `anime` | Anime / アニメ | 0 | 10 | 10 | 3 | pastel, candy, red |
| `sports` | Sports / スポーツ | 0 | 10 | 10 | 3 | team-navy-gold, team-red-white, mono-light |
| `minimal` | Minimal / ミニマル | 0 | 10 | 10 | 3 | mono-light, mono-dark, ocean |
| | **Total** | **104** | **91** | **195** | **37 + 36 = 73** | **33 schemes** (11 ported + 22 new) |

Why these: the eleven the program named, plus three the ported content already forms
(`elements`, `gems`, `wa`), which would otherwise crowd Casino and Metal. Pachislot's machine assets
(orbs, plates, 確定 outlines) stay in Casino. Ten looks per family is enough for a real choice and
small enough to author well; the ported families keep everything they have.

### 6.7 Porting Pachislop

#### 6.7.1 Pipeline

```
Pachislop clone (read-only) ──dump.mjs──▶ <scratch>/pachislop-dump.json        (host, Node, never CI)
                                              │ copied by the maintainer to
                                              ▼
            tools/titleart/pachislop-port/dump/pachislop-1c32502.json           (committed)
                                              │ title-library port-pachislop: pachislop-map.json,
                                              │ S83 pachislop-fx-map.yaml, S86 fromPachislop, overrides.json
                                              ▼
 editor/resources/titleart/{looks,color-schemes,templates}/**  +  provenance/pachislop.json
                                              │ title-library normalize
                                              ▼
                      canonical sparse files (committed, reviewed as a diff)
```

`port-pachislop --check` (CI, AC11) runs the last two stages into a temporary directory and compares
with the committed files. `pachislop-map.json` holds: `ids` (Pachislop id → new id), `families`,
`names` (the split of Pachislop's mixed names: a leading CJK run is `ja`, the rest `en`; missing
halves written out by hand), `fonts` (Pachislop `fontId` → pack pair; S87 uses the same table),
`environments`, `fields` (exceptions to the camelCase → snake_case rule), `dropped` (`{path,
reason}`), `template_texts` (the replaced sample texts of §6.7.3 and every template's `en` sample of
§6.10.1).

Field mapping rules (in `PachislopPort`, §6.7.1): keys and enum values camelCase → snake_case (`doubleRing` → `double_ring`,
`shapeBurst` → `shape_burst`) unless `fields` says otherwise; `kind: title2d | title3d` → `dimension`;
`style3d` → `style_3d`; rim and band ids dropped; `partEffects` keys `rim:N` → `rim#N`, `band:N` →
`band#N`; `companions` → `generators` (`outer: true` → `scope: "group"`); `groupEffects` →
`group_effects`; `refSize` → `ref_size`; 2D `reflection.map` names (`highway`, `city`, `sky`, `studio`) → S86's
photo environments of the same assets (`studio` → `softbox`), copied in on first use (§6.12); 3D
lookups through S86's `TitleDocument3D::fromPachislop` (S86 R67); effects and generators through
S83's `PachislopFxMap` (S83 §6.9: generator angles negated, `x`/`y` pairs to Vec2, renames); template layers `title2d` / `title3d` → `text`, `shape`, `vector` or `image` by their source
with `dimension`; `background` layers → S80 `fill` generator layers; ids in templates re-minted
deterministically (FNV-1a of the layer path and index); colours lowercase. S80 kept Pachislop's
coordinate space, units and angle conventions (S80 §6.1), so style values copy unchanged.

#### 6.7.2 New ids of the 104 ported looks

Rule: `<family>/<slug>`, slug from the English name, lowercase words joined by `-`, 3D looks end in
`-3d`, 2D looks have no suffix.

- **casino (41):** `f-outline-<c>` → `outline-<c>` for white, blue, green, purple, red, gold, silver,
  zebra, giraffe, leopard, rainbow (11); `f-rainbow-kakutei` → `rainbow-confirm`;
  `f-rainbow-glass-gold` → `rainbow-glass-gold`; `f-pat-zebra` → `zebra-gold`; `f-pat-giraffe` →
  `giraffe`; `f-pat-leopard` → `leopard`; `f-pat-tiger` → `tiger`; `f-pat-checker` → `checker`;
  `f-plate-red` → `red-plate-gold-rim`; `f-plate-makura` → `pillow-plate-black-gold`;
  (source id withheld: denied term, §6.7.3) → `slanted-box-plate`; `f-fx-jackpot` → `jackpot-rays-sparkles`;
  `f-3d-gold-plate` → `gold-on-red-stack-3d`; `f-3d-box-plate` → `slanted-box-plates-3d`;
  `f-3d-rainbow` → `rainbow-candy-3d`; `f-3d-rainbow-glass-gold` → `rainbow-glass-gold-3d`;
  `f-3d-popout` → `pop-out-gold-3d`; `f-3d-candy` → `candy-glitter-3d`; `f-3d-inflate` →
  `inflate-candy-3d`; `f-3d-two-tone` → `two-tone-split-3d`; `f-orb-candy-<c>` → `orb-candy-<c>-3d`
  (blue, green, red, gold, rainbow, white, purple), `f-orb-glass-<c>` → `orb-glass-<c>-3d` (green,
  purple), `f-orb-metal-<c>` → `orb-metal-<c>-3d` (gold, silver).
- **metal (29):** `f-gold-classic` → `classic-gold`; (source id withheld: denied term, §6.7.3) → `double-stroke-gold`;
  `f-gold-ultimate` → `gradient-map-gold`; `f-gold-liquid` → `liquid-gold`; `f-gold-crumple` →
  `crumpled-gold`; `f-gold-pale` → `pale-gold`; `f-silver-premium` → `silver-premium`; `f-rose-gold`
  → `rose-gold`; `f-copper` → `copper`; `f-black-gold` → `black-gold`; `f-black-silver` →
  `black-silver`; `f-chrome` → `chrome-bevel`; `f-chrome-satin` → `satin-chrome`; `f-chrome-liquid`
  → `liquid-chrome`; `f-fx-deepglow-gold` → `light-glow-gold`; `f-fx-chrome-glint` →
  `chrome-star-glints`; `f-3d-gold` → `gold-3d`; `f-3d-chrome` → `chrome-3d`; `f-3d-silver` →
  `silver-premium-3d`; `f-3d-black-chrome` → `black-chrome-red-line-3d`; `f-3d-red` →
  `candy-apple-red-3d`; `f-3d-chrome-blue` → `chrome-blue-stack-3d`; `f-3d-titanium` →
  `anodised-titanium-3d`; `f-3d-kin-gin` → `silver-on-gold-3d`; `f-3d-copper-bronze` →
  `rose-copper-teal-3d`; `f-3d-rose-chocolate` → `rose-gold-chocolate-3d`; `f-3d-carbon-orange` →
  `carbon-gunmetal-orange-3d`; `f-3d-emerald` → `emerald-metal-3d`; `f-3d-holo-silver` →
  `holographic-silver-3d`.
- **neon (5):** `f-neon` → `neon-tube`; `f-neon-knockout` → `hollow-neon`; `f-3d-neon-glass` →
  `neon-glass-tube-3d`; `f-3d-violet-neon` → `violet-neon-edge-3d`; `f-3d-matte-glow` →
  `matte-black-glowing-rim-3d`.
- **elements (9):** `f-fire-gekiatsu` → `red-hot-fire`; `f-lightning-blue` → `electric-blue`;
  `f-corona-purple` → `corona-purple`; `f-ice` → `ice`; `f-3d-ice-crystal` → `ice-crystal-glass-3d`;
  `f-3d-fire` → `fire-3d`; `f-3d-lava` → `lava-crust-3d`; `f-3d-toxic` → `toxic-acid-glow-3d`;
  `f-3d-aurora` → `aurora-teal-violet-3d`.
- **gems (9):** `f-glass-red` → `red-glass`; `f-glass-blue` → `blue-glass`; `f-hologram` →
  `hologram-foil`; `f-3d-sapphire` → `sapphire-silver-frame-3d`; `f-3d-amethyst` →
  `amethyst-crystal-3d`; `f-3d-jade` → `jade-gold-rim-3d`; `f-3d-obsidian` → `obsidian-gold-line-3d`;
  `f-3d-diamond` → `diamond-3d`; `f-3d-pearl-pink` → `pearl-pink-rim-3d`.
- **wa (9):** `f-brush-black` → `ink-brush`; `f-brush-red` → `red-brush`; `f-brush-kasure` →
  `dry-brush-ink`; `f-pat-sakura` → `sakura-pink`; `f-pat-seigaiha` → `seigaiha-gold`;
  `f-3d-shu-lacquer` → `vermilion-lacquer-gold-leaf-3d`; `f-3d-lacquer-gold` →
  `black-lacquer-gold-3d`; `f-3d-sometsuke` → `porcelain-cobalt-3d`; `f-3d-sakura` →
  `sakura-pearl-3d`.
- **scifi (2):** `f-3d-galaxy` → `galaxy-starfield-3d`; `f-pat-hazard` → `hazard-stripes`.

The 2D/3D split stays Pachislop's: 53 2D, 51 3D (the 11 orbs are 3D).

#### 6.7.3 Renames for names and texts (franchise, brand, product, creator)

Grep of every id, name, group, template text and comment of `src/presets/` and the generator and
effect labels (`src/generators/`, `src/effects/`) at `1c32502`; the hits and their replacements:

| Where | Pachislop | Problem | Ours |
|---|---|---|---|
| look | `f-plate-hokuto-box` "北斗 Slanted Box Plate" | franchise | `casino/slanted-box-plate` "Slanted Box Plate / 斜めボックスプレート" |
| look | `f-3d-box-plate` "北斗 Slanted Box Plates" | franchise | `casino/slanted-box-plates-3d` "Slanted Box Plates 3D / 斜めボックスプレート 3D" |
| template | `t-hokuto-box` "北斗-style slanted box plate (2D)", text 覇王伝説 | franchise name and a phrase tied to its key art | `casino/slanted-box-plate`, text 一撃必殺 / ONE STRIKE |
| template | `t-kanzen-fukkatsu`, texts 完全復活 + 転生 (source comment: "as on the 北斗 転生 / 完全復活 key art") | imitates a specific product's title treatment | `metal/liquid-gold-vertical-kicker`, texts 限界突破 + 覚醒 / LIMIT BREAK + AWAKEN |
| template | `t-ganso-3d`, text 元祖ツラヌキ | ツラヌキ is a mode name of commercial machines (precaution) | `casino/original-depth-fade-3d`, text 元祖カミナリ / THE ORIGINAL |
| look | `f-rainbow-glass-gold`, `f-3d-rainbow-glass-gold` "青ブタ虹 ..." | franchise (anime) | "Rainbow Glass + Gold Rim / 虹ガラス + 金縁" (and "3D") |
| look | `f-gold-classic` "Classic Gold (まぺろに)" | creator name | "Classic Gold / クラシックゴールド" |
| look | `f-gold-hiro` "Hiro Gold (double stroke)" | creator name | `double-stroke-gold` "Double-stroke Gold / 二重縁ゴールド" |
| look | `f-gold-ultimate` "Ultimate Gold (gradient map)" | a tutorial's title | `gradient-map-gold` "Gradient-map Gold / グラデーションマップゴールド" |
| look | `f-chrome` "Chrome (Eye Candy)", `f-chrome-liquid` "液体クローム Liquid Chrome (Eye Candy)" | product name | `chrome-bevel` "Chrome Bevel / クロームベベル"; "Liquid Chrome / 液体クローム" |
| look | `f-3d-gold` "Element Gold", `f-3d-chrome` "Element Chrome" | product name (Element 3D) | "Gold 3D / ゴールド 3D", "Chrome 3D / クローム 3D" |
| comments | Sammy, カバネリ, Eye Candy, Element 3D, tutorial site names | not copied: the converter carries no comments; provenance points at file and line | — |

The provenance of the renamed items records file, line and commit but `item: null` (their source ids
contain denied terms). `denied_terms.txt` starts with: hokuto, 北斗, 青ブタ, aobuta, まぺろに,
maperoni, ヒロデザ, "hiro gold", photoshopvip, "eye candy", "alien skin", "element 3d",
"video copilot", カバネリ, kabaneri, sammy, サミー, 覇王伝説, ツラヌキ, and the pachinko and
pachislot makers and major game and anime franchises (sankyo, daito, kyoraku, heiwa, newgin, sanyo,
konami, sega, nintendo, capcom, bandai, namco, pokemon, ポケモン, gundam, ガンダム, evangelion, エヴァ,
"dragon ball", naruto, "one piece", mario, zelda, "street fighter", "final fantasy", "dark souls",
"you died", "monster hunter", macross, lupin, ルパン, jojo, ジョジョ, juggler, ジャグラー, 海物語,
番長, まどか); the list grows by review, never shrinks. Generic genre words (確定, 激アツ, 大当り,
BIG BONUS, RUSH, FEVER, CHANCE, 保留, まくら, JACKPOT) are not trademarks and stay.

#### 6.7.4 Review of the output

1. The data diff: canonical sparse files, one item per file, reviewed as text (`git diff`).
2. The port report `port-pachislop` prints (and writes into the scratch directory): per item what mapped,
   what was approximated (a field our renderer treats differently), what was dropped.
3. Review sheets (R22): ours beside Pachislop's reference PNG, at the reference's text and scale,
   for every ported item. The PNGs stay outside the repository (they are the review aid only; our
   renderer is not expected to match them pixel for pixel).
4. `review.json` records the verdict; `tuned` items have their patch in `overrides.json`. A
   maintainer re-running the port after S83 or S86 change reviews only items whose render or data
   changed (`review-sheet --changed-since <git ref>`).

### 6.8 New looks (91)

Notation: Face = the face material; Rims inner → outer as `width% colour [extras]`; "base" = a
rim with offset `[0, −w]`; effects and generators by S80/S83 type; fonts as pack ids `latin / ja`.
Colours are the starting values; the author tunes them by eye against our renderer and records the
final values in the file. All 2D unless marked 3D (S86 material presets named as Pachislop's:
`gold`, `chrome`, `silver`, `candy`, `glass`, `plastic`, `emissive`, `ruby`, `blue_metal`, `black_gloss`,
`white_gloss`...).

**Casino (3)**

| id | Build | Font |
|---|---|---|
| `casino/vegas-marquee` | Face linear `#fff4c0 → #ffc020 → #a05a00`, smooth bevel 3 % with gloss `ring`, gold finish; rims 1 % `#200800`, 6 % `#b00010` with pattern `dots` (`#fff6d0`, scale 3 %) as bulbs and inner glow `#ffe0a0`, 1.5 % `#ffd040` bevelled; `outer_glow` `#ffb030` 24 px behind; generator `glints` (highlights) | bungee / dela-gothic-one |
| `casino/poker-velvet-red` | Face radial `#ff3040 → #8a0010`, texture `noise` 0.15 soft light, satin multiply 0.4; rims 1.2 % gold (gold finish, cove bevel), 3 % `#0a0204`; `drop_shadow` opacity 0.55, size 20 px, colour `#2a0006` | playfair-black-italic / zen-old-mincho-black |
| `casino/lucky-clover` | Face linear `#d8ffd0 → #20c040 → #005a20`, green metal finish; rims 1.5 % white with bevel, 4 % gold metal, base 2 % `#002a10`; generator `glints` (stars) | titan-one / dela-gothic-one |

**Metal & Chrome (2)**

| id | Build | Font |
|---|---|---|
| `metal/brushed-steel` | Face linear `#e8ecf0 → #8a94a0 → #d0d6dc`, texture `brushed` 0.5 at 0°, chamfer bevel 2.5 % depth 4 gloss `linear`; rims 1 % `#1a1e24`; `drop_shadow` opacity 0.6 size 10 px | anton / zen-kaku-gothic-new-black |
| `metal/riveted-iron-3d` (3D) | Extrude depth 20 %, chamfer bevel; face custom `#5a5e64` metallic 1 roughness 0.55 with `surface {kind: hammered}`; band 1: box plate 8 % gunmetal with pattern `dots` relief-only (rivets); environment `warehouse` | russo-one / zen-kaku-gothic-new-black |

**Neon (5)**

| id | Build | Font |
|---|---|---|
| `neon/sign-pink` | Knockout; rims 1 % `#ffe0f4` (tube core), 2 % `#ff2a9a`; `outer_glow` `#ff2a9a` 40 px add; `light_glow` tint pink, intensity 1.2 | tilt-neon / rocknroll-one |
| `neon/double-tube-cyan` | Knockout; rims 0.8 % white, 1.6 % `#00c8ff`, 1.4 % transparent gap (fill opacity 0), 0.8 % white, 1.6 % `#00c8ff`; `outer_glow` `#00e0ff` 32 px add | monoton / rocknroll-one |
| `neon/outline-lime` | Face `#c8ff3a` fill opacity 0.15; rim 1.4 % `#c8ff3a`; inner glow `#e8ff9a`; `outer_glow` `#a0ff20` 28 px add | tilt-neon / rocknroll-one |
| `neon/synthwave-chrome` | Face linear horizon chrome `#ffffff → #7ae0ff (0.45)`, hard stop to `#1a0050 (0.5) → #ff3ad0 → #fff0ff`, button bevel; rims 1 % white, 3 % `#2a0060`; `outer_glow` `#ff3ad0` 30 px; generator `light_streaks` (`cyan`) | audiowide / rocknroll-one |
| `neon/tube-3d` (3D) | Inflate; face `emissive` `#ff40c0` `emission_intensity` 2 under a `glass` band 1 % (transmission 1); environment `night_city`; glint on | tilt-neon / rocknroll-one |

**Fire, Ice & Energy (1)**

| id | Build | Font |
|---|---|---|
| `elements/cracked-earth` | Face linear `#c8a070 → #6a4020`, textures `grunge` (surface) and `crumple`, chamfer bevel 2 %; rims 1.5 % `#2a1608`, base 2 % `#140a04`; `inner_shadow` 4 px; `perspective_shadow` length 60 px | dela-gothic-one / dela-gothic-one |

**Gems & Glass (1)**

| id | Build | Font |
|---|---|---|
| `gems/ruby-faceted-3d` (3D) | Extrude 10 %, chamfer bevel 4 % (reads as facets); face `ruby`; band 2 % gold plate; generator `glints` (stars); `softbox` | playfair-black-italic / zen-old-mincho-black |

**Japanese Wa (1)**

| id | Build | Font |
|---|---|---|
| `wa/gold-leaf-washi` | Face solid `#e8c060` with texture `crumple` 0.6 (leaf) and finish gold ramp 0.5; rims 1 % `#c01a10` (vermilion line), 2 % `#1a1208`; `drop_shadow` soft `#2a1a00` | zen-old-mincho-black / zen-old-mincho-black |

**Sci-fi (8)**

| id | Build | Font |
|---|---|---|
| `scifi/hud-cyan` | Face `#00e0ff` fill opacity 0.2 with `scanlines` (intensity 0.35, spacing 4 px); rim 0.8 % `#8af6ff`; `outer_glow` `#00e0ff` 20 px add; generator `magic_circle` (`hud`) below, following | orbitron (wght 900) / train-one |
| `scifi/hologram-glitch` | Face linear `#bff8ff → #2a8aff`, opacity 0.75, sheen 0.8; `scanlines`; `glitch` amount 0.3; `chromatic_aberration` 4 px | audiowide / train-one |
| `scifi/plasma-edge` | Face `#0a0f1e`; rims 1 % `#50a0ff`, `outer_glow` `#3a80ff` 18 px; generator `lightning` (`electrify`, blue) | orbitron (wght 900) / train-one |
| `scifi/titanium-bevel-3d` (3D) | Extrude 16 %, chamfer 3 %; face `blue_metal` with `pattern` `brushed_metal` (S83's kind) and `flakes` 0.2; band 0.8 % `emissive` cyan keyline; `warehouse` | russo-one / train-one |
| `scifi/visor-chrome-3d` (3D) | Extrude 12 %, round bevel; face `chrome` with gradient `#ffffff → #2060ff (0.55) → #000820`; side gunmetal; `night_city`; glint | audiowide / train-one |
| `scifi/contour-light-guide` | Face `#0b1a2a`; rim 0.8 % `#2ad8ff`; `contour_lines` (4 lines inward, cyan, glow) | orbitron (wght 700) / train-one |
| `scifi/warp-streak` | Face linear `#ffffff → #6ae8ff`; rim 1 % `#04203a`; `motion_trail` (smear, 180°, 60 px, taper); generator `speed_lines` (`flow_h`) below | orbitron (wght 900) / train-one |
| `scifi/reactor-core-3d` (3D) | Inflate; face `emissive` `#ff8a20` `emission_intensity` 1.5; band 1.5 % `chrome`; band 3 % `black_gloss`; generator `aura` (`rising_weak`, orange) below; `night_dark` | russo-one / train-one |

**Fantasy (10)**

| id | Build | Font |
|---|---|---|
| `fantasy/engraved-gold` | Face classic gold ramp + finish, cove bevel 2.5 % (engraved) and `inner_shadow` 3 px; rims 1 % `#2a1404`, 3 % antique bronze (texture `hammered`, bevel), base 2 % `#140800` | cinzel-decorative-black / zen-old-mincho-black |
| `fantasy/carved-stone` | Face linear `#a8a49c → #6a665e`, textures `grunge` (surface) + `clouds`, chamfer 3 % depth 5; rim 1 % `#2a2824`; `inner_shadow` 4 px; `drop_shadow` soft | cinzel-decorative-black / zen-old-mincho-black |
| `fantasy/elven-silver` | Face linear `#ffffff → #c8d8f0 → #8aa0c8`, smooth bevel, inner glow `#e0f0ff`; rim 0.8 % `#f0f8ff`; `outer_glow` `#a0d0ff` 26 px; generator `glints` (stars, small) | cinzel-decorative-black / zen-old-mincho-black |
| `fantasy/dragon-fire` | Face linear `#ffe0a0 → #d06010 → #5a1a00` bronze finish; rims 1 % `#1a0600`, 3 % `#5a2a08`; generator `fire` (`behind`); `light_glow` orange | medievalsharp / dela-gothic-one |
| `fantasy/arcane-sigil` | Face linear `#f0c8ff → #8a2aff → #2a0060`, glass on (tint 0.6); rim 1 % white; generator `magic_circle` (`gold`, recoloured violet) below; `outer_glow` `#b060ff` 30 px add | cinzel-decorative-black / zen-old-mincho-black |
| `fantasy/royal-crest` | Face `#6a1aa0` with gloss and satin; rims 1 % `#1a0030`, 2 % gold metal, base 2 % gold metal; generator `glints` (highlights) | cinzel-decorative-black / zen-old-mincho-black |
| `fantasy/carved-wood` | Face linear `#c08850 → #7a4a20`, texture `brushed` 0.6 at 0° (grain) + `clouds` 0.2, chamfer 2 %; rim 1.2 % `#2a1606`; `drop_shadow` | medievalsharp / dela-gothic-one |
| `fantasy/parchment-ink` | Face `#2a1a0a`, erode amount 0.35 edge 4 %; no rims; generator `ink_splatter` (`ink_behind`, small) | medievalsharp / yuji-syuku |
| `fantasy/rune-glow-3d` (3D) | Extrude 12 %; face `emissive` `#50e0ff` `emission_intensity` 1.2; side and bevel custom stone `#6a6660` roughness 0.8 with `surface {kind: crumple}`; `night_dark`; glint | cinzel-decorative-black / zen-old-mincho-black |
| `fantasy/gilded-shield-3d` (3D) | Extrude 10 %; face `gold`; band 6 % box plate `blue_metal` enamel; band 1.5 % `gold`; `sunset_harbour` | cinzel-decorative-black / zen-old-mincho-black |

**Horror (10)**

| id | Build | Font |
|---|---|---|
| `horror/blood-drip` | Face linear `#c00010 → #5a0008` with gloss; rim 0.8 % `#200004`; `drips` (`blood` preset, length 30 px, colour from the layer); `inner_shadow` 2 px | nosifer / yuji-syuku |
| `horror/rotting-green` | Face linear `#8aa040 → #2a3a10`, texture `grunge` 0.7 (surface), erode 0.4; inner glow `#c0ff60` 0.3; generator `smoke` (`grey`, recoloured green) behind | creepster / yuji-syuku |
| `horror/scratched-bone` | Face linear `#f0e8d0 → #b0a080`, texture `grunge`, `distress` (`scratched`, angle 70°), smooth bevel 1 %; rim 1 % `#2a2010`; `drop_shadow` `#2a0000` soft | creepster / zen-old-mincho-black |
| `horror/ghost-mist` | Face white, fill opacity 0.55, inner glow `#e0f4ff`; `outer_glow` `#c0e8ff` 30 px; generator `smoke` (`steam`) above, opacity 0.6 | creepster / yuji-syuku |
| `horror/rusted-cage` | Face linear `#a04a1a → #4a1a08`, textures `grunge` + `hammered`, chamfer bevel; rims 1 % black, 3 % `#3a3a3a` with pattern `stripes` vertical (bars) | black-ops-one / dela-gothic-one |
| `horror/burnt-ember` | Face `#1a1410` with texture `grunge`; inner glow `#ff5010` noise 0.6 (embers in cracks); `outer_glow` `#ff3000` 12 px; generator `particles` (`embers`) | creepster / yuji-syuku |
| `horror/static-signal` | Face linear `#ffffff → #9a9a9a`; `scanlines` (`interlace`); `glitch` 0.5; `chromatic_aberration` 6 px; `grain` 0.4 | creepster / dela-gothic-one |
| `horror/gothic-candle` | Face linear `#ffe8b0 → #c08030`; rim 1 % `#100800`; `outer_glow` `#ffb040` 16 px; generator `backlight` (`god_rays`, gold, intensity 40) | cinzel-decorative-black / zen-old-mincho-black |
| `horror/obsidian-ooze-3d` (3D) | Inflate; face `black_gloss`; side `emissive` `#40ff60` `emission_intensity` 0.6; `night_dark` | nosifer / yuji-syuku |
| `horror/bloodstone-3d` (3D) | Extrude 10 %; face `ruby` darkened (`#5a0008`, transmission 0.3); band 2 % `black_gloss`; `night_dark`; glint intensity 0.5 | creepster / zen-old-mincho-black |

**Cartoon (10)**

| id | Build | Font |
|---|---|---|
| `cartoon/bubble-pop` | Face linear `#ffffff → #7ad8ff → #2a8aff`, gloss opacity 0.8, smooth bevel 6 % depth 6; rims 2 % white, 4 % `#10307a`, base 3 % `#0a1a40` | titan-one / mochiy-pop-one |
| `cartoon/comic-burst` | Face linear `#ffe020 → #ff8a00` with `halftone` (6 px, 45°, on the face part); rims 1.5 % black, 4 % white, 1.5 % black | bangers / dela-gothic-one |
| `cartoon/sticker-white` | Face `#ff5a8a`; rim 6 % white (round join); `drop_shadow` opacity 0.35 distance 6 px size 10 px | lilita-one / mochiy-pop-one |
| `cartoon/chunky-shadow` | Face `#ffd000`; rim 1.5 % `#3a1a00`; `extrude` (long shadow, 225°, 18 px, `#3a1a00`) | titan-one / potta-one |
| `cartoon/wooden-sign` | Face `#f0d090` texture `brushed`; rims 1 % `#4a2a10`, 5 % box plate (corner 3 %) `#8a5a2a` texture `brushed`, base 2 % `#2a1406` | lilita-one / potta-one |
| `cartoon/slime` | Face linear `#8aff4a → #20a010`, glass on (tint 0.4), gloss; rim 1.2 % `#0a3a00`; `drips` (`slime` preset, length 20 px) | titan-one / potta-one |
| `cartoon/fluffy-cloud` | Face white, smooth bevel 8 % soften 2 %; rim 3 % `#bfe4ff` round; `outer_glow` white 16 px | mochiy-pop-one / mochiy-pop-one |
| `cartoon/candy-stripe` | Face pattern `stripes` `#ff2a4a` on `#ffffff` at 45° relief 0.2, gloss; rims 1 % `#6a0010`, 3 % white, base 2 % `#6a0010` | titan-one / potta-one |
| `cartoon/jelly-3d` (3D) | Inflate; face `candy` `#ff4aa0` transmission 0.6; `softbox`; glint | potta-one / potta-one |
| `cartoon/toy-plastic-3d` (3D) | Extrude 18 %, round bevel; face `plastic` `#ffcc00`; side `plastic` `#ff6a00`; `softbox`; AO on | titan-one / potta-one |

**Retro Arcade (10)**

| id | Build | Font |
|---|---|---|
| `retro/pixel-8bit` | Face `#ffffff`; rims 4 % `#2a40ff` (miter), base 4 % `#000000`; `pixelate` (`retro_8bit`, size 4 px); lint `hard_shadow` allowed ("pixel art has hard shadows") | press-start-2p / dela-gothic-one |
| `retro/chrome-80s` | Face horizon chrome `#ffffff → #5ac8ff (0.48)`, hard stop to `#ffe0a0 (0.5) → #ff6a00 → #3a0a00`, button bevel; rims 1 % white, 2.5 % `#1a0040`; generator `glints` (stars); generator `light_streaks` (`cyan`) | bungee / rampart-one |
| `retro/vector-green` | Knockout; rim 0.6 % `#40ff60`; `outer_glow` `#40ff60` 14 px add; `grain` 0.15 | audiowide / train-one |
| `retro/crt-scanline` | Face linear `#ff4a2a → #ffd040`; `scanlines` (`crt` preset); `chromatic_aberration` 2 px; `outer_glow` `#ff8a40` 12 px | press-start-2p / dela-gothic-one |
| `retro/sunset-stripes` | Face linear `#ffe040 → #ff6a2a → #ff2a8a → #6a1aa0` with pattern `stripes` `#1a0040` horizontal scale 6 %; rim 1 % `#1a0040` | bungee / rampart-one |
| `retro/pinball-checker` | Face `chrome`-like ramp `#ffffff → #9aa4b4 → #f0f4f8`, button bevel; rims 1 % black, 3 % pattern `checker` black/white, 1 % black | bungee / rampart-one |
| `retro/vhs-tape` | Face white; `chromatic_aberration` 6 px horizontal; `glitch` 0.2; `grain` 0.3; `motion_blur` 6 px at 0° | silkscreen (Bold) / dela-gothic-one |
| `retro/16bit-gradient` | Face linear with hard stops `#ffffff`, `#ffe040`, `#ff9a00`, `#c03000` (stop pairs at 0.25, 0.5, 0.75); rims 1 % black, 3 % white, 1 % black; `drop_shadow` hard (distance 6 px, size 0); `pixelate` size 2 px; lint `hard_shadow` allowed | press-start-2p / rampart-one |
| `retro/arcade-bevel-red-3d` (3D) | Extrude 14 %; face `candy` red; side dark red; band 2 % `chrome`; `softbox` | bungee / dela-gothic-one |
| `retro/pixel-block-3d` (3D) | Extrude 25 %, no bevel; face `plastic` `#ffd000`, side `#ff6a00`; `softbox` | press-start-2p / dela-gothic-one |

**Anime (10)**

| id | Build | Font |
|---|---|---|
| `anime/impact-burst` | Face linear `#ffffff → #ffe040`; rims 1.5 % black, 4 % `#ff2a2a`, 1 % black; generator `speed_lines` (`focus`) below | dela-gothic-one / dela-gothic-one |
| `anime/kira-sparkle` | Face linear `#fff0f8 → #ff8ac8 → #d02a8a`, gloss; rims 2 % white, 1 % `#8a1a5a`; generators `glints` (stars, many) and `particles` (`star_shower`, small) | mochiy-pop-one / mochiy-pop-one |
| `anime/shonen-red-black` | Face linear `#ff1a1a → #8a0000`, satin; rims 1 % white, 4 % black (round); `drop_shadow`; generator `speed_lines` (`beta`) below | dela-gothic-one / dela-gothic-one |
| `anime/magical-pastel` | Face linear `#ffd0f0 → #d0e0ff → #d0fff0`, glass on (tint 0.3); rims 1.5 % white, 1 % `#c08aff`; generators `glints` and `particles` (sakura) | mochiy-pop-one / mochiy-pop-one |
| `anime/mecha-steel-3d` (3D) | Extrude 20 %, chamfer 3 %; face `silver`; band 2 % `red_metal`; band 1 % `black_gloss`; `warehouse` | russo-one / dela-gothic-one |
| `anime/manga-ink` | Face black with `halftone` (screen tone, 5 px) on the face; rims 2 % white, 0.8 % black | reggae-one / reggae-one |
| `anime/idol-holo` | Face white with `holographic` (`foil`) on the face part; rims 1.5 % white, 1 % `#ff5ac8`; generator `glints` | mochiy-pop-one / mplus1p-black |
| `anime/title-card-brush` | Face `#e01020` with erode 0.3; rim 2 % white; generator `brush_swirl` (`red`) below | reggae-one / yuji-syuku |
| `anime/candy-pop-3d` (3D) | Inflate; face `candy` `#ff6ab0`; band 1.5 % `white_gloss`; `softbox`; generator `glints` | mochiy-pop-one / mochiy-pop-one |
| `anime/ki-aura` | Face linear `#ffffff → #ffe060`; rim 1 % `#5a3a00`; generator `aura` (`strong`, gold) below; `light_glow` gold | dela-gothic-one / dela-gothic-one |

**Sports (10)**

| id | Build | Font |
|---|---|---|
| `sports/varsity` | Face `#c8102e`; rims 2 % white, 3 % `#0a1f44`; base 1.5 % `#0a1f44` | archivo-black / dela-gothic-one |
| `sports/trophy-gold-3d` (3D) | Extrude 12 %, round bevel; face `gold` (punchy); band 1.5 % `chrome`; `sunset_harbour`; generator `glints` | anton / zen-kaku-gothic-new-black |
| `sports/racing-checker` | Face chrome ramp `#ffffff → #8a94a0 → #e8ecf0`, linear gloss; rim 4 % pattern `checker` black/white; generator `speed_lines` (`flow_h`) below | teko (wght 700) / dela-gothic-one |
| `sports/scoreboard-led` | Face `#ffb000` with pattern `dots` `#1a1000` scale 1.2 % opacity 0.8 (dot matrix); `outer_glow` `#ffb000` 10 px add | teko (wght 600) / dela-gothic-one |
| `sports/jersey-number` | Face white; rims 3 % `#0a3a8a`, 1.5 % white, 2 % `#0a3a8a` (tackle twill); `drop_shadow` soft | anton / zen-kaku-gothic-new-black |
| `sports/stadium-lights` | Face linear `#ffffff → #c8d0dc`, linear gloss; rim 1 % `#10141c`; generators `glints`, `lens_flare` (`streak`, upper left), `backlight` (`god_rays`, white) | anton / zen-kaku-gothic-new-black |
| `sports/esports-angular` | Face linear `#00e0ff → #7a2aff`; rims 1 % white, 3 % `#10102a` as a box plate slanted 12°; `outer_glow` `#00c8ff` 18 px | teko (wght 700) / dela-gothic-one |
| `sports/fireball` | Face linear `#fff0a0 → #ff8a00`; rim 1 % `#3a0a00`; generator `fire` (`over_edges`); `motion_trail` (smear, 200°, 40 px) | black-ops-one / dela-gothic-one |
| `sports/metal-crest-3d` (3D) | Extrude 10 %; face `silver`; band 5 % box plate `blue_metal`; band 1.5 % `gold`; `sunset_harbour` | archivo-black / zen-kaku-gothic-new-black |
| `sports/chalk-board` | Face white; `chalk` (`chalkboard` preset); no rims | archivo-black / zen-kaku-gothic-new-black |

**Minimal (10)**

| id | Build | Font |
|---|---|---|
| `minimal/flat` | Face solid white; nothing else (a scheme colours it) | inter (wght 800) / zen-kaku-gothic-new-black |
| `minimal/outline-thin` | Knockout; rim 1 % white | inter (wght 700) / zen-kaku-gothic-new-black |
| `minimal/soft-shadow` | `applies: effects`: `drop_shadow` opacity 0.35, distance 8 px, size 24 px | — |
| `minimal/duotone` | Face linear `#ff6a5a → #7a5aff`, span line | inter (wght 900) / mplus1p-black |
| `minimal/long-shadow` | Face white; `extrude` (long shadow 225°, 60 px, black, opacity 0.25, fade) | archivo-black / mplus1p-black |
| `minimal/frosted-glass` | Face white, glass on (tint 0.2, opacity 0.5), inner glow white 0.4; rim 0.5 % white at fill opacity 0.6 | inter (wght 800) / zen-kaku-gothic-new-black |
| `minimal/ink-inset` | Face `#1a1a1a`; `inner_shadow` 2 px white 0.4 (letterpress) | inter (wght 900) / zen-kaku-gothic-new-black |
| `minimal/clean-gradient` | Face linear `#ffffff → #d0d6e0`, gloss 0.2; `drop_shadow` opacity 0.2 size 12 px | inter (wght 800) / zen-kaku-gothic-new-black |
| `minimal/mono-stroke` | Face `#111111`; rims 1.5 % white, 0.8 % `#111111` | archivo-black / mplus1p-black |
| `minimal/matte-3d` (3D) | Extrude 8 %, round bevel 1 %; face `plastic` white roughness 0.7; AO on; `softbox` | inter (wght 800) / zen-kaku-gothic-new-black |

#### 6.8.1 What the new looks need from S83 and S86

Every effect, generator and preset named above is in S83's catalogue (§6.6, §6.7, §6.8 of S83),
including its genre effects `drips`, `scanlines`, `pixelate`, `distress`, `chalk` and `holographic`;
the colour-scheme hooks are S83's `ParamDef::light` and `SpectrumSwitch` (S83 §6.2). From S86: extrude
and inflate, bevel, bands with box plates, the material presets named (S86 R17), `emissive`, glint,
AO, and the environments named (S86 R24 plus the four S84 adds, §6.12). Where S83 or S86 rename
something before landing, their names win and the look files follow.

### 6.9 Color Schemes (33)

The 11 ported schemes keep Pachislop's ids, tables, rules and light ramps (`colourways.ts`
`COLOURWAY_DEFS`, `LIGHT`): `white` 白, `blue` 青, `green` 緑, `purple` 紫, `red` 赤, `gold` 金,
`silver` 銀, `zebra` ゼブラ, `giraffe` キリン, `leopard` ヒョウ, `rainbow` 虹 (with the production rules
in their data: blue and green take silver rims, red takes a gold rim and stays at hue 0, gold never
goes olive, no black shadows).

#### 6.9.1 Authoring a new scheme

A new scheme is written from anchors with `python3 -I tools/titleart/expand_scheme.py <anchors.json>`,
which writes the full `.titlecolors` file (the committed file is the full one; the anchors file is
not kept). Derivations:
- `paints.face.fill`: linear gradient of the face ramp's stops, angle 90; `ramp` (metal finish):
  `[dark, face darkest, face mid, face lightest, #ffffff]` when `finish` is `metal`, else null;
  `material_3d`: `metal` → metallic 1, roughness 0.25, colour = face ramp at 0.4; `gloss` →
  metallic 0, clearcoat 1, roughness 0.15; `matte` → roughness 0.7; `shade` = `shadow`.
- `paints.dark`, `base`, `accents[]`: solid fills of the anchors (an accent named `gold metal` or
  `silver metal` takes the `gold` / `silver` scheme's accent paint); `side_3d` = face darkest × 0.7.
- `face_glow` = face lightest; `glow_gradient` = `[glow, face mid]`; `glint` = face lightest;
  `light_ramp` = the face stops, `glow` and `shadow` sorted by Rec.601 luma, near-duplicates (ΔE < 3)
  removed, 4 to 7 entries; `orb.ramp` = face at 0.1, 0.55, 0.95, radial; `swatch` = face stops,
  dark, last accent.

| id | Name (en / ja) | Face ramp (light → dark) | Dark | Accents (inner → outer) | Base | Glow | Shadow | Finish | `family_hint` |
|---|---|---|---|---|---|---|---|---|---|
| `neon-pink` | Neon Pink / ネオンピンク | `#ffe6f6 #ff4ab8 #c0007a` | `#1a0010` | `#ffffff`, `#ff2a9a` | `#2a0018` | `#ff2a9a` | `#3a0020` | gloss | neon, retro, anime |
| `neon-cyan` | Neon Cyan / ネオンシアン | `#e6fbff #3ae0ff #0090c0` | `#001a24` | `#ffffff`, `#00c8ff` | `#002030` | `#00e0ff` | `#002a3a` | gloss | neon, scifi |
| `synthwave` | Synthwave / シンセウェーブ | `#fff0ff #ff5ad0 #7a2aff #1a0a5a` | `#0a0020` | `#5ae0ff`, `#ff3ad0` | `#12002a` | `#ff3ad0` | `#1a0030` | metal | retro, neon |
| `ember` | Ember / 残り火 | `#fff4c0 #ffb020 #ff4a00 #5a0a00` | `#1a0400` | `#ffd040`, `#8a1a00` | `#2a0800` | `#ff6a00` | `#3a0a00` | metal | elements, fantasy, sports |
| `ice` | Ice / 氷 | `#ffffff #cfefff #7ac8f0 #1a5a8a` | `#06203a` | `#ffffff`, `#8ad8ff` | `#0a2a4a` | `#9ae0ff` | `#0a2a40` | gloss | elements, fantasy |
| `blood` | Blood / 血 | `#ff6a6a #c00010 #5a0008 #1a0002` | `#0a0000` | `#3a0004`, `#8a0010` | `#100000` | `#ff1a1a` | `#200000` | gloss | horror |
| `bone` | Bone / 骨 | `#fffaf0 #efe4c8 #c8b48a #6a5a3a` | `#1a140a` | `#3a2a14`, `#8a7450` | `#140e06` | `#fff0c8` | `#2a2010` | matte | horror, fantasy |
| `toxic` | Toxic / 毒 | `#f4ffc0 #b8ff2a #40c000 #0a3a00` | `#061400` | `#1a1a1a`, `#c8ff00` | `#0a1a00` | `#a0ff20` | `#102a00` | gloss | horror, scifi, cartoon |
| `royal` | Royal / ロイヤル | `#f0d8ff #a050f0 #5a10a0 #1a0040` | `#0a0018` | gold metal, `#ffd040` | `#12002a` | `#c080ff` | `#20003a` | metal | fantasy, casino |
| `emerald` | Emerald / エメラルド | `#e0ffe8 #40e080 #008a40 #002a14` | `#00140a` | silver metal, `#ffffff` | `#001a0c` | `#40ff90` | `#00200f` | metal | gems, casino |
| `bronze` | Bronze / ブロンズ | `#fff0d0 #e0a050 #9a5a20 #3a1a06` | `#140800` | `#5a3010`, `#e0b070` | `#1a0c02` | `#ffb060` | `#2a1406` | metal | metal, fantasy, sports |
| `obsidian` | Obsidian / 黒曜 | `#8a8f9a #3a3e48 #14161c #000000` | `#000000` | `#5a606c`, `#c8ced8` | `#000000` | `#9aa8c0` | `#000000` | gloss | metal, horror, minimal |
| `mono-light` | Mono Light / モノ（白） | `#ffffff` (solid) | `#111111` | `#111111`, `#ffffff` | `#000000` | `#ffffff` | `#000000` | matte | minimal, sports |
| `mono-dark` | Mono Dark / モノ（黒） | `#111111` (solid) | `#111111` | `#ffffff`, `#111111` | `#000000` | `#ffffff` | `#000000` | matte | minimal |
| `ink` | Ink / 墨 | `#1a1612 #000000` | `#000000` | `#f4eee0`, `#c01a10` | `#000000` | `#f4eee0` | `#000000` | matte | wa, anime, fantasy |
| `pastel` | Pastel / パステル | `#ffffff #ffd6ec #c8d8ff #d0fff0` | `#5a4a7a` | `#ffffff`, `#c08aff` | `#3a2a5a` | `#ffd6ff` | `#6a5a8a` | gloss | anime, cartoon |
| `candy` | Candy / キャンディ | `#fff0f6 #ff7ab8 #ff2a7a` | `#5a0024` | `#ffffff`, `#3ad8e0` | `#3a0018` | `#ff8ad0` | `#5a1030` | gloss | cartoon, anime, casino |
| `sunset` | Sunset / 夕焼け | `#fff0a0 #ffa040 #ff4a7a #7a1a8a` | `#1a0420` | `#ffe0a0`, `#4a0a5a` | `#200828` | `#ff7a40` | `#300a30` | metal | retro, sports |
| `ocean` | Ocean / 海 | `#e0fbff #40c8e0 #0a6aa0 #02203a` | `#001424` | `#ffffff`, `#0a3a6a` | `#000c18` | `#40e0ff` | `#001a2a` | gloss | elements, sports, minimal |
| `forest` | Forest / 森 | `#e8ffd0 #7ac040 #2a6a20 #0a2008` | `#0a1404` | `#c8a060`, `#3a2410` | `#0a0a02` | `#b0ff80` | `#102008` | matte | fantasy, cartoon |
| `team-navy-gold` | Navy & Gold / ネイビー＆ゴールド | `#2a4aa0 #0a1f44` | `#000814` | `#ffffff`, gold metal | `#000814` | `#ffd040` | `#000a20` | gloss | sports |
| `team-red-white` | Red & White / 紅白 | `#ff3a3a #c8102e` | `#2a0006` | `#ffffff`, `#0a1f44` | `#1a0004` | `#ff6a6a` | `#2a0008` | gloss | sports, casino |

### 6.10 Templates (73)

#### 6.10.1 The 37 ported templates

Each keeps Pachislop's construction (guide backdrops, groups, generator stacks, part effects,
clipping, adjustment layers) and its Japanese text as `sample.ja`; `sample.en` is listed.

| Pachislop | Ours | `sample.en` (`sample.ja`) |
|---|---|---|
| `t-kakutei-rainbow` | `casino/confirm-rainbow` | WINNER (確定) |
| `t-gekiatsu-fire` | `casino/red-hot-fire` | RED HOT (激アツ) |
| `t-gekiatsu-burst` | `casino/red-hot-burst` | RED HOT (激アツ) |
| `t-oatari-3d` | `casino/big-win-3d` | BIG WIN (大当り) |
| `t-bigbonus-3d` | `casino/big-bonus-3d` | BIG⏎BONUS |
| `t-bonus-ribbon` | `casino/bonus-ribbon` | BONUS |
| `t-counter-100g` | `casino/game-counter` | +100G |
| `t-giji-ren` | `casino/chain-x3-slanted` | CHAIN ×3 (擬似連×3) |
| `t-horyu-orb` | `casino/reserve-orbs-3d` | (no text) |
| `t-push-button` | `casino/push-button-candy-3d` | PUSH |
| `t-zugara-7` | `casino/reel-symbol-7` | 7 |
| `t-serif` | `casino/speech-frame-red` | THIS IS IT! (ここで決めろ！) |
| `t-cutin` | `casino/cut-in-band` | CHANCE (チャンス) |
| `t-kakutei-giraffe-3d` | `casino/confirm-giraffe-3d` | WINNER (確定) |
| `t-shindai-flyer` | `casino/new-machine-flyer` | NEW ARRIVAL (新台入替) |
| `t-chance` | `casino/chance-green-blue` | CHANCE + GO FOR IT (チャンス + CHANCE) |
| `t-gekinetsu-brush` | `wa/red-hot-brush-vertical` | RED HOT (激熱), 1080 × 1920 |
| `t-premium` | `casino/premium-black-gloss` | PREMIUM (プレミアム) |
| `t-rainbow-3d` | `casino/attack-chance-rainbow-3d` | ATTACK⏎CHANCE |
| `t-popout-3d` | `casino/rush-pop-out-3d` | RUSH |
| `t-cho-gekiatsu` | `casino/super-red-hot` | SUPER⏎RED HOT (超⏎激アツ) |
| `t-satin-chrome` | `metal/special-satin-chrome` | SPECIAL |
| (source id withheld, §6.7.3) | `casino/slanted-box-plate` | ONE STRIKE (一撃必殺) |
| `t-kanzen-fukkatsu` | `metal/liquid-gold-vertical-kicker` | LIMIT BREAK + AWAKEN (限界突破 + 覚醒) |
| `t-push-inflate` | `casino/push-button-inflate-3d` | PUSH |
| `t-ganso-3d` | `casino/original-depth-fade-3d` | THE ORIGINAL (元祖カミナリ) |
| `t-yaku-80` | `casino/about-80-percent-3d` | ABOUT 80% (約80%) |
| `t-neon-knockout` | `neon/fever-hollow-neon` | FEVER |
| `t-kasure-brush` | `wa/awakening-dry-brush` | AWAKEN (覚醒) |
| `t-gekiatsu-rim-glow` | `casino/red-hot-rim-glow` | RED HOT (激アツ) |
| `t-kakutei-zebra-clip` | `casino/confirm-zebra-clip` | WINNER (確定) |
| `t-bigbonus-3d-groups` | `casino/big-bonus-3d-groups` | BIG BONUS |
| `t-recipe-gekinetsu` | `casino/red-hot-plate-fx` | RED HOT (激熱) |
| `t-recipe-cutin` | `casino/cut-in-band-fx` | CHANCE + THIS IS IT! (CHANCE + ここで決めろ！) |
| `t-recipe-rush` | `casino/rush-title-fx` | RUSH |
| `t-recipe-premium-rainbow` | `casino/premium-rainbow-fx` | PREMIUM |
| `t-recipe-horyu-issen` | `casino/reserve-flash-fx` | (no text) |

(⏎ = a line break.) Templates whose text has per-character styling (`t-gekiatsu-burst`: 激 red, アツ
gold) store per-language character adjustments in the placeholder (`chars: {en: {...}, ja: {...}}`),
applied with the sample; carried user text keeps only adjustments within its length (S80 `set_text`).
Two templates have no text layer: their `placeholders` is empty.

#### 6.10.2 The 36 new templates

Canvas 1920 × 1080 transparent unless stated; backdrops are guide layers (a `fill` generator with
the family's gradient) unless stated; "title" / "subtitle" are the `text` / `subtitle` placeholders.

| id | Texts en (ja) | Build |
|---|---|---|
| `casino/vegas-title` | LUCKY 777 (ラッキー777) / JACKPOT | title `casino/vegas-marquee`; subtitle `casino/red-plate-gold-rim`; `sunburst` (`two_colour_wedges`, red and gold) in a group masked by a radial gradient mask; `particles` (`gold_dust`) |
| `metal/chrome-title` | STEEL FORCE (鋼鉄) / RISE | title `metal/chrome-3d`; subtitle `metal/silver-premium`; `lens_flare` on the title's top left |
| `metal/gold-award-badge` | CHAMPION (優勝), 1024 × 1024 | shield Shape `metal/gold-3d`; title `metal/black-gold` arched (warp `arc` 30); `glints` |
| `neon/bar-sign-title` | OPEN LATE (深夜営業) / BAR | backdrop dark brick (`pattern_fill` checker scaled to bricks, guide); title `neon/sign-pink`; subtitle `neon/outline-lime` |
| `neon/game-over-neon` | GAME OVER (ゲームオーバー), 1280 × 720 | title `neon/double-tube-cyan`; `glitch` on a group with the title |
| `elements/inferno-title` | INFERNO (灼熱) / BURNING | title `elements/red-hot-fire`; `fire` and `particles` (embers) owned by the look; subtitle `elements/cracked-earth` |
| `elements/frozen-banner` | FROZEN (氷結) | title `elements/ice`; `particles` (`light_motes`, white, slow fall); ribbon Shape behind `elements/ice-crystal-glass-3d` |
| `elements/storm-counter` | ×999, 1024 × 512 | digits `elements/electric-blue` with `lightning` (`bolts`) |
| `gems/crystal-title` | CRYSTAL (水晶) / LEGEND | title `gems/diamond-3d`; `glints`; `particles` (`bokeh`) |
| `gems/treasure-banner` | TREASURE! (お宝ゲット！) | ribbon Shape `metal/gold-3d`; title `gems/ruby-faceted-3d` |
| `gems/gem-counter` | +500, 1024 × 512 | digits `gems/sapphire-silver-frame-3d`; `glints` |
| `wa/ink-title-vertical` | LEGEND (伝説), 1080 × 1920, vertical text for ja | title `wa/ink-brush`; red seal (rect Shape `wa/red-brush`, kicker 印); `ink_splatter` |
| `scifi/hud-title` | SYSTEM ONLINE (システム起動) | title `scifi/hud-cyan` (with its `magic_circle`); subtitle `scifi/contour-light-guide` |
| `scifi/mission-complete` | MISSION COMPLETE (任務完了) | title `scifi/visor-chrome-3d`; `light_streaks` (`anamorphic`); `shockwave` (`halo`) behind |
| `scifi/countdown` | 00:59, 1024 × 512 | digits `scifi/contour-light-guide` |
| `fantasy/kingdom-title` | ASHEN REALM (灰の王国) / CHRONICLES | title `fantasy/engraved-gold`; subtitle `fantasy/carved-stone`; `backlight` (`god_rays`, gold) |
| `fantasy/quest-complete` | QUEST COMPLETE (クエスト達成) | shield Shape `fantasy/gilded-shield-3d`; title `fantasy/royal-crest`; `glints`, `particles` (`gold_dust`) |
| `fantasy/spell-label` | FIREBALL (火球), 1024 × 512 | title `fantasy/arcane-sigil` |
| `horror/nightmare-title` | NIGHTMARE (悪夢) / NO ESCAPE | title `horror/blood-drip`; subtitle `horror/scratched-bone`; `smoke` (`black`) behind |
| `horror/the-end` | THE END (終焉) | title `horror/gothic-candle`; `grain` adjustment-layer effects (`effects_only` with `grain`) |
| `horror/dread-counter` | 13, 1024 × 512 | digits `horror/burnt-ember` |
| `cartoon/party-title` | PARTY TIME (パーティータイム) | title `cartoon/bubble-pop`; burst Shape behind `cartoon/comic-burst`; `particles` (`confetti`) |
| `cartoon/level-up` | LEVEL UP! (レベルアップ！) | title `cartoon/chunky-shadow`; `sunburst` behind |
| `cartoon/coin-counter` | ×120, 1024 × 512 | digits `cartoon/candy-stripe` |
| `retro/arcade-title` | SUPER ARCADE (スーパーアーケード) / PRESS START | title `retro/chrome-80s`; subtitle `retro/pixel-8bit`; backdrop grid (`pattern_fill` stripes, guide) |
| `retro/insert-coin` | INSERT COIN (コインを入れてね) | title `retro/crt-scanline` |
| `retro/high-score` | HI-SCORE 123450, 1024 × 256 | text `retro/16bit-gradient` |
| `anime/episode-title` | EPISODE 1 (第一話) / THE BEGINNING | title `anime/title-card-brush`; subtitle `anime/manga-ink`; `speed_lines` |
| `anime/power-up` | POWER UP! (覚醒！) | title `anime/ki-aura` |
| `anime/combo-counter` | 12 HIT (12連撃), 1024 × 512 | digits `anime/impact-burst` |
| `sports/match-title` | FINAL MATCH (決勝戦) / 2026 | title `sports/metal-crest-3d`; subtitle `sports/varsity`; `lens_flare`, `backlight` |
| `sports/goal-banner` | GOAL!! (ゴール！！) | title `sports/fireball`; `speed_lines` (`flow_h`) |
| `sports/scoreboard` | 3 - 2, 1024 × 256 | digits `sports/scoreboard-led` |
| `minimal/clean-title` | STILL WATERS (静寂) / A STORY | title `minimal/flat` with `minimal/soft-shadow`; subtitle `minimal/outline-thin` |
| `minimal/chapter-card` | CHAPTER 1 (第1章) | title `minimal/duotone`; a thin rect Shape rule `minimal/flat` |
| `minimal/hud-label` | SCORE 0 (スコア 0), 512 × 128 | text `minimal/ink-inset` |

`gems/treasure-banner`, `elements/frozen-banner`, `metal/gold-award-badge`, `fantasy/quest-complete` and
`sports/match-title` use 3D looks and are unavailable until S86 (R6). `horror/the-end` is a
generic ending banner; game-specific catchphrases are denied terms.

### 6.11 The font pack (32 families, 46,181,548 bytes)

Fetched from `https://raw.githubusercontent.com/google/fonts/2fe1e8c6b137fb13041a7cfca6fdcd8a910ea4dc/<path>`
(sizes below read from the GitHub contents API at that commit on 2026-11-04; the eleven Japanese
files are the ones Pachislop bundles, byte-identical per report p6). Every one is under `ofl/`
(SIL Open Font License 1.1, no Reserved Font Name). Directory = the pack id.

| Pack id | Family, style (variation used) | `google/fonts` path | Bytes | Scripts | Main families |
|---|---|---|---|---|---|
| `dela-gothic-one` | Dela Gothic One | `ofl/delagothicone/DelaGothicOne-Regular.ttf` | 2,508,848 | latin, ja | casino, anime, elements |
| `mplus1p-black` | M PLUS 1p Black | `ofl/mplus1p/MPLUS1p-Black.ttf` | 1,774,332 | latin, ja | minimal, anime |
| `zen-kaku-gothic-new-black` | Zen Kaku Gothic New Black | `ofl/zenkakugothicnew/ZenKakuGothicNew-Black.ttf` | 2,300,600 | latin, ja | metal, sports, minimal |
| `zen-old-mincho-black` | Zen Old Mincho Black | `ofl/zenoldmincho/ZenOldMincho-Black.ttf` | 5,416,348 | latin, ja | casino, wa, fantasy, gems |
| `yuji-syuku` | Yuji Syuku (brush) | `ofl/yujisyuku/YujiSyuku-Regular.ttf` | 8,430,348 | latin, ja | wa, horror |
| `reggae-one` | Reggae One | `ofl/reggaeone/ReggaeOne-Regular.ttf` | 2,153,256 | latin, ja | anime |
| `rocknroll-one` | RocknRoll One | `ofl/rocknrollone/RocknRollOne-Regular.ttf` | 2,682,824 | latin, ja | neon |
| `rampart-one` | Rampart One | `ofl/rampartone/RampartOne-Regular.ttf` | 3,722,352 | latin, ja | retro |
| `train-one` | Train One | `ofl/trainone/TrainOne-Regular.ttf` | 2,152,516 | latin, ja | scifi, retro |
| `potta-one` | Potta One | `ofl/pottaone/PottaOne-Regular.ttf` | 4,918,516 | latin, ja | cartoon |
| `mochiy-pop-one` | Mochiy Pop One | `ofl/mochiypopone/MochiyPopOne-Regular.ttf` | 5,163,948 | latin, ja | cartoon, anime |
| `playfair-black-italic` | Playfair Italic (variable; `wght 900`, `opsz` 5) | `ofl/playfair/Playfair-Italic[opsz,wdth,wght].ttf` | 1,783,732 | latin | casino, gems |
| `anton` | Anton | `ofl/anton/Anton-Regular.ttf` | 170,812 | latin | metal, sports |
| `russo-one` | Russo One | `ofl/russoone/RussoOne-Regular.ttf` | 39,124 | latin, cyrillic | metal, scifi, anime |
| `bungee` | Bungee | `ofl/bungee/Bungee-Regular.ttf` | 118,996 | latin | casino, retro |
| `monoton` | Monoton | `ofl/monoton/Monoton-Regular.ttf` | 51,604 | latin | neon |
| `tilt-neon` | Tilt Neon (variable `XROT`, `YROT`) | `ofl/tiltneon/TiltNeon[XROT,YROT].ttf` | 583,288 | latin | neon |
| `cinzel-decorative-black` | Cinzel Decorative Black | `ofl/cinzeldecorative/CinzelDecorative-Black.ttf` | 62,692 | latin | fantasy, horror |
| `medievalsharp` | MedievalSharp | `ofl/medievalsharp/MedievalSharp.ttf` | 148,712 | latin | fantasy |
| `orbitron` | Orbitron (variable `wght`) | `ofl/orbitron/Orbitron[wght].ttf` | 38,576 | latin | scifi |
| `audiowide` | Audiowide | `ofl/audiowide/Audiowide-Regular.ttf` | 69,916 | latin | scifi, neon, retro |
| `creepster` | Creepster | `ofl/creepster/Creepster-Regular.ttf` | 62,724 | latin | horror |
| `nosifer` | Nosifer | `ofl/nosifer/Nosifer-Regular.ttf` | 43,272 | latin | horror |
| `bangers` | Bangers | `ofl/bangers/Bangers-Regular.ttf` | 93,148 | latin | cartoon |
| `lilita-one` | Lilita One | `ofl/lilitaone/LilitaOne-Regular.ttf` | 28,092 | latin | cartoon |
| `titan-one` | Titan One | `ofl/titanone/TitanOne-Regular.ttf` | 55,712 | latin | cartoon, casino |
| `press-start-2p` | Press Start 2P | `ofl/pressstart2p/PressStart2P-Regular.ttf` | 118,204 | latin | retro |
| `silkscreen` | Silkscreen Regular + Bold | `ofl/silkscreen/Silkscreen-{Regular,Bold}.ttf` | 32,220 + 30,632 | latin | retro |
| `teko` | Teko (variable `wght`) | `ofl/teko/Teko[wght].ttf` | 292,108 | latin, devanagari | sports |
| `black-ops-one` | Black Ops One | `ofl/blackopsone/BlackOpsOne-Regular.ttf` | 166,532 | latin | sports, horror |
| `inter` | Inter (variable `opsz`, `wght`; upright only) | `ofl/inter/Inter[opsz,wght].ttf` | 876,576 | latin, greek, cyrillic | minimal |
| `archivo-black` | Archivo Black | `ofl/archivoblack/ArchivoBlack-Regular.ttf` | 90,988 | latin | sports, minimal |

Japanese: 41,223,888 bytes; Latin: 4,957,660 bytes.

#### 6.11.1 Decisions

- **All 32 in the install, none in the binary, a copy in a project on use.** Every look must render
  offline the moment the editor is installed, and Japanese titles are first class, so the pack is
  whole (44 MiB staged with the install, like S36's library). The editor binary does not grow by a
  font. A project only gets the faces its documents use; a game only gets what S81 subsets from them.
- **Variable fonts stay variable.** Playfair, Tilt Neon, Orbitron, Teko and Inter ship as upstream's
  variable files; looks set axes through S80's `variations`. No derived static instance is made, so
  the files stay byte-identical to upstream and the OFL's modification clause does not arise.
- **The editor UI** (Library tile names in Japanese, the text field) needs a CJK UI font; that is
  S82's (e2 §7): the pack's Black weights are display faces, not UI faces.

#### 6.11.2 Files and notices

`editor/resources/titleart/fonts/<pack id>/`: the font file(s) as named upstream, `OFL.txt` (upstream's
copy, verbatim), `SOURCE.json` (`{repo, commit, path, sha256, bytes, fetched}`); `fonts.json` (the
manifest). `NOTICE.md` gets a "Fonts of the Title Art library (editor install; copied into a project
when a document uses them)" table: Font | Version (google/fonts `2fe1e8c6b137`, the font's own
version string) | Location (`editor/resources/titleart/fonts/<id>/` with `OFL.txt`) | Licence (OFL-1.1)
| Used for. `check-notices.py` enforces it (R31).

#### 6.11.3 Import on use

`TitleLibraryImports::font(packId)` returns the project asset for a pack face: an existing asset under
`assets/TitleArt/Fonts/<pack id>/` whose SHA-256 equals the pack file (any other path with the same
bytes is also found through S04's asset database), else a copy plan: the font file and `OFL.txt` into
`assets/TitleArt/Fonts/<pack id>/`, the font's `.meta` minted by S04 with a `provenance` section
`{licence: OFL-1.1, source: titleart-pack, url: <upstream path at the commit>, builtin: {set: fonts,
id: <pack id>, sha256}}` (S36 R8's schema; if S36 has not landed, S84 adds the section to
`AssetMeta::isKnown` with S36's exact schema). The copy is part of the op's undo step (`CopyFileCmd`
per file and `SetAssetMetaCmd`, grouped with the document write). A file already there with other
bytes is never overwritten (`font_conflict`, the import uses `<pack id>-2/`).

### 6.12 Environments (S86's system; four added by S84)

S86 R24 defines the environment ids: procedural `studio`, `strips`, `backlit`, `under`, `sunset`,
`neon`, `gold`, `night`, `softbox` (Studio Small 09 plus the key softbox), photo `highway` (Autumn
Road), `city` (Potsdamer Platz), `sky` (Derelict Highway Midday), `custom`, `matcap`. Ported looks keep
Pachislop's names, which are S86's. S84 adds four photo environments to S86's manifest, for families
Pachislop had no light for (asset pages checked on 2026-11-04: CC0, 1K to 16K available):

| id | Poly Haven asset | Author (asset page) | Used by |
|---|---|---|---|
| `night_city` | `shanghai_bund` | Greg Zaal | neon, sci-fi (neon skyline reflections) |
| `night_dark` | `moonless_golf` | Greg Zaal | horror, fantasy runes, reactor |
| `sunset_harbour` | `venice_sunset` | Greg Zaal | sports, fantasy gold |
| `warehouse` | `empty_warehouse_01` | Sergej Majboroda | metal, sci-fi, mecha |

They are fetched, licensed, staged and noticed exactly as S86's four (S86 §6.5 "Fetch", R66): 2k
`.hdr` in `editor/resources/titleart/hdri/`, the manifest's slug, URL and SHA-256, never from the
Pachislop repository. 2D looks reach a photo environment as a reflection map (§6.7.1): Pachislop's
2D map `studio` is `softbox`'s HDRI, `highway`, `city`, `sky` are S86's photos of the same names.
Import on use: `assets/TitleArt/Environments/<slug>_2k.hdr` with a `.meta` (`import.textureImporter`
type HDR; provenance `{licence: CC0-1.0, source: polyhaven, url}`), the same rules as fonts (§6.11.3).

### 6.13 Derived tags and the search index

`TitleLibraryIndex` derives for every item: `uses` (effect, generator, pattern, texture and 3D
material types), `features` (`bevel`, `knockout`, `erode`, `glass`, `reflection`, `finish`,
`pattern:<kind>`, `texture:<kind>`, `3d`, `inflate`, `extrude`, `generators`) and `cost`
(S80's `CostClass` `C0`..`C4`: the maximum over the item's effects and generators, at least `C3`
for any 3D look). Each derived term has an English phrase and a Japanese
word from a table in `TitleLibrarySearch.cpp` (`light_glow` → "light glow", "power glow" / ライトグロー,
`pattern:zebra` → "zebra" / ゼブラ, `knockout` → "hollow" / 中抜き, `inflate` → "puffy" / ぷっくり);
both are searchable. Family names are searchable in both languages and by id.

### 6.14 Quality

#### 6.14.1 Style lint rules (`check_title` check `style_lint`)

Colours are sampled at 9 points per element (the fill gradient, pushed through a 2D finish by its
amount; the 3D gradient or colour) and judged in CIE L\*a\*b\* (D65), as `lint.ts`. Rings: the face
then every enabled rim without offset (2D) or band (3D), inner to outer.

| Check id | Fires when (starting threshold) | Fix op |
|---|---|---|
| `style_lint.contrast` | neighbouring rings differ by ΔL\* < **12**; exempt: either is a spectrum (chromatic spread > 120°), both L\* < **25**, both banded (L\* range > **30**) | `add_rim` of 0.6 % between them, white or `#0a0604` (whichever contrasts more with the pair's mean L\*) |
| `style_lint.hue_count` | ≥ **3** chromatic hue families (C\* > **25**; one family = within **30°**) across face and rings | none (advice: keep rims achromatic or in the face's family) |
| `style_lint.rims_thin` | 2D text, rim total > 0 and < **5 %** of size | `set` rims scaled to a total of 8 % |
| `style_lint.rims_thick` | rim total > **16 %** (heavy-face stem width) | `set` rims scaled to 12 % |
| `style_lint.hard_shadow` | a `drop_shadow` with opacity > 0.8, size < 4 % of size (in px at the layer's size) and luma < 0.15; | `set` opacity 0.6 and size 7 % |
| `style_lint.spectrum_span` | multi-line text whose face or a rim has a spectrum gradient with span `layer` | `set` span `line` |
| `style_lint.noop_knockout` | knockout on with no enabled rim of width > 0, or with face fill opacity 1 | `add_rim` 1 % white / `set` face opacity 0 |
| `style_lint.noop_erode` | erode on with amount 0 or edge 0 | `set` amount 0.5, edge 6 % |

Two pixel checks (`editor/titleart/library/LibraryChecks.cpp`), on the layer's isolated render:

| Check id | Measures (starting threshold) | Fix op |
|---|---|---|
| `color_scheme_match` | for a layer with `look.color_scheme`: the mean OKLCH hue of its face ink (alpha ≥ 0.5, excluding white cores) against the hue span of the scheme's face ramp; FAIL beyond **20°** outside the span, or chroma below half the ramp's (gold gone olive, red gone pink) | `apply_color_scheme` with the recorded scheme |
| `metal_range` | for a face with a metal finish or bevel (2D `finish` or `bevel` on; 3D `metallic` ≥ 0.5): L\* range (p95 − p5) of the face pixels ≥ **30**; below, the metal reads as flat paint | `set face.finish.enabled true` (2D) / raise the environment intensity (3D) |

Rule texts (our words), e.g. contrast: "Neighbouring rims need different lightness, or they read as one
blurred band: alternate light and dark outward." Thresholds are constants in `StyleLint.h`, the
starting points `calibration.json` confirms (R37).

#### 6.14.2 The gate

`test_title_library.py` renders through `render_title` at 0.25 × the canvas (Text layers at size 200
on a 1920 × 1080 canvas for looks and schemes, the template's own canvas for templates), on the
`black`, `white` and `checker` backgrounds for `legibility_contrast`, and calls `check_title` with
every default check. It shards by item over `-j` editors (the harness's sharding). Outputs on failure:
the render, the check's crops and a three-panel sheet in the kept scratch directory, the item id and
check in the assertion text. On pass it prints per kind the count checked, unavailable (by reason) and
allowlisted. It runs in the full suite and in `tools/check-changed` when a path under
`editor/resources/titleart/`, `editor/titleart/`, `engine/core/titleart/` or the title shaders changes.

#### 6.14.3 Calibration rule

For a check with a numeric threshold `T` passing when `metric ≥ T` (others mirrored): `L` = the
library's worst non-allowlisted value over all gate renders, `R` = the red controls' best value. The
check separates when `R < L`, and `T` is calibrated when `R + 0.25 (L − R) ≤ T ≤ L − 0.25 (L − R)`
(the middle half of the gap). `calibration.json` records `L`, `R`, `T`, the margins and the item
holding `L`; a threshold change without a new file, or a file whose `T` differs from the code, fails.
A check that cannot separate (`R ≥ L`) fails the test until its metric or its red controls change: a
threshold is never moved onto the library's worst item.

#### 6.14.4 Red controls

`tools/mcp/tests/fixtures/title-library-red/`: one bad item and its fixed twin per lint rule (eight
layers, one per check id; the same eight are `lint_cases` in the dump, so our results are compared with `lint.ts`'s)
and per `check_title` check S80 defines and the two S84 adds (an olive gold, a flat metal; a clipped glyph, a missing glyph, a flooded counter, a
no-op effect, an unseeded random element...), plus library-level ones: a look with an unmapped
member, a denied term in a synonym, a stale thumbnail recipe, a look whose `part_effects` key its
style lacks, a template without placeholders over a text layer, a scheme with a 3-colour light ramp.
The gate asserts each bad item fails exactly its check and its twin passes.

#### 6.14.5 Goldens

`tools/mcp/tests/test_title_library_golden.py` (llvmpipe class in `tools/mcp/gpu_lane.json`, as the
MCP suite's other goldens): 36 cases in `tools/mcp/tests/golden/title-library/`: two looks per family
(one 2D, one 3D; 3D cases are added when S86 lands), four schemes on `casino/outline-gold`
(`gold`, `rainbow`, `zebra`, `neon-pink`), four templates (`casino/big-win-3d`, `casino/rush-title-fx`,
`fantasy/kingdom-title`, `minimal/clean-title`) on their `en` sample, at 0.25 × on transparent
backgrounds, compared premultiplied with Pachislop's three limits (max channel difference 48/255,
mean 0.25/255, at most 0.1 % of pixels over 8/255; a premultiplied mode added to
`tools/mcp/pngdiff.py`). Each case renders twice first (identical bytes). Updating a golden
(`JASPER_TITLE_UPDATE_GOLDEN=1`) needs a row in S80's title golden ledger (name, metric before and
after, reason) in `docs/titleart/goldens.md`, checked by S80's `tools/titleart/check_golden_ledger.py`.

### 6.15 MCP: `list_title_library` and the ops

S80 registers `list_title_library` in group `title_art` (Read, read-only, idempotent, allowed while
playing). Its schema as this spec needs it (`McpSchema` builders, closed objects):

```
kind        stringEnum (S80's program enum: effects|generators|blend_modes|fonts|looks|color_schemes|
            templates|environments|materials_3d|bevel_profiles|animation_presets)
id          string            one item: detail tier
query       string            words, en/ja/romaji
family      string
dimension   stringEnum 2d|3d
tags        array of string   all must match
scope       stringEnum builtin|project|user|all   default all
favorites   boolean
limit       integer 1..48     default 20 (S80)
offset      integer ≥ 0
sheet       object {text?, ids? (≤ 9), path? + layer? (render on a document's layer), cell? [w,h],
                    columns? 1..5, background? black|white|checker|machine, color_scheme?}
```

Description (≤ 400 characters): "Title Art library: Looks, Color Schemes, Templates, fonts,
environments. No arguments: the catalogue. kind + query/family/tags: rows with a ready `use` op. id:
one item. sheet: items rendered on your text or layer as one numbered image: pick by seeing. Apply
with edit_title ops apply_look, apply_color_scheme, apply_template." Listing size: S84's row of S80
§6.11's program budget table: ≤ 450 characters on `list_title_library` (which ends at ≤ 1,430 with
S80's 860 and S83's 120) and 0 on `edit_title` (S80's description already names the library op
family and points at `jasper://docs/title-art#ops`). No new tool. `available` is not a parameter: unavailable items are listed with
`available: false` and the reason.

| Call | Returns | Size |
|---|---|---|
| `{}` | `{looks: {total, available, families: {casino: 44, ...}}, color_schemes: {...}, templates: {...}, fonts: 32, environments: 8, next: "list_title_library {kind:\"looks\", query:\"gold\"}"}` | ≤ 600 chars (≈ 150 tokens) |
| `{kind:"looks", query:"chrome", dimension:"3d"}` | rows `{n, id, name, family, dim, tags (≤ 4), cost, available, use: {op:"apply_look", layer:"<layer>", look:"metal/chrome-3d"}}`, `total`, `next_offset` | ≤ 120 chars a row (≈ 30 tokens) |
| `{id:"metal/chrome-3d"}` | name en/ja, description, family, dimension, `ref_size`, structure summary (`face: chrome, bevel round 3 %; bands: 2; effects: outer_glow; generators: glints; environment: softbox`), font pair, keeps / replaces line, `requires`, allowlists, provenance, the thumbnail as an image only if asked (`sheet`) | ≤ 1,600 chars (≈ 400 tokens) |
| `{kind:"looks", query:"gold", sheet:{text:"大当り", cell:[320,160], columns:3}}` | rows as above numbered 1..n and one PNG (cells labelled with `n`), `render_ms` | 960 × 320 ≈ 410 image tokens; ≤ 1024 × 576 (≈ 786) |

Sheets render through `TitleLibraryApply::preview` on the S80 renderer: a sheet is a job past 1 s
(`McpJobs`); cells are cached by preview key (R33). With `path` + `layer`, each cell is that layer
with the look or scheme applied (the agent's real content); with `text`, a Text layer of size 200 in
the item's font pair. Errors: unknown `kind` lists the kinds, unknown `family` the families, an id that
does not resolve the R4 suggestions.

Ops (in `edit_title`'s `ops`, S80 §6.2):

| Op | Members |
|---|---|
| `apply_look` | `layer`, `look`, `apply_font` false, `keep_color_scheme` true |
| `apply_color_scheme` | `layer?` (absent = document), `color_scheme` |
| `apply_template` | `template`, `keep_text` true, `language` `en` \| `ja` (default `en`, or `ja` when the document's text is Japanese) |
| `save_look` | `layer`, `name` (string or `{en, ja}`), `id?`, `scope` `project` \| `user`, `family?`, `tags?`, `include_font` true, `overwrite` false |
| `save_color_scheme` | `layer?`, `name`, `id?`, `scope`, `overwrite` |
| `save_template` | `name`, `id?`, `scope`, `placeholders?`, `overwrite` |
| `copy_library_item` | `ref`, `to` `project` \| `user`, `id?`, `overwrite` |
| `delete_library_item` | `ref` |

Guide: the Library section of `docs/mcp/guide/title-art.md` (≤ 3 KB): the three axes with one
keep-and-replace line each, refs and scopes, "pick by seeing: use `sheet` on your own text",
font pairs, Save As ops, "passing checks is not approval".

### 6.16 API sketch

```cpp
// engine/core/titleart/ColorScheme.h  (JASPERENGINE_TITLE_ART)
namespace jasperengine::titleart {
    struct ColorPaint { Fill fill; std::optional<Gradient> ramp; Material3D material3d; Color shade;
                        std::optional<Pattern> pattern; };
    struct OrbTint { Gradient ramp; bool radial = true; };
    struct ColorScheme {
        std::string id; int revision = 1;
        ColorPaint face, dark, base; std::vector<ColorPaint> accents; std::optional<ColorPaint> bevelRim3d;
        Material3D side3d; Color faceGlow, glow, shadow, glint; Gradient glowGradient;
        std::vector<Color> lightRamp; OrbTint orb; std::optional<Pattern> pattern;
        std::optional<Color> patternGround; bool spectrum = false;
    };
    constexpr const char* TITLE_COLORS_FORMAT = "jasperengine.titlecolors";
    constexpr int TITLE_COLORS_VERSION = 1;
    bool parseColorScheme(const nlohmann::json& j, ColorScheme& out, std::vector<std::string>* warnings);
    nlohmann::json writeColorScheme(const ColorScheme& s);                  // canonical, sparse
    struct SchemeTarget { std::string layerId; };                           // empty = the document
    std::vector<std::string> applyColorScheme(TitleArtDoc& doc, const ColorScheme& s, const SchemeTarget& t);
}
// editor/titleart/library/TitleLibrary.h
namespace jasperengine::editor::titleart {
    enum class LibraryKind : uint8_t { Look, ColorScheme, Template };
    enum class LibraryScope : uint8_t { BuiltIn, Project, User };
    struct LibraryItemInfo {
        std::string ref, family; LibraryKind kind; LibraryScope scope; int revision = 1;
        LocalizedText name, description; std::vector<std::string> tags, derivedTags;
        std::string dimension; std::string cost; bool available = true; std::string unavailableReason;
        std::filesystem::path file;                                          // empty for built-ins
    };
    class TitleLibrary {                                                     // main thread
    public:
        static TitleLibrary& get();
        void setProject(const std::filesystem::path& projectPath);           // rescans project scope
        const std::vector<LibraryItemInfo>& items(LibraryKind kind);         // lazy index (R40)
        const Look* look(std::string_view ref, LibraryError* error);
        const titleart::ColorScheme* colorScheme(std::string_view ref, LibraryError* error);
        const TitleArtDoc* templateDoc(std::string_view ref, LibraryError* error);
        std::vector<LibraryHit> search(const LibraryQuery& query);
        static std::filesystem::path packDir();                              // FileUtils::getTitleArtLibraryDir()
    };
    namespace TitleLibraryApply {
        OpResult applyLook(TitleArtDoc& doc, const ApplyLookArgs& args, ImportPlan& imports);
        OpResult applyColorScheme(TitleArtDoc& doc, const ApplyColorSchemeArgs& args);
        OpResult applyTemplate(TitleArtDoc& doc, const ApplyTemplateArgs& args, ImportPlan& imports);
        Look captureLook(const TitleArtDoc& doc, std::string_view layer, const CaptureOptions& o, AssetCopies& files);
        titleart::ColorScheme extractColorScheme(const TitleArtDoc& doc, std::string_view layer);
        Look lookFromStyle(const StyleSource& style, double refSize, const CaptureOptions& o);   // S87's .asl import
        TitleArtDoc preview(const TitleArtDoc& doc, const nlohmann::json& op);   // no imports, no history
    }
    class TitleLibraryPreviews {
    public:
        const ImageHandle* request(const PreviewKey& key, Size size, int priority);  // null while pending
        void frame(double budgetMs = 4.0);       // called once per editor frame while a client is open
        void cancelAll(); void onDeviceLost();
    };
}
```

`PachislopPort` (`editor/titleart/library/PachislopPort.h`: `portDump(json, PortOptions) -> PortResult
{files, provenance, notes}`) is the converter's stage 2 and S87's importer core. `TitleArtOps` (S80)
dispatches the eight ops to `TitleLibraryApply` / the library store; an
`ImportPlan` becomes `CopyFileCmd`s and `SetAssetMetaCmd`s grouped with the document's
`WriteFilesCmd` into the op batch's one undo step.

### 6.17 Settings and commands

| Key (S25 registry) | Scope | Type | Default |
|---|---|---|---|
| `title_library.favorites` | User | list of refs | `[]` |
| `title_library.recents.looks`, `.color_schemes`, `.templates` | User | list of refs (≤ 8) | `[]` |
| `title_library.apply_font` | User | bool | false |
| `title_library.keep_color_scheme` | User | bool | true |
| `title_library.folded_sections` | User | list of `<kind>/<family>` | `[]` |

Commands (S25 CommandCatalog, placed by S82): `titleDesigner.saveAsLook`, `titleDesigner.saveAsColorScheme`,
`titleDesigner.saveAsTemplate`, `titleDesigner.revertToLook`, `assets.createTitleArtFromTemplate`. No default
shortcuts.

### 6.18 Player size (S34)

Only `ColorScheme.cpp` / `ColorSchemeApply.cpp` reach players, as part of the `JASPERENGINE_TITLE_ART`
module (a player has them exactly when it has the module, S81 R52). Estimated
≤ 30 KB Linux Release, ≤ 15 KB wasm gzip. Everything else (library, previews, lint, baker, pack): editor
only.

## 7. Acceptance criteria

| AC | Verification |
|---|---|
| AC1 (R1–R3, R5, R49) | gtest `editor/tests/titleart/TitleLibraryIOTest.cpp`: every built-in file parses with no warning and `write(parse(f)) == f`; sparse writing omits defaults and keeps unknown members and unknown types; `TitleLookMigrationsTest.DefaultChangeKeepsOldLooks` registers a test-only v1→v2 step that changes a default and fills the old one: a v1 look renders identically before and after; **red control**: the same test without the fill step fails; a file of a newer version loads read-only with `version_newer`. |
| AC2 (R4, R6) | `TitleLibraryTest.Refs`: built-in, `project/`, `user/` refs resolve; a typo returns three suggestions and the `list_title_library` call; an `aliases` entry resolves with `deprecated`. `TitleLibraryTest.Availability`: with a fake registry lacking `drips`, `horror/blood-drip` is `available: false` with reason "needs effect drips (S83)" and `apply_look` refuses it. |
| AC3 (R7, §6.4 look column) | `TitleLibraryApplyTest.LookKeepsContract`: a layer with every kept member of §6.4 set to non-default values; after `apply_look` each kept member is byte-identical (canonical JSON of the member) and each replaced member equals the look's; **red control**: a test hook that drops `masks` in apply makes the test fail naming `masks`. |
| AC4 (R7) | `TitleLibraryApplyTest`: `PxScaleByRefSize` (ref 200 on size 400 doubles every px param, handles land at `position + 2 × value`); `OwnedGeneratorsReplaced` (owned ones replaced, a user generator following the layer kept); `AutoGroup` (created, reused, dissolved with the transform folded); `DimensionSwitch` (3D look on a 2D layer sets `dimension: 3d`, back again; skipped with a reason until S86); `FreshLinks` (two applications of a linked look do not share a link); `KeepColorScheme` (scheme re-applied, cleared when off). |
| AC5 (R13, §6.5.4) | `TitleLibraryCaptureTest.RoundTripsEveryBuiltIn`: for every available built-in look, `capture(apply(look))` normalised equals the look (ids, positions, `ref_size`, `from_look` marks); a look with an image fill saved to `user` scope writes `<slug>.files/` and re-applies in a second project with the image imported. |
| AC6 (R8, §6.5.2) | gtest `engine/tests/core/titleart/ColorSchemeApplyTest.cpp`: role fixtures (a keyline at luma 0.1 → dark; an offset rim → base; three accent rings and two accents → the outermost takes the last); structure kept (rim count, widths, joins, offsets, bevels, textures, depths, non-colour params byte-identical); face span `layer` → `line`, `char` kept; alpha profile kept; relief-only pattern kept; white cores kept; `fire`, `smoke`, `ink_splatter`, `lens_flare`, `glints` untouched; spectrum on `rainbow`; orb tint; `Idempotent`: for every scheme × every available built-in look, applying twice equals once. |
| AC7 (R8, R19 parity) | `ColorSchemeApplyTest.MatchesPachislop`: for the dump's `colourway_cases` (21 looks × 11 colourways computed by Pachislop's `applyColourway`), our `applyColorScheme` on the ported look equals the mapped Pachislop result member by member, colours within 1/255. |
| AC8 (R9, §6.5.3) | `tools/mcp/tests/test_title_library_ops.py`: `apply_template` keeps `guid`, `name` and non-default `outputs`, carries an exposed `text` value with `keep_text`, sets `sample.ja` with `language: ja`, remaps ids (no dangling `follow` or mask), imports its fonts once; one undo step restores the document and removes the imported files; create-from-template writes one file in one step. |
| AC9 (R14–R17) | `tools/mcp/tests/test_title_library_user.py`: `save_look` project scope writes `EditorSettings/TitleLibrary/Looks/<slug>.titlelook`, undo removes it; `user` scope writes the config directory (the harness's isolated settings dir), `delete_library_item` moves it to `.trash/`; `copy_library_item` both ways; id clash refused without `overwrite`; `save_color_scheme` from `metal/classic-gold` re-applied to the same layer changes no pixel by more than 2/255 (`diff_images`, premultiplied); `save_template` without an exposed text on a text document fails with the fix op; deleting a built-in fails `library_item_builtin`. |
| AC10 (R18) | `test_title_library_user.py::test_favorites_recents`: Title Designer applies (through `run_command` and the UI test hooks) record recents, `edit_title` applies do not; favorites persist in settings across an editor restart; a favorite whose file was deleted lists as missing. |
| AC11 (R20, R21) | gtest `editor/tests/titleart/PachislopPortTest.cpp` (CI): `port-pachislop --check` on the committed dump reproduces every committed ported file and `provenance/pachislop.json`; **red controls**: one changed byte in a committed look → exit 1 naming it; a fixture dump with an unmapped member → exit 1 naming the member; a denied term in a mapped name → exit 1. |
| AC12 (R19) | Host, maintainer, recorded in Progress: `node dump.mjs --pachislop <clone at 1c32502> --scratch <dir>` twice gives byte-identical dumps equal to the committed one; `node dump.mjs --self-test` (also on the host, no clone needed) shows the refusal of a scratch directory inside the repository and that a touched `document` shim throws. |
| AC13 (R22) | `PachislopPortTest.ReviewRecords`: every ported id has a record, `tuned` ↔ an `overrides.json` entry, non-`match` verdicts have notes. |
| AC14 (R23–R27) | `test_title_library.py::test_content`: 195 looks with the per-family counts of §6.6, 33 schemes, 73 templates; ≥ 2 3D looks per family; ids follow §6.7.2 (`-3d` ↔ `dimension: 3d`); every Pachislop look, colourway and template id in `pachislop-map.json` maps to a committed item. |
| AC15 (R36, §6.14.2) | `test_title_library.py::test_gate` (CI, sharded): every available item on its sample texts passes as R36 states; the run prints counts per kind (checked, unavailable by reason, allowlisted ≤ 10 %); **red controls**: every fixture of §6.14.4 fails exactly its check and its twin passes. |
| AC16 (R35, R45) | gtest `editor/tests/titleart/StyleLintTest.cpp`: per rule, fires on the bad twin with the measured value, not on the good twin, and its fix op makes it pass; `LibraryChecksTest`: a `gold`-schemed face moved to olive (hue +40°) fails `color_scheme_match` and its fix passes, a metal face with finish and bevel off fails `metal_range`, their twins pass; `MatchesPachislop`: our findings on the dump's `lint_cases` equal `lint.ts`'s (rule ids mapped). `test_title_library_mcp.py`: `check_title` returns `style_lint.contrast` with `measured`, `threshold`, `rim#1` and a `fix` that `edit_title` applies. |
| AC17 (R37, §6.14.3) | `test_title_library_calibration.py`: for each check, `calibration.json`'s `L`, `R`, `T` are recomputed from the gate's run and the red controls; `T` sits in the middle half; **red controls**: a threshold changed in code without the file → fail; a `T` set to the library's worst value → fail. |
| AC18 (R38) | `title-library verify` and `PachislopPortTest.DeniedTerms`: an injected term in a name, synonym, tag, template sample and text layer each fail; a Latin denied term matches as a whole word and not inside a longer word, a CJK denied term matches as a substring (test terms read from `denied_terms.txt`). |
| AC19 (R28–R31) | `tools/ci/test_check_notices.py` new cases: a font directory without `OFL.txt` or without a NOTICE row fails; `--installed` on the Linux install tree finds `licenses/titleart-font-*-OFL.txt`; `fetch_fonts.py --verify` (offline) matches every font file's SHA-256 and size with `SOURCE.json` and every path is under `ofl/`; S84's four HDRIs are in S86's manifest with SHA-256 and pass S86's checks. `test_title_library_ops.py::test_font_import`: `apply_look` with `apply_font` imports `assets/TitleArt/Fonts/dela-gothic-one/` with `OFL.txt` and a `.meta` provenance once (a second apply imports nothing); with `JASPERENGINE_TITLEART_LIBRARY_DIR` pointing at an empty directory the op succeeds with `font_unavailable` and the layer's font unchanged. |
| AC20 (R32, R33) | `title-library bake-thumbnails --check` exits 0 on the committed tree; **red control**: a colour edited in a look's file → exit 1 naming that look. gtest `TitleLibraryPreviewsTest` (fake renderer): a position change keeps the key, a text change makes a new one; never more than 2 in flight; a frame's slice stays within the budget; LRU evicts the oldest past 240; a pending tile keeps its old image only for the same layer; device loss re-requests visible tiles. |
| AC21 (R40, R41) | `tools/perf` bench `title_library_bench` on a build server and lane machine H (before/after table in Progress): every R40 budget; the editor binary's growth ≤ 1.5 MiB; the staged thumbnails ≤ 16 MiB (the baker fails above `JASPER_TITLEART_THUMB_BUDGET`, red control with a 1 MiB budget); a project without `color_schemes` exports byte-identical with and without S84 (S34's size tools), one with it grows by the measured `ColorSchemeApply` size (≤ 30 KB). |
| AC22 (R43, §6.15) | `tools/mcp/tests/test_title_library_mcp.py`: the group is hidden until `tool_groups` enables `title_art`; catalogue ≤ 600 characters; rows ≤ 120 characters each and a row's `use` op, pasted into `edit_title`, applies; detail ≤ 1,600 characters; a 9-cell sheet returns one image ≤ 1024 × 576 with the JSON numbered alike; a sheet over 1 s returns a `job_id`; `test_tool_budget.py`, `test_tool_conventions.py`, `test_tool_coverage.py` pass. |
| AC23 (R12, R39) | `ColorSchemeApplyTest.EmbeddedSchemes`: `expose` of a `color_scheme` parameter with two options writes both into `color_schemes`; the engine function on the document equals the editor op's result byte for byte (canonical JSON); empty `options` gives `color_scheme_options_empty` in `check_title`. The player-side switch through Lua is S81's AC. |
| AC24 (R34) | gtest `TitleLibrarySearchTest`: `gold` finds `metal/classic-gold`; `きん` and `kin` find the `gold` scheme; `ゴールド` and `ごーるど` find the same items (kana folding); `ＧＯＬＤ` (full width) equals `gold`; "light glow" finds `metal/light-glow-gold` through derived tags; all words must match (`gold zebra` finds `casino/zebra-gold`, not `metal/classic-gold`); runs under the harness's network guard. |
| AC25 (R42) | `tools/mcp/tests/test_title_library_ui.py` (xvfb, isolated editor; runs once S82's panel exists): clicking a Looks tile applies with one undo step named `Apply Look Classic Gold`; Ctrl+Z restores; Revert to Look re-applies; Save As Look writes the file; a `→3D` tile converts with the toast; screenshots of the three tabs in the evidence. |
| AC26 (R10, R11) | `TitleLibraryApplyTest.Deterministic`: one document with two id seeds, the same look applied → identical canonical JSON after id normalisation and identical renders; `preview` leaves the history, the files and the project untouched (no import even for a pack font). |
| AC27 (R47) | `test_docs.py`: `title-library gen-docs --check` passes; `jasper://docs/title-library` serves per-family sections; `gen_tool_docs.py --check` and `tool_budget.py --check` pass. |
| AC28 (R36, §6.14.5) | `test_title_library_golden.py`: the 36 cases match their goldens; the summary prints how many differ at all and the largest difference; **red control**: one colour of `gold` moved by 6/255 in the face ramp makes its case fail with a three-panel sheet; a golden changed without a ledger row fails the ledger check. |

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

- **Depends on.** S80: the document codec and its JSON migration framework, `TitleArtOps` and its
  commit, `look {id, color_scheme}` (S84 adds `look.owner` and effect `from_look`, §6.4), part keys
  with positional aliases, `exposed` with `color_scheme`, `ParamUnit::Px` and gizmo hints, `CostClass`,
  the renderer and readback, `render_title` previews and jobs, `check_title`'s registry (S80 §6.12
  assigns S84 `style_lint`, `color_scheme_match`, `metal_range`), `list_title_library` (S80 §6.11:
  800 characters; S84 adds ≤ 450 for `family`, `dimension`, `tags`, `scope`, `favorites`, `sheet` and
  the kind `environments`, and fills `looks`, `color_schemes`, `templates`, pack rows of `fonts`), and
  the `create.template` / `create.look` path of `edit_title`. S83: the catalogue and presets
  (§6.6-6.8 of S83), `ParamDef::light`, `SpectrumSwitch`, `PachislopFxMap` (S83 §6.9). S86:
  `style_3d`, bands, materials (R17), environments (R24, R66) and `fetch_hdris.py`,
  `TitleDocument3D::fromPachislop` (R67). S82: the Library panel and dialogs, `foldForSearch` (R27),
  `TitlePreviewQueue` (R67), the CJK UI font. S81: the module condition for `color_schemes`, Lua
  `setColorScheme`, export of imported fonts and environments. S04 (asset database and `.meta`), S05
  (folder watching), S14 (HDR import type), S24, S25 (settings, commands), S31 (MCP rules), S34, S36
  (`provenance` section, or S84 adds it with S36's schema).
- **Blocks.** S82's Library panel content; S87's Pachislop importer (`PachislopPort`,
  `pachislop-map.json`, `TitleLibraryApply::lookFromStyle`) and template variants (it extends
  `placeholders`); S85's `animation_presets` rows in `list_title_library` (S80's enum carries the
  kind).
- **Aligned with the specs as written on 2026-11-04** (their names win where they move): S83's
  effect ids (`light_glow`, `aura`, `drips`, `scanlines`, `pixelate`; S80's `outline` for Pachislop's
  keyline) and preset ids; S86's environment ids, its HDRI folder and fetcher (S84 adds four entries
  instead of a second pack) and its material preset names; S82's featured-template slot, search fold
  and preview queue (now S80's `TitlePreviewQueue`); S81's `colorSchemes` "any scheme" when
  `options` is empty (R12). **Settled with the owners:** `ColorSchemeApply` is part of the module
  (S81 R52); S81 exports the fonts S84 imported under `assets/TitleArt/Fonts/` only as its
  subsetting decides; S86 R66 accepts the four extra HDRIs within its manifest and size rule; S80
  §6.1 lists `look.owner`, `from_look` and `color_schemes`; the title golden ledger is S80's.
- **Merge hotspots.** `editor/mcp/tools/TitleArtTools*.cpp` (S80, S83, S85: `list_title_library`
  kinds), `editor/titleart/TitleArtOps.cpp` (op dispatch), `tools/ci/check-notices.py` and
  `NOTICE.md`, root `CMakeLists.txt` (embedding, install staging), `editor/AppSettings`/settings
  registry, `docs/mcp/guide/title-art.md` (S80 owns the file; S84 writes the Library section).

## 9. Risks & defaults

- **Pachislop's looks render differently on our renderer.** Default: the review records each verdict;
  `tuned` patches live in `overrides.json`; the goal is the same look, not the same pixels.
- **Repository growth (≤ 92 MB binary content).** Default: one squashed commit as S36 did; no LFS
  (the repository uses none); thumbnails are the only part that can be regenerated and they have a
  hard budget.
- **Install growth (≤ 92 MB with fonts, S84's HDRIs, thumbnails).** Default: accepted for offline
  Japanese-first titles; a later "minimal install" could drop the extended Japanese faces and fall back
  to imports from a user-chosen folder, not in this spec; S84's four HDRIs drop to 1k if they pass
  30 MB (S86's rule).
- **S83 or S86 lag.** Default: unavailable items are listed with reasons, excluded from the gate with a
  count, and become gated the day their requirements register (the gate then fails until they pass).
- **Default changes in S80/S83/S86 shift sparse looks.** Default: R5's rule (a version bump with a
  fill step) and AC1's red control; the 36 library goldens (§6.14.5) catch what slips through.
- **Trademark misses.** Default: the denied-terms list is reviewed with every content change and only
  grows; names are descriptive (material, colour, structure), never a product's.
- **Japanese names written by hand.** Default: every built-in has both names; a native-speaker review
  pass is recorded in `review.json` notes for new items.
- **ThorVG/HarfBuzz render a pack font differently from browsers.** Not a risk for the port review
  (Pachislop references are aids), but thumbnails use our renderer only.
- **A user edits an imported pack font in place.** It is a project file; its `.meta` provenance keeps
  the source and S81's credits still list it.

## Milestones

- **M1** Formats, codecs, migrations, refs, the store with embedded data and scopes (R1–R6, R16,
  R49); AC1, AC2.
- **M2** `ColorSchemeApply` and `apply_color_scheme`, the ported schemes (R8, R12, R24 schemes); AC6,
  AC7, AC23.
- **M3** `apply_look`, capture, `apply_template`, the library ops, favorites and recents (R7, R9–R11,
  R13–R15, R17, R18, R44); AC3–AC5, AC8–AC10, AC26.
- **M4** The converter, the port of 104 looks and 37 templates, review records (R19–R22, R24); AC11–AC13.
- **M5** Font pack, S84's four HDRIs in S86's set, imports, notices (R28–R31); AC19.
- **M6** Lint, gate, calibration, denied terms, goldens (R35–R38, R45); AC15–AC18, AC28.
- **M7** New content: 91 looks, 22 schemes, 36 templates (R23, R25–R27); AC14 (items needing S83/S86
  become available as those land).
- **M8** Baker, thumbnails, live previews, search, MCP tiers, docs, CLI (R32–R34, R43, R46, R47); AC20,
  AC22, AC24, AC27; then budgets and sizes AC21; the UI AC25 with S82.

## Deviations

1. **Reconciled: `ColorSchemeApply` has no separate link condition**; it is part of the
   `titleart` module, which only Realtime and Glyph Atlas titles pull in (S81 R52). A per-member
   condition would need a third sub-option for about 30 KB.
2. **Reconciled: `list_title_library` kinds are S80's program enum**; `environments` rows are S86's
   (they include S84's four HDRIs), `animation_presets` S85's (was "reveals").
3. **Reconciled: no op names in `edit_title`'s description** (was ≤ 160 characters); S80's
   description names the op families, so `edit_title` stays under 1,500.
4. **Reconciled: item `cost` uses S80's `CostClass` C0-C4** (was `CostTier`), at least C3 for 3D looks.
5. **Reconciled: Lua reads `colorSchemes` (S81's property)**, not `getColorSchemes()`; command ids use
   the `titleDesigner.` prefix of S82's window.
6. **Reconciled: names.** `metal/deep-glow-gold` is `metal/light-glow-gold` and the derived term is
   "power glow" (no product names); source ids and old texts that carry denied terms appear only in
   §6.7.3's rename table; sample texts `ASHEN REALM` and `伝説` replace texts tied to published works.
7. **Reconciled: 3D look members use S86's names** (`metallic`, `emission_intensity`, `surface`,
   no 3D shadow catcher); 2D members use S80's (`gloss` ring, gloss `opacity`); Backlight intensity
   is S83's percent.

8. **Implementation: namespace `jasperengine::editor::titlelibrary`** (§6.16 said `editor::titleart`),
   as S82 Deviation 14: `editor::titleart` would shadow the engine's `titleart::` in every editor file.
   `LibraryItemInfo::requires` is `requirements` (`requires` is a C++20 keyword).
9. **Implementation: what the ops carry besides the document.** S80's `TitleOpsResult` gains `files`
   (`TitleOpFile`: path, content, undoable, remove) and `details` (op index -> the op's `changes`
   object of §6.5.1 step 13). `TitleArtDocument::edit` writes the undoable files in the document's
   own `WriteFilesCmd` (one undo step: the fonts, `OFL.txt`, `.meta` and environments an apply
   imports, a project Save As), the personal ones after it; `TitleArtDocument::create` takes the same
   files (create-from-template). A batch of `copy_library_item` / `delete_library_item` only runs
   without a document (`runLibraryBatch`: a `WriteFilesCmd` or a `RemoveFilesCmd` named by the op);
   a project delete inside a document batch is refused (it is its own step). `ImportPlan` writes the
   `.meta` itself (S36's `provenance` section, opaque to S04, already parsed by `AssetMeta`), so no
   `CopyFileCmd` / `SetAssetMetaCmd` pair is needed.
10. **Implementation: S80 hooks added.** `TitleArtOps::registerBatchFinisher` (keeps `color_schemes`
    filled for exposed `color_scheme` parameters after every batch, AC23), `TitleArtDocument::setProjectHook`
    (the library follows the project every document opens in), a batch of identical `Apply ...` steps
    is named `Apply Look X (n layers)` (R7). `edit_title`'s `path` is no longer `required` in the
    schema (library-only batches, R44); a missing path still fails for document ops.
11. **Implementation: the embedded library is a zstd tar.** `cmake -E tar --zstd --format=gnutar` packs
    `looks/`, `color-schemes/`, `templates/` and `manifest.json` at configure time into
    `titleart_library.h`; `TitleLibrary` decompresses it (zstd streaming) on the first library call
    and reads the tar itself. `pachislop-map.json` and the provenance file stay files of the source
    tree (tools read them; the editor needs neither to apply items).
12. **Implementation: scopes rescan by listing**, not on S05 file-watcher events: every library call
    compares each scope folder's listing (names, sizes, times) with the last scan (one directory read,
    well under a millisecond for a user library), which also covers folders the watcher does not
    watch (the personal library outside the project).
13. **Implementation: Library tiles.** S82's window renders the live tiles (the item's ops applied to
    the user's layer, `TitleLayerThumbnails` through `TitlePreviewQueue` at priority Tiles, keyed by
    the layer's content hash, the previous picture kept per tile, cancelled when the window closes,
    device loss handled by the queue): the registered provider is handed that renderer
    (`TitleLibraryProvider::attachTileSource`, a small S82 hook) instead of duplicating it in a
    `TitleLibraryPreviews` class. S84 adds the baked first paint (the staged thumbnail of the script
    of the layer's text: the window's target key now carries the text) and the LRU of baked
    textures (240). There is no separate S84 disk cache of live tiles: the preview queue's renders
    are fast enough on a GPU and the baked tiles cover first paint; AC20's preview-service test is
    therefore the baked-sample selection test plus S82's own thumbnail tests.
14. **Implementation: GPU subcommands drive a headless editor.** `bake-thumbnails`, `review-sheet` and
    `calibrate` write a scratch project (the documents of every item and sample, the pack's fonts and
    HDRIs at the paths applies name) and run `jasperengine-editor batch --project <scratch>` with
    `render_title {save_path}` / `check_title` calls, so the CLI renders and checks exactly as MCP does;
    the PNGs are framed (alpha bounds plus a margin, fitted to 240 x 135) and stamped with the recipe
    `tEXt` chunk in the CLI. `build-templates` (no GPU) builds the 36 new templates from recipes of
    edit_title ops (`tools/titleart/templates/`), so they are reproducible and reviewed as data.
15. **Content: thin hairline looks allowlisted.** `retro/vector-green`, `minimal/outline-thin` and
    `minimal/frosted-glass` carry `lint_allow: rims_thin` with reasons (a hairline is the look).
16. **Finding: eleven pack fonts declare a Reserved Font Name** (§6.11 said none): Audiowide, Lilita One,
    Orbitron, Russo One, Creepster, Cinzel Decorative, Monoton, MedievalSharp, Press Start 2P, Titan One
    and Nosifer (read from each upstream `OFL.txt`; `NOTICE.md` names them). The pack ships the fonts
    byte-identical, so it is unaffected; S81's export subsetting makes a Modified Version under the OFL,
    which may not carry a reserved name: S81 needs a decision (rename the subset's name table, or do
    not subset these eleven). Not changed here.
17. **Binary content lands in two commits** (R50 said one): the fonts and HDRIs first, the thumbnails
    with the finished library, because the thumbnails need every item and the commits must be safe
    against interruptions. Squash on landing if the one-commit rule still matters.
18. **Implementation: rows of `list_title_library` are up to about 300 characters** (§6.15 said 120): the
    row's own members (`id`, `name`, `family`, `dim`, four `tags`, `cost`, `available`, `use` repeating the
    id) cannot fit 120. AC22's test holds them to 300.
19. **Implementation: sparse reading follows normalize.** A missing member of a present object takes the
    kind's default, so a sparse object is written against the kind's defaults; only an absent object takes a
    member-level default (`style_3d.face`, a band's bevel). The first writer diffed against the
    member default and normalize then changed ten ported 3D looks.
20. **Implementation: `apply_color_scheme` stays idempotent with material links.** When rings a link made
    equal cannot be explained by one count of accents, each keeps the kind of paint that produced it and
    the link sync settles the colours (a look with linked bands on a scheme whose accents are dark).
21. **AC21 measured on build box 2 (RelWithDebInfo, 16 shared cores, llvmpipe); not on lane machine H, which was
    unreachable:** index 23.8 ms (60; built-ins now parse across the cores), `apply_look` on 16 rims 5.0 ms (**over 3**),
    `apply_color_scheme` 1.5 ms per layer and 12.7 ms for 64 layers (2, 25), `list_title_library` catalogue 24.1 ms and
    rows 22.7 ms including an MCP round trip of 25.9 ms (30), the baker 1353 s (**over 20 min** on this box), the embedded
    archive 267 KB (1.5 MiB), thumbnails 12.4 MB (16 MiB). Not measured: tile GPU times on H, the player switch, the export
    size delta. AC25 runs as `test_title_library_ui.py` (tile click with one undo step, Ctrl+Z, a 3D tile converting,
    Revert to Look, Save As Look, the three tabs). AC20's `TitleLibraryPreviewsTest` is replaced as Deviation 13 says.
22. **Spec conflict: Minimal holds one 3D look.** R23 and AC14 ask for two 3D looks per family, §6.8's catalogue
    gives Minimal one (`minimal/matte-3d`) and §6.6 fixes its count at 10. The catalogue stands; `test_content`
    holds Minimal to one. Adding a second needs a catalogue change (and 195 becomes 196, or a 2D look goes).
23. **Owner's rule: every item passes every check, with no exception lists; the gate refuses `check_allow` and
    `lint_allow`.** **Green**: the whole gate on the merged tree (2c86409bc, 2026-11-10; 6 shards over both build
    boxes) passes: 301 items, 830 sample renders (looks 585, schemes 99, templates 146), 0 failures. Before that
    (ta/lib-last-looks, 2026-11-10): the whole gate before the merge of feature/title-art
    (863cb9c78), 6 shards over both build boxes: 301 items, 829 sample renders (looks 584, schemes 99, templates 146),
    5 failures in 4 items (counter_survival at 8 delta E; the feature line's threshold is 4, Deviation 29): `3d_tier_budget` on `neon/tube-3d` and `scifi/reactor-core-3d` (Medium) and
    `gems/treasure-banner` en and ja (Low), and `counter_survival` on the scheme `mono-dark`; the briefed
    `casino/premium-black-gloss`, `casino/red-hot-rim-glow` and `gems/diamond-3d` passed. Those four are fixed on the
    feature line (S86 Deviation 35's mesher, Deviation 29's mono-dark ramp); after the merge the gate over the twelve
    items it touches (those, the three briefed, `minimal/soft-shadow`, `elements/electric-blue`, `elements/storm-counter`,
    `casino/confirm-giraffe-3d`, `sports/goal-banner`; 30 renders) has no failure. Earlier states:
    - fallback_parity: none since Deviation 32 (`casino/confirm-giraffe-3d`'s 3D glint, judged by S86 AC9). `elements/electric-blue` and
      `elements/storm-counter` pass since S83 Deviation 43 (Light Glow read Lightning's light clipped at 1 on the RGBA8
      path; 11 % and 52 % of pixels over 8/255, now 0.4 % and 1.9 %);
    - 3d_tier_budget: the inflate mesher's floor at Medium and the extruder's at Low, **fixed in the mesher** (S86
      Deviation 35: it coarsens to each tier's budget, and the solid limit drops the thinnest band);
    - counter_survival: `casino/red-hot-rim-glow` ja "passed alone and failed late in a batch" was a renderer bug, not
      carried state between documents: a group's cached node was keyed by every child of the document, so a check's
      isolated render of the echo layer after a render of the canvas (`render_title`, or the checks' own base render)
      read the whole group; checked alone (`checks: ["counter_survival"]`) it rendered fresh. The group's key now holds
      the children the draw tree draws (e125488d3 on the feature line, the same fix as 21f9545b5 here;
      `TitleArtRendererGpu.IsolationKeysTheGroupNode`, red before). 69d584ba6 was the same class (a node-cache key
      without what the pixels depend on: a generator's params), not this cause. `casino/premium-black-gloss` and
      `gems/diamond-3d` are tuned on the feature line (Deviation 29); this branch's alternatives (tracking 0 with a
      graphite rim; a silver Outline) also passed and were dropped in the merge for the versions closer to the design.
    What was fixed, by cause: the tier budget measured islands instead of the stack, and the mesher now builds at most the
    tier's stack (bands past it are reported per tier as `3d_tier_budget.<tier>.bands_dropped`, a warning); fallback parity
    uses each document's class and S86 AC9 for 3D, and skips the max for grid-read layouts (Lightning and Backlight, as
    S83's own test does, and following Glints, measured in S83 Deviation 44); counter_survival is visible contrast (below); legibility is judged on
    the item's declared background (46 looks, 11 templates). About 45 items were tuned as data, each with a targeted change, and reviewed on
    before-and-after sheets of every family in both samples: no look lost its identity; the colour scheme mono-dark did, and
    is restored (it failed counter_survival again until Deviation 29's ramp); two template sample texts were shortened to fit ("NEW IN", "BREAK").
24. **counter_survival is visible contrast** (owner's decision): a counter survives when it is open or its fill is at least
    delta E 8 from the letter around it, on the item's background; a rim of the face's colour and a counter flooded by an
    outline of it are the red controls (6.5 at most). Latin keeps its design; a look's `cjk` member thins rims and bands
    only on Japanese text where kanji still needs it. Other metric changes: legibility reads the silhouette's bands;
    `check_title` gained `render_scale` (the gate's 0.25); `3d_environment_missing` no longer warns for an HDRI it has.
25. **Thresholds from §6.14.3 on the final numbers:** `metal_range` 30 -> 9 L* (red 1.45, library 12.7), `legibility_contrast`
    30 -> 22 (red 4.7, library 30.3), `counter_survival` 8 delta E (red 6.5; the library still holds 2.7 until the items above
    pass), `style_lint.contrast` stays 12 for Pachislop parity. The style lint still warns on 13 looks (hairline rims, two
    contrasts, one hard shadow): warnings, not gate failures, listed for the owner.
26. **Thumbnails are posterized** (RGB 3 bits, alpha 2 bits, adaptive PNG filters) to fit 830 files in 12.5 MB of
    the 16 MiB budget (R32); the 3D template `casino/big-win-3d` renders at 0.33 of its canvas for this reason
    (0.67 ran out of target memory).
27. **Reserved Font Names, resolved with S81's subsetter (Deviation 16):** detection read only name ID 0, so `Audiowide` and
    `Monoton` (declared in `OFL.txt` only) and `MedievalSharp` (declared in ID 13) would have shipped subsets under a reserved
    name. Detection now reads IDs 0 and 13 and a small table of the pack's fonts whose names do not declare it;
    `TitleFontSubset.ReservedNamesOfThePackFonts` holds all three plus an ID-0 font.
28. **Renderer bugs found by the gate.** After `render_title`, `fallback_parity` failed on `sports/goal-banner`: not the RGBA8
    path (it was the fresh render, Speed Lines drawn) but the float render, which met the node cache and drew the look's Fire
    twice and no Speed Lines. Generator params sat in the Node hash scope (an object member inherits the walk's scope), which
    the node cache's content key leaves out, so two generators following one layer from one place shared an entry (and a
    param edit hit the old pixels). The `generator` member is now Geometry, like its kind (`TitleArtHashTest.AGeneratorsParamsAreItsContent`
    and `test_fallback_parity_after_a_render`, both red before); the gate renders before it checks again. In the Title Designer, Revert to Look from the layer menu crashed (layer rows pointed
    into the edited document); fixed, and Save As's buttons were below the Library strip's edge; moved beside its title.
29. **Follow-up of 23 (ta/lib-looks):**
    - The "render state carried between documents" was the renderer's node cache: a group drawn for an isolated layer was
      keyed by the group's content hash, which chains every child, so an isolated render after the full one got the whole
      group back (and a full render after an isolated one got the one layer). `casino/red-hot-rim-glow` ja's isolated echo
      layer held the main layer drawn over its counters. Group and clip nodes now key by what they draw
      (`TitlePlanner::drawnHash`; red control `TitleArtRendererGpu.IsolationKeysTheGroupNode`). It passes in every order.
    - `mono-dark`: the face takes a dark ramp (#000 -> #111 -> #2e2e2e): the black face's bevel no longer lights the letter
      edges as bright as the white line in the counters; base and rims stay black, so the look is unchanged.
    - `gems/diamond-3d`: the outline effect 2.5 -> 1.5 (the schema default); at 2.5 it closed the gaps the digits' bands
      enclose on `1234`. `gems/crystal-title`, built from it, follows.
    - `casino/premium-black-gloss`: en and ja pulled opposite ways (en's P and R counters darker than the bevel-lit
      letter, ja's handakuten counter, 6 x 6 px at the gate's 0.25 render and filled by the pinstripes at any rim width,
      lighter). English: at tracking -40 Zen Old Mincho's M and I serifs touched and enclosed a gap the rims filled; the
      sample's I, U and M sit 4 % further right (a lighter face edge, bevel highlight 1.0, made that gap worse). Japanese:
      templates now carry the per-script `cjk` member (§6.3, the look's: `rim_scale`, `band_scale`, and new
      `highlight_scale` on the face bevel's highlight), applied to the layers that hold Japanese text;
      `highlight_scale: 0.5`. Both samples measure 7.3.
    - Sample texts: `casino/new-machine-flyer` is NEW ARRIVAL again, on two lines (NEW / ARRIVAL fit the template
      unchanged). `metal/liquid-gold-vertical-kicker` keeps BREAK: LIMIT BREAK in one vertical column is taller than the
      2880 canvas, and in two columns the Latin reads across as BL / RI / EM / AI / KT.
    - The style lint warns on 17 looks and the scheme `royal` (and on five templates through the looks they apply), all
      by design and left: hairline rims are the look of `minimal/outline-thin`, `minimal/frosted-glass`,
      `retro/vector-green`, `metal/liquid-chrome`, `neon/hollow-neon`, `neon/neon-tube`, `wa/dry-brush-ink` (0.2-2.8 %);
      `gems/blue-glass`, `gems/red-glass`, `casino/rainbow-glass-gold`, `metal/gradient-map-gold` sit at 4.6-4.9 % of
      a 5 % threshold (Pachislop's widths); the contrasts are gold-on-gold strokes separated by their bevels
      (`metal/double-stroke-gold`, `casino/rainbow-glass-gold`) and 3D bands the lighting separates
      (`casino/gold-on-red-stack-3d`, `casino/rainbow-glass-gold-3d`, `elements/ice-crystal-glass-3d`,
      `metal/holographic-silver-3d`); `retro/16bit-gradient`'s hard shadow is the pixel-art style.
    - A font "sometimes missing" in a full gate run (`cartoon/sticker-white`: missing font LilitaOne) was the asset
      database learning a library apply's imported files only on the file watcher's later walk, which raced the check
      that followed the apply. A title step that writes files besides `.titleart` documents now refreshes the database
      before it returns (red control `TitleArtDocumentTest.FilesAStepImportsAreInTheAssetDatabaseWhenItReturns`).
    - `TitleLibraryBuiltins.Budgets` failed its apply_look budget (9-17 ms against 9) depending on what ran before it
      in the process: every apply_look read and SHA-256ed the look's 2.5 MB pack font up to three times, even with the
      font already in the project, so the op cost the font's size and its timing followed the process's memory state
      (measured 14.8 ms fresh, 4.1 ms after a project load in the same process; the low-level cause is inferred). Pack
      hashes are cached by path, size and time and the bytes are read only to copy them: 1.4 ms in either order.
    - counter_survival's threshold, recomputed by §6.14.3 on the 830 gate renders and the 30 red controls: R = 0 (the
      same-colour rim and the flooded counter both measure delta E 0 on their failing counters; the 6.5 the file held
      was the flooded fixture before its rim followed the lint), L = 7.3 (`casino/premium-black-gloss`, then 8.1
      `gems/crystal-title`). The middle half is 1.8-5.5: 8 was outside it, on the library's side. T = 4 (mid-gap 3.65,
      rounded); the red controls fail by 4. Deviation 24's "delta E 8" reads 4 from here.
30. **AC21's `apply_look`: the commit invariants' link sync.** Profiled with scoped timers (build box 2): of 5.3 ms on 16
    rims, 3.1 were the import plan reading and hashing the look's pack files on every apply (fixed on ta/lib-looks,
    007742f38: hashes cached, bytes read only to copy), and S80's link sync copied and dumped the previous document's
    materials on every batch; it now does so only when a link has two members (1.13 -> 0.24 ms of the invariants).
31. **AC21's baker within 20 min: 1364 s -> 1234 s cold, 1040 s warm** (build box 2: 16 shared cores, the image's 8-CPU
    quota, llvmpipe; load average 1-9 from other worktrees; 830 thumbnails byte-identical to the 1364 s run). Profiled:
    `time` gave 136 CPU-min over 22.7 min (llvmpipe already fills 6 of the 8 CPUs), `perf` 76 % of the samples in
    llvmpipe's shaders and 14 % in LLVM compiling them on the editors' main threads, the engine under 2 %. So more editors
    at once did not help (4 editors: 1357 s, 175 CPU-min: the quota is the limit; not kept). What was cut: (1) every
    `render_title` of a current revision also rendered a 768 px copy kept for `compare: "rev:<n>"`; the baker passes
    `options: {keep: false}` (one render; `test_render_title_keep_false`, red with the option ignored): 1239 s; (2) Mesa
    caches llvmpipe's compiled shaders under the passwd entry's home, not $HOME, so in the build image (HOME under
    build/) each bake compiled every shader again; the headless editors get `MESA_SHADER_CACHE_DIR=$HOME/.cache/
    mesa_shader_cache` unless MESA_SHADER_CACHE_DIR or XDG_CACHE_HOME is set (Mesa's own default where HOME is the
    user's; 20 MB): a cold run 1234 s, every later one 1040 s (17.3 min). A run on a clean machine stays over 20 min;
    a 101-render subset measured 192 s cold, 121 s warm. llvmpipe thread counts (4, 7, 8, Mesa's 16) were measured on
    that subset and changed nothing warm (4: slower), so none is set.

32. **Resolved: the 3D glint of `casino/confirm-giraffe-3d` (en) on the RGBA8 path** (S86's compressed light). At the gate's
    0.25 it measured 2.8 % of pixels over 24/255 (the check's limit 1 %), mean 2.0; with the glint off 0.35 %; `ja` passes, as
    every other 3D item does. The MSAA resolve averaged the encoding `C/(1 + C)`, not the light; S86 Deviation 36 makes the
    supersampled fallback hold linear light (resolves average light, `TitleToneMapTest.SupersampledFallbackResolvesToTheMeanLight`):
    giraffe en is now 2.0 %, mean 1.6, and the other 251 samples of the 91 3D items pass. What remains is the 8-bit precision
    of the glint's input (the same encoding in a float target: 0.0 %), which no 8-bit encoding of light up to 60 removes.
    The integrator's decision: `fallback_parity` now checks a document with 3D glints on twice, as S86 AC9 reads: the same
    document with its glints off to the 3D share and max (giraffe en 0.0 % over 24/255, max 24), the glint-on render to AC9's
    mean alone (3/255; giraffe en 1.6), so the stars are judged by the spec's own criterion and everything else per pixel
    (S86 Deviation 37). Red controls (`FallbackParityCheckTest`): a glint document broken outside its glints fails, and so
    does a glint whose difference passes the mean; without glints the share stays.
33. **Implementation: the pixel checks' renders live with their call.** `rendersOf` kept one static set of renders keyed by
    the context's address and the document's hash, so a later `check_title` call on the same document (its context in the
    same stack slot) with another `render_scale` read the earlier call's renders. They now live in the context
    (`TitleCheckContext::renders`), one set per call (`FallbackParityCheckTest.EachCallRendersAtItsOwnOptions`, red before).
