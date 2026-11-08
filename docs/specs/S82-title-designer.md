# S82 — Title Designer: the window where people author Title Art

**Wave / Depends on.** Title Art program (spec-first), **wave 6B** (with S81, S83, S86; program
graph in §8, program rules in S80 §6.15), after S80. Depends on:
- **S80**: the `.titleart` document v1 (§6.1), `TitleArtOps` and `TitleArtDocument::commit` (§6.2),
  the renderer, the member schema and effect registry, `check_title`'s checks, bake, `get_title`;
- **S81** (same wave, S81 owns them): the Title Art Renderer's Glyph Atlas mode (Preview As), its
  create menus and drops (S81 R6), the instance rectangle for In Context, the Characters UI it
  hosts here (S81 R62);
- **S84** (wave 6C): the Looks, Color Schemes and Templates data, `TitleLibraryApply::preview`, live
  tile previews and their cache (`TitleLibraryPreviews`), families, favorites and recents, the
  bundled OFL font pack and the baked first-paint thumbnails. The Library panel (M6) is the one
  part of S82 that lands with S84 in wave 6C; until then the window shows no Library region and the
  empty state offers New Text instead of template cards;
- **S83** (effect and generator catalogue, Add menus, gizmo hints), **S85** (Animation strip, Record),
  **S86** (3D layers, the 3D gizmo target) and **S87** (languages) plug into the window through the
  registries and the hooks defined here (§6.8); none blocks it;
- **Doriax catch-up `unicode-text`** (`docs/superpowers/specs/2026-11-04-unicode-text-design.md`,
  ledger row 1): CJK editor fonts (`editor/util/SystemFonts`), 32-bit ImGui characters. S82 does
  not load editor fonts itself (§8 states the fallback if it has not landed).

Owner area:
- `editor/window/TitleDesigner*.{h,cpp}` (the window and its panels);
- `editor/titleart/designer/` (state, staging, preview, search index, canvas view, gizmos, text
  editing: the non-UI logic, unit-tested);
- `editor/titleart/fonts/` (font catalogue, system font lists, web font decoding, preview strips);
- `editor/widget/DocRow.*`, `editor/widget/RampField.*`, `editor/widget/ThumbnailGrid.*`;
- `editor/util/ImePreedit.*` and the IME parts of the three editor backends;
- `libs/brotli/` and `libs/woff2/` (decoders, editor only);
- `editor/resources/titleart/search-terms.json`;
- `docs/editor/title-designer.md` and the "Title Designer" section of
  `docs/mcp/guide/title-art.md` (`jasper://docs/title-art`, S80's file).

## 1. Goal

A person opens a `.titleart` and makes a title the way they would in the best design tool they
know, inside a Unity-like editor: type the words on the canvas (Japanese included), browse Looks
rendered on their own words, preview anything by pointing at it, change any setting by searching
for it in English or Japanese, drag a handle, and press Ctrl+Z for any of it. Nothing blocks a
frame, nothing is ever unsaved, and a closed window costs nothing.

Everything the window does is a call of S80's document operations, so an agent can do, check and
show the same things through MCP, and a person and an agent can work on one title in turn.

## 2. Scope

**In scope**
- The **Title Designer** window (Window > Title Designer): layout, docking, toolbar, empty state
  with template cards, status bar with the performance readout.
- **Layers** panel: tree, eye/lock/guide, thumbnails, blend mode, opacity, drag reorder, groups,
  clipping, parts and effects under layers, masks, multi-select with mixed values.
- **Properties** panel: table-driven document rows (`DocRow`) with reset arrows, source dots with
  edit-to-override, multi-target editing (set-all and shift-each), and **live property search**
  whose results are the live controls and whose index derives from the drawn views, with
  English/Japanese synonyms.
- **Canvas**: a private scene drawn through `Engine::drawView` (the `VatPreviewView` pattern),
  pan/zoom/fit, backgrounds (including a snapshot of a game scene), device frames and safe areas,
  rulers, guides, snapping, the 2D gizmo (move, rotate, scale, rect, pivot), per-effect handles
  (light angle, gradient line, glow centre, ...), and the fixed screen-space 3D gizmo that S86's
  layers use.
- **Text editing on the canvas** with IME (Japanese) on Windows, macOS and Linux, including an
  inline preedit where the platform gives one.
- **Library** panel UI over S84's data: tiles rendering the user's own text, hover/arrow preview
  through the same apply path as a transient state (Esc restores, Enter commits), filters,
  search, style families, favorites, recents, tile sizes, and the shared per-frame preview budget
  (S84 owns the tiles' renders and cache).
- **History** panel (the document's named steps), **style lint** badge with cited rules and
  one-click fixes, the unified **Add** palette (layers, generators, effects) with preview.
- **Font picker**: previews of the user's text, missing-glyph warnings, `.ttc` faces, variable
  axes sliders, the Google Fonts importer (reusing `editor/htmlui/GoogleFontsImporter`), system
  font lists per OS (fontconfig, CoreText, DirectWrite) that copy the file into the project,
  `.woff`/`.woff2` import (Brotli and the WOFF2 decoder vendored, editor only).
- Scene integration: double-click opens the window; dragging the shown document from the window
  into a scene (S81 R6 owns the drops from the Project window and what they create).
- Keyboard shortcuts in a new **Title Designer** shortcut context, consistent with Unity.
- Agent parity: the action-to-op table (§6.3), `TitleDesignerWindowTestAccess`, `focus_window` and
  `screenshot` support for the window, the `designer` block of `get_title`.

**Out of scope**
- The document model, ops grammar, renderer, effects, bake and the MCP tools themselves (S80,
  S83); the component, Lua and export (S81); library content, its converter, families and check
  calibration (S84); animation time axis and its timeline UI (S85: it adds its own panel to this
  window through §6.8's hooks); 3D layer data, materials and lighting rows (S86: they use
  `DocRow` and the 3D gizmo defined here); localisation variants and PSD/ASL import UI (S87).
- An editor-wide UI translation layer. The window's labels are English, like every panel
  (`editor/` has no string tables, report e2 §7).
- Recovering the whole editor from a GPU device loss in process (D12).
- The Scene view's own gizmos: titles in a scene use the existing Move/Rotate/Scale/Rect tools.
- Nothing from the do-not-copy list (`docs/specs/README.md`) is involved: ImGui, our widgets, our
  docking.

## 3. Current state (feature/title-art 3ffafdfaa, read)

- **No title UI exists.** `editor/titleart/` does not exist; S80-S87 are specified in parallel
  (read on 2026-11-04: S80's document, ops, renderer, registry, MCP tools and checks; S81's drops;
  S84's previews; S86's gizmo target).
- **Window precedents.** A panel is a hand-wired class (`show/open/setOpen/isOpen/
  resetForProjectSwitch`, `WINDOW_NAME`): constructed in `editor/App.cpp:201` (Sprite Normals), a
  Window menu row (`App.cpp:711`), docked as a tab beside Scene and Game in `dock_id_middle_top`
  (`App.cpp:2077-2085`), focus through `focus_window` (`editor/mcp/tools/ViewTools.cpp:1545`).
  `AudioMixerWindow` (`editor/window/AudioMixerWindow.h`) + `AudioMixerDocument`
  (`editor/audio/AudioMixerDocument.h`: `commit` with merge key, `applyOps` shared with MCP) is
  the document-window shape; "a drag is heard live and written once, when it ends".
- **Private-scene viewport.** `VatPreviewView` (`editor/render/preview/VatPreviewRender.h`): a
  scene never added to the Engine, `requestDraw(w, h)`, `processFrame` from `App::engineRender`,
  `Engine::drawView(VAT_PREVIEW_VIEW_ID, &target, {layer})` (`VatPreviewRender.cpp:307`,
  `engine/core/Engine.h:811`), `SETTLE_FRAMES = 6`, no read-back. View ids 1-4 are taken
  (`editor/render/RenderUtil.h:18-23`).
- **Widgets.** `colorField`, `gradientField`, `curveField` with the phase contract
  (`editor/widget/FieldEdit.h`: Begin/Changing/Committed/Cancelled/Applied; "the widgets never
  touch a CommandHandle"); the non-component glue is `FieldUi` in
  `editor/window/InspectorParticles.cpp:60-90`, private to that file, without reset arrows.
  `ui::registerViewImage` lets the eyedropper sample any view (`editor/widget/ColorField.h:44`).
  The engine `Gradient` holds at most 8 keys and has no midpoints (`engine/core/math/Curve.h:196`),
  while S80's Gradient has 2..32 stops with Photoshop midpoints and an OKLCH mode.
- **Inspector rows** (`propertyRow`, `InspectorWindow.h:607`) need an ECS component and entity list;
  they cannot draw a document field (report e2 §2.1).
- **Search.** `search::fuzzyScore`, `filterMatch`, `highlightRanges` (`editor/search/SearchScorer.h`).
  `search::normalize` keeps only `std::isalnum` bytes (`SearchScorer.cpp:67`), so **every non-ASCII
  character becomes a space**: a Japanese query matches nothing today, in any editor search.
  Settings search (`editor/settings/SettingsSearch.h`) is bound to `SettingsRegistry`.
- **Undo.** `CommandHistory::beginInteractive` / `InteractiveEdit` (`editor/command/CommandHistory.h:72`)
  and the per-widget hook `beforeRecord` (`:216`); `WriteFilesCmd` with merge key and name
  (`editor/command/type/WriteFilesCmd.h:31-52`); one Ctrl+Z timeline over scene and project
  histories with `jumpTo` (`editor/command/UndoTimeline.h:93`); Undo History panel
  (`editor/window/UndoHistoryWindow.h`).
- **Thumbnails.** Project window disk cache `<project internal>/thumbs` (`Project::getThumbnailPath`,
  `editor/Project.h:2257`; 128 px, `editor/Configs.h:7`); `PoseThumbnails` (GPU LRU keyed by the
  caller's hash, a few requests in flight, `editor/pose/PoseThumbnails.h`); a background
  `jobs::SerialQueue` (`engine/core/jobs/SerialQueue.h:40`).
- **Device frames.** `deviceProfiles()`, `profileSafeRect`, `drawDeviceOverlay`
  (`editor/window/DeviceProfiles.h`); `builtInGameResolutions()` (`editor/window/GameResolutions.h`).
- **Drops.** Scene view `resource_files` branches per type (`editor/window/SceneWindow.cpp:584-640`;
  the VAT drop calls `createVatObject`, `editor/vat/VatSceneActions.h:92`); Hierarchy
  (`HierarchyWindow.cpp:1966`); OS file drops become an ImGui `external_files` payload
  (`App.cpp:2746-2752`); `ProjectWindow::openFile` (`ProjectWindow.cpp:4676`).
- **Shortcuts.** `ShortcutContext { Global, Editor, SceneView, Hierarchy, Project, Console,
  Animation, PoseAnimation }` (`editor/shortcuts/CommandCatalog.h:36`); the name parser loops to
  `PoseAnimation` (`CommandCatalog.cpp:60`); panel contexts are pushed in `AppCommands.cpp:410-411`.
  Unity tool keys Q/W/E/R/T are `tools.*` in the Editor context (`DefaultCommands.cpp:261-265`).
- **IME.**
  - Windows: `imgui_impl_win32` with ImGui's default `Platform_SetImeDataFn`, which places the
    IMM composition window at the caret (`libs/imgui/imgui.cpp:21905`). The OS draws the preedit.
  - Linux: XIM with `XIMPreeditNothing | XIMStatusNothing` (`editor/backend/BackendLinux.cpp:1020-1024`):
    the input method draws its preedit in its own window, not at the caret.
  - macOS: `imgui_impl_osx`'s `KeyEventResponder` implements `NSTextInputClient`, but
    `hasMarkedText` returns NO and `setMarkedText` is empty
    (`libs/imgui/backends/imgui_impl_osx.mm:240`, `:255`): **the text being composed is never
    shown**, only the committed text arrives (inferred from the code; not run).
- **GPU device loss**: the editor logs "Graphics device was reset, closing the editor without
  saving" and exits (`editor/EditorFrame.cpp:144-147`).
- **Fonts.** Font assets are `.ttf`/`.otf` (`editor/util/Util.h:41-43`,
  `editor/asset/AssetTypes.cpp:32`; S80 adds `.ttc`). No WOFF: `GoogleFontsImporter` refuses
  WOFF-only answers (`editor/htmlui/GoogleFontsImporter.cpp:597-600`) and writes a CSS file and an
  HTML UI manifest entry for every import (`:628-643`, `:710`). Brotli is not vendored (`libs/`,
  `engine/libs/`); miniz is, editor only (`CMakeLists.txt:628-632`). No system font listing exists;
  unicode-text adds a CJK lookup for the editor UI only (its §6.6).
- **GPU timing** for a secondary view: `SystemRender::getPassList(viewTag, resolvedOnly)` and
  `acquireGpuTiming` (`engine/core/render/SystemRender.h:136-152`), off unless held.
- **MCP.** `focus_window` (core, `ViewTools.cpp:2014`) returns the panel `rect` (`:1564-1570`);
  `screenshot` views are `game`, `scene`, `editor` (`:1948`). The scripted-window test model is
  `SpriteNormalsWindowTestAccess` (`editor/window/SpriteNormalsWindow.h:48`,
  `editor/tests/SpriteNormalsWindowTest.cpp:73`).

## 4. Reference

### 4.1 Pachislop (`/tmp/pachislop-analysis/repo`, SafEight, used with the author's permission)

Ported UI ideas and the algorithms behind them (TypeScript/React to C++/ImGui). The look of the
panels is ours (ImGui, the editor's theme); the behaviour is theirs, improved where reports p5 and
p7 found faults.

| Pachislop | Files | S82 |
|---|---|---|
| Properties search with live controls, index read from the real views (hidden probe render + DOM contract), rank tiers, synonyms, `+ Add` results, landing (select, open, scroll, flash 1.6 s) | `src/ui/properties/PropertySearch.tsx`, `searchIndex.ts` (synonym groups `:110-133`, `norm` `:136-143`), `Findable` in `src/ui/controls.tsx` | Ported as a collect pass over the same page functions (D4). The synonym groups become `search-terms.json`. Fixed: the search text is cleared after a jump (p7 "stale search box") |
| Multi-target controls: mixed display, set-all versus shift-each per gesture | `src/ui/controls.tsx`, `src/store/history.ts` (`setEachAt`), `tests/unit/multiTarget.test.ts` | `DocRow` over a span of field refs (R22) |
| Scrub labels, 3 px slop, ⇧ ×10 / ⌥ ×0.1, wheel only when focused, log mapping at ≥ 50× | `src/ui/controls.tsx` `NumberField`, `ScrubLabel` | `DocRow` (R21); modifiers are Unity's (Shift faster, Alt slower) |
| Per-value source dots, edit-to-override | `src/ui/properties/style/Lighting3D.tsx` `SourceMark` | `DocRow` source dot (R23) over S80's `use_global_light` flags |
| Appearance rows: summary line, the most-changed number on the row, collapsed sections state their contents | `src/ui/properties/AppearanceRow.tsx`, `GroupTitle` | Appearance section and section headers (R19, R20) |
| Gradient bar: add by click, drag down 40 px deletes, midpoints, reverse, vivid | `src/ui/GradientEditor.tsx` | `RampField` (R25) for S80's Gradient (32 stops, `mid`, `vivid`) |
| Layers rows, drop zones (halves, middle 40% into groups, left of indent steps out), twirl into view, panel keys | `src/ui/layers/LayersPanel.tsx`, `LayerRow.tsx`, `rows.ts`, `dnd.ts`, `commands.ts` | R10-R16. **Not ported:** the folded companion wrappers and "generators shown as effects" (p5 §7.2.2; contract): generators are layer rows |
| Layer thumbnails: debounced, idle-time, keyed by content | `src/ui/layers/thumbs.ts` | `TitlePreviewQueue` (D7) |
| Library: tiles on the selected layer's own text, dedicated offscreen renderer, visible-first, cache keyed by a content key, `useSettled` 300 ms, never show another layer's picture, failed tiles say so | `src/ui/library/LibraryPanel.tsx`, `thumbs.ts`, `search.ts`, `prefs.ts`, `TemplateMenu.tsx` | R40-R49. Fixed: thumbnail work had no frame budget and kept running after the dialog closed (p7 §6: long tasks of 22 s) |
| Hover intent: 150 ms after real pointer movement; preview override never undoable; double-click's second click ignored | `LibraryPanel.tsx`, `src/presets/apply.ts`, `previewOverride` in `src/store/store.ts` | R42-R44 |
| Add Effect popover with category rail, counts, hover/arrow preview, header naming the target | `src/ui/properties/AddEffectMenu.tsx` | The unified **Add** palette (R17). Fixed: generators had a separate three-level flyout (p7 §6 "Effect or generator?") |
| Gizmo: fixed screen-space schematic (`SCHEMATIC_ARMS` X 84 px, Y 84 px, Z 60 px down-left), quarter arcs at radius 50, free ring at 114, centre square, halo strokes, separate hit shapes, shafts hit from 0.74 of the arm, Euler branch nearest the previous value, readout of only the changed values | `src/ui/viewport/Gizmo3D.tsx`, `gizmoEdit.ts`, `rotation3d.ts`, `Handle.tsx`, `tests/unit/rotation3d.test.ts`; commit `ecaa136` | `TitleGizmo3D` (R35), the decision of `ecaa136` kept |
| Shape and effect handles (points, directions, labels, Shift snaps), light dial, gradient line, distort corners | `src/ui/viewport/shapeHandles.ts`, `ParamHandles.tsx`, `LightDial.tsx`, `gradient.ts`, `gradientTarget.ts`, `DistortHandles.tsx`, `GlyphGizmo.tsx` | `TitleHandles` (R33) driven by the registry's handle declarations |
| Canvas: auto-select, snapping within 8 screen px with drawn guides, marquee, part pick under the cursor, effects' reach box, full-frame generators picked only where alpha ≥ 0.05 | `src/ui/Viewport.tsx`, `viewport/drag.ts`, `pick.ts` | R29, R32, R34. Part pick moves from ⌥-click to S26's root-then-drill clicks (p5 §7.2: ⌥ is Unity's orbit) |
| Inline text editor over the art; Esc reverts the typing burst | `src/ui/viewport/TextEditOverlay.tsx` | R36-R39, drawn in the real look with an IME preedit |
| Settled sharpness: visible window at device resolution after 150 ms; interactive draft during drags | `src/ui/viewport/previewWindow.ts`, `src/ui/engineHost.ts`, `tests/perf/budgets.json` | R29, R68 |
| History panel, path-derived step labels, multi-target counts | `src/ui/layers/HistoryPanel.tsx`, `src/store/labels.ts`, `src/store/history.ts` | R50-R52 over S24's timeline |
| Style lint badge, ⓘ citations, fixes as one step "Style fix: ..." | `src/ui/properties/StyleLint.tsx`, `src/presets/lint.ts` | R53 over S80's checks |
| Empty-comp card | `src/App.tsx` (`EmptyCanvasCard`) | R5, with template cards on it (p7 §3: the template route was the exciting one) |
| Font browser: names in their own face, the layer's text as sample, hover preview | `src/ui/FontBrowser.tsx`, `fontFiles.ts` (refuses WOFF, WOFF2 and `.ttc`), `LocalFontDialog.tsx` | R54-R60: adds WOFF/WOFF2, `.ttc`, axes, system fonts on every OS, Google Fonts |

Improvements over Pachislop that the reports asked for (p7 §7 "Change"): no frame ever blocked by
thumbnails (R67); render progress on the canvas, never a half-ready image presented as final
(R71); resource rebuild after GPU invalidation (R64); a slow-renderer notice and Draft preview
(R70); clamping with feedback and an explicit empty-text state (R21, R24); the main title
selected after a template (R5); Color Schemes on the whole title (R45); one Add palette (R17);
structural side effects named in the undo step (R75); expert sections collapsed on an empty
document (R18).

### 4.2 Engine precedents to copy

| Need | Precedent |
|---|---|
| Window shell, document commits, staged drags | `AudioMixerWindow` + `AudioMixerDocument::commit(…, mergeKey)`; S80's `TitleArtDocument` |
| Private-scene canvas | `VatPreviewView` (`requestDraw`, `processFrame`, settle frames, `drawView`) |
| Scripted API under every button and MCP | `SpriteNormalsWindow.h:13-16` and its `TestAccess` |
| Card grid with thumbnails, categories, search, user badge | `editor/window/PoseAnimationWindowTemplates.cpp`, `editor/pose/PoseThumbnails.h` |
| Table-driven rows, foldouts, Add Module | `InspectorParticles.cpp:60-100, 515-700` |
| Eye and lock gutter | `editor/window/HierarchyVisibilityGutter.cpp` |
| Device overlay, resolutions | `DeviceProfiles.h`, `GameResolutions.h` |
| Root-then-drill picking, click cycling | S26 (`docs/specs/S26-scene-view.md`) |
| Field windows and phases | `editor/widget/FieldEdit.h`, `ColorField.h`, `CurveEditor.h` |
| Object from a dropped asset | `createVatObject` and the Scene view drop branch (`SceneWindow.cpp:587-603`) |

Third-party code: Brotli decoder (google/brotli `c/common`, `c/dec`, MIT) and the WOFF2 decoder
(google/woff2 `woff2_dec.cc`, `woff2_common.cc`, `table_tags.cc`, `variable_length.cc`,
`woff2_out.cc`, MIT) are vendored with their headers under `libs/brotli/` and `libs/woff2/`, editor
only, each with a `NOTICE.md` entry. The ported Pachislop UI code gets the program's `NOTICE.md`
entry (S80's), extended with the files above.

## 5. Requirements

### Functional — the window

- **R1. Opening.** One window class, `TitleDesignerWindow` (`WINDOW_NAME = "Title Designer"`),
  shows one `.titleart` at a time. It opens from:
  - Window > Title Designer (the last document, or the empty "Open a Title Art" state);
  - double-click on a `.titleart` in the Project window (`ProjectWindow::openFile`);
  - the `.titleart` Inspector summary's **Open in Title Designer** button (S80) and the Title Art
    Renderer's **Edit** button (S81), which also sets the In Context background (R30);
  - Assets > Create > Title Art (S80), once the new file's inline rename ends;
  - `focus_window {window: "title_designer", title_designer: {path}}` (R78).

  Opening another document replaces the shown one (nothing is ever unsaved, R8). The toolbar's
  document chip lists the 10 most recent documents of the project (workspace setting).
- **R2. Default dock and layout.** The window docks as a tab in `dock_id_middle_top` beside Scene
  and Game, like the other preview-first windows (`App.cpp:2077-2085`). Inside, regions are child
  windows with splitters (the `AudioMixerWindow` pattern, not a nested dockspace):

  ```
  +-------------------------------------------------------------------------------------------+
  | [logo.titleart v] | Q W E R T  Pivot | + Add v | Preview: Realtime v | BG v | Device v |   |
  |                     Guides v  Snap [x] | 50% v | Bake (out of date) | lint ok | Stats      |
  +--------------------+------------------------------------------------+--------------------+
  | Layers | History   |                                                | Properties         |
  | Normal v  100%     |                     canvas                     | [Find a setting  ] |
  | o L [##] Title  fx |      (rulers top/left, guides, gizmo, handles) | Title > Rim 2      |
  |   v Face           |                                                | [T] Title  ok      |
  |     Rim 1   3%     |                                                | v Transform        |
  |     Rim 2   1.2%   |                                                | v Text             |
  |   v Effects        |                                                | v Appearance       |
  |     o Outer Glow   |                                                |   Face  Gold ...   |
  | o L [##] Burst GEN |                                                |   Rim 1 3%  ...    |
  | [+][fx][grp][clip] |  1920x1080  x 312 y -40  Full 12.4 ms          | v Effects          |
  +--------------------+------------------------------------------------+--------------------+
  | Library: Looks | Color Schemes | Templates   Applies to "Title"  [search] Family v 2D/3D  |
  | [tile][tile][tile][tile][tile][tile][tile][tile][tile][tile]  Favorites  Recent  S M L    |
  +-------------------------------------------------------------------------------------------+
  ```

  Default sizes (logical px, scaled by the editor UI scale): Layers 260 wide, Properties 340 wide,
  Library 196 high (one row of M tiles plus its header), the canvas the rest. Each region
  collapses (View menu of the toolbar's overflow, and a chevron on its edge). Sizes and collapsed
  states are editor settings (`settings.yaml`, per user, not the project).
- **R3. Narrow windows.** Below 960 px of window width the Layers, History and Properties regions
  become tabs of one side column (Properties first when something is selected), and the Library
  collapses to its header bar (one click opens it as an overlay over the canvas bottom). Below
  480 px the canvas alone shows, with a "Panels" button.
- **R4. Toolbar** (left to right; every item has a tooltip naming its shortcut):
  - document chip (name, recent list, Show in Project, Duplicate As…);
  - tools Hand (Q), Move (W), Rotate (E), Scale (R), Rect (T), and the Pivot/Center toggle (Z);
  - **Add** ▾ (the Add palette, R17);
  - **Preview As** Realtime | Baked | Glyph Atlas (the S81 Render Modes; Baked shows the baked PNGs
    with a "stale" badge when their recipe differs, Glyph Atlas shows the atlas approximation with
    the note "Cross-glyph effects are drawn per glyph");
  - **Quality** Auto | Draft | Full (Auto: Draft while interacting, Full when settled);
  - **Background** ▾ (R30), **Device** ▾ (R31), **Guides** ▾ (Rulers, Guides, Safe Areas, Pixel
    Grid, Effect Reach, Clear Guides), **Snap** toggle;
  - zoom field (percent; Fit, 50, 100, 200, 400);
  - **Bake** button with state (Up to date | Out of date | Baking 40%), Auto Bake toggle (R9);
  - **Stats** toggle (R72).

  The toolbar wraps onto a second row when narrow; nothing hides without the overflow menu.
- **R5. Empty state.** When the document has no visible, non-guide content layer (hidden
  ancestors count; Pachislop's `useCompEmpty` rule), the canvas shows a card:
  - title "Start your title", the primary button **Add Text** (Enter), then Shape ▾, Image…,
    Vector (SVG)…;
  - a row of six Template cards (S84's featured templates, its rendered stills) and **All
    Templates…**, which opens the Library on Templates;
  - one line: "Or pick a Look below: it adds a text in that look."
  Applying a template selects the layer of its `title` placeholder (S84: the exposed `text` parameter
  with `role: "title"`, else the topmost text layer), so the first Look click restyles it (p7 §7
  change 6). The card disappears once a visible content layer exists and comes back if all are
  hidden or removed.
- **R6. Read-only states.** A document S80 refuses to edit (`version_newer`, a parse error, a VCS
  lock held by someone else, S29) opens read-only: a banner says why (and which build wrote it),
  every edit control is disabled with that reason in its tooltip, and the canvas still renders
  what S80 can render.
- **R7. Play mode.** The window works during Play. Commits write the asset (project history) and
  Realtime titles in the running game pick them up (S81 hot reload); the toolbar shows "Playing:
  changes are saved to the asset".
- **R8. No unsaved state.** Every commit is written by `TitleArtDocument::commit` (S80) as one
  project undo step; the window has no Save button and no dirty mark. A staged gesture (a drag in
  progress) is the only state not on disk, and it lasts until release or Esc.
- **R9. Bake status.** The toolbar shows whether the outputs (S80's sibling PNGs) are Fresh, Stale
  or Missing, from the same status `get_title` reports. The **Auto Bake** toggle shows and sets S80's
  project setting `title_art.auto_bake` (S80 R34: on by default, a rebake 500 ms after the last
  edit, not an undo step); **Bake** (Ctrl+B) runs S80's bake of every output at once (`export_title`'s
  job), with progress and Cancel in the status bar. A bake failure shows there with its reason and
  Retry.

### Functional — Layers panel

- **R10. Tree.** Rows, top of the list = front (S80 stores bottom first; the panel reverses):
  - **layer row**: visibility eye, lock (both in a gutter left of the indent, as
    `HierarchyVisibilityGutter`), indent, twirl, a 24 × 24 thumbnail (R15), name, badges;
  - under an open Text, Shape, Image or Vector layer: **Parts** (Face, Rim 1..N for 2D; Face,
    Bevel, Side, Band 1..N for 3D, S86), each with a colour swatch of its fill, a link icon when its
    material is linked, and its part effects nested under it;
  - **Effects** (the layer's stack, applied top to bottom = S80 index order, header note
    "Applied top to bottom"), each with an enable eye, its icon and name; **Masks** as rows;
  - a Group's children follow it, indented. Generators and Adjustments are layer rows (never shown
    as effects of another layer).

  Badges: type icon (Text, Shape, Image, Vector, Group, Generator, Adjustment), `3D`, `fx` (has
  effects), guide `G`, clip arrow ↴ on clipped rows with the base's name underlined, a chain icon
  when the layer has exposed fields, a warning icon for problems (unknown type, missing font,
  missing glyphs) with the problem in the tooltip.
- **R11. Twirls.** Groups open by default, other layers closed; twirl state is view state (per
  document, in the workspace). S80's group member `collapsed` gives a group's first twirl state
  when the workspace has none for it (templates and agents use it to open tidy), and the panel
  never writes it (a twirl is not an undo step, D8). Alt-click on a twirl opens or closes all
  siblings. Selecting something elsewhere (canvas, search, breadcrumb) opens the rows that show it
  and scrolls to it.
- **R12. Selection.** Click selects (primary = what Properties shows); Ctrl-click toggles; Shift-click
  selects the range of shown rows; Ctrl+A selects all layers; Esc goes one level up (effect → part
  → layer → group → document). Selection is view state, never an undo step (D8). Parts multi-select
  inside one layer; effects select one at a time (their pages are full height).
- **R13. Header.** Above the tree: **Blend** (S80's blend modes) and **Opacity** of the selection
  (mixed shows `—`), Lock and Guide toggles, a count when several layers are selected. The same two
  fields head the Properties page (R18): one op each, two places.
- **R14. Drag and drop.**
  - Layer rows: above/below by row half; the middle 40 % of a Group row drops into it; dragging
    left of a row's indent steps out of that many groups; below the last row drops at the bottom
    at top level. Dragging an unselected row selects it first; a selected row carries the
    selection. One drop = one `move_layer` batch = one step "Move Layer" / "Move 3 Layers".
  - A group cannot go into itself: the row shows no drop highlight and a tooltip says why.
  - Effect rows drag within a stack, to another layer or part (move; Ctrl copies), one step.
  - Dropping a layer between clipped rows clips it; dragging it out releases it (`clip` set in the
    same batch, named in the step: "Move Layer (Clipped)").
  - Project window drops onto the tree: a Texture makes an Image layer, an `.svg` a Vector layer, a
    font sets the font of the text layer under the pointer; OS files (`external_files`) are
    copied into the document's folder first (fonts into `<assets>/TitleArt/Fonts/`, web fonts converted, R59), one
    step for the import and the layer.
- **R15. Thumbnails.** Each layer row's thumbnail is that layer alone (with its effects and clipped
  layers) rendered by S80's renderer at 24 × 24 logical px × the display scale (at most 2×),
  through `TitlePreviewQueue` (D7) at priority Layers, keyed by the layer's content hash (its
  subtree, clipped layers, mask sources, the canvas size), re-rendered 250 ms after the layer
  settles. While pending, the previous picture of **the same layer** stays; a picture of another
  layer is never shown in its place.
- **R16. Keys and context menu.** Panel keys (Title Designer context, §6.5): Up/Down move, Left/Right
  close/open/go to parent, F2 rename, Delete, Ctrl+D duplicate, Ctrl+G group, Ctrl+Shift+G
  ungroup, Ctrl+Alt+G clipping mask, Ctrl+] / Ctrl+[ bring forward / send backward, with Shift to
  front / to back, H hide, L lock. Context menu: Rename, Duplicate, Delete, Group, Ungroup,
  Create/Release Clipping Mask, Convert to 3D/2D (S86), Copy Look, Paste Look, Save As Look… and
  Revert to Look (S84 R42),
  Expose Text as Parameter…, Select Layers Using This Material Link, Frame on Canvas. Parts are never
  deleted from the keyboard (Pachislop's rule); rims are removed from their row's menu.
- **R17. The Add palette.** One searchable popover for everything that adds: Layers (Text, Shape
  and its 11 shapes, Image…, Vector (SVG)…, Group, Adjustment and its 14 types), Generators (S83
  §6.10's generator categories and presets), Effects (S83 §6.10's categories, with its "Generators
  that follow this layer" section and its style-setting entries, for the selected layer or parts;
  the header names the target and offers "Selected parts | Whole layer" when parts are selected). It
  opens from the toolbar's **Add**, the Layers footer, **Add Effect** buttons (filtered to Effects)
  and Ctrl+Shift+N. Category rail with match counts; English and Japanese search (R26); one-line
  descriptions, the full one in the footer. **Hovering (with intent, R42) or arrowing to an entry
  previews it on the canvas**; Enter or click adds it (one step "Add Effect Outer Glow to Title",
  "Add Layer Sunburst"); Esc closes and restores.

### Functional — Properties panel

- **R18. Pages.** The panel shows one page chosen by the primary selection:

  | Selection | Page |
  |---|---|
  | nothing | Document: Canvas (size, background), Global Light, Seed, Outputs (bake), Exposed Parameters, then the sections later specs register (§6.8): Runtime (S81 R62: Characters), 3D Scene (S86 R51), Languages (S87); every section but Canvas closed on a new document |
  | Text layer | header, Transform, Text, Font, Typography, Appearance, Letters, Warp & Distort, Effects, Masks, Dimension (the 2D \| 3D switch; S86 R50-R51 registers its subsections) |
  | Shape / Image / Vector | header, Transform, Shape / Image / Vector, Appearance, Warp & Distort (shape, vector `shape` mode), Effects, Masks, Dimension |
  | Group / Generator / Adjustment | header, Transform, its own section (S80 members; the generator's or adjustment's registry params), Effects, Masks |
  | part | that part's material sections and its part effects (also editable in place under its Appearance row) |
  | effect / mask | its card at full height; its canvas handles show (R33) |
  | several layers | only the sections all of them have; each header says "3 layers" |

  A breadcrumb (`Title › Logo › Rim 2 › Outer Glow`) heads the page, each crumb a button. The header
  row holds the type badge, the name (editable), the lint badge (R53), Blend and Opacity.
- **R19. Sections.** Each section is a collapsing header whose closed title states its contents
  ("Bevel · Smooth 3 % · Ring", "Typography · Tracking 40 · Vertical", "Effects · 3"). Material
  groups open by default only if something in them is on. Open/closed state is remembered per node
  kind and section (closing Rim › Bevel closes it on every rim, not on the face), in editor
  settings. Scroll position is remembered per node kind and layer for the session. Appearance lists
  Face and each rim as one row: eye, swatch, link chip, name, summary ("Gold · bevel 2.5 %"), the
  rim width as a number field on the row, and hover actions (reorder, duplicate, remove); clicking
  a row edits it underneath; **+ Rim** adds one (`add_rim`).
- **R20. Rows are table-driven.** Every row is a `DocRow` drawn from S80's member schema (type,
  range, default, enum values) and its effect/generator/adjustment registry (`ParamDef`), plus
  S82's editor-only row data (`editor/titleart/designer/TitleRows.cpp`: label, section, order,
  UI range, slider mapping, unit text, `when` visibility, the summary formatter, the row number
  shown on the Appearance row). No page hand-writes a numeric range or a default.
- **R21. `DocRow` editing** (`editor/widget/DocRow.h`):
  - scrub by dragging the label or the value, 3 px slop (under it a click focuses and selects the
    text); Shift ×10, Alt ×0.1 (as Unity's number fields), also for arrow keys and the wheel;
  - the wheel changes a value only when the field is focused, so scrolling never edits; a wheel
    burst within 600 ms is one step;
  - sliders map linearly, or logarithmically when the UI range spans 50× or more (said in the
    tooltip); integer ranges of at most 24 values get − / + steppers;
  - typing may go past the UI range up to the hard range; past it the value is clamped, the field
    flashes amber for 0.6 s and the tooltip says "Clamped to 2,000 px (the maximum)";
  - the tooltip shows the description, range, default and the **field path**
    (`style.rims[1].width`); the row menu has Reset, Copy Value, Paste Value, **Copy Property
    Path**, Expose as Parameter… (`expose`).
- **R22. Mixed values and multi-target edits.** A row edits a span of targets (several layers,
  parts or rims). Different values show `—` (number, hex), a striped swatch or ramp, an
  indeterminate checkbox or a blank combo. **Set-style edits** (typing, picking, Paste, Reset)
  give every target that value; **relative gestures** (scrub, arrows, wheel, − / +) shift each
  target from its own value at the gesture's start, so three rims of widths 2, 3 and 5 scrubbed by
  +2 become 4, 5 and 7. One gesture = one step ("Change Rim Width on 3 Rims").
- **R23. Reset arrows and source dots.**
  - A field that has a default and is not inherited shows a reset arrow right of the value while
    it differs from the default; clicking it (or double-clicking the label) sets the default
    (`set {path: default}`), one step "Reset Bevel Depth". Rows equal to their default show no
    arrow and their label is dimmer, so changed values stand out.
  - An **inheriting** field (`editor/titleart/designer/TitleInheritance.cpp`: S80's
    `use_global_light` members of bevel and satin and the `withGlobalLight` effect params (drop
    shadow, inner shadow and every S83 effect decorated with it); S86's three lighting levels,
    whose "Inherited" rows of S86 R51 are these dots, register their rows there) starts with a
    dot: filled = set here, hollow = from the source, with a one-line legend above the first such
    row ("● set here ○ from Global Light"). Clicking the dot toggles. **Editing
    an inherited value sets it here automatically**, one step "Override Bevel Light Angle" (flag and
    value in one batch). These rows have no reset arrow; their menu has "Use Global Light".
- **R24. Text row.** A multi-line field (Enter = new line, Ctrl+Enter = done) with the IME of R37.
  An empty text is kept as typed: the row warns "Text is empty: this layer draws nothing" and the
  canvas draws a dashed box with "Empty text" at the layer's place (p7's silent desync). When the
  editor font lacks characters of the text (the §8 fallback), a strip rendered with the layer's own
  font (R56) sits under the field so the words are always readable.
- **R25. Gradient and curve rows.** `RampField` (`editor/widget/RampField.h`) edits S80's Gradient
  inline: click the bar to add a stop sampling the current colour, drag stops, drag 40 px down (or
  Delete) removes, diamond midpoints (double-click resets to 0.5), per-stop colour (the shared
  `colorField`), alpha and position fields for the selected stop, Reverse (midpoints mirrored),
  **Vivid** (OKLCH), presets from the swatch libraries (`SwatchLibrary` gradient presets), mixed =
  striped bar. Curves (bevel profile, gloss, satin contour, adjustment curves) use the embedded
  `CurveEditor` with the S80 Curve shape (monotone, corners); the bevel row also shows a
  cross-section preview of the profile.
- **R26. Live property search.** A field at the top of the panel (Ctrl+F in the window):
  - **The index derives from the drawn views** (D4): a collect pass runs the same page functions
    for every node of the selection (layer page, each part, each effect, each mask) and the Document
    page, with every section treated as open and every `when` evaluated on the real data; each
    `DocRow` records its label, aliases, section chain, node, field path, kind and enum value
    labels. So a 3D-only row is never a hit on a 2D layer, and a row hidden by its `when` is not
    either. No list of searchable fields exists to keep in sync.
  - **Results are the controls**: each hit is its location path ("Rim 2 › Bevel") over the same
    `DocRow` bound to the same targets, editable in place with normal undo; rows that only work on
    their page (the Letters grid, the ramp editor's stops) show as links. 30 live results, then
    "Show more". While a result is being edited, the list keeps its order (stable keys) and
    re-indexes 250 ms after the last change.
  - **Matching**, in tiers: whole query in the label (equal, prefix, word start, inside), every word
    in the label, words spread over label and location ("rim width" finds Rim 1 › Width), synonyms
    (`search-terms.json`), an enum value label ("Ring" finds Gloss = Ring); within a tier,
    `search::fuzzyScore`. English and Japanese are always both searched, after the fold of R27.
  - Effects the layer does not have are offered below the hits as **+ Add Outer Glow** (the Add
    palette's entry, with hover preview); generators as **+ Add Generator Layer Sunburst**.
  - Up/Down pick, Enter focuses the live control (a number field selects its text) or follows the
    link: the node is selected, sections open, the page scrolls the row to the centre and it
    flashes for 1.6 s; **the search field is then cleared** (Esc in the field clears it too; Esc
    again returns focus to the canvas). IME composition keys never trigger Enter or Esc.
  - Scope is the selection plus the Document page; a footer row "Search every layer" widens it, with
    results grouped by layer.
- **R27. Text folding for search** (`editor/titleart/designer/TitleSearchFold.h`): full-width ASCII
  to ASCII, U+3000 to a space, half-width katakana to full-width (with voiced marks composed:
  ｶﾞ → ガ), katakana to hiragana (ボカシ finds ぼかし), ASCII lower case, runs of spaces and
  punctuation to one space. And **`search::normalize` keeps UTF-8 bytes ≥ 0x80 as word
  characters** (today they become spaces, `SearchScorer.cpp:67`): one change that makes Japanese
  names searchable in the Hierarchy, Project and every other editor filter too. S84's
  `TitleLibrarySearch` (S84 R34) folds with the same `foldForSearch`, so the Library and Properties
  agree on what matches.

### Functional — canvas

- **R28. Rendering.** `TitleCanvasView` (`editor/titleart/designer/TitleCanvasView.h`) renders the
  shown document (preview, else working, D3) with S80's `TitleArtRenderer`: `begin(RenderRequest
  {doc snapshot, pxScale, region = the visible window, includeGuides, assets = the window's
  resolver})` and `TitleRenderJob::step` within the canvas slice budget (R68), into its own target
  (S81 R24: the Designer's previews do not use the component scheduler). A private scene (never
  added to the Engine) with an orthographic front camera holds the backdrop entity and a quad showing
  the finished `TitleImage` (the previous one until the next is done); the window calls
  `requestDraw(w, h)` and `App::engineRender` calls `processFrame` after its views, which draws
  `Engine::drawView(TITLE_DESIGNER_VIEW_ID, …)` (a new id 5 in `RenderUtil.h`) when the image, view
  or backdrop changed, or settle frames remain; an unchanged canvas issues no draw. **Preview As**
  Baked shows the bake's main output PNG in that quad (stale badge when the recipe differs); Glyph
  Atlas places an S81 Title Art Renderer in Glyph Atlas mode on the committed document in the private
  scene (what that mode ships, through S81's own scheduler); S86 adds its Realtime tiers there
  (S86 R51 "Realtime Preview") and S85 its time (§6.8). The image is shown with `Widgets::image`
  and registered with `ui::registerViewImage`, so the eyedropper samples the canvas exactly. The canvas draws in
  S80's pixel contract (linear premultiplied intermediates) and displays sRGB over the backdrop.
- **R29. View.** Zoom 1/16× to 32× about the cursor (wheel; Ctrl+= / Ctrl+-), pan with the middle or
  right button, or the Hand tool (Q) and left button; **F** frames the selection, **Home** (and
  Alt+F, as the Scene view's Frame All) fits the canvas, Shift+0 is 100 %. While panning or zooming
  the last image scales; 150 ms after the view settles, the **visible window** re-renders at display
  resolution (at most 8,192 px a side), so text is sharp at 400 %. Rulers (top, left) in canvas px
  with the origin at the canvas centre and +y up (S80 §6.1), the cursor's position in the status
  bar.
  **Picking**: a click picks the topmost visible, unlocked layer under the cursor, by its content
  box for Text, Shape, Image and Vector layers and by an `isolateLayer` render at ¼ scale (alpha ≥
  0.05 within 4 screen px, cached until the layer changes) for Generators and Adjustments, so a
  full-frame sparkle does not take every click.
  The selected layer wins while it is under the cursor (it can be dragged out from under others).
  Inside a group the first click picks the top-level group, a second click at the same spot its
  child, a third the part under the cursor (S26's root-then-drill; parts are found on the CPU
  against S80's outlines and rim bands, §6.6). Shift-click adds, Ctrl-click
  toggles; a drag from empty canvas is a marquee (layers whose box it touches).
- **R30. Backgrounds** (view state per document, in the workspace): Checker (default), Black, White,
  50 % Gray, Color…, Image (a Texture asset, cover-fit), **Game Scene** and **In Context**.
  - Game Scene: **Snapshot** renders the selected scene once through its main camera at the
    Device's resolution (or the Game view's), as the stopped Game view renders it, into a texture
    held by the window and cached as `<project internal>/titleart/snapshots/<scene guid>.png`;
    **Refresh** renders it again; it never refreshes by itself.
  - In Context (when opened from a Title Art Renderer, R1): the snapshot of that instance's scene
    without the instance, with the title placed at the instance's screen rectangle (S81 gives it).
  - The document's own `canvas.background` (what bakes composite over) is a Document page row,
    distinct from these backdrops (S80: "preview backdrops are view state").
- **R31. Device frames and safe areas.** **Device** ▾: None (default: the canvas is the document),
  the built-in Game view resolutions, and every device profile with its orientations. With a device,
  the canvas shows that screen at its resolution, with the title placed by **Anchor** (9 points) and
  **Display Height** (the title's frame height on that screen, default 25 % of the screen height,
  in px or %); `drawDeviceOverlay` draws rounded corners, cutouts, the home indicator and the safe
  rectangle; for TV resolutions (16:9, 1280 px and up) the title-safe (90 %) and action-safe (93 %)
  rectangles of SMPTE ST 2046-1 are drawn too. Content outside the safe rectangle is hatched red.
  The same Display Height is what the window passes to S80's check engine function
  (`CheckOptions::displayHeightPx`, S80 §6.12), so `legibility_contrast` is measured at the size
  the title has on that device.
- **R32. Guides and snapping.** Drag from a ruler to add a guide; drag a guide to move it, back onto
  the ruler to delete it; Guides ▾ > Clear Guides. Guides are view state per document in the
  workspace (D15). With **Snap** on, moving, scaling and handle drags snap within 8 screen px to the
  canvas edges and centre, the other layers' bounds and centres, guides, and the pixel grid when
  shown, drawing the matched lines and the distances to the neighbours ("24 px"). Holding Ctrl
  (Cmd) snaps by increments instead, as Unity's: 10 px, 15°, 0.1 scale (editable in the Snap menu).
  Shift constrains to an axis (move), keeps 15° steps (rotate).
- **R33. Effect and part handles** (`editor/titleart/designer/TitleHandles.h`): drawn for the
  selected effect, generator, mask, part or layer from the registry's gizmo hints (S80
  `EffectDef::gizmos`, S83 R40: `point`, `direction`, `radius`, `light`) and, for document members
  outside the registry (fills, shapes, warp, distort, masks), from S82's row data. Kinds:
  - **point** (crosshair + label; glow centre, flare position, generator centre; Shift snaps to the
    layer centre);
  - **direction** (an arrow from an anchor with angle and optional distance; drop shadow, inner
    shadow). With `use_global_light` on, the arrow edits `global_light.angle` and its label says
    "Global Light 120°";
  - **light** (angle + altitude: a dial on the canvas, Pachislop's `LightDial`, and a draggable
    sphere in the row, its `LightSphere`; bevel and satin lights, S83's lit effects), writing the
    global light while `use_global_light` is on;
  - **angle dial** (a fill's `angle`), **gradient line** (start/end of a linear or radial fill:
    `angle`, `scale`, `offset`), **radius** (a ring), **corner set** (distort corners, mesh points,
    with an on-canvas Reset), **shape handles** (corner, inner radius, points count, speech tail,
    warp bend), each labelled with its live value.

  Cards of effects with handles show ✥; hovering ✥ highlights the handles. Every handle drag is a
  staged gesture with one step named after the field ("Change Drop Shadow Angle").
- **R34. 2D gizmo** (`TitleGizmo2D`, ImDrawList, for every layer whose Dimension is 2D):
  - **Rect (T, default)**: the layer's content box (before effects; a faint outer box shows the
    effects' reach when Guides > Effect Reach is on), 8 handles (8 px drawn, 24 px hit). Corner
    handles scale uniformly (`transform.scale`, S80's scale is uniform) about the **opposite
    corner** (position compensated), Alt about the pivot. Edge handles: on Text layers
    `h_scale`/`v_scale` (labelled "Horizontal Scale 92 % (長体)" when the CJK font is present), on
    Shape layers `aspect` and `size`, on Image and Vector layers uniform scale. Just outside a
    corner the cursor becomes a rotate cursor: drag rotates about the pivot (Shift 15° steps).
  - **Move (W)**: X and Y arrows and a centre square; **Rotate (E)**: a ring; **Scale (R)**: a box
    handle at the centre (uniform).
  - **Pivot**: a circle at the layer's `transform.position` (Pivot mode, Z toggles Center = the
    content box centre, Unity's Pivot/Center). Dragging the pivot circle moves the gizmo's pivot
    only (view state, per layer, for the session); rotations and scales about it write
    `position`, `rotation` and `scale` (D16).
  - Several selected layers: one gizmo on their union, each layer transformed about the shared
    pivot, one step ("Rotate 3 Layers").
  - Arrow keys nudge the selection 1 canvas px (Shift 10); a burst of presses less than 800 ms apart
    on the same selection is one step "Nudge".
  - Locked layers have no gizmo and are not picked; guide layers are picked only when Guides are
    shown.
  - Drawing: every stroke twice (a 7.5 px translucent dark halo, then the 3.5 px colour; hovered
    5 px), so handles read over any art; during a drag a readout under the gizmo lists only the
    changed values ("Rotate · 12.5°", "Scale · 1.25 · X 40").
- **R35. 3D gizmo** (`TitleGizmo3D`, for S86's Dimension 3D layers and their letters): Pachislop's
  fixed screen-space schematic of commit `ecaa136`, identical at every position, zoom and rotation:
  X arm 84 px right, Y 84 px up, Z 60 px down-left; three quarter arcs at radius 50 px (elliptical
  towards Z), each between the other two arms; a free-rotate ring at 114 px (thin, quieter); a
  centre square for screen-plane moves; the centre snapped to the device-pixel grid; not drawn when
  the pivot is behind the camera. Hit rules: shafts take presses from 0.74 of the arm outwards
  (arcs sit at 0.6, so both stay grabbable where they cross), heads have their own hit shape with a
  4 px margin, strokes 20 px wide, points at least 24 px; the object stays draggable between
  strokes. Drag mapping: an arrow measures travel along its drawn arm divided by the true px per
  unit at the pivot; an arc steps along its tangent and settles on the nearest ring point within
  ±45°, with a rate floor; turns compose on the orientation at press and are written back as the
  Euler solution nearest the previous value (no flips); the ring is a trackball at 0.4° per px;
  Shift = 15° steps or the target's move step. Colours X red, Y green, Z blue with lighter hovered
  variants; axis letters on dark discs at the tips. S86 supplies a `Gizmo3DTarget` (origin, axes,
  values, write-back) for a layer or a letter; the schematic is used only in this front-on canvas,
  never in the Scene view (p5 §7.2).

### Functional — text editing and IME

- **R36. In-place editing.** Double-click a text layer on the canvas, press Enter with a text layer
  selected, or add a Text layer (which starts editing with the placeholder selected, so typing
  replaces it). The canvas keeps rendering the **real look** as you type: each keystroke updates the
  working document (staged, no commit); the caret and selection are drawn on the canvas from S80's
  layout cluster boxes (horizontal and vertical text, per-line alignment); click and drag select,
  double-click selects a word, Shift+arrows extend, Home/End, Ctrl+A, Ctrl+C/X/V (plain text).
  The art outside the edited layer dims to 60 %. The status bar says "Enter: new line · Ctrl+Enter:
  done · Esc: cancel".
- **R37. IME.** An ImGui text field owns the text, the caret and the IME; it is laid out under the
  edited layer and drawn invisibly, and the window sets ImGui's IME position
  (`ImGuiContext::PlatformImeData.InputPos`, `InputLineHeight`) to the on-canvas caret each frame,
  so candidate windows open at the caret on every platform. The text being composed (preedit):
  - **Windows**: drawn inline in the title, underlined, from `WM_IME_COMPOSITION` (`GCS_COMPSTR`,
    `GCS_CURSORPOS`) while the canvas editor has focus, with the default composition window
    suppressed then; elsewhere ImGui's default stays (the composition window at the caret);
  - **macOS**: drawn inline from `setMarkedText`, through a small patch of `imgui_impl_osx.mm`'s
    `KeyEventResponder` (`hasMarkedText`, `markedRange`, `setMarkedText`, `unmarkText` backed by
    `ImePreedit`; marked `// JasperEngine:` in the vendored file), which also makes composing show in
    every ImGui text field of the editor (today it is invisible there);
  - **Linux**: the XIM input context uses over-the-spot (`XIMPreeditPosition`) when the input
    method offers it (`XGetIMValues(XNQueryInputStyle)`), with `XNSpotLocation` updated from
    ImGui's IME position; the input method draws the preedit at the caret. Otherwise the current
    `XIMPreeditNothing` stays.

  `editor/util/ImePreedit.h` holds the preedit (UTF-8 text, caret, active) the backends fill; the
  canvas inserts it at the caret in a preview of the working text (never committed) and underlines
  it. Commit, Enter and Esc during composition belong to the IME, never to the editor.
- **R38. Letters.** While editing, selecting characters shows **Selected Characters** in
  Properties (S80 CharAdjust: offset, scale, horizontal/vertical scale, rotation, own face
  material) and a small gizmo on each selected glyph (move, rotate, scale, with S80's per-char
  units); Letters (the section) lists every adjusted character with per-character reset and "Reset
  All".
- **R39. Commit and cancel.** Ctrl+Enter, a click outside the layer, or selecting something else
  commits the edit as **one** step "Edit Text" (`set_text`; character adjustments past the new
  length are dropped as S80 says, and the step name then says "Edit Text (2 Letter Adjustments
  Removed)"). **Esc cancels**: the working document returns to the text before editing, no step is
  added (the editor's Esc-reverts rule; contract decision 10). When the document's `name` equals the
  text that was replaced, the toolbar offers "Rename title to 激熱? Rename / Keep" (Pachislop's
  prompt; Rename is one step that sets `name`).

### Functional — Library panel (S84 data)

- **R40. Layout.** Header line naming what a click does: `Applies to "激熱" (Text)`, `Applies to 3
  layers`, or with nothing selected `Adds a text in this look`. Then: tabs **Looks | Color Schemes |
  Templates**; search (Ctrl+F while the Library has focus; S84 R34's `TitleLibrarySearch`, bilingual);
  **Family** chips (S84 R23's fourteen: Casino, Metal & Chrome, Neon, Elements, Gems & Glass,
  Japanese Wa, Sci-fi, Fantasy, Horror, Cartoon, Retro Arcade, Anime, Sports, Minimal) with match
  counts, and the grid in foldable family sections (fold state per user); Dimension All | 2D | 3D;
  **Favorites** (a star on each tile) and **Recent** (S84 R18: personal editor settings, the last 8
  per kind, recorded by Designer applies only), shown first when no filter is on; project and personal
  items with a "user" badge and rename/delete on hover (S84 R16-R17; dropping `.titlelook`,
  `.titlecolors`, `.titletemplate` files on the panel copies them into the project through S84);
  tile size S (120 × 68) | M (160 × 90) | L (240 × 135); **Apply Font** (off by default) and **Keep
  Color Scheme** (on by default), both per user (S84 R42); **Random Look** (from the current filter,
  one step named after the pick); the menu's **Save As Look…**, **Save As Color Scheme…**, **Save As
  Template…** dialogs (S84 R42's fields, validated by S84's functions). No hits: `No Looks match
  "zzz"` with Clear. Tab, filters and tile size are view state (workspace).
- **R41. Tiles show the user's own content.** A Look tile is S80's renderer drawing the selected
  layer's own text (or shape, image) with its font, warp, distort, weight, scale and rotation, with
  that Look applied through S84's apply function: the same function the click runs. With several
  layers selected, the primary. With nothing selected, the document's topmost text layer; with
  none, the sample `TITLE` in the project's first font (else S80's built-in face). A Color Scheme tile recolours the selected
  layer, or the whole title when its scope is Whole Title (R45). A Template tile is S84's rendered
  still. Tiles show a `→3D` / `→2D` badge when applying converts the layer (S86), with the tooltip
  "Converts the layer to 3D (undoable)", and are disabled with the reason when the target cannot
  take them. The frame of each tile is the text bounds plus the Look's reach (rotation included), so
  rims never clip. A tile whose render failed says "Render failed" with the reason in its tooltip and
  is not retried for that key.
- **R42. Hover preview.** Pointing at a tile previews it on the canvas **only with intent**: 150 ms
  after the pointer moved at least 1 px over it (a list scrolling under a still pointer, a popup
  closing over a tile or a zero-movement enter never previews). Arrow keys move the highlight and
  preview at once. The preview is a **transient state**: the same apply op run on a copy of the
  working document, shown by the canvas and the Layers thumbnails, never committed and never an
  undo step (D8). Leaving the tile area, closing the panel or Esc restores the document as it is; a
  commit by anyone else (an agent's `edit_title`) recomputes the preview on the new document.
- **R43. Commit.** Click or Enter applies, as **one** step named after the item ("Apply Look Chrome
  Gold", "Apply Color Scheme Red to Whole Title", "Apply Template Big Win"); the second click of a
  double-click is ignored. Templates on a document with content first ask "Replace the whole title?
  Undo brings it back." (no question on an empty document).
- **R44. Preview equals apply.** The preview and the commit run the same S84 function with the same
  arguments on the same document; the committed result is bit-identical to the previewed one (a
  render of each is compared in AC19).
- **R45. Color Scheme scope.** Color Scheme tiles have **Selected Layers | Whole Title**; the default is
  Whole Title when the selection is empty or a group, Selected Layers otherwise (p7: recolouring one
  layer only left orange rays around red text).
- **R46. Thumbnail freshness.** Live tiles come from S84's `TitleLibraryPreviews` (S84 R33: its key
  of item, revision, the layer's content key, scheme, Dimension and `TITLE_ART_BAKE_VERSION`,
  excluding position and canvas; a pending tile keeps its picture only if it shows the same layer).
  The Library asks for them with the tile size and display scale, holds its requests while the
  target's key has been stable for less than 300 ms (typing, dragging), and asks only for visible
  tiles and the next screen. Moving a layer or editing another layer invalidates no Look tile.
- **R47. First paint.** Until a tile's live render exists, S84's baked thumbnail of the script that
  matches the layer's text (S84 R33) shows at 50 % opacity with a small spinner, never as if it were
  the user's text; Template tiles are S84's stills.
- **R48. Caches.** S84 R33's disk cache (`.jasperengine/cache/titleart/library/`) and GPU LRU (240
  tiles) are the Library's; S82 adds none. A disk-cache hit is decoded on a worker and shown within 3
  frames of the tile becoming visible. After a GPU resource loss the visible tiles are requested again
  (R64).
- **R49. Library keys.** Arrows move the highlight (and preview), Enter applies, Esc restores and
  returns focus to the canvas, Home/End, Page Up/Down; `S`/`M`/`L` are not bound (they would steal
  typing in the search).

### Functional — History and lint

- **R50. History panel** (a tab beside Layers): the project-history steps that touched this
  document, from "(Opened)" to the newest, with their S24 names, origin icon (person, or agent with
  its client name) and time; redo rows dimmed. Rows are read from the one undo timeline
  (`UndoTimeline`), filtered by the step's touched path.
- **R51. Jumping.** Clicking a row jumps there with `UndoTimeline::jumpTo` when no step of another
  history (a scene, another asset) lies between; otherwise a popover lists those steps ("Also
  undoes 2 other steps: Move Player, Paint Terrain") with **Jump Anyway** and **Restore This
  Version** (writes that version's text as one new step "Restore Title Version 'Apply Look
  Neon'", leaving everything else alone).
- **R52. Hovering a history row** previews that version on the canvas as a transient state (R42's
  mechanism; the version's text comes from the step's stored after-text, S80's command), with no
  step.
- **R53. Style lint badge.** The Properties header shows the findings of `check_title`'s engine
  function (S80 §6.12) for the selection, with S84's `style_lint` findings (S84 R35: Pachislop's
  rules with their cited sources) and the checks later specs register (S85's animation checks):
  `✓ 14 checks` or `⚠ 3`. Checks of kind `document` and S84's `style_lint` run 300 ms after the last
  commit, on the main thread within 2 ms, only while the panel shows; checks of kind `pixels`
  (contrast against the backgrounds, legibility at the Device's Display Height, clipping) run when
  the popover opens or on **Check Rendered**, as a job, never by themselves. The popover lists each
  finding with its message, an ⓘ with the rule and its cited source, the measured value against the
  threshold, the crop to look at (`at`), **Fix** when the check returns fix ops (one step "Style Fix:
  Contrast"), and **Fix All** (one step). The badge never says the title looks good: "No issues found
  by 14 checks" is the wording, and the popover footer repeats S80's guidance ("These are
  measurements; passing them does not mean the title looks good").

### Functional — fonts

- **R54. Font row and picker.** The Font row shows the current font's name drawn as a preview strip
  in that font, a missing-glyph badge ("3 missing") and a ▾ that opens the picker:
  - search over family, style and file names; sections **Project** (every Font asset, `.ttc` faces
    listed separately), **Library** (S84's bundled OFL fonts, copied into the project on use),
    **Google Fonts** (R57), **System** (R58);
  - each row: family and style in its own face, then the user's text (the selected text layer's
    text on one line, the first 24 code points, else `Title タイトル 0123`), as a preview strip;
  - filter chips **Covers My Text** (on by default when the text has a character outside Basic
    Latin) and Category (Sans, Serif, Display, Handwriting, Monospace, from the fonts' OS/2 and
    Google's metadata);
  - hovering with intent or arrowing previews the font on the canvas (transient; a system or Google
    font not yet in the project previews from its file or download cache, R57, R58); Enter or click
    sets it (one step "Change Font to Dela Gothic One"), copying it into the project first when it
    is not there;
  - **Import Font File…** (`.ttf`, `.otf`, `.ttc`, `.woff`, `.woff2`).
- **R55. Missing glyphs.** For the layer's text, the row and each picker entry count the code points
  the font (with the layer's Fallback Fonts) cannot map; the preview draws them as a red box with a
  cross. The Font section lists them ("Missing in Dela Gothic One: 鬱 鷹") with **Add Fallback
  Font…**, which offers the project fonts that have all of them, first; one step.
- **R56. Preview strips** (`editor/titleart/fonts/FontPreviewRaster.h`): S80's shaper (HarfBuzz, the
  layer's language and direction, vertical when the layer is) and outlines (hb-draw, the variation
  axes) rasterized on the CPU (ThorVG) on the Background lane, uploaded as small textures, for
  visible rows only, at most 2 uploads per frame, kept in an LRU of 512 strips. ImGui's font atlas
  is never used for a font preview (it has no shaping and no axes).
- **R57. Google Fonts.** The family list is Google's metadata (`GoogleFontsImporter::metadataUrl`,
  fetched once per session, cached for 7 days under the user cache directory). Hovering a family
  downloads its regular style into the user cache (`<user cache>/jasperengine/google-fonts/`, at most
  2 downloads at once) to preview it. Choosing it imports through `GoogleFontsImporter` in a new
  **fonts-only mode** (`Options{writeCss = false, writeManifest = false, dir = "TitleArt/Fonts/Google"}`):
  the styles the user ticks (default Regular and Bold; the full weight range when the family is
  variable) go to `<assets>/TitleArt/Fonts/Google/<family-slug>/` with its licence file (`OFL.txt`
  or the family's), the importer's file naming, in the program's one font folder (S80 §6.15). Offline, the section says "Google Fonts needs a connection" and the rest works.
- **R58. System fonts** (`editor/titleart/fonts/SystemFontList.h`): listed on a background job when the
  picker first opens, cached for the session:
  - Linux: `fc-list --format '%{file}\t%{index}\t%{family}\t%{style}\t%{fontformat}\n'` through
    `CommandRunner::capture` (no shell, 5 s timeout), keeping TrueType and CFF outlines;
  - macOS: CoreText (`CTFontManagerCopyAvailableFontURLs`, faces from `CTFontManagerCreateFontDescriptorsFromURL`);
  - Windows: DirectWrite (`IDWriteFactory::GetSystemFontCollection`, each face's file through
    `IDWriteLocalFontFileLoader::GetFilePathFromKey`).

  Choosing one copies the file into `<assets>/TitleArt/Fonts/System/` (a `.ttc` whole, with the face index in
  the document) as one step, after reading its OS/2 `fsType`. **Restricted License embedding**
  (0x0002 alone) asks first: "This font's licence does not allow embedding it in a game. Exports of
  titles using it fail unless you confirm you hold a licence (S81). Copy it anyway?"; every other
  system font is copied with the note "System font: check its licence before you ship a game with
  it", repeated in the Font section. S81's export applies its own fsType rule and acknowledgement
  (S81 AC15); the picker only tells the user early. The listing shares nothing with unicode-text's CJK UI font
  lookup except `SystemFonts::isLoadable` and `faceIndexByPostScriptName` (their §6.6), which it
  reuses.
- **R59. WOFF, WOFF2 and collections** (`editor/titleart/fonts/WebFontDecode.h`): `.woff` (zlib tables
  through miniz) and `.woff2` (Brotli + the glyf/loca and hmtx transforms through the vendored
  google/woff2 decoder) are decoded to sfnt on import and written as `<stem>.ttf` (TrueType outlines)
  or `<stem>.otf` (CFF), next to where the import was asked (picker: `<assets>/TitleArt/Fonts/`; a drop: the
  dropped folder); the `.woff` itself is not copied. A `.woff`/`.woff2` already inside the assets is
  listed in the picker with a **Convert** badge; choosing it converts it (one step creates the font
  file; the web font stays). Files over 64 MB decoded or failing the decoder's checks are refused
  with the reason. `.ttc` faces come from `hb_face_count` and each face's name table.
- **R60. Variable axes.** For a font with `fvar`, the Font section shows **Style** (named instances,
  `hb_ot_var_get_named_instance_count`) and one `DocRow` slider per axis (name from the name table,
  range and default from `fvar`), writing `variations` (S80). Changing font keeps the axes the new
  font has and drops the others in the same step (named "… (Axes Reset)" when that happens).

### Functional — scene integration

- **R61. Drops into scenes.** Dropping a `.titleart` into the Hierarchy, the Scene view or the Game
  view creates a GameObject with a Title Art Renderer (UI or world by where it lands), one step
  "Create Title Art (logo)": S81 R6 implements those drop branches and the create menus. S82 adds
  the same drag from the window's **document chip** (the `resource_files` payload of the shown
  file), so a title being designed goes into a scene without finding it in the Project window.
- **R62. Double-click** in the Project window opens the window (R1); the Inspector of a selected
  `.titleart` (S80's summary) and of a Title Art Renderer (S81) have the buttons of R1.

### Platform & backends

- **R63. Backends.** The canvas, thumbnails and previews are S80's renderer through sokol-gfx on GL 3.3,
  GLES3/WebGL2 (the editor does not run on web, but nothing here adds a desktop-only GPU path),
  Metal, D3D11 and Vulkan; no compute, SSBO, bindless, indirect draw or 64-bit atomic. The editor
  runs on Windows, Linux and macOS; each OS's font listing and IME code is compiled only there, with
  the pure parts (parsers, folding, decoders, gizmo maths) compiled and tested everywhere.
- **R64. GPU resource rebuild.** The canvas, thumbnails and previews detect render targets or
  textures in a failed or destroyed state (a new `SystemRender::getContextId()`, S80 R19; a sokol
  resource in the failed state, S81's `TITLE_ART_TEST_FAIL_TARGETS`; a project switch; the renderer
  reporting `out_of_target_memory`) and rebuild their resources and redraw by themselves,
  showing "Rebuilding preview…" over the canvas meanwhile; a black or half-built image is never
  shown as the result. A real device loss still closes the editor (D12): nothing of the document
  is lost (R8), and on reopening the window comes back with the same document, selection, view and
  panel sizes (workspace).

### Performance (budgets)

- **R65. Closed costs nothing.** With the window closed: no private scene exists, no renderer
  instance, no preview queue work, no font listing, no timer, no file watch beyond the project's;
  `show()` returns at its first line. Opening it creates them; closing destroys them within one
  frame and cancels every queued preview.
- **R66. Idle costs nothing on the GPU.** With the window open and nothing changing, view 5 issues no
  draw call (the settle frames done) and the preview queue is empty.
- **R67. Frame budget for previews.** `TitlePreviewQueue` is the one arbiter of S80 R45's shared
  editor budget: S82's layer thumbnails, font strip uploads, history previews and hover previews,
  S84's `TitleLibraryPreviews` (S84 R33), S80's Project-window thumbnailer (S80 §6.13) and S86's
  picker thumbnails (S86 R54) submit their render steps through it and keep their own keys and
  caches. It admits work until the frame's preview budget is spent: **4 ms** per frame by default
  (Preferences > Title Designer > Preview Budget, 1-16 ms),
  counted as the main-thread time of its submissions plus the GPU time of its passes from the
  newest resolved frame when GPU timing is held (rolling mean per item kind), else the CPU time
  only. A single item predicted to cost more than the budget runs alone, at most once every
  ⌈cost / budget⌉ frames. Order: the hover/arrow preview, then visible layer thumbnails, then
  visible Library tiles, then the next screen of tiles; anything that scrolls out of view is
  cancelled. On close everything is cancelled within one frame.
- **R68. Interaction.** While a gesture is active the canvas renders at Draft quality: the request's
  `pxScale` halved (at least ¼ of the display scale) and shown scaled up, with S80's node cache
  re-placing unchanged layers (S80 R42: a position change ≤ 2 passes; S86 R49 for 3D layers), and its
  slice budget is 8 ms per frame; after release one Full render, budget 12 ms per frame, time-sliced
  with the previous image kept (S80 decision 2). Draft must cost at most 1/4 of Full for the
  reference documents (AC31). Hover previews show their first Draft image within 2 frames of intent
  for 2D Looks.
- **R69. UI and search.** For the reference 40-layer document (S80 fixture `bench_40_layers`), the
  window's ImGui work costs at most 1.5 ms per frame on server A (headless frame, rendering excluded);
  the search index of a selected text layer with 6 rims and 8 effects builds in at most 3 ms and a
  query answers in at most 1 ms; the Layers tree is clipped (`ImGuiListClipper`) so 256 layers cost
  no more than the rows shown.
- **R70. Slow renderers.** When `SystemRender::getDeviceName()` names a software renderer
  (llvmpipe, SwiftShader, Microsoft Basic Render Driver), Quality starts at Draft, the Preview Budget
  at 8 ms, and the status bar says once "Software renderer: previews at Draft quality" (p7: a 3×
  supersample default on SwiftShader).

### Editor UX (human)

- **R71. Progress and busy feedback.** The canvas shows a 2 px progress bar at its top and "Rendering
  3/7 passes" after 250 ms of a Full render; the previous image stays meanwhile; a newly opened
  document shows the checker and a spinner until its first image (never a partial one). Tiles and
  strips show spinners; font downloads, imports and bakes show progress in the status bar with Cancel.
  Every refused action says why in its tooltip ("Select a text layer", "Locked layer").
- **R72. Status bar and Stats.** Bottom of the canvas: canvas size, cursor position (canvas px,
  +y up), selection size, zoom, and the render state "Full · 12.4 ms · 1920 × 1080 · cached 5 of 7
  layers" (or "Draft"). **Stats** acquires GPU timing while it is on (released when off, so it costs
  nothing otherwise) and opens a small table: per pass and per layer GPU time from
  `getPassList(TITLE_DESIGNER_VIEW_ID, true)`, target memory, preview queue length and the frame's
  preview budget use.
- **R73. Japanese in the UI.** Layer names, text fields, search and Library names show Japanese
  through unicode-text's CJK editor fonts. Where English has no exact term, the label adds the
  Japanese typesetting term in parentheses (Horizontal Scale (長体/平体), Proportional (詰め),
  Vertical (縦書き), Tate-chu-yoko (縦中横), Erode (かすれ)), only when the editor font has those
  glyphs (`ImFont::IsGlyphInFont` on the first character), else English only.
- **R74. External changes.** When the file changes outside the window (an agent's `edit_title`, a
  VCS pull, another editor window), the window reloads it, keeps the selection (by layer id, then
  path), cancels a staged gesture with the notice "The title changed on disk: your drag was
  cancelled", and shows a chip "Edited by Claude Code: Apply Look Neon · Undo" for 8 s (Undo while
  that step is the newest).
- **R75. Undo granularity.** Every gesture is one named step (§6.4): a drag (gizmo, slider, scrub,
  handle, ramp stop) commits once on release with a merge key; a wheel burst (600 ms) or nudge burst
  (800 ms) is one step; a text edit session is one step; Esc during any gesture restores the values
  and adds nothing. Every side effect of a gesture is named, in its step ("Move Layer (Clipped)",
  "Change Font (Axes Reset)", "Edit Text (2 Letter Adjustments Removed)") or, for S84's structure
  (a Look's generator layers and auto group, S84 R7), in a toast naming what was added ("Apply Look
  Fire Gold added a Group and 2 Generator layers"); the applied layer stays selected (p7 §6: an
  unannounced group).
- **R76. Keyboard.** Everything except free-form drags is reachable by keyboard: Tab cycles toolbar
  → Layers → canvas → Properties → Library; each panel's keys are in §6.5; free-form drags have
  numeric fields. Ctrl+Z / Ctrl+Y stay the editor's (the timeline).

### Agent UX (MCP, CLI, Lua)

- **R77. Parity.** Every window action maps to S80's ops (§6.3) and runs through `TitleArtOps`, so
  `edit_title` with the same ops produces the same file bytes (AC34). Transient states map to
  `render_title` (`try` variants: what hover preview shows), view state to `focus_window`.
- **R78. `focus_window`** (core) gains the window `"title_designer"` and one object
  `title_designer` (`anyObject`, keys checked by the handler, listed in `jasper://docs/title-art`):
  `path` (open this document), `select` (a layer path, part `Title/rim#1`, effect `Title/fx:outer_glow`
  or `""` for the document), `panel` (`layers`, `properties`, `library`, `history`), `search`
  (types into the property search), `library` (`{tab, family, query}`), `preview_as`, `background`,
  `device` (`{profile, orientation, display_height}`), `zoom` (number or `"fit"`). The result adds
  `state`: `{path, selection, panel, zoom, preview_as, busy, staged, preview_active, last_render_ms,
  rect}`.
- **R79. `screenshot`** (core) gains the view `"title_designer"`: the editor framebuffer cropped to
  the window's rect (fronting its tab, as `focus`), so an agent sees exactly what a person sees; the
  title alone at full resolution stays `render_title`'s.
- **R80. `get_title`** (S80) adds `designer` when the document is open in the window: `{selection,
  staged, preview_active, view: {zoom, background, device}}`. No schema change.
- **R81. No new tools.** The `title_art` group gains nothing from S82; core grows by at most 120
  characters (R78, R79), measured by `tool_budget.py`. `find_ui_item` and `send_input` reach every
  control of the window by its label (the window uses stable ImGui labels with `###` ids).
- **R82. Guide.** `jasper://docs/title-art` gets a "Title Designer" section (≤ 1,200 bytes): the
  `focus_window` keys, the screenshot view, `get_title.designer`, and "to show a person a variant,
  use `render_title` `try`; to leave it on their screen, `focus_window` with `select`". The agent
  guide itself gets no line from S82.
- **R83. Lua and CLI.** No runtime API (an editor window). Editor scripts reach the window through
  `Editor.call("focus_window", {...})`; `jasperengine-editor call focus_window` and `call screenshot
  '{"view":"title_designer"}'` work as for every tool.

### Data & compatibility

- **R84. No document changes.** S82 adds no member to the `.titleart` format. Everything it keeps is
  view state: in the project workspace (`editor/Workspace.h`, a `TitleDesignerWorkspace` struct
  keyed by document GUID: recent documents, twirls, background, device, guides, zoom/pan, Library
  tab and filters, recents, Auto Bake) or in editor settings (`settings.yaml`: region sizes, section
  open states, favorites, preview budget, quality). Old projects load: a missing block takes its
  defaults.
- **R85. New data files.** `editor/resources/titleart/search-terms.json` (embedded, editor only):
  `{"version": 1, "groups": [[terms…]…], "names_ja": {"<effect or generator type>": "…"}}`. The
  thumbnail cache and snapshots live under the project's internal directory (never exported, never
  committed). The vendored decoders are editor only: players never link them.

## 6. Design constraints & decisions

- **D1. One window, internal regions.** Docking Layers, Properties and Library as separate editor
  windows would scatter one document over the dock and break the window-scoped shortcuts and the
  narrow layout; Unity's Shader Graph and VFX Graph keep their panels inside one window too. The
  regions resize and collapse (R2, R3).
- **D2. The canvas drives S80's renderer itself.** S81 R24 keeps the Designer's previews off the
  component scheduler, and the canvas needs what a component does not give: the staged working
  document every frame, a region and scale that follow the view, a slice budget per gesture and a
  Draft scale. It still shows the runtime's pixels, because Realtime runs the same renderer
  (contract decision 1); Preview As Glyph Atlas uses a real S81 instance, the one mode whose output
  differs, and Baked shows the shipped PNG. Thumbnails and tiles call the same renderer into pooled
  targets (a scene per tile would cost more than the tile).
- **D3. Working copy and staging.** The window holds `committed` (what the file says) and `working`
  (`committed` plus the staged gesture's ops). Widgets and handles apply ops to `working` every frame
  of a gesture; release commits the gesture's final op batch through `TitleArtDocument::commit` with
  a merge key; Esc copies `committed` back. Previews are a third layer, `preview` = `working` plus
  the hovered item's ops, shown when set and discarded on leave. The ImGui per-widget hook
  (`CommandHistory::beforeRecord`) is not used for document rows: they push no command until release,
  so the window handles Esc for its own staged gestures (the `App::handleInteractiveEdits` path only
  covers open interactive edits, report e2 §3.4).
- **D4. The search index is a collect pass.** The page functions take a `RowContext` whose mode is
  Draw or Collect; in Collect every `DocRow` records itself and returns without submitting ImGui
  items, sections report open, and `when` conditions run on the real document. Drawing a hit calls
  the same row with the same targets. This is the ImGui form of Pachislop's hidden probe render, and
  the only way a hand-drawn section can never be missing from search.
- **D5. Unity's keys, not Photoshop's.** The canvas tools are Q/W/E/R/T with Z for Pivot/Center, F
  frames, Ctrl+D duplicates, F2 renames, H hides, L locks, Delete deletes; no single letter creates a
  layer (Pachislop's `T` did); Alt is never a viewport modifier except "scale about the pivot" (Unity
  users' Alt means orbit, which the front-on canvas does not have) and slow scrub on fields.
- **D6. Generators are layers, parts and effects are child rows.** No folded wrappers, no render
  setting drawn as an effect (contract; p5 §7.2.2). The tree is longer for a title with ten sparkle
  generators; Groups and twirls keep it tidy, and the tree never lies about what a thing is.
- **D7. One preview queue with a frame budget.** Every offscreen render the window starts goes
  through `TitlePreviewQueue`, so a budget, the visible-first order and cancel-on-close hold for all
  of them at once (p7 §6: Pachislop's queue blocked the page for tens of seconds and outlived its
  dialog).
- **D8. Selection, view and previews are not undo steps.** As Pachislop and S24 Deviation 16: undoing
  should undo work, not looking. Selection follows ids across undo.
- **D9. The schematic 3D gizmo only here.** The canvas camera is fixed front-on, the case the
  schematic was designed for (`ecaa136`); the Scene view keeps true-axis gizmos.
- **D10. Every font is a project asset.** S80 references fonts by AssetRef; the picker copies system,
  Google and library fonts into the project before setting them, so a project builds the same on any
  machine. Previews read system files and the download cache directly through a transient asset
  resolver passed in S80's `RenderRequest::assets` (§6.6), and never commit them.
- **D11. History is a filtered view of the one timeline.** A per-document stack would be a fourth
  routed undo exception (report e2 §3.3); the filter, the interleave warning and Restore This Version
  give per-document control without one.
- **D12. Device loss closes the editor, as today.** In-process recovery would need every GPU resource
  of the editor and engine rebuilt (`EditorFrame.cpp:144-147` chose to exit). S82 guarantees no loss
  (every commit is on disk), restores its state on reopen, and rebuilds its own resources when they
  alone are invalidated (R64), the case that is testable on every backend.
- **D13. No new MCP tools.** The window is a view over S80's document: `edit_title` already does
  every edit, `render_title` every preview, `get_title` reads; what is left is view state, which the
  existing `focus_window` and `screenshot` carry (R78-R81).
- **D14. English UI, bilingual search.** No editor-wide translation exists; the window does not start
  one. Japanese reaches the user through search terms, Library names and the few typesetting terms
  in parentheses (R73).
- **D15. Guides are view state.** S80 keeps preview aids out of the document ("preview backdrops are
  view state"); guides follow: per user, per document, in the workspace. Clear Guides is the undo.
- **D16. The pivot is the gizmo's.** S80's transform rotates and scales about `position` and has no
  pivot member; a gizmo pivot (Unity's Pivot/Center) changes `position` with the rotation or scale it
  drives, so the document needs no new member.
- **D17. Layers list front first.** Every layer-based image tool lists the front layer at the top;
  S80 stores back first (draw order) and the panel reverses. The Effects list keeps S80's order (index
  0 first, applied first), labelled, as Unity lists components in execution order.
- **D18. New text starts plain; the Library is in view.** S80's plain default keeps layers honest
  (Pachislop `1c32502`); the default layout shows the Library strip with "Applies to" already naming
  the new text, so the fix for a flat word is in front of the user (p5 §7.2.1).
- **D19. Esc cancels a text edit.** Unity's text fields revert on Esc and the program's rule is that
  Esc reverts (contract decision 10); Ctrl+Enter or a click outside keeps the text. The status bar
  says so while editing (p7 found the opposite expectation confusing without a hint).

### 6.1 Files

| File | What |
|---|---|
| `editor/window/TitleDesignerWindow.{h,cpp}` | Shell, layout, toolbar, status bar, empty state, scripted API (§6.7), `friend struct TitleDesignerWindowTestAccess` |
| `editor/window/TitleDesignerLayers.cpp`, `TitleDesignerProperties.cpp`, `TitleDesignerCanvas.cpp`, `TitleDesignerLibrary.cpp`, `TitleDesignerHistory.cpp`, `TitleDesignerFonts.cpp`, `TitleDesignerAdd.cpp` | The panels (members of the window class, split by file) |
| `editor/titleart/designer/TitleDesignerState.{h,cpp}` | committed / working / preview documents, selection, staging, view state load/save |
| `editor/titleart/designer/TitleCanvasView.{h,cpp}`, `TransientAssetResolver.{h,cpp}` | Renderer jobs, private scene, `requestDraw`, `processFrame`, view id 5, backdrops, snapshots, resource rebuild; the resolver that serves not-yet-imported fonts and images to previews |
| `editor/titleart/designer/TitleLayerThumbnails.{h,cpp}` | The layer-thumbnail GPU LRU and its submissions to S80's `TitlePreviewQueue` (`editor/titleart/TitlePreviewQueue.{h,cpp}`, S80 §6.13: the shared budget arbiter of S80 R45) |
| `editor/titleart/designer/TitlePropertyIndex.{h,cpp}`, `TitleSearchFold.{h,cpp}`, `TitleRows.cpp`, `TitleInheritance.cpp` | Collect pass, ranking, synonyms, folding; row data; inheritance table |
| `editor/titleart/designer/TitleGizmo2D.{h,cpp}`, `TitleGizmo3D.{h,cpp}`, `TitleHandles.{h,cpp}`, `TitleSnap.{h,cpp}` | Pure hit-test and drag maths plus ImDrawList drawing |
| `editor/titleart/designer/TitleTextEdit.{h,cpp}` | Caret/selection over S80 cluster boxes, preedit insertion |
| `editor/titleart/fonts/FontCatalog.{h,cpp}`, `SystemFontList.{h,cpp}`, `SystemFontListMac.mm`, `WebFontDecode.{h,cpp}`, `FontPreviewRaster.{h,cpp}`, `FontLicence.{h,cpp}` | Fonts (§5 R54-R60) |
| `editor/widget/DocRow.{h,cpp}`, `RampField.{h,cpp}`, `ThumbnailGrid.{h,cpp}` | Reusable widgets (particles and S86 can adopt `DocRow`) |
| `editor/util/ImePreedit.{h,cpp}`; `editor/backend/BackendWin.cpp`, `BackendLinux.cpp`; `libs/imgui/backends/imgui_impl_osx.mm` (patch) | IME preedit |
| `editor/render/RenderUtil.h` | `TITLE_DESIGNER_VIEW_ID = 5` |
| `editor/App.{h,cpp}`, `AppMenuCommand.h`, `AppCommands.cpp`, `AppSettings.{h,cpp}`, `Workspace.h`, `shortcuts/CommandCatalog.{h,cpp}`, `shortcuts/DefaultCommands.cpp` | Wiring (report e2 §1.2's 15 places), the shortcut context |
| `editor/window/ProjectWindow.cpp` | Double-click (S80 adds the `FileType`; S82 its `openFile` target) |
| `editor/search/SearchScorer.cpp` | UTF-8 bytes kept by `normalize` (R27) |
| `editor/htmlui/GoogleFontsImporter.{h,cpp}` | fonts-only `Options` (R57) |
| `editor/mcp/tools/ViewTools.cpp` | `focus_window`, `screenshot` (R78, R79) |
| `libs/brotli/`, `libs/woff2/`, `CMakeLists.txt` | Vendored decoders, editor sources |

### 6.2 API sketch

```cpp
namespace jasperengine::editor {
    class TitleDesignerWindow {
    public:
        static constexpr const char* WINDOW_NAME = "Title Designer";
        explicit TitleDesignerWindow(Project* project);
        void show(); void setOpen(bool); bool isOpen() const; void resetForProjectSwitch();
        // the scripted API: every button, menu, shortcut, drop and MCP call goes through these
        bool open(const std::filesystem::path& file, std::string* error = nullptr);
        bool openInContext(Entity instance, uint32_t sceneId, std::string* error = nullptr);
        bool select(const std::vector<std::string>& paths, std::string* error = nullptr); // S80 paths
        bool runOps(const nlohmann::json& ops, const std::string& stepName, std::string* error); // commit now
        void setTool(titleart::designer::Tool); void setPreviewAs(TitleArtRenderMode);
        void setBackground(const titleart::designer::Backdrop&); void setDevice(const titleart::designer::DeviceView&);
        void setZoom(float zoom); void fit(); void frameSelection();
        void setSearch(const std::string& query);
        bool previewItem(const titleart::designer::PreviewItem&); void clearPreview(); bool commitPreview();
        nlohmann::json state() const;                       // focus_window / get_title.designer
    private:
        friend struct TitleDesignerWindowTestAccess;
    };
}
namespace jasperengine::editor::titleart::designer {
    enum class Tool : uint8_t { Hand, Move, Rotate, Scale, Rect };
    struct FieldRef { std::string layer; std::string path; };             // layer "" = the document
    struct PreviewItem { enum Kind { Look, ColorScheme, Template, Effect, Generator, Layer, Font, HistoryVersion } kind;
                         std::string id; nlohmann::json ops; uint64_t historyEntry = 0; };
    class TitleDesignerState {
    public:
        const TitleArtDoc& committed() const; const TitleArtDoc& shown() const;   // preview, else working
        void beginGesture(std::string stepName, std::string mergeKey);
        bool stage(const nlohmann::json& ops, std::string* error);                // applies to working
        bool endGesture(std::string* error);                                      // one commit
        void cancelGesture();                                                     // Esc
        bool setPreview(nlohmann::json ops);  void clearPreview();
        void onFileChanged();                                                     // R74
    };
    enum class RowMode : uint8_t { Draw, Collect };
    struct RowContext { RowMode mode; TitleDesignerState* state; std::span<const FieldRef> targets;
                        std::vector<std::string> sectionChain; PropertyIndex* sink; };
    struct SearchHit { std::string label, location; std::vector<FieldRef> targets; std::string node; int tier; };
    class PropertyIndex {
    public:
        void rebuild(const TitleDesignerState&, std::span<const std::string> scope);  // runs the page functions in Collect
        std::vector<SearchHit> query(std::string_view text, size_t limit) const;
    };
    std::string foldForSearch(std::string_view text);                            // R27
    class TitlePreviewQueue {   // S80's (editor/titleart/TitlePreviewQueue.h, S80 §6.13), shown for reference
    public:
        enum class Priority : uint8_t { Hover, Layers, Tiles, Thumbnails, Prefetch };
        using Key = std::array<uint8_t, 20>;                                      // SHA-1 of the content key
        uint64_t request(Priority, Key, PreviewJob job);   void cancel(uint64_t);  void cancelAll();
        void runFrame(double budgetMs);                                           // main thread, App::engineRender
        TextureRender* texture(uint64_t) const;           QueueStats stats() const;
    };
}
namespace jasperengine::editor::ui {
    struct DocRowOptions { const char* label; const char* tooltip; bool mixed = false; bool inherited = false;
                           bool showReset = true; float uiMin, uiMax, hardMin, hardMax; SliderMap map; const char* unit; };
    // the row reports the S27 phases; relative gestures report the delta for shift-each
    EditPhase docRowFloat(const char* id, std::span<float> values, float defaultValue, const DocRowOptions&, float* delta);
    EditPhase rampField(const char* id, TitleGradient& gradient, const RampFieldOptions&);
}
namespace jasperengine::editor {
    struct ImePreedit { std::string text; int caretByte = 0; bool active = false;
                        static const ImePreedit& current(); static void set(ImePreedit); static void clear();
                        static void setInlineWanted(bool);   // the canvas editor has focus
                        static bool inlineWanted(); };
}
```

### 6.3 Window action → S80 op (agent parity)

| Window action | Op batch (S80 §6.2) | Undo step name |
|---|---|---|
| Add Text / Shape / Image / Vector / Group / Generator / Adjustment | `add_layer {type, parent, after, values, generator?, adjustment?}` | "Add Layer Text" |
| Delete | `remove_layer` per layer | "Delete Layer", "Delete 3 Layers" |
| Duplicate | `duplicate_layer` | "Duplicate Layer" |
| Drag reorder, into or out of a group | `move_layer` (+ `set clip`) | "Move Layer", "Move Layer (Clipped)" |
| Group / Ungroup | `group` / `ungroup` | "Group Layers", "Ungroup" |
| Dimension 2D \| 3D switch, Convert to 3D/2D | `set_dimension {layer, dimension}` (S86 R50) | "Set Dimension 3D" |
| Rename; eye; lock; guide; blend; opacity; clipping mask | `set {layer, values: {name / visible / locked / guide / blend / opacity / clip}}` | "Rename Layer", "Hide Layer", ... |
| Any Properties row, gizmo, handle, ramp stop, axis slider, Letters gizmo | `set {layer, values}` (one per target layer) | "Change <Label>", "Change <Label> on 3 Layers", "Move Layer", "Rotate Layer", "Scale Layer" |
| Reset arrow | `set {path: default}` | "Reset <Label>" |
| Source dot / edit an inherited value | `set {use_global_light: false, <field>: value}` | "Override <Label>" / "Use Global Light" |
| Text edit commit | `set_text {layer, text}` | "Edit Text" |
| Font, face, Fallback Fonts, axes | `set {font, face_index, fallback_fonts, variations}` (after the file import step, R58) | "Change Font to <family>" |
| Add / remove / reorder / enable effects, drag to another layer | `add_effect`, `remove_effect`, `move_effect`, `set {effects[i].enabled}`; a cross-layer drag is `remove_effect` + `add_effect` in one batch | "Add Effect Outer Glow to Title", "Move Effect" |
| + Rim, rim menu | `add_rim`, `remove_rim`, `move_rim` | "Add Rim" |
| Masks | `add_mask`, `remove_mask`, `set` | "Add Mask" |
| Expose as Parameter… | `expose {name, type, targets}` | "Expose Parameter text" |
| Material link chip | `link_material` / `unlink_material` | "Link Materials" |
| Apply Look / Color Scheme / Template, Random Look, Paste Look | `apply_look`, `apply_color_scheme`, `apply_template` (S84) | "Apply Look Chrome Gold" |
| Paste layers (Ctrl+V) | `add_layer {from: <layer JSON>}` (S80 alignment item, §8) | "Paste 2 Layers" |
| Style Fix / Fix All | the check's fix ops | "Style Fix: Contrast" |
| Document page rows | `set {values: {canvas.width, global_light.angle, seed, outputs[0].scale, ...}}` | "Change Canvas Width" |
| Restore This Version | the whole-document write S80's command offers (`TitleArtDocument::commitText`) | "Restore Title Version '<step>'" |
| Hover / arrow previews, history hover | the same ops on a copy, never committed = `render_title {try}` | none |
| History jump, chip Undo | `undo` / `redo` / `jumpTo` (`undo {expect_top_id}` for agents) | (the steps' own) |
| Selection, zoom, pan, background, device, panel, search, Library filters | view state = `focus_window {title_designer}` | none |

### 6.4 Undo names and merge keys

Names come from the field path through `TitleRows` labels ("style.rims[1].bevel.depth" → "Rim 2
Bevel Depth"), counted for multi-target edits, translated never (English). Merge keys:
`titleart:<guid>:<gesture id>` for drags (one step per gesture even when the drag crosses several
frames' commits, which it does not: it commits once), `…:wheel:<path>` within 600 ms,
`…:nudge:<selection hash>` within 800 ms. Each step's origin is Person (an agent's are S24's
`UndoOrigin{Agent}` through `edit_title`).

### 6.5 Shortcuts (new context `ShortcutContext::TitleDesigner`, key `title_designer`)

Appended to the enum (`CommandCatalog.h:36`), named "Title Designer" / `title_designer`, with the
parser's loop bound moved to it (`CommandCatalog.cpp:60`), and pushed in `AppCommands.cpp` while the
window has focus and no text field is active. Commands shadow the broad ones with the same chord while
the window has focus (as Pose Animation's do), all rebindable and listed in the Shortcuts window.

| Id | Name | Default |
|---|---|---|
| `titleDesigner.toolHand` / `toolMove` / `toolRotate` / `toolScale` / `toolRect` | Hand / Move / Rotate / Scale / Rect Tool | Q / W / E / R / T |
| `titleDesigner.togglePivot` | Toggle Pivot/Center | Z |
| `titleDesigner.frameSelected` / `titleDesigner.fitCanvas` / `titleDesigner.zoomActual` | Frame Selected / Fit Canvas / Actual Size | F / Home, Alt+F / Shift+0 |
| `titleDesigner.zoomIn` / `titleDesigner.zoomOut` | Zoom In / Out | Ctrl+= / Ctrl+- |
| `titleDesigner.editText` | Edit Text | Enter |
| `titleDesigner.find` | Find Setting | Ctrl+F |
| `titleDesigner.add` | Add… | Ctrl+Shift+N |
| `titleDesigner.duplicate` / `titleDesigner.delete` / `titleDesigner.rename` / `titleDesigner.selectAll` | Duplicate / Delete / Rename / Select All Layers | Ctrl+D / Delete, Backspace / F2 / Ctrl+A |
| `titleDesigner.copy` / `titleDesigner.cut` / `titleDesigner.paste` | Copy / Cut / Paste Layers | Ctrl+C / Ctrl+X / Ctrl+V |
| `titleDesigner.group` / `titleDesigner.ungroup` / `titleDesigner.clip` | Group / Ungroup / Clipping Mask | Ctrl+G / Ctrl+Shift+G / Ctrl+Alt+G |
| `titleDesigner.forward` / `titleDesigner.backward` / `titleDesigner.toFront` / `titleDesigner.toBack` | Bring Forward / Send Backward / Bring to Front / Send to Back | Ctrl+] / Ctrl+[ / Ctrl+Shift+] / Ctrl+Shift+[ |
| `titleDesigner.toggleVisible` / `titleDesigner.toggleLock` | Hide / Lock Layers | H / L |
| `titleDesigner.nudgeLeft/Right/Up/Down` (+ Shift: ×10) | Nudge | Arrows (canvas focus) |
| `titleDesigner.up` | Select Parent | Esc |
| `titleDesigner.bake` | Bake Now | Ctrl+B |

### 6.6 What S82 needs from S80, S81, S84 and S86 (alignment)

Every item below is now in its owner's text under the name used here (S80's in its §6.15 hook
table).

- **S80 renderer** (§6.3): `TitleArtRenderer::begin(RenderRequest)` and `TitleRenderJob::step(StepBudget)`
  cover the canvas, layer thumbnails (`isolateLayer`), Draft (`pxScale`) and visible-window renders
  (`region`); progress (`passesDone`, `passesTotal`, `progress()`) and per-node cost (`nodeCosts()`)
  feed R71/R72. The **resolver** (`RenderRequest::assets`) is how previews inject transient fonts:
  S82's `TransientAssetResolver` wraps the editor's resolver and maps a font or image that is not
  yet a project asset (a system file, the Google download cache, S84's pack) to its file for that
  request only. S85's playhead reaches the canvas as `RenderRequest::overrides` (a sampled pose).
  Device loss is S80 R19 (`SystemRender::getContextId()`); S82's own targets follow the same
  signal (R64).
- **S80 layout and geometry** (§6.6 "Layout and geometry queries"): `layout()`'s per-cluster boxes
  in document space (caret, selection, the per-glyph gizmo), `contentBounds` and `reachBounds`
  (gizmo box, tile frame), `outlines` and `rimBands`, which S82's part picking tests the pointer
  against on the CPU (inside the outline: Face; within rim k's cumulative band: Rim k+1).
  Generators and adjustments are picked from an `isolateLayer` render at ¼ scale (alpha ≥ 0.05).
- **S80 schema and registry** (R9a, §6.5): `TitleArtSchema` (type, hard range, default, enum
  values, unit, label, `FxAnim`) queried by path; `ParamDef` (`label`, `kind`, `unit`,
  `min`/`max`/`step`, `defaultValue`, `options`, `when`, `description`, `anim`) and
  `EffectDef::gizmos` (`GizmoHint`), `costClass`, `aliases`. S82's `TitleRows.cpp` adds only UI
  data.
- **S80 document** (§6.2, §6.8): `TitleArtDocument::commit` with a step name and a merge key;
  `commitText` for Restore This Version; `stepText` for History hover; `changed` notifications
  with their origin for R74; `add_layer {from}` for paste and cross-document drags.
- **S80 preview queue** (§6.13): `TitlePreviewQueue`, which S82 submits to (R67) as S80's, S84's,
  S86's and S87's previews do.
- **S81** (same wave): R6's create menus and drops (S82 only adds the document-chip drag); the
  instance's screen rectangle for In Context (R30); a Glyph Atlas instance on an in-memory
  document for Preview As; R62's Characters UI, hosted as the Document page's Runtime section
  through §6.8.
- **S84** (wave 6C, Library panel only): `TitleLibraryApply::preview(doc, op)` for previews and the
  ops it commits (S84 R11), so R44 holds; `TitleLibraryPreviews` (S84 R33) submitting through
  `TitlePreviewQueue`; families (R23); favorites and recents (R18); save dialogs' validators (R42);
  each template's main placeholder (`placeholders[].role: "title"`, S84 §6.3) as the layer R5
  selects; the six featured templates of the empty state (S84 R42: `manifest.json` `featured`).
- **S83** (same wave): the Add palette's categories and entries (S83 §6.10, including "style
  setting" entries such as Dry Brush, which switch on the style member and select its section:
  they never become effect rows, D6), cost badges on rows and the Effects header (S83 R41),
  preset chips and seed dice on cards (S83 R39), the "Follows a deleted layer" row warning (S83
  R43).
- **S86** (same wave): `Gizmo3DTarget` (S86 R56), its rows and sections through §6.8 (S86 R51),
  `set_dimension` for the Dimension switch (S86 R59), its Realtime tiers in Preview As, its picker
  thumbnails through `TitlePreviewQueue` (S86 R54).
### 6.7 Scripted API and test access

The window's public methods (§6.2) are what its UI calls; `TitleDesignerWindowTestAccess` (declared
friend, defined in `editor/tests/TitleDesignerWindowTest.cpp`) adds: `drawFrame` (one headless ImGui
frame of `show()`), `state`, `canvasImage` (read back view 5's target as RGBA8 sRGB), `hoverTile(tab,
id)` with a simulated pointer movement, `arrowLibrary(n)`, `pressKey(chord)`, `dragGizmo(handle,
from, to, steps)`, `dragHandle(effectPath, kind, to)`, `dragRow(row, dropZone)`, `scrubRow(path, dx,
modifiers)`, `typeText(utf8)`, `compose(preedit, caret)` / `commitComposition(utf8)` (drives
`ImePreedit` as a backend would), `search(query)` (the hit list), `editHit(i, value)`, `jumpToHit(i)`,
`previewQueue()` (stats, a fake clock and a fake cost model for budget tests), `invalidateGpu()`,
`historyRows()`, `lintFindings()`, `fontCatalog()` (with an injected system list and fake HTTP).

### 6.8 Extension hooks for later specs

Registered at startup by the spec's editor code; a spec compiled out registers nothing, so its UI
does not exist and costs nothing.

| Hook | Used by |
|---|---|
| `addPanel(PanelDef{id, title, region: bottom \| right, draw, visibleByDefault})` | S85's Animation strip (bottom, open once the document has animations) |
| `addSection(SectionDef{page: document \| layer \| part, id, title, after, draw, applies(selection)})` | S81's Runtime (Characters), S86's Dimension subsections, 3D Scene and part material rows, S87's Languages. Sections draw `DocRow`s, so the property search indexes them (D4) |
| `addToolbarItem(ToolbarItemDef{id, draw, overflowOnly})` | S87's language switcher, S85's Record indicator |
| `setEditRouter(fn(ops) -> ops)` | S85's Record mode (R45 there): `set` ops of animatable fields become key ops before staging and commit, so every surface (rows, gizmo, handles, search results) records the same way |
| `setTimeSource(fn() -> double)` | S85's playhead: the canvas's `RenderRequest` carries the time; changing it re-renders |
| `addPreviewMode(PreviewModeDef)` | S85's Exact, S86's tiers in Preview As |
| `addCheckSource(fn(selection) -> findings)` | S85's animation checks in the lint popover |
### 6.9 MCP budget

| Change | Characters (estimate, measured by `tool_budget.py`) |
|---|---|
| `focus_window`: `"title_designer"` in the enum, `{"title_designer", anyObject("")}`, description "title_designer: {path, select, panel, …} (jasper://docs/title-art)" | ≤ 95 |
| `screenshot`: `"title_designer"` in `view`'s enum, description word | ≤ 25 |
| `title_art` group | +0 (the `designer` block of `get_title` is a result field) |

These are S82's row of S80 §6.11's program budget table. The core listing grows by at most 120
characters with no new tool, and the measured number goes in the evidence. If the default listing lacks that room, the description words go first (the keys are
already listed in `jasper://docs/title-art`), leaving about 60 characters (the enum values and an
`anyObject("")`); if even those do not fit, room is made by moving existing `focus_window`
description detail into `jasper://docs/editor-integration`, the conventions' rule (detail goes to
the docs, never into a raised limit). The choice is recorded in Deviations.

## 7. Acceptance criteria

All window tests run headless in a child process with the editor's App (the
`SpriteNormalsWindowTest` pattern); MCP tests use an isolated editor on a scratch copy of
`build/fixture-unity` with S80's title fixtures.

| AC | Verification |
|---|---|
| AC1 (R1, R65) | `TitleDesignerWindowTest.OpensFromEveryEntry`: Window menu, `ProjectWindow::openFile`, Inspector buttons, Assets > Create (after rename) and `focus_window` each open the document. `TitleDesignerWindowTest.ClosedCostsNothing`: closed → no `TitleCanvasView`, queue empty, 0 draw calls on view 5 over 120 frames, no font listing job; closing an open window empties the queue within 1 frame. Red control: constructing the canvas view in the window's constructor fails it |
| AC2 (R2, R3, R84) | `TitleDesignerWindowTest.LayoutPersists`: region sizes and collapses survive a restart (settings.yaml); 900 px wide gives the tabbed side column; 400 px gives canvas only. View state per document round-trips through the workspace; the `.titleart` bytes never change from view actions (hash compared after 30 view actions) |
| AC3 (R5) | `TitleDesignerWindowTest.EmptyState`: empty document shows the card with six template cards; hiding every content layer shows it again; applying a template selects its main text layer and the Library header says `Applies to "<text>"`. Screenshot `title-designer-empty.png` (AC33) |
| AC4 (R10, R14) | `TitleDesignerLayersTest.DropZones`: above/below halves, the middle 40 % into a group, left-of-indent steps out of 1 and 2 groups, below the last row to the bottom; group into itself refused with no step; each drop is one step with the §6.3 name; clipped-row drop sets `clip` in the same step; rows list front first (S80 index order reversed) |
| AC5 (R22) | `DocRowTest.ShiftEachVersusSetAll`: rims of 2, 3, 5 scrubbed +2 → 4, 5, 7 in one step "Change Rim Width on 3 Rims"; typing 6 → 6, 6, 6; mixed shows `—`. Red control: applying the scrub as set-all gives 4, 4, 4 and fails |
| AC6 (R10, R12, R13, R34) | `TitleDesignerLayersTest.FlagsAndSelection`: eye, lock, guide, blend, opacity each one step; a locked layer is neither picked nor given a gizmo; Esc walks effect → part → layer → group → document; selection changes add no undo step |
| AC7 (R20, R21, R23) | `TitlePropertyIndexTest.EveryNumericRowResets`: the collect pass over every page of S80's fixture `all_members.titleart` finds every numeric row with a schema default, sets it to a non-default, and checks a reset arrow is drawn and resets in one step; clamping 99999 px font size gives the schema maximum and the tooltip text. Red control: a row whose default is dropped from the schema lookup is reported by name |
| AC8 (R23) | `TitleDesignerPropertiesTest.SourceDots`: bevel light angle inherited (hollow dot); scrubbing it gives one step "Override Bevel Light Angle" with `use_global_light` false and the new angle; clicking the dot restores the global angle in one step |
| AC9 (R26, R85) | `TitlePropertyIndexTest.IndexFromViews`: on a 2D text layer, no hit has a 3D-only path (S86's rows registered by the test); a row hidden by `when` (Bevel size with `size_mode: fit`) is absent until its condition holds; `フチ` finds Rim widths, `ぼかし` and `blur` find the same rows, `光彩` finds Outer Glow (+ Add when absent), `Ring` finds Gloss. `SearchTermsTest.EveryEffectHasJapaneseName`: every registry type has a `names_ja` entry. Red control: an index built from the schema tables instead of the collect pass lists the 3D rows and fails |
| AC10 (R26) | `TitleDesignerPropertiesTest.LiveResults`: typing `glow` shows live rows; scrubbing a hit's slider is one step with the same name as on its page; the list keeps its order while editing; Enter on a link hit selects the node, opens the section, scrolls it in view and flashes it; the search field is empty afterwards |
| AC11 (R27) | `SearchScorerTest.KeepsUtf8`: `normalize("激熱 Glow")` = `"激熱 glow"`; Hierarchy filter `激` finds a GameObject named 激熱 (`HierarchyFilterTest`). `TitleSearchFoldTest`: full-width, half-width katakana with voiced marks, katakana → hiragana, ideographic space. Red control: the current `normalize` returns `"glow"` and the filter finds nothing |
| AC12 (R28, R29, R66) | `TitleCanvasViewTest.DrawsOnlyOnChange`: 0 draws on view 5 over 120 idle frames after settling; one change draws within `SETTLE_FRAMES`; zoom limits 1/16×, 32×; F frames the selection's box; Home fits; after a 150 ms settle at 400 % the rendered region equals the visible window at display resolution (target size checked) |
| AC13 (R32, R34) | `TitleGizmo2DTest`: hit tests of 8 handles at 24 px, rotate zone outside corners; corner scale keeps the opposite corner fixed (position compensated, 1e-4 px), Alt about the pivot; edge handles write `h_scale` on text, `aspect`/`size` on shapes; Shift rotation steps 15°; pivot drag changes no document value; snapping within 8 screen px to edges, centres, guides; Ctrl increments 10 px / 15° / 0.1. `TitleDesignerCanvasTest.GizmoDragOneStep`: a 30-frame drag is one step "Rotate Layer"; Esc mid-drag restores every value and adds no step |
| AC14 (R35) | `TitleGizmo3DTest`: the ported cases of `tests/unit/rotation3d.test.ts` (nearest Euler branch, full turns, no flips); the drawn arm endpoints relative to the centre are identical for 50 random positions, zooms and object rotations; at the crossing of an arc and a shaft both are hit by their own zones; the Z arrow moves at the X arrow's rate per px |
| AC15 (R33) | `TitleHandlesTest`: each handle kind maps drags to its fields; a drop shadow direction with `use_global_light` edits `global_light.angle` and the label says "Global Light"; a gradient line edits `angle`, `scale`, `offset`; handles show only while their node is selected. Red control: with the global-light branch removed the layer's own angle changes and the test fails |
| AC16 (R30, R31) | `TitleDesignerCanvasTest.BackgroundsAndDevices`: Snapshot renders the scene's main camera once (one render, none on later frames) and is cached on disk; In Context places the title at the S81 instance rectangle; an iPhone profile draws the overlay and hatches content outside the safe rectangle; 1920 × 1080 draws 90 % and 93 % rectangles; the checks receive the Display Height |
| AC17 (R36, R37, R39) | `TitleDesignerTextTest`: `typeText("激熱")` updates the canvas before commit; Esc restores the old text and adds no step; Ctrl+Enter adds one step "Edit Text"; `compose("げきあつ", 4)` shows the preedit inline (canvas readback differs from the committed render, underline present) and `commitComposition("激熱")` inserts it; Enter during composition does not commit the edit; empty text shows the dashed box and the warning. Manual check on Windows and macOS (recorded in the evidence with screenshots): type げきあつ with the system Japanese IME on the canvas, see it inline and underlined, convert, commit; on Linux with ibus or fcitx (Mozc), the candidate window opens at the caret |
| AC18 (R38) | `TitleDesignerTextTest.Letters`: selecting characters 2-3 shows Selected Characters; the per-glyph gizmo rotation writes `chars.1.rotation` and `chars.2.rotation` in one step |
| AC19 (R41-R44, R49) | `TitleDesignerLibraryTest`: a Look tile's render contains the selected layer's text (the tile equals S80's render of `TitleLibraryApply::preview(doc, op)`, pixel-exact); a zero-movement enter and a scroll under a still pointer never preview; 150 ms after movement the canvas shows the preview; arrows preview at once; Esc and leaving restore the working document and add no step; Enter commits one step whose name is S84's ("Apply Look <name>"); **the committed render equals the previewed render bit for bit**; a double-click adds one step. Red control: a preview built by a second code path (a test-only shim that drops `keep_color_scheme`) fails the equality |
| AC20 (R40, R45-R48) | `TitleDesignerLibraryTest.ScopeKeysFirstPaint`: Whole Title recolours every layer (`apply_color_scheme` without `layer`), Selected Layers only those; moving a layer sends no new tile request, editing the text sends them once its key has been stable 300 ms; the script-matched baked thumbnail shows at 50 % with a spinner until the live tile arrives; favorites and recents read and write S84's settings keys; Apply Font starts off and Keep Color Scheme on |
| AC21 (R67) | `TitlePreviewQueueTest` (fake clock and costs): no item starts after 4 ms of a frame; an item predicted at 10 ms runs alone every 3rd frame; hover beats layers beats visible tiles beats prefetch; scrolled-out items are cancelled; `cancelAll` empties it; S84's, S80's and S86's submitters share the one budget (three fake submitters never exceed it together). `TitleDesignerLibraryTest.NeverBlocksFrames`: with 200 Look tiles and 40 layer thumbnails queued on server A (llvmpipe), no editor frame's preview work exceeds 4 ms plus the longest single pass (`get_frame_times`, p95 over 200 frames, as S80 AC30). Red control: budget disabled (`runFrame(inf)`) fails both |
| AC22 (R50-R52) | `TitleDesignerHistoryTest`: rows are only this document's steps with their names; a jump with no interleaved step undoes exactly those; with a scene step between, the popover lists it, Jump Anyway undoes it too, Restore This Version leaves it and adds one step; hovering a row previews that version with no step |
| AC23 (R53) | `TitleDesignerLintTest`: S80's fixture with a low-contrast rim pair shows `⚠ 1`; Fix adds one step "Style Fix: Contrast" and the badge shows `✓`; the popover shows the rule, citation, measured value and threshold; no wording contains "good", "approved" or "looks" (string scan of the badge and popover texts) |
| AC24 (R54, R55, R56, R60) | `TitleDesignerFontsTest`: picker sections and `.ttc` faces listed per face; each row's strip is the layer's first 24 code points shaped with that font (compared with S80's shaper output); Covers My Text hides fonts missing 激; the missing badge counts 2 for a Latin-only font on 激熱; Add Fallback Font offers fonts covering both first; a variable font shows its axes and named instances, sliders write `variations`, changing to a static font drops them in a step named "… (Axes Reset)" |
| AC25 (R58) | `SystemFontListTest.ParsesFcList` (a captured `fc-list` output: files, faces, styles, CFF/TrueType kept, bitmap dropped); `FontLicenceTest.FsType`: Restricted (0x0002) asks and copies only after Copy Anyway, Installable (0), Preview & Print (0x0004) and Editable (0x0008) copy with the note; the copy lands in `<assets>/TitleArt/Fonts/System/` as one step. The CoreText and DirectWrite listers compile and list ≥ 1 font in the macOS and Windows CI unit runs |
| AC26 (R59) | `WebFontDecodeTest`: an OFL fixture font encoded as WOFF and WOFF2 (committed fixtures) decodes to sfnt whose every table equals the reference TTF's (WOFF2: glyf/loca reconstructed, compared by outline through HarfBuzz hb-draw); truncated, bad-checksum and over-64 MB inputs are refused with reasons. Red control: skipping the glyf transform fails the outline comparison |
| AC27 (R57) | `GoogleFontsImporterTest.FontsOnlyMode` (the importer's existing fake HTTP server): fonts-only mode writes the TTFs and the licence under the `dir` it is given (`TitleArt/Fonts/Google/<slug>/` from the picker), no CSS and no manifest entry; the default mode is unchanged (its existing tests pass) |
| AC28 (R61, R62) | `test_title_designer_window.py::test_document_chip_drag`: `send_input` drags the window's document chip into the Hierarchy: S81's drop creates one GameObject with a Title Art Renderer referencing the shown file, one step "Create Title Art (logo)" (S81 owns the Project-window drops and their tests); double-click on the `.titleart` in the Project window opens the window (`focus_window` state names it); the Inspector buttons of S80 and S81 open it |
| AC29 (R75, §6.4) | `TitleDesignerUndoTest.OneStepPerGesture` (parameterized over every gesture of §6.3: gizmo drag, row scrub, wheel burst, nudge burst, handle drag, ramp stop drag, text session, Library apply, Add palette, drop): exactly one project-history step with the expected name; Esc mid-gesture: zero steps and the document hash unchanged. Red control: committing per frame (a test flag) produces N steps and fails |
| AC30 (R74) | `test_title_designer_window.py::test_agent_edit_while_open`: an `edit_title` while a person's drag is staged cancels the drag, reloads, keeps the selection by id, shows the chip; the chip's Undo removes the agent's step only (`get_undo_history`) |
| AC31 (R68, R69, R70) | `tools/perf/title_designer_bench` on server A, reference documents `bench_2d_rims`, `bench_40_layers`: Draft ≤ 1/4 of Full render time; ImGui work ≤ 1.5 ms per frame; search index ≤ 3 ms, query ≤ 1 ms; 256-layer tree costs within 10 % of the 40-layer one. Results appended to Deviations when a budget is missed. `TitleDesignerWindowTest.SoftwareRendererNotice`: a device name "llvmpipe" sets Draft and 8 ms and shows the notice once |
| AC32 (R64, R71) | `TitleCanvasViewTest.RebuildsAfterInvalidation`: a new context id (the test hook S80 R19's tests use) and S81's `TITLE_ART_TEST_FAIL_TARGETS` mid-render → the overlay shows, resources are recreated, the next settled image equals the one before (S80's float/RGBA8 tolerance); no frame in between presents an all-black or partial image (frame readbacks checked); visible Library tiles are requested again. Red control: skipping the rebuild leaves a blank image and fails |
| AC33 (visible changes) | `tools/editor-shot.sh` on a scratch copy of `build/fixture-unity` with S80's fixtures, an isolated editor: `title-designer-empty`, `-layers-parts-effects`, `-search-glow`, `-library-hover`, `-gizmo-rect`, `-gizmo-3d` (S86 fixture, or skipped and listed as skipped until S86), `-device-iphone`, `-font-picker`, `-lint-popover`, `-narrow`. Kept outside the repository, linked in the evidence |
| AC34 (R77, §6.3) | `test_title_designer_window.py::test_parity`: for each row of §6.3, the window action through the scripted API and `edit_title` with the listed ops on a fresh copy produce identical `.titleart` bytes and step names |
| AC35 (R78-R81) | `test_title_designer_window.py`: `focus_window {window: "title_designer", title_designer: {path, select, panel, search, zoom}}` opens, selects, searches and returns `state` with `rect`; `screenshot {view: "title_designer"}` returns an image of the rect's size (scaled to `max_width`); `get_title` has `designer` while open and not after closing; unknown `title_designer` keys fail listing the valid ones. `test_tool_budget.py` (core growth ≤ 120 measured), `test_tool_conventions.py`, `test_call_cli.py`, `gen_tool_docs.py --check`; `title_art` group tool count unchanged |
| AC36 (§6.5, R76) | `ShortcutCatalogTest.TitleDesignerContext`: `parseShortcutContext("title_designer")` works (red control: the old loop bound `<= PoseAnimation` fails it); Q/W/E/R/T with the window focused change its tool and leave the Scene view's tool unchanged; Delete while a text field is active edits the field, not the layers; every §6.5 command is in the catalog and the Shortcuts window |
| AC37 (R57, R6) | `TitleDesignerFontsTest.Offline`: no network → the Google section shows the message, every other section works; `TitleDesignerWindowTest.ReadOnly`: a `version_newer` fixture opens read-only with the banner, every edit is refused, the canvas renders |
| AC38 (R17) | `TitleDesignerAddTest`: the palette lists S83 §6.10's categories with match counts; `集中線` and `focus lines` find Focus Lines; arrowing previews on the canvas, Enter adds one step "Add Effect Focus Lines to Title", Esc restores with no step; a generator entry adds a Generator layer row (never an effect row); the style-setting entry Dry Brush sets `style.erode.enabled` and selects its section, adding no effect row; with parts selected the header offers Selected parts / Whole layer |
| AC39 (R25) | `RampFieldTest`: a click on the bar adds a stop with the colour sampled there; a 40 px downward drag removes it; double-clicking a midpoint resets it to 0.5; Reverse maps positions p → 1 − p and mirrors midpoints; Vivid sets `interp: "vivid"`; the 33rd stop is refused with "At most 32 stops"; mixed targets show the striped bar and an edit sets all; each gesture is one step |
| AC40 (R7, R19, R72, R73, R82) | `TitleDesignerPropertiesTest.SectionSummaries`: closed headers carry the summaries of R19 and fold state is shared per node kind; `TitleDesignerWindowTest.PlayMode`: a commit during Play writes the file and shows the banner; `TitleDesignerWindowTest.StatsCostsNothingOff`: `SystemRender::getGpuTimingState()` is held only while Stats is on; R73's parentheses appear only when the injected editor font has the glyphs; `test_docs.py` and `test_autosync.py` pass with the guide section (≤ 1,200 bytes, every tool and key it names exists). The window's unit and MCP tests run on Linux CI under GL and again with `GRAPHIC_BACKEND=vulkan` (R63) |

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

- **Depends on:** S80 (document, ops, `TitleArtDocument` with `commitText` and `stepText`,
  renderer and job progress, schema, registry, layout and geometry queries, checks, bake,
  `get_title`, `TitlePreviewQueue`: all in S80 §6.15's hook table), S81 (same wave: component,
  creation functions, Glyph Atlas instance), S83 (same wave: menus, badges, presets), S86 (same
  wave: `Gizmo3DTarget`, rows), S84 (wave 6C: the Library panel's data; S82's M6 lands with it),
  S24 (timeline, `jumpTo`, origins), S25 (command catalog), S26 (drop surface, root-then-drill
  rule), S27 (field widgets), S04 (AssetRefs, `.ttc` as Font from S80), S06 (Background lane), S15
  (pass lists, GPU timing), unicode-text (CJK editor fonts, `IMGUI_USE_WCHAR32`,
  `SystemFonts::isLoadable`).
- **unicode-text fallback.** If it has not landed when S82 is implemented: S82 does not add editor
  fonts or `IMGUI_USE_WCHAR32` itself. Japanese then draws as boxes in ImGui labels and fields; the
  text row's rendered strip (R24) and the canvas show the real text; R73's parentheses are off by
  their glyph test; tests compare strings and canvas readbacks, never ImGui glyph pixels, so they
  pass either way; the AC33 screenshots with Japanese are retaken after it lands. Without
  `SystemFonts::isLoadable`, `SystemFontList` checks candidates with HarfBuzz (`hb_face_count` > 0 and
  a non-zero glyph count of the face), already linked through S80, and switches to the shared check
  when it lands.
- **Blocks:** S85 (its panel through §6.8), S86 (its rows and the 3D gizmo target, same wave), S87
  (toolbar hook). None is blocked from starting: they use the interfaces of §6.2 and §6.8.
- **Alignment items** (resolved): `add_layer {from}`, job progress and per-node cost, the layout and
  geometry queries, `commitText`/`stepText` and the preview queue are S80's (S80 §6.15); S84 R42
  names the six featured templates; S84, S86 and S87 submit through `TitlePreviewQueue`.
- **Merge hotspots:** `editor/App.cpp` (construction, menu, dock, `engineRender`, shortcut panels),
  `ProjectWindow.cpp` (`openFile`, also S80's), `editor/mcp/tools/ViewTools.cpp` (`focus_window`, `screenshot`; S24, S26 and HTML UI
  specs touch it), `editor/search/SearchScorer.cpp` (S25), `editor/backend/BackendWin.cpp`,
  `BackendLinux.cpp` (unicode-text edits the same `WM_CHAR` and XIM code), `libs/imgui/backends/
  imgui_impl_osx.mm` (an ImGui upgrade must re-apply the patch; the patch is listed in
  `libs/imgui/README` or the libs notes), `editor/shortcuts/CommandCatalog.*` (any spec adding a
  context), `CMakeLists.txt` (editor sources, vendored libs), `NOTICE.md`.

## 9. Risks & defaults

- **IME behaviour differs per input method** (Mozc, ibus-anthy, fcitx5, MS-IME, Kotoeri). Default:
  inline preedit where the platform gives the string (Windows, macOS), at-caret popup on Linux; the
  manual checks of AC17 are part of the evidence on each OS. If a Linux input method rejects
  over-the-spot, the code falls back to `XIMPreeditNothing` (today's behaviour) without an error.
- **Patching vendored `imgui_impl_osx.mm`.** Default: a minimal patch marked `// JasperEngine:` and
  noted beside the ImGui version; an ImGui upgrade re-applies it (the hotspot list says so). If
  upstream ImGui gains marked-text support, the patch is dropped.
- **Draft/Full ratio and the slice budgets depend on S80's renderer.** Default: the budgets of R68
  are targets measured by AC31; a miss is recorded in Deviations with the measured numbers, never
  hidden by raising the threshold.
- **Snapshots of heavy scenes take long on llvmpipe.** Default: Snapshot is explicit (never
  automatic) and shows progress; it runs as the stopped Game view renders (one frame), cancellable.
- **System font licences cannot be fully known from the file.** Default: `fsType` decides the hard
  refusal; every copied system font carries a warning in the picker and S81's export credits list it
  as "system font, licence unchecked".
- **Google Fonts may change its CSS answers** (the importer's User-Agent trick). Default: the
  importer's existing tests and error codes apply (`HTMLUI-IMP-0002`, `-0006`); the picker shows the
  importer's message.
- **The search index of a large selection** (all layers of a 256-layer document with "Search every
  layer"). Default: built incrementally over frames within 3 ms per frame, results appearing as
  layers are indexed, with "Indexing 120/256".
- **Japanese typesetting terms in English labels** may look odd to non-Japanese users. Default: only
  five rows carry them, in parentheses, and only with the CJK font present.
- **History Restore This Version needs the step's text.** Default: S80's command keeps whole before
  and after text (the `WriteFilesCmd` contract); if S80 stores diffs instead, Restore reconstructs by
  replaying, and History hover previews are limited to the last 50 steps.

## Milestones

- **M1** Window shell, wiring, layout, state and staging, canvas view with pan/zoom/backgrounds,
  `DocRow`, Properties pages from the schema, reset arrows, undo naming (R1-R9, R18-R23, R28-R30,
  R65, R66, R75, R84); AC1, AC2, AC5, AC7, AC8, AC12, AC29.
- **M2** Layers panel, Add palette, drag and drop, thumbnails through the preview queue, shortcuts
  context (R10-R17, R67, §6.5); AC4, AC6, AC21, AC36, AC38.
- **M3** 2D gizmo, handles, snapping, guides, devices and safe areas, `RampField` (R25, R31-R34);
  AC13, AC15, AC16, AC39.
- **M4** Search: collect pass, folding, `normalize` change, synonyms, live results (R26, R27, R85);
  AC9, AC10, AC11.
- **M5** Text editing, IME on three backends, Letters (R24, R36-R39); AC17, AC18.
- **M6** Library, History, lint (R40-R53); AC19, AC20, AC22, AC23.
- **M7** Fonts: picker, strips, Google fonts-only mode, system lists, WOFF/WOFF2, axes (R54-R60);
  AC24-AC27, AC37.
- **M8** Scene drops, external changes, resource rebuild, MCP (focus_window, screenshot,
  get_title.designer), guide section, 3D gizmo (target ready for S86), performance, screenshots (R35,
  R61-R64, R68-R83); AC3, AC14, AC28, AC30-AC35, AC40.

## Deviations

1. **Reconciled: `TitlePreviewQueue` is S80's** (`editor/titleart/TitlePreviewQueue.{h,cpp}`, S80
   §6.13), because S80 lands in wave 6 and its thumbnails must share the budget from the start; the
   API and priorities are this spec's (plus `Thumbnails`), and S82 keeps the layer-thumbnail LRU
   (`TitleLayerThumbnails`).
2. **Reconciled: the Library panel milestone (M6) lands in wave 6C with S84**; the rest of S82 is
   wave 6B. Until then the empty state offers New Text instead of template cards.
3. **Reconciled: fonts copied by the picker go under `<assets>/TitleArt/Fonts/`** (`Google/<slug>/`,
   `System/`), the program's one font folder (S80 §6.15), instead of `<assets>/Fonts/`.
4. **Reconciled: command ids use the prefix `titleDesigner.`** (was `title.`), shared with S84's
   window commands; S85 keeps `titleAnimation.` for its own context.
5. **Reconciled: "too small to read" is `legibility_contrast` at `CheckOptions::displayHeightPx`**
   (S80 §6.12); no separate size check exists.
6. **Implementation: view state in its own per-user file.** The per-document view state and the
   recent documents live in `.jasperengine/user/title-designer.yaml` (`TitleDesignerWorkspace`,
   `editor/titleart/designer/TitleDesignerPrefs.h`), beside the editor's `workspace.yaml` in the
   same uncommitted user folder, instead of a struct in `editor/Workspace.h`: the editor's workspace
   is a merge hotspot every window touches, and the designer's block is read and written only by
   the window. The per-user settings are one `title_designer` node of `settings.yaml`
   (`AppSettings::getTitleDesignerSettings` / `setTitleDesignerSettings`), which also remembers
   whether the window was open (R64).
7. **Implementation: Preview As modes, the Library and the 3D gizmo target are hooks.** Preview As
   lists Realtime and Baked; S81's Glyph Atlas and S86's tiers register through `addPreviewMode`
   (§6.8). The Library panel draws S84's data through `TitleLibraryProvider`
   (`editor/titleart/designer/TitleDesignerHooks.h`: items, families, the apply ops a hover previews
   and a click commits, tiles, favorites, recents, Save As dialogs); without a provider the window
   shows no Library region (Deviation 2). S86 supplies `Gizmo3DTarget`s through
   `setGizmo3DProvider`. The window's tests stand in for S84 with a fake provider.
8. **Implementation: font tables are read directly.** HarfBuzz's symbols are hidden in the editor's
   shared engine, so `hb_face_count` and `hb_ot_var_*` are not callable from the editor:
   `editor/titleart/fonts/FontTables.{h,cpp}` reads collections, names, OS/2, `fvar` and `cmap`
   itself. Shaping and outlines of the preview strips still go through S80's `TitleTextLayout`
   (HarfBuzz, hb-draw) and the rasterizing through S80's ThorVG `rasterizeElement`. The WOFF2 decoder
   and Brotli are a static `woff2_decoder` library linked into the editor (not compiled as editor
   sources), so the editor's precompiled header and unity batches stay off vendored code.
9. **Implementation: search.** `TitlePropertyIndex` has one tier R26 does not list, before the enum
   tier: every query word (or a synonym of it) starting a word of the location, which is what lets
   `フチ` find the rows labelled Width under Rim 1. `search::normalize` also treats Unicode
   punctuation (・「」、) as word separators, and its typo tolerance applies to ASCII words only
   (one byte apart is a different kana).
10. **Implementation: IME.** The macOS patch of `imgui_impl_osx.mm` is marked `[JasperEngine patch]`
    (the convention `libs/imgui/JASPER_PATCHES.md` already uses) rather than `// JasperEngine:`. The
    preedit is drawn by the canvas text editor only: the patch keeps the input method's keys away from
    every ImGui field while composing (so Enter and Esc stay the input method's) but other fields do
    not draw the composition.
11. **Implementation: two small additions to S80's and the editor's files.**
    `SokolTitleTarget::framebufferObject()` (`render/TitleGpu.h`) lets the canvas's private scene
    show a finished `TitleImage` on its quad without a copy; `WriteFilesCmd::contentOf(path)` gives
    the History panel the text each step wrote (S80's `TitleArtDocument` has no `stepText`), which
    is how rows are filtered to this document, previewed on hover and restored.
12. **Implementation: the canvas quad shows straight-alpha sRGB through the engine's sprite path**
    (blending as the engine blends a baked PNG in a game), with the checker as a repeating 2 x 2
    texture whose squares stay 8 screen pixels at every zoom.
13. **Implementation: tests without a GPU.** The editor's unit-test binary has no GPU context, so
    the canvas's pixel criteria (AC12's draws, AC17's readback, AC19's render equality, AC21's frame
    budget, AC32's rebuild) are covered by the MCP test `test_title_designer_window.py` (screenshots
    of the window: an idle canvas is stable, an edit changes it) and, for R44, by the stronger
    document check of the unit tests: the committed document equals the previewed one byte for byte
    (ids included), and S80's renderer is deterministic (R17), so their renders are equal. The
    Auto Bake toggle is the Project Settings command (`TitleArtSettingsCmd`, an undo step as there).
14. **Implementation: namespaces `jasperengine::editor::titledesigner` and `::titlefonts`** instead of
    §6.2's `jasperengine::editor::titleart::designer` / `::fonts`: a namespace
    `jasperengine::editor::titleart` would shadow `titleart::` (the engine's `jasperengine::titleart`)
    in every editor file that names it unqualified.
15. **Implementation: the core listing room for R78/R79.** The default listing had 38 characters
    left under its 99,000 limit, less than the window enum values and the `title_designer` object
    need (95). `focus_window` paid for them inside the tool: its description is shorter, and
    `page` no longer lists the Preferences pages (a wrong page still fails listing them). Core
    grows by 21 characters, to 98,983 (`tool_budget.py`; R81 allows 120).
16. **Implementation: a starter Library until S84 registers its provider.** The integrator asked for
    the Library strip in view by default (D18) before S84 lands, so with no provider registered the
    window uses `TitleStarterLibrary` (`editor/titleart/designer/TitleStarterLibrary.h`): ten Looks
    and five Color Schemes written as S80 ops (a Look replaces the face fill, bevel, rims and
    effects of the target; a Color Scheme recolours the face and rims), with no templates,
    favorites, recents or saving. Its tiles are the window's: the target layer with the item's ops
    applied, rendered through `TitlePreviewQueue` at priority Tiles (`TitleLayerThumbnails` keyed by
    slot). S84's `setLibraryProvider` replaces it. Below 960 px the Library's header arrow opens the
    strip in place under the canvas for the session, instead of as an overlay over the canvas
    bottom (R3).
17. **Addition: the no-CJK-font notice.** When the editor's system font search (S-unicode-text's
    `SystemFonts`) found no Chinese, Japanese or Korean font and the open title has CJK characters in
    a text or a layer name, the window shows one line under the toolbar naming
    `cjk_font_language` and linking `docs/editor/text-and-languages.md`; dismissed, it is not shown
    again that session. `state().notices` lists `cjk_font_missing` while it shows.
18. **Implementation: the Letters gizmo's steps** (R38, AC18). Its drags are named "Move Letters",
    "Rotate Letters" and "Scale Letters" (one `set` of `chars.<i>.offset|rotation|scale` for every
    selected character). A step taken while the text is being edited carries the text typed so far
    (`set_text` first: the indexes are of that text), and the edit goes on from it, so Esc no
    longer reverts what that step kept.
19. **Implementation: Tab is the window's key** (R76). While no field takes text, the window owns
    `ImGuiKey_Tab`, so ImGui's own tabbing between widgets does not run inside it; the region Tab
    reaches is outlined and `state().keyboard_focus` names it. On a narrow window Layers and
    Properties are the side column's tabs, selected by Tab and by `focus_window`'s `panel`.
20. **AC31 measured on this host, not on server A.** `tools/perf/title_designer_bench.py` builds
    the reference documents with `edit_title` ops (S80 has no `bench_2d_rims` / `bench_40_layers`
    fixtures) and reads the window's `state().timings`, its canvas render times and, with
    `--profile`, the profiler's `TD ...` scopes. Host: software GL (llvmpipe), the CPU shared with
    other streams' builds (load average 6.7-7.5 during the runs), window 956 x 538; medians of four
    runs at `68e251f25` (`uid1-3.json`, `bench7.json`):
    - Draft / Full render: 0.009 (bench_2d_rims: 63 ms / 6875 ms) and 0.090 (bench_40_layers:
      60 ms / 661 ms): met.
    - The window's ImGui work per frame: 0.65 ms (Layers shown) and 0.73 ms (Properties shown) with
      bench_40_layers: met. The first runs measured 8.4-16.8 ms; the profiler put 8.6 of 9.7 ms in
      the toolbar's bake status (a document hash and every output's recipe read, every frame), now
      made once per change.
    - The 256-layer tree against the 40-layer one: 1.01 x (0.65 ms / 0.65 ms): met (was 4.6 x).
      Layer bounds, hashes, the flattened tree, the empty-state check and the empty-text boxes are
      made once per change, and `IdleFramesMakeNoDocumentPasses` counts the whole-document passes of
      idle frames at 40 and 256 layers (none), load-insensitive.
    - Search query: 0.25 ms: met.
    - **Missed: the index build.** After an edit it is incremental (the rows of objects that did
      not change are replayed: 125 of 790 rows re-collected, `IncrementalIndexMatchesAFullBuild`):
      5.1 ms; a full build (a newly selected layer) takes 12.8 ms, budget 3 ms. Of the incremental
      pass, the rows take 1.3 ms; the rest is the pages' own work and the per-object keys. Either
      build runs ahead, 300 ms after the document and the selection settle, so a search keystroke
      never pays it; the frame that builds does. Server A's numbers are still to take.
21. **Open: the Windows and macOS IME manual checks** (M5, R37). They need a person on those
    systems (Microsoft IME, Kotoeri); this host builds Linux only. The `imgui_impl_osx` patch
    compiles by reading only. AC28's chip drag and In Context wait for S81, at integration.
