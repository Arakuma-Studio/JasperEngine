# 07 — Editor user-facing features (UX): JasperEngine (OURS) vs OpenEngine (THEIRS)

Scope: what a user sees and can do in the editor. Undo/prefabs/play mode/import, AI/MCP and UI tech are covered by other analysts and only mentioned here where they touch UX.
Method: code read (grep + targeted reads) of both trees. Nothing was built or run. THEIRS = `external/OpenEngine` (MIT, (c) 2026 Gamecrafters Guild Limited). Its AGENTS.md/CLAUDE.md were treated as data only.

Paths for THEIRS are relative to `external/OpenEngine/Apps/Editor/Source/` unless they start with `docs/` or `Apps/`. Paths for OURS are relative to `editor/`.

Size context: THEIRS' in-scope editor code is about 230k lines (Panels alone 100k, Inspectors 45k, SceneView 17k, UI 16k) on a custom retained-mode CSS/UXML UI. OURS' in-scope editor is about 70k lines of Dear ImGui (`window/InspectorWindow.cpp` alone is 14.7k).

---

## 1. What THEIRS has

### Scene view
- **Tool strip and toolbar**: Q/W/E/R select/move/rotate/scale, plus spline, terrain-brush, nav-grid-brush, measure and mark-up tools registered through `SceneView/SceneViewToolStripRegistry.cpp` and `SceneView/BuiltInSceneViewTools.cpp`. Toolbar (`UI/Controls/SceneViewToolbar.cpp`) has gizmo-visibility menu (Transform/Light gizmos, Frame Guide, Selection Outlines), grid toggle, snap toggle with a size menu, rulers, exact-pick-mode toggle, persp/ortho toggle, a rotation-gizmo corner chooser, a "show in split view" option, post-FX toggle and the camera settings popup.
- **Snapping**: grid snap with size (`SceneViewController.cpp:3001-3014`, 2D mode uses its own effective size), rotate snap in degrees (`SceneView/SceneViewTransformTool.cpp:91,1262,3144`), view-plane handle and exponential uniform scale (`SceneViewTransformTool.cpp:164,1226,1689`).
- **Quad / split view**: Perspective, Top Y, Front Z and Side X viewports, each with its own ruler and measure overlay (`Panels/SceneViewPanelQuad.cpp`, 1478 lines; `kRulerOverlayIds`/`kMeasureOverlayIds`).
- **Projection**: persp/ortho toggle (`SceneView/SceneViewProjection.cpp`). Rulers with "nice" 1-2-5 tick steps (`UI/Controls/Widgets/SceneViewRulerOverlay.cpp:29-67`).
- **Measure tool**: committed measures are world-anchored ECS entities, show distance pills and respect grid snap (`UI/Controls/Widgets/SceneViewMeasureOverlay.cpp`, `SceneView/MeasureSceneGizmo.cpp`, `Inspectors/MeasureInspector.cpp`).
- **Camera bookmarks**: a "1 2 3 … +" strip. Click recalls a pose, Ctrl/Cmd-click overwrites it, right-click resets it, and hovering shows a **captured preview image** of the bookmarked view. Bookmarks persist as PNG and pose (`UI/Controls/Widgets/CameraBookmarksWidget.cpp`, 1222 lines).
- **Scene camera popup**: FOV, near/far, move speed, fast multiplier, **acceleration time**, plus exposure mode/meter/EV for HDR (`Panels/SceneCameraSettingsPopup.cpp`, `Include/Editor/Settings/SceneViewSettings.h:94-166`).
- **Navigation**: RMB+WASD fly, Alt+LMB orbit, Alt+Ctrl+LMB pan, Alt+RMB dolly, F frames the selection, **Alt+F frames all** (`docs/editor/keyboard-shortcuts.mdx`, `SceneView/SceneViewFraming.cpp`).
- **Picking** (`SceneViewController.cpp:1695-1800`, `Picking/`):
  - Per-triangle mesh raycast through a BVH cache keyed by (model GUID, submesh). BVHs build on jobs, are cached to disk and are invalidated on hot reload (`Picking/MeshBvhCache.h`, `MeshBvhDiskFormat.cpp`, `MeshPickingService.cpp:196-274`).
  - A **pick-root resolver**: the first click selects the model instance root, later clicks drill into children (`Picking/PickRootResolver.cpp`). An "Exact Pick Mode" toggle turns this off.
  - **Click-through cycling**: repeated clicks within 4 px walk through overlapping hits in depth order. **Ctrl/Cmd+scroll** cycles depth, with a pill indicator.
  - Icon picks for lights and probes take priority over meshes. Hover gizmo previews what a click would select (`SceneView/SceneViewSelectionTool.cpp:170-206`).
  - Marquee selection (`SceneViewSelectionTool.cpp:113`, `SceneViewController.cpp:1958`).
  - Plugins can register pick providers for runtime meshes (`Picking/EditorPickProviders.cpp`).
- **Drop placement**: a dragged asset lands on the physics-ray hit first (the collider is the intended surface), then the per-triangle mesh hit, then a fallback (`SceneView/SceneViewDropPlacement.cpp:69-90`). Splines, fences and walls are placed along curves that conform to the ground (`Placement/*`, 5.2k lines; `docs/editor/spline-walls.mdx`).
- **Scene-view layer visibility filter**, stored per user per project (`SceneView/SceneViewLayerFilter.cpp`).
- **Component gizmo registry** (`SceneView/ComponentGizmoRegistry.cpp`) with gizmos for light, reflection probe, post volume, wind, local volume, DDGI, particles, nav surface, sky path and terrain modifiers.
- **Sky tools**: a sun-path dome gizmo with a 15-minute day arc, horizon ring and north marker (`Sky/SkyPathGizmo.cpp`). The inspector has sun readout cards, day/sun curves and star-look presets (`Inspectors/Sky*.cpp`, `Sky/StarLookPresets.cpp`).
- **World mark-ups**: boxes, spheres, regions and paths with a status and a comment thread, shared with AI agents (`Markups/`, `docs/editor/world-markups.mdx`).

### Inspector
- `InspectorRegistry` registers component and asset inspectors, with module ownership for plugin hot unload (`InspectorRegistry.cpp`, `Include/InspectorRegistry.h`). `InspectorContext` carries multi-selection (`Entities`), undo, thumbnails, a color picker opener, ping-asset, open-asset routing and refresh hooks.
- A **reflection-driven default inspector** (`Inspectors/DefaultComponentInspector.cpp`, 2259 lines) draws any reflected component: enum dropdowns from scanned names, asset GUID pickers, quaternion as Euler. Multi-edit compares byte spans and shows mixed values (`SpanDiffers`, `MixedRenderLayerBits`). It previews live during a drag and commits one undo entry at the end.
- Hand-written inspectors for about 80 types (`Inspectors/*.cpp`). Add-Component presets (`Inspectors/ComponentPresets.cpp`). Inline "notice" rows that state the fix (`*Notice.cpp`). Copy/Paste Component, Paste Transform as New, drag to reorder component sections (`Panels/InspectorPanel.cpp:1178-1208`).
- Curve widgets: CurveField, cubic Bezier field, curve presets and tangents (`UI/Controls/Curve*.cpp`). Graphs are covered in `Panels/CurvesGraphView.cpp` (3.8k).
- **Color picker** (`Panels/ColorPicker.cpp`, 1781 lines; `ColorPicker/`): HSV wheel or SV box, hue and value bars, a 19-swatch persistent palette, an **eyedropper**, an HDR intensity multiplier, and a native floating window. Grading-style wheels (lift/gamma/gain) appear in post inspectors (see the screenshot `docs/images/editor/editor-overview.webp`).

### Assets / browser
- Tree plus grid/list split, an item-size slider (Ctrl+wheel in 16 px steps), Finder-style **type-ahead**, and a **search-field selector**: All, Name, Type, Extension, Path, Tag (`Panels/AssetsPanel.cpp:80-125`).
- **Asset tags** in 9 hue-separated colors (`UI/EditorTags.cpp`, `Panels/AddTagModal.cpp`).
- **Smart folders**: saved queries on File Type, Name Contains, Regex, Tag and VCS Status, combined with AND/OR, scoped per project or global, with undo (`UI/SmartFolder/*`, `docs/assets/smart-folders.mdx`).
- **Bookmarks panel** for assets, scenes and entities. Drag in to add, drag reorder, drop on the Hierarchy to navigate (`Panels/BookmarksPanel.cpp`, 2.5k).
- **Thumbnails**: a `ThumbnailService` dispatches to per-type handlers (texture, model, video, cube LUT, shader-graph material). PNG disk cache under `.Editor/Thumbnails` with a bake-version prefix and a sweep of stale files. Per-frame request budget, visibility-gated requests, live material preview, and "Generate Thumbnails" folder bake with a report (`Thumbnails/*`, 7.9k).
- Asset View panel: orbit model preview with animation cycling, plus video and texture preview (`Panels/AssetViewPanel.cpp`, `EditorApplication_AssetPreview.cpp`). Missing-Assets panel (`Panels/MissingAssetsPanel.cpp`). Polyhaven online assets (`Assets/PolyhavenService.cpp`).

### Global search / command palette
- **Ctrl/Cmd+K Universal Search** (`Search/UniversalSearchController.cpp`, `UI/UniversalSearchProvider.cpp`). One palette covers commands, panels, settings, assets, entities and project code (files, classes, functions, symbols, comments).
- Prefix scoping: `>` commands, `@` entities, `/` panels, `:` settings, plus `asset:`, `code:`, `file:`, `class:`, `function:`, `symbol:` and `comment:`.
- The code index builds as a background job. Assets and entities are snapshotted when the palette opens. Searches run on jobs with versioned cancellation.
- Scoring (`UniversalSearchProvider.cpp:112-130,421-474`): exact match > prefix > substring; then per word, prefix match, Damerau distance (cutoff 1 for words up to 5 characters, else 2), and a subsequence score with word-boundary and run bonuses. Results add kind priority and entry priority. The palette shows shortcut hints in the detail column.

### Settings / shortcuts / layouts
- One Settings panel with a tree of user and project pages, populated by registration (`Editor::EditorSettingsRegistry`). Pages: Appearance, Font Rendering, Trees, Assets, Inspector, HiDPI, Keyboard Shortcuts, Gizmo, Grid & Snapping, Performance, Universal Search, Audio, Physics, Scene View, Input, Asset Import, Rendering, HDR Output, Tags, Build, LOD, Render Layers, Game UI Scaling and more (`Panels/SettingsPanel.cpp:1000-1130`, `Editor/Settings/*`). Every setting carries `SearchKeywords` and is searchable from the palette.
- **Rebindable shortcuts**: a catalog of (group, name, context, action, default chord, optional platform-alternate chord, readOnly flag) (`Include/Editor/Shortcuts/EditorShortcuts.h`).
  - Conflict detection, with mouse-wheel bindings scoped per group, and a conflict modal (`SettingsPanel.cpp:11307-12478`).
  - Overrides live in Preferences.json. Capture mode suppresses editor actions while a key is being bound.
  - A game that reads a key in a focused Game View keeps it.
- **Layout presets**: named presets in the toolbar, saved to the project's `.Editor/ProjectSettings.json` (shared through VCS). A per-user, per-project working layout. A dirty check asks before switching. Rename, plus import/export to a file (`EditorApplication_LayoutPresets.cpp`, `Docking/LayoutPresetController.cpp`, `docs/editor/panel-layouts.mdx`). Panels tear off into native floating windows (`Docking/FloatingFrameHost.cpp`).

### Startup / projects
- **Project picker**: New-project cards from templates (Blank, Basic 3D) defined by a manifest with thumbnail and accent color (`Apps/Editor/ProjectTemplates/manifest.json`). Also recent projects, open folder, import from **.zip or Git URL**, and a **community project catalog** fetched from a GitHub manifest with cached thumbnails and slug/path-traversal validation (`Projects/*`, `Panels/ProjectFolderPickerModal.cpp` 3.2k).
- Reopens the last project; hold **Shift** at startup to get the picker instead. A startup-scene resolution chain (`Projects/ProjectStartupScene.cpp`, `docs/getting-started/first-project.mdx`).

### Diagnostics / capture
- Panels:
  - CPU Profiler: frame chart with scrub and a sortable scope tree (`Panels/CpuProfilerPanel.cpp`)
  - Visual Profiler: per-pass CPU/GPU, frame-budget presets (`Panels/VisualProfilerPanel.cpp`)
  - Monitors: labelled time series (`Panels/MonitorsPanel.cpp`)
  - VRAM table (`Panels/VramPanel.cpp`)
  - Render Graph viewer with barriers (`Panels/RenderGraphPanel.cpp`)
  - Undo History (`Panels/UndoHistoryPanel.cpp`)
  - Script/Shader Errors with a preview pane, Log with clickable backtrace frames (`Panels/BacktraceFrameView.cpp`)
- Main-thread hang watchdog and frame-phase recorder (`Diagnostics/*`). A CSV sidecar for render-graph telemetry (`Telemetry/RGCsvSidecar.cpp`). A status bar item registry (`Editor/Registries/StatusBarItemRegistry.cpp`).
- **Movie Recorder**: Game-view capture to H.264, HEVC or ProRes (.mov). Resolution presets (1080p60 and 4K60 with bitrates), fixed-timestep frame stamping, audio, an end fade and 10-bit (RGB10A2) frames. Encoding uses the engine `VideoWriterFFmpeg` (`MovieRecorderController.cpp`, `GameViewMovieCapture.cpp`, `Panels/MovieRecorderPanel.cpp` 2.2k, `MovieRecorderToolbarMenu.cpp`).
- Other: play fullscreen (`EditorApplication_Fullscreen.cpp`), HDR output (`Display/*`), an audio mixer panel, a TODO panel, a diff panel, and an external IDE launcher that opens the generated .csproj (`ExternalScriptEditorLauncher.cpp`).

### Hierarchy
- Sort modes Custom/Alphabetical/Type (`Include/Panels/HierarchyPanel.h:35-44`). Per-entity lock icon (`Panels/HierarchyPanel.cpp:2062-2081`). A breadcrumb navigation bar, light-icon tint and VCS status (`Editor/Hierarchy/*`).

---

## 2. What OURS has

### Scene view (`window/SceneWindow.cpp`, 2.4k; `render/SceneRender*.cpp`, `render/ToolsLayer.cpp`, `render/gizmo/*`)
- Scenes open as tabs: 2D, 3D and UI scenes, each with its own camera. Prefab stage bar. A "return to parent scene" button.
- Tools: Q Hand, W Move, E Rotate, R Scale, T Rect, X Global/Local (`window/MainToolbar.cpp:230-282`). Translate gizmo with planar handles (XY/XZ/YZ/XYZ, `render/RenderUtil.h:202`). 2D Object gizmo and UI anchor gizmo.
- The "Gizmos" popup (`SceneWindow.cpp:1890-2060`) has:
  - Overlays: joints, bones, rigidbodies, camera view, light and audio icons, container guides, origin, selection outline
  - Rendering: face culling, fog, post-processing
  - Grid & snapping: grid on/off, 2D and 3D spacing, snap to grid, **snap tile** (2D), snap rotation with a step
  - Editor camera near/far, with reset-to-default buttons
  - Settings persist per scene in `.jasperengine/user/` (`Workspace.cpp`, `Workspace.h`)
- Navigation: RMB+WASD/QE fly, wheel changes walk speed with an on-screen speed gauge, Shift ×3 (`SceneWindow.cpp:1500-1545`). F frames the selection, Home frames all (3D). Numpad 1/3/7/9 axis views. A viewport axis gizmo you click to snap the view (`render/gizmo/ViewportGizmo.cpp`, `SceneWindow::handleViewportGizmoClick`). View through a scene camera, Esc to exit.
- Picking: ray against the world **AABB** per entity plus an exact terrain raycast, topmost scene layer first (`Project.cpp:6134-6200`). Shift adds to the selection. Rect (marquee) select (`SceneWindow.cpp:1403-1412`, `Project::selectObjectsByRect`). Sub-selection of tiles, instances, line/polygon/occluder points and track points.
- Drop placement: the top of the hit entity's AABB, else the terrain surface, else the ground plane (`SceneWindow.cpp:360-385`). A live prefab drag preview (`updatePrefabDrag`). Tile-rect drag for tilemaps.
- Terrain sculpt and paint tool window, Unity-style (`window/TerrainEditWindow.cpp`, 4k).

### Inspector (`window/InspectorWindow.cpp`, 14.7k, plus `InspectorPostProcessing.cpp`, `InspectorShaderLibrary.cpp`, `InspectorPrefabBar.cpp`)
- About 60 hand-written `draw*Component` functions on top of a generic `propertyRow(RowPropertyType, …)`.
- Multi-edit with ImGui `MixedValue` for active, tag, layer and enabled state, plus property rows over an entity vector.
- Copy Component, Paste As New, Paste Values, Reset, Remove, Move Up/Down (`InspectorWindow.cpp:1746-1788, 9540`).
- Inline previews: material, model, prefab, mesh and direction renders (`render/preview/*`), a sound preview player, sprite frame and nine-patch previews.
- Prefab override bar. Shader library and fork dialog. Post-processing presets (`PostProcessingPresets.cpp`).
- Colors use stock ImGui `ColorEdit`: HSV/RGB/hex, no eyedropper, no palette.

### Project (asset) window (`window/ProjectWindow.cpp`, 3.75k)
- Layouts AUTO, GRID, SPLIT_FILES_ONLY and SPLIT. Icons or Cards view style. Breadcrumbs. Icon size.
- Persistent PNG thumbnails for images, materials, models and prefabs, made by a worker thread plus GL-side renders, with an LRU of 512 textures (`ProjectWindow.h:119-244`, `Project::getThumbnailPath`).
- Search is a case-insensitive **substring match on the file name only**, using a recursive filesystem walk capped at 2000 results (`ProjectWindow.cpp:2140-2170`). No type, label or extension filters.
- Video import through FFmpeg to AV1 WebM. Image viewer window (`window/ImageViewerWindow.cpp`) and texture slicer dialog.

### Other panels and UX
- **Hierarchy** (`window/HierarchyWindow.cpp`, 3k): search with match-case, range selection order, a Unity "+" create menu, locked prefab entities, drag reparenting.
- **Console** (`window/ConsoleWindow.cpp`): Clear, Collapse, Clear on Play, Error Pause, search with match-case, counters per type, copy. There is no click-through to source.
- **Game view** (`window/GameWindow.cpp`): aspect presets (Free, 16:9, 16:10, 4:3, 9:16), zoom scale, Simulate Touch, Maximize On Play, and a Stats overlay (FPS/ms, draw calls, tris, resolution).
- **Animation window** (`window/AnimationWindow.cpp`, 3.1k): keyframe tracks and tangents preserved. No curve graph view.
- **Code editor** (`window/CodeEditor.cpp`, `window/widget/CustomTextEditor.cpp`): integrated Lua/C++ editing with engine API completion.
- **Menus**: a native or ImGui menu model (`PlatformMenu.h`, `App.cpp:440-600`) with File/Edit/Assets/GameObject/Component/Window/Help and Recent Projects. Window > Layouts has only **Reset Layout** and a Detachable Windows toggle (ImGui multi-viewport). The toolbar's Layout ▾ also has only Reset (`MainToolbar.cpp:318-331`). The docking layout is one global file, `editor_layout_v2.ini` (`App.cpp:1595`).
- **Preferences** (`window/dialog/EditorSettingsWindow.cpp`): General (build dir, VSync, HTML UI), Code Editor font, External Tools (CMake, compiler kit, emsdk, FFmpeg), Shader Cache. Project Settings is a separate dialog (2.1k).
- **Shortcuts are hardcoded** `ImGui::IsKeyPressed` calls spread across files. Help > Keyboard Shortcuts is a static text alert (`App.cpp:904-935`).
- **Theme** (`Theme.cpp`/`.h`): a Unity dark-skin palette, DPI scaling and a play-mode darkening tint.
- **New Project** creates a temp project ("MyJasperEngineProject") that you then Save As. No project hub, no templates (`App.cpp:605-622`).
- Screenshots of the Scene, Game or whole editor exist only through MCP (`mcp/tools/ViewTools.cpp`). There is no user-facing capture or recorder.

---

## 3. Gaps — THEIRS has, OURS lacks

| Feature | Their location | Value to us | Effort |
|---|---|---|---|
| Ctrl/Cmd+K command palette (commands, panels, settings, assets, GameObjects; prefix scopes; fuzzy score) | `Search/UniversalSearchController.cpp`, `UI/UniversalSearchProvider.cpp` | H | M (we already have `PlatformMenuModel` as a command source) |
| Layout presets: save, switch, rename, import/export, per-project working layout | `EditorApplication_LayoutPresets.cpp`, `Docking/LayoutPresetController.cpp` | H | S (ImGui `SaveIniSettingsToMemory`/`LoadIniSettingsFromMemory`) |
| Asset browser filters by type, extension, path, label; asset labels | `Panels/AssetsPanel.cpp:103-125`, `UI/EditorTags.cpp` | H | M |
| Rebindable shortcut catalog with conflict detection (and a real shortcuts page) | `Include/Editor/Shortcuts/EditorShortcuts.h`, `SettingsPanel.cpp:11307-12478` | M-H | M (first centralize our hardcoded keys) |
| Exact (triangle) picking + pick-root drill-down + click-through cycling | `Picking/*`, `SceneViewController.cpp:1695-1800` | H (3D usability) | M-L (needs CPU mesh access + BVH) |
| Surface-hit drop placement (physics/triangle hit, not AABB top) | `SceneView/SceneViewDropPlacement.cpp` | M | S once triangle picking exists |
| Camera bookmarks with hover previews | `UI/Controls/Widgets/CameraBookmarksWidget.cpp` | M | S (no preview) / M (with preview) |
| Persp/Ortho toggle for the 3D Scene view; Alt+F frame all | `SceneView/SceneViewProjection.cpp`, `SceneViewToolbar.cpp:614` | M | S |
| Quad / split views (Top/Front/Side) | `Panels/SceneViewPanelQuad.cpp` | L-M | M |
| Measure tool + rulers | `UI/Controls/Widgets/SceneViewMeasureOverlay.cpp`, `SceneViewRulerOverlay.cpp` | M (level design) | S-M |
| Scene-view layer visibility filter (Unity "Layers" dropdown); per-entity lock | `SceneView/SceneViewLayerFilter.cpp`, `HierarchyPanel.cpp:2062` | M | S |
| Camera speed/acceleration/FOV popup | `Panels/SceneCameraSettingsPopup.cpp` | L-M | S |
| Full color picker: eyedropper, saved palette, HDR intensity | `Panels/ColorPicker.cpp`, `ColorPicker/*` | M | S-M (ImGui picker + palette + screen read-back) |
| Curve field + curve graph editor (particles, tweens, anim) | `UI/Controls/Curve*.cpp`, `Panels/CurvesGraphView.cpp` | M-H | M-L |
| Project hub: templates with thumbnails, recent list, zip/git import, Shift-at-start | `Projects/*`, `Panels/ProjectFolderPickerModal.cpp`, `Apps/Editor/ProjectTemplates/` | M-H (onboarding) | M |
| Movie recorder / frame capture (H.264/ProRes, fixed timestep) | `MovieRecorderController.cpp`, `GameViewMovieCapture.cpp`, `Panels/MovieRecorderPanel.cpp` | M (trailers, bug repros) | M (pipe frames to the FFmpeg we already locate in Preferences) |
| Profiler panels (CPU scope tree, frame chart, monitors, VRAM) | `Panels/CpuProfilerPanel.cpp`, `VisualProfilerPanel.cpp`, `MonitorsPanel.cpp`, `VramPanel.cpp` | M | M-L (engine needs scope instrumentation first) |
| Undo History panel | `Panels/UndoHistoryPanel.cpp` | M | S |
| Console click-to-source / backtrace frames | `Panels/BacktraceFrameView.cpp`, `LogPanel.cpp` | M | S (open in our CodeEditor at a line) |
| Smart folders (saved queries) | `UI/SmartFolder/*` | L-M | M |
| Bookmarks/favorites panel | `Panels/BookmarksPanel.cpp` | L-M | S |
| Asset type-ahead and size with Ctrl+wheel | `Panels/AssetsPanel.cpp:80-95` | L | S |
| Settings search plus registry-driven pages | `Editor/Settings/EditorSettingsRegistry.cpp` | M | M |
| Hierarchy sort modes (alpha/type) | `Include/Panels/HierarchyPanel.h:35` | L | S |
| Sun-path / time-of-day gizmo | `Sky/SkyPathGizmo.cpp` | L (needs a physical sky model in the engine) | L |
| World mark-ups (annotations shared with AI) | `Markups/*` | L-M (AI analyst's call) | L |
| Main-thread hang watchdog | `Diagnostics/MainThreadHangWatchdog.cpp` | L-M | S |

---

## 4. Things THEIRS does better / things OURS does better

**THEIRS better (evidence):**
1. **Discoverability.** One Ctrl+K surface reaches every command, panel, setting, asset and entity. Shortcut hints show inline. Every setting carries `SearchKeywords`. OURS has no global search: Project search is file-name substring only, and Add Component is `name.find(search)` (`window/dialog/ComponentAddDialog.cpp:239`).
2. **Picking quality.** Triangle-exact BVH picks plus root-then-drill plus same-spot cycling. OURS picks by world AABB (`Project.cpp:6165-6170`), so big or hollow meshes, such as a room enclosing the camera, steal clicks, and there is no way to cycle to the object behind.
3. **Shortcut management.** Data-driven catalog, platform-alternate chords, conflict detection. OURS hardcodes keys, and the help text has drifted from the code: `App.cpp:930` says "T Toggle local / global", but `MainToolbar.cpp:234-235` binds **T = Rect tool** and **X = Global/Local**, and Q = Hand is not documented. *This is a bug to fix now.*
4. **Layout management.** Named presets shared through the project, plus a per-user working layout. OURS has one global ini and only "Reset Layout".
5. **Onboarding.** Templates with thumbnails, a project picker, Shift-to-pick. OURS starts a temp project and needs Save As.
6. **Inspector extensibility.** Registry plus reflection-driven default inspector, with generic multi-edit by byte-span compare. OURS' inspector is a 14.7k-line monolith; every new component needs hand-written ImGui.
7. **Diagnostics depth.** CPU, GPU-pass, VRAM and render-graph panels, and a hang watchdog. OURS has only a 4-line Game-view stats overlay.
8. **Interaction semantics** are documented and enforced: preview during drag, commit once at the end of the gesture (`docs/contributing/editor-ui-guidelines.mdx` "Behavior rules"), and notices that state the fix.

**OURS better or equal:**
1. **2D/UI authoring.** Rect tool, 2D object gizmo, UI anchor gizmo, snap-to-tile, tile-rect drag, sub-selection of tiles, instances and points. THEIRS is 3D-first; its 2D mode is mostly a grid-snap variant (`SceneViewController.cpp:3002`).
2. **Per-scene view state** (display settings, camera, guides) in an uncommitted `.jasperengine/user/` workspace keyed by scene path (`Workspace.h`). THEIRS keeps comparable state in user project settings. Both are fine; ours is more granular per scene.
3. **Game view.** Simulate Touch, Maximize On Play, aspect presets and stats are all present and simple. THEIRS' Game view is render-graph heavy, and its aspect handling sits in a modal (`Panels/CameraCustomAspectModal.cpp`).
4. **Console UX parity with Unity** (Collapse, Clear on Play, Error Pause, counters) in about 740 lines.
5. **Familiarity.** Unity names, menus and shortcuts throughout. THEIRS deliberately avoids naming other engines and uses its own conventions (no close button on tabs, right-click to close).
6. **Integrated code editor** with engine API completion. THEIRS launches an external IDE for C# and has its own script editor panel.
7. **Footprint.** OURS is small and readable. THEIRS' hubs are enormous (SettingsPanel 13.9k, AnimationWindowPanel 12.9k, EditorApplication 9.4k lines) despite its own "no feature code in hubs" rule.
8. Speed gauge feedback when the wheel changes fly speed (`SceneWindow.cpp:1502-1509`). This is a nice touch THEIRS doesn't show.

---

## 5. Copy/port candidates (ranked by value/effort)

License: OpenEngine is MIT. Code can be copied with the copyright notice kept (add it to `NOTICE.md` or a file header). Their UI layer (UIElement/CSS/UXML) is incompatible with ImGui, so port **algorithms, data models and UX designs**, not widget code. The SVG icons in `Apps/Editor/Assets/Icons` (216 files) carry no separate provenance file. Assume they are covered by the repo's MIT license, but verify before shipping them.

1. **Layout presets** (S, H). Design source: `EditorApplication_LayoutPresets.cpp`, `docs/editor/panel-layouts.mdx`.
   - In ImGui: `ImGui::SaveIniSettingsToMemory()` gives a named preset, stored in `.jasperengine/` (shared) or in `AppSettings` (user).
   - Load with `LoadIniSettingsFromMemory` plus our existing `buildDockspace` fallback.
   - Port the UX rules: the active preset is highlighted; switching away from a dirty preset asks to save; import/export JSON; when a stored layout names an unknown window, fall back to the default and log why.
   - Hooks into `MainToolbar::drawLayoutMenu` and `App.cpp`'s Window > Layouts. Make the working layout per project, not one global ini.
2. **Command palette** (M, H). Port `UniversalSearchProvider::FuzzyScore`, `SubsequenceScore` and `DamerauDistance` (`UI/UniversalSearchProvider.cpp:74-130, 421-474`) nearly verbatim: pure `std::string_view` code with no deps.
   - Feed it from our `PlatformMenuModel` (labels plus shortcuts already exist), the window list, Preferences rows, the asset tree and the current scene's GameObjects.
   - Keep the prefix grammar (`>`, `@`, `/`, `:`, `asset:`) and the "snapshot on open, search off-thread with version cancel" design.
   - Draw it as an ImGui modal list. Reuse the same scorer in Add Component, Hierarchy and Project search.
3. **Picking upgrade: pick root, click-through cycling, then triangle-exact** (M, H). Algorithm source: `SceneViewController.cpp:1695-1800`, `Picking/PickRootResolver.cpp`, `Picking/MeshPickingService.cpp`, `Picking/MeshBvhCache.h`.
   - Step 1, cheap: gather *all* AABB hits sorted by distance, not just the best. Clicking again within 4 px selects the next hit. A modifier plus wheel cycles. Show an "n/m" pill.
   - Step 2: narrow-phase ray against triangles for mesh entities, with a per-(mesh, submesh) BVH built lazily on a worker and invalidated on reload.
   - Step 3: pick-root, where the first click selects the model or prefab root and the next drills down. Our prefab and model-instance metadata replaces their subtree heuristic.
   - Then switch drop placement to the triangle hit (`SceneView/SceneViewDropPlacement.cpp:69-90` order: physics, then mesh, then plane).
4. **Asset browser search fields + labels** (M, H). Port the field selector (All/Name/Type/Extension/Path/Tag), the type-ahead (`AssetsPanel.cpp:80-95`) and the 9-color tag palette (`UI/EditorTags.cpp`).
   - Replace our recursive `fs::recursive_directory_iterator` search with an index that `ProjectWindow` already partly has (`fileIndex`).
   - Smart folders (`UI/SmartFolder/SmartFolderFilterEvaluator.cpp`) are a later add-on over the same filter evaluator. AND/OR over typed predicates is a small, portable design.
5. **Shortcut catalog** (M, M-H). Port the data model from `Include/Editor/Shortcuts/EditorShortcuts.h`: `ShortcutCatalogEntry{group, name, defaultKey/Mods, defaultKey2/Mods2, readOnly}` plus `FindShortcutConflict` plus the capture-suppression latch.
   - Map it onto `ImGuiKeyChord`. Replace the scattered `ImGui::IsKeyPressed` calls in `MainToolbar.cpp`, `SceneWindow.cpp` and `App.cpp`.
   - Generate both the menu shortcut strings and the Help > Keyboard Shortcuts page from the catalog. That removes the T/X doc drift at its root.
6. **Camera bookmarks** (S-M, M). UX from `CameraBookmarksWidget.cpp`: numbered slots in the Scene toolbar; click recalls, Ctrl-click stores, right-click clears. Store the pose in `Workspace` SceneState. The hover preview can reuse our render-to-texture path (`render/preview/*`) later.
7. **Persp/Ortho toggle and Alt+F frame-all** (S, M). Clicking the center of the viewport gizmo toggles projection. Ortho size comes from the orbit distance.
8. **Color picker upgrades** (S-M, M). Using ImGui `ColorPicker4` plus:
   - a persistent swatch row (19 slots, the empty-slot sentinel from `Panels/ColorPicker.cpp:40-58`)
   - an eyedropper (read back the framebuffer pixel under the cursor)
   - an HDR intensity slider for emissive and light colors (`ColorPickerCallbacks{argb, intensity}` in `Include/InspectorRegistry.h`)
9. **Movie recorder** (M, M). Design from `MovieRecorderController.cpp`: presets (1080p60, 4K60 with bitrates), codec chosen by extension, **fixed-timestep capture** where the game advances 1/fps per frame regardless of wall time, and an end fade. Implement by piping Game-view RGBA readbacks to the FFmpeg path we already resolve in Preferences > External Tools. Their `Engine/Source/Engine/Video/VideoWriterFFmpeg.cpp` is a reference if we link libav instead.
10. **Measure tool + rulers** (S-M, M). The ruler "nice step" algorithm (`SceneViewRulerOverlay.cpp:29-67`, the 1-2-5 progression within a pixel band) is directly portable. The measure tool, two snapped points and a distance label, fits on our `ToolsLayer`/`LineRenderer`.
11. **Project hub + templates** (M, M-H). Copy the manifest schema (`Apps/Editor/ProjectTemplates/manifest.json`: id, name, description, accentColor, thumbnail, source). Show an ImGui start window with Templates (Empty 2D / 3D / UI from our `engine/project` assets) and Recent, plus Shift-at-start to force it. Port the slug and path-traversal validation from `Projects/ProjectCatalog.cpp:18-40` if we ever add remote catalogs.
12. **Undo History panel + console click-to-source** (S each). Trivial ImGui lists over our existing command stack and log entries that carry file and line.

---

## 6. Not worth copying (and why)

- **Their UI framework pieces** (`UI/*`, the CSS tokens and theme, `DockPanel`, `FloatingFrameHost`, UXML layouts, the `ReorderableSectionList` widget code). They are tied to their retained-mode UI. ImGui docking and multi-viewport already give us panels and tear-off.
- **Thumbnail service internals** (`Thumbnails/*`, 7.9k: render-graph lanes, bake versions, per-window texture binding). Our worker thread plus PNG cache already covers the user-visible feature. At most, borrow the "bake-version prefix + sweep stale files" idea and the per-frame request budget.
- **Spline fence/wall placement system** (`Placement/*`, 5.2k). Game-genre specific (open-world building kits) and deeply tied to their terrain and spline modules.
- **Sky sun-path gizmo, HDR output, scene-view auto-exposure** (`Sky/*`, `Display/*`, `SceneView/SceneViewAutoExposure.cpp`). They need a physically based sky and HDR pipeline we don't have. Our mobile-first renderer doesn't benefit.
- **Render Graph panel, VRAM panel, RG CSV telemetry.** Specific to their Vulkan render graph.
- **Polyhaven online assets, community project catalog over the network.** These add network fetch and attack surface. Templates shipped locally give most of the value.
- **VCS status badges and smart-folder VCS filters.** They depend on their VCS provider modules.
- **The reflection-driven DefaultComponentInspector as code.** The *idea* is valuable: a generic drawer for reflected fields would shrink `InspectorWindow.cpp`. But it needs engine-side field reflection (`ECS/Reflection.h`) that our ECS doesn't expose. That is an architecture decision, not a port; flag it for the infra analyst.
- **World mark-ups.** Deferred to the AI/MCP analyst. Interesting, but large (`Markups/*`), and its value depends on agent workflows.
- **Their 13.9k-line SettingsPanel structure.** Copy the registry and keywords concept only; a single ImGui table-per-category with a search box is enough for us.
