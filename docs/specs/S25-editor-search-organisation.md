# S25 — Editor search & organisation: Search (Ctrl+K), layouts, asset labels and filters, saved searches, settings search, bookmarks, rebindable shortcuts

Wave 3. Owner area: new `editor/search/`, `editor/shortcuts/`, `editor/layout/`, `editor/organisation/`,
`editor/settings/`; edits to `editor/App.cpp`, `editor/window/MainToolbar.cpp`,
`editor/window/ProjectWindow.*`, `editor/window/HierarchyWindow.*`,
`editor/window/dialog/{ComponentAddDialog,EditorSettingsWindow,ProjectSettingsWindow}.cpp`,
`editor/mcp/tools/ProjectTools.cpp`.

## 1. Goal

Make everything in the editor findable from one place, and let users organise their own work.
Ctrl/Cmd+K opens **Search**, a palette over commands, windows, settings, assets, GameObjects and
components. It uses one fuzzy scorer, and Add Component, the Hierarchy, the Project window and
the settings windows use the same scorer. Every key binding moves into one command catalog. The
catalog drives menus, tooltips, Help > Keyboard Shortcuts and user rebinding, which also fixes
the help text that is wrong today. Named window layouts, coloured asset labels, Unity-style
search filters, saved searches and bookmarks complete the set. Today the editor has none of
these: there is no global search, a single layout file, keys are hard-coded in about 40 places,
and Project search is a file-name substring match over a filesystem walk.

## 2. Scope

In scope:
- **Search palette** (Edit > Search All..., Ctrl/Cmd+K). Prefix scopes, the ported fuzzy scorer,
  recent items, shortcut hints. The scorer is also used by Add Component, Hierarchy search,
  Project search and settings search.
- **Command catalog and shortcuts.** One table of commands with default chords and platform
  alternates. Conflict detection. User overrides persisted. Edit > Shortcuts... window. Menus,
  toolbar tooltips and Help > Keyboard Shortcuts are generated from the table. The T/X/Q help
  bug is fixed (R1).
- **Layouts.** Named presets: save, switch, rename, delete, import (Load Layout from File...)
  and export (Save Layout to File...). One working layout per project.
- **Project window.** Filters by type, label, extension and path (Unity `t:`/`l:` grammar plus
  `ext:`, `path:`, `re:`, and S04's `ref:`/`missing:`). Coloured asset labels stored with S04
  identity. Type-ahead. Ctrl/Cmd+wheel item size. The search runs over an index instead of a
  directory walk.
- **Saved searches** (OpenEngine "smart folders"), shown under **Favorites** at the top of the
  Project window folder tree, as in Unity.
- **Settings.** A settings registry, lifted out of the MCP settings table, that drives the
  Preferences and Project Settings pages, their row-level search, and the palette's `:` scope.
- **Bookmarks window** for assets, folders, scenes and GameObjects, which also feeds Favorites.
- MCP tools for all of the above. Docs. Tests.

Out of scope:
- OpenEngine's UI toolkit, `DockNode` JSON, docking, `SettingsStore`, its 13.9k-line
  `SettingsPanel` structure and floating-frame host (README do-not-copy: "their docking and
  editor UI toolkit"). We use ImGui docking and its ini.
- Code search scopes (`code:`, `class:`, `function:`, `symbol:`, `comment:`) and their
  background code index. Script files are still found by name through `asset:`.
- VCS-status filters and badges (S29 may add a `vcs:` token through R27's token table).
- Shortcut profiles (Unity's profile drop-down). There is one override set per user.
- Camera bookmarks (S26). Project-shared layout presets checked into VCS (§9 D6).
- A Lua API. These are editor-only features with no runtime part.

## 3. Current state

- **Menus.** `editor/PlatformMenu.h` defines `PlatformMenuModel` / `PlatformMenuItem {type, role,
  label, shortcut, command{id,payload}, enabled, checked, children}`. `App::buildMenuModel()`
  (`App.cpp:406-595`) builds File/Edit/Assets/GameObject/Component/Window/Help with
  **literal chord strings** (`menuShortcut(..., "Ctrl+S")`, `App.cpp:483` onward). Commands are
  `enum class AppMenuCommand` (`App.cpp:218-279`), dispatched by `executeMenuCommand`. On
  macOS, Windows and Linux the native menu backends render the same model
  (`Backend::setMainMenu`). `BackendMac.mm:351-385` parses "Ctrl"/"Cmd" into the Command key.
  GameObject create items carry their own chord literal (`HierarchyWindow.cpp:464`,
  `"Ctrl+Shift+N"`).
- **Keys are hard-coded.** There are `ImGui::IsKeyPressed` chord checks in `App.cpp:1904-2010`
  (save, open, undo/redo, the Unity menu chords, clipboard, F2, F), in
  `MainToolbar::handleShortcuts` (`MainToolbar.cpp:193-236`: Ctrl+P family, F5/F6/F7, and
  Q/W/E/R/T/X tools) and in `SceneWindow.cpp:1630-1705` (F, Home, numpad views, terrain brush
  `[`/`]`). Menu chord strings, handler code and tooltips (`MainToolbar.cpp:250-254`) are kept
  in sync by hand.
- **Help bug.** Help > Keyboard Shortcuts is a static alert (`App.cpp:904-935`). Line 931 says
  `"T    Toggle local / global transforms"`. The code binds **T = Rect tool** and
  **X = Global/Local** (`MainToolbar.cpp:234-235`). **Q = Hand** (`:230`) is not listed. The
  text also omits Ctrl+2 (Game), Ctrl+Alt+P (Step), the numpad views and the terrain brush keys.
- **Layouts.** The ImGui ini is one global file, `editor_layout_v2.ini` in the config directory
  (`App.cpp:1595-1596`). Window > Layouts and the toolbar Layout menu offer only Reset Layout
  (`App.cpp:576-578`, `MainToolbar.cpp:318-331`). Open/closed state is not in the ini. It lives
  in `AppSettings::PanelVisibilitySettings` (`AppSettings.h`). Dock sizes are pixels and are
  rescaled by `rescaleRestoredLayout` (`App.cpp:1275`). Document windows are `###Scene<id>` and
  `###<filepath>` and are re-docked by `dockProjectTabs` (`App.cpp:1364`).
- **Project search** (`ProjectWindow::collectSearchResults`, `ProjectWindow.cpp:2140-2170`)
  walks the filesystem with `fs::recursive_directory_iterator` from the current folder,
  substring-matches the file name, and stops at 2000 results. There are no type, label or
  extension filters and no labels. The icon size is only a slider (`ProjectWindow.cpp:973`). There
  is no Ctrl+wheel and no type-ahead. `FileType` (`ProjectWindow.h:32-46`) is the only type notion.
- **Hierarchy search** is a substring match with a Match Case toggle
  (`HierarchyWindow.cpp:1218-1260`, cached per query at `:1243`).
- **Add Component search** is `name.find(search)` over display names
  (`ComponentAddDialog.cpp:239`), unranked.
- **Settings.** Preferences (`EditorSettingsWindow.cpp:39-44`, 4 categories) and Project
  Settings (`ProjectSettingsWindow.cpp:134-146`, 8 categories) use static category arrays with
  keyword strings. `settingsui::categoryMatches` (`SettingsCategoryList.h`) filters categories
  only, never rows. Separately, MCP has a real settings table: `struct Setting {key, type,
  description, values, get, set, preference}` (`mcp/tools/ProjectTools.cpp:491-500`) and
  `settingsTable()` (`:817`). It has 34+ keys and is used by `get_project_settings` and
  `set_project_settings`.
- **Persistence locations.** Per-user editor files live in `AppSettings::getConfigDirectory()`
  (`settings.yaml`, the layout ini). Per-user, per-project state lives in
  `.jasperengine/user/` (`Workspace.h`). The generated `.gitignore` ignores all of
  `.jasperengine/` (`Project.cpp:5232-5237`), so nothing there is shared.
- **MCP** already has `focus_window`, `list_assets {directory,type,pattern,query,...}`,
  `find_entities` and `search_files` (grep). It has no palette search, layout, label or shortcut
  tools.

## 4. Reference (OpenEngine, MIT, read-only)

Paths are relative to `external/OpenEngine/Apps/Editor/`. The licence is MIT, "Copyright (c) 2026
Gamecrafters Guild Limited and contributors". Copied functions keep that line in a header
comment and get a `NOTICE.md` entry.

| What | File | Take | Improve / differ |
|---|---|---|---|
| Fuzzy scorer | `Source/UI/UniversalSearchProvider.cpp:74-130` (`DamerauDistance`, `SubsequenceScore`), `:384-474` (`NormalizeForSearch`, `FuzzyScore`), `:495-535` (`ParseQuery`), `:45-60` (`KindPriority`), `:870-930` (rank, sort, dedupe; `kMaxResults = 100` at `:34`) | **Port almost verbatim.** Pure `std::string_view` code. | Adds a Filter mode (§6.1, R14). Our scopes and kinds (`+` components). Recents boost. |
| Palette controller | `Source/Search/UniversalSearchController.cpp:100-160` | Scope chips, snapshot-on-open, versioned cancel | ImGui popup instead of their `SearchDialog` |
| Layout presets | `Source/EditorApplication_LayoutPresets.cpp` (import with unique names `:119-185`, dirty-switch prompt `:535-588`, remove keeps current layout `:611-638`), `Source/Docking/LayoutPresetController.cpp` | UX rules | ImGui ini text (`SaveIniSettingsToMemory`/`LoadIniSettingsFromMemory`) instead of DockNode JSON |
| Asset search fields, type-ahead | `Source/Panels/AssetsPanel.cpp:75-125` (750 ms repeat window, A–Z/0–9), `:142-143` (16 px size steps) | Type-ahead rules, size step | Unity tokens instead of a field drop-down |
| Label palette | `Source/UI/EditorTags.cpp:8-22` | The 9 default colours | Stored in `project.yaml` with labels in S04 metas, not a JSON settings store |
| Smart folders | `Include/UI/SmartFolder/SmartFolder.h`, `Source/UI/SmartFolder/SmartFolderFilterEvaluator.cpp` | Filter kinds (type, name, regex, tag), AND/OR, project vs global scope | One query grammar for the search box and saved searches |
| Settings registry | `Include/Editor/Settings/EditorSettingsRegistry.h`, `Source/Editor/Settings/EditorSettingsRegistry.cpp:47-95` | Descriptor rows with `SearchKeywords`; reject unwired rows | Built from our existing MCP `settingsTable()` |
| Bookmarks | `Include/Panels/BookmarksPanel.h:37-52` (`Bookmark {Type, Reference, Name, Order}`), `Source/Panels/BookmarksPanel.cpp` | Kinds, drag in, reorder, drop to navigate | GUID + path hint (S04), saved entity id + `EntityPath` hint |
| Shortcut catalog | `Include/Editor/Shortcuts/EditorShortcuts.h:16-111` | `{group, name, context, default, second default, readOnly}`, `FindShortcutConflict`, the capture-suppression latch | Commands carry their action. Menus and Help are generated from them. |

## 5. Requirements

### Functional — shortcuts and commands
- **R1 Help bug fix, shipped first.** As a standalone change before the rest of S25: the
  Help > Keyboard Shortcuts text lists `Q Hand Tool`, `W/E/R Move/Rotate/Scale Tool`,
  `T Rect Tool`, `X Toggle Global/Local`, `Ctrl+2 Game`, `Ctrl+Alt+P Step`,
  `Home Frame All (3D)`, `Numpad 1/3/7 (Ctrl: opposite) Front/Right/Top views`, and
  `[ / ] (Shift: strength) Terrain brush size`. R5 later replaces the text with a generated list.
- **R2 Catalog.** `editor/shortcuts/CommandCatalog` holds every editor command that has a
  shortcut or a menu item. Each entry has a stable id (`edit.duplicate`), group, name, context,
  a primary chord, an optional secondary chord, an optional macOS primary, `readOnly`, `run`,
  `enabled` and an optional `AppMenuCommand`. Every chord in §6.3 is registered with exactly the
  behaviour it has today, including gating.
- **R3 One dispatcher.** `ShortcutDispatcher::process()` runs once per frame and replaces every
  chord check in `App.cpp:1904-2010`, `MainToolbar::handleShortcuts` and
  `SceneWindow.cpp:1630-1705`. Precedence: the focused context's entries first, then `Editor`,
  then `Global`. Single-key chords never fire while `io.WantTextInput` is set, while the Code
  Editor is focused, or while the game has keyboard focus in Play (`MainToolbar.cpp:226-228`).
  Modifier chords do not fire while a modal popup is open. Each press fires at most one command.
  Local keys stay local and are not catalog commands: Esc in dialogs, Console arrow keys, the
  text editor's bindings, and Play input.
- **R4 Menus and tooltips are generated.** `buildMenuModel()` gets each shortcut string from
  `CommandCatalog::menuShortcutText(id)`. The Hierarchy create entry and toolbar tooltips use
  the catalog too. After this change, `App.cpp`, `MainToolbar.cpp`, `HierarchyWindow.cpp` and
  `SceneWindow.cpp` contain no chord string literal and no `IsKeyPressed` call with a letter,
  digit or function key outside text-input code. A CI source check enforces this.
- **R5 Help > Keyboard Shortcuts** opens a generated, filterable, read-only reference grouped by
  group. It includes the read-only entries for mouse navigation and Ctrl+wheel, and has an
  "Edit Shortcuts..." button. `docs/editor/keyboard-shortcuts.md` is generated from the same
  function and checked in. A test fails when the file is out of date.
- **R6 Rebinding.** Edit > Shortcuts... opens the **Shortcuts** window (Unity naming). It has a
  search field (Filter mode), a group list, and a table with Command, Shortcut and Context
  columns. Click a cell to capture a chord, then Enter or click-away to keep it, Esc to cancel,
  and Backspace to clear it. Per-row and global "Reset to Default". `readOnly` rows cannot be
  edited. While capturing, the dispatcher is suppressed and the captured key does not leak to
  another command (the latch from OpenEngine's `SetShortcutCaptureActive` /
  `NoteShortcutCaptureConsumedKey`).
- **R7 Conflicts.** Two bindings conflict when they have the same chord and the same context.
  A chord in `Global`/`Editor` that is also used in a panel context is reported as *shadowed*
  (shown, not blocking). On capture, a conflict opens "Ctrl+D is already used by Edit / Duplicate"
  with [Reassign] [Cancel]. Reassign unbinds the other command. A conflict in a loaded override
  file is resolved deterministically: the binding equal to the catalog default wins, otherwise
  the earlier catalog entry. One Console warning names the commands and Edit > Shortcuts.
- **R8 Overrides persist** per user in `<configDir>/shortcuts.yaml` (§6.6), written with S03
  `writeFileAtomic` under the settings lock. An unknown command id or an unparsable chord is
  kept in the file and ignored with one warning, never dropped. Native menus are rebuilt with
  the new strings in the same frame.
- **R9 macOS.** "Ctrl" in the catalog means Command on macOS (ImGui `ImGuiMod_Ctrl` with
  `ConfigMacOSXBehaviors`, and `BackendMac.mm:360`). It is shown as "Cmd+S". `macPrimary`
  covers real differences, for example Redo = Cmd+Shift+Z (`App.cpp:499-503`). While the native
  menu is active, menu-bound chords are dispatched only by the NSMenu key equivalent. No chord
  fires twice on any platform.

### Functional — Search palette
- **R10 Opening.** Ctrl/Cmd+K (catalog `edit.searchAll`, menu Edit > Search All...) opens a
  centred popup with an autofocused query field. It also works while the Code Editor is
  focused, and not while a modal is open. Up/Down/PageUp/PageDown move the selection, Enter
  runs it, Esc closes, and Tab cycles the scope chips.
- **R11 Providers and scopes.** Snapshotted when the palette opens:
  - **Commands** (`>`): every enabled-or-disabled command item of `buildMenuModel()`, walked
    recursively. The label is the item, the detail is the menu path ("Edit > Duplicate"), and
    dynamic items (Recent Projects, GameObject create) are included. Catalog commands that have
    no menu item (tools, Scene view views) are added. Disabled items are shown dimmed and do
    not run.
  - **Panels** (`/`): the dockable windows (Scene, Game, Inspector, Hierarchy, Project, Console,
    Animation, Terrain, MCP Activity, Bookmarks). Running one opens and focuses the window.
  - **Settings** (`:`): every registry row (R30). Running one opens Preferences or Project
    Settings at the row's page, scrolls to the row and flashes it.
  - **GameObjects** (`@`): every GameObject of the open scenes and prefab stage. The detail is the
    `EntityPath` and scene name. Enter selects and pings it in the Hierarchy. Ctrl+Enter also
    frames it.
  - **Components** (`+`): the Add Component list. Enter adds the component to the selection
    through the same undoable path as Add Component. Dimmed when nothing is selected.
  - **Assets** (`asset:`, the rest of the query uses the Project grammar R20): S04 assets and
    folders. Enter selects and pings in the Project window. Ctrl+Enter opens it, as a
    double-click does.
  An empty query shows up to 8 **Recent** entries, then commands and panels (as OpenEngine does).
- **R12 Scoring.** Ranked mode: `score = max(FuzzyScore(q, label), FuzzyScore(q, searchText) − 80)
  + KindPriority + entryPriority + recentBoost`, where `recentBoost = 150 − 15·rank` for the
  8 most recent entries. Ties break by kind priority, then label. Results are de-duplicated by
  (kind, label, detail) and capped at 100. Matched characters are highlighted. The shortcut hint
  is right-aligned on command rows.
- **R13 Recents.** Running an entry records its stable key (`command:<id|AppMenuCommand+payload>`,
  `asset:<guid>`, `gameobject:<sceneGuid>:<entityId>`, `setting:<key>`, `panel:<name>`,
  `component:<type>`). The 20 most recent keys are kept per project in
  `.jasperengine/user/organisation.yaml`. Stale keys are skipped silently.
- **R14 Reuse.** Add Component ranks with the same scorer, using the display name as the label
  and category plus description as the search text. When searching it shows a flat ranked list.
  The Hierarchy and Project filters and the settings search use **Filter mode** (§6.1).
  Hierarchy search also accepts `t:<Component>` (Unity), matching GameObjects that have that
  component by display name. Match Case keeps today's exact-substring behaviour.

### Functional — layouts
- **R15 Presets.** A preset is `{name, uiScale, panels (open state by window name), ini}`, where
  `ini` is `SaveIniSettingsToMemory()` with the document-window sections (`###Scene<id>`,
  `###<filepath>`) removed. The built-in **Default** is `buildDefaultLayout()`: it is read-only
  and cannot be renamed or deleted. User presets live in `<configDir>/layouts/<slug>.layout.yaml`,
  are user-wide, and are listed in every project (Unity).
- **R16 Menus.** Window > Layouts and the toolbar Layout menu list the presets (the active one
  is checked), then: Save Layout..., Rename Layout ▸, Delete Layout ▸, Save Layout to File...,
  Load Layout from File..., Reset Layout (applies Default). The item ids are catalog commands,
  so they are searchable and bindable.
- **R17 Apply.** A preset is applied between frames, before `NewFrame`:
  `ClearIniSettings` + `LoadIniSettingsFromMemory`, then panel open states, then
  `buildDockspace(false)` to recover the central node, then the scale fix (the
  `rescaleRestoredLayout` ratio between `uiScale` and the current scale), then document tabs
  are force-docked into the central node. An ini whose dock root is missing after one frame,
  or a file that fails to parse, falls back to Default with a Console warning that gives the
  reason. Window names the build does not know are kept in the text and ignored.
- **R18 Dirty and switching.** "Dirty" means the current filtered ini differs from the snapshot
  taken one frame after the active preset was applied or saved. Switching away from a dirty
  user preset asks "Layout 'X' has unsaved changes" with [Save] [Don't Save] [Cancel]. Save
  overwrites X. For Default, Save becomes Save As... with a name. Re-selecting the active dirty
  preset reloads it. Deleting the active preset keeps the current layout and clears the check
  mark.
- **R19 Working layout per project.** `io.IniFilename = nullptr`. The editor saves the working
  layout (same format plus `activePreset`) to `.jasperengine/user/layout.yaml` when
  `io.WantSaveIniSettings` is set (debounced to 1 s) and when the project closes. The first time
  a project opens without one, it is seeded from the global `editor_layout_v2.ini`. With no
  project open, the global file is still used. Import gives names unique suffixes " (2)",
  " (3)" (OpenEngine's rule). Export writes one file per preset, or an "export all" bundle
  `{layoutBundleVersion: 1, presets: [...]}`.

### Functional — Project window, labels, saved searches, bookmarks
- **R20 Search grammar** (one parser, `editor/search/AssetQuery`). Terms are separated by
  spaces. `"quoted phrase"` is an exact substring. Bare words match the name in Filter mode.
  `t:<type>` matches S04 type ids or Unity aliases (Texture2D, Material, Scene, Prefab, Model,
  AudioClip, Font, Shader, Script, TextAsset, Folder). Also `l:<label>`, `ext:<ext>`,
  `path:<substring of project-relative path>`, `re:<ECMAScript, icase, on name>`,
  `ref:<path|guid>` (assets that reference it, S04 index), and `missing:` (assets with missing
  references, S04 tracker). Terms with the same key OR together, terms with different keys AND
  together, and ` OR ` (upper case) joins whole groups. Unknown keys are bare text.
  `re:` patterns that fail to compile match nothing and show an inline error.
- **R21 Project search** evaluates the grammar over an in-memory asset index (S04
  `AssetDatabase` entries plus folders, kept current by its change events and S05). It never
  walks the filesystem. A scope bar shows **All** / **In '<folder>'** (the default is the
  current folder, as today). Results are sorted by score, then name. The first 2000 are shown,
  with "N more results" after them. The toolbar has Unity's **Search by Type** and
  **Search by Label** drop-downs (checkbox lists that edit the `t:`/`l:` tokens) and a
  **Save Search** star.
- **R22 Labels.** An asset has 0–32 labels. A label is 1–32 characters from `[A-Za-z0-9 _.-]`
  after trimming, compared case-insensitively, and keeps the casing it was first used with.
  - Labels live in the S04 meta `labels:` list (sorted). For in-file assets (`.scene`,
    `.prefab`, `.material`) they live in a top-level `labels:` key, written with the same
    line-edit helper as `writeGuid`, and `Stream` encodes/decodes it so saving an open scene
    keeps it.
  - Editing labels does not change a scene's dirty flag.
  - Label edits are one undo step each on the project history (S24 names: "Set Asset Labels",
    "Rename Label").
- **R23 Label UI.** The Project window context menu has **Labels ▸**: tri-state checkboxes over
  the selection, and New Label.... An "Asset Labels" strip is added wherever the Inspector shows
  a selected asset. Grid items get a colour dot per label (up to 3, then "+N"). List rows get
  chips.
  - Label colours are set in Project Settings > Editor > **Asset Labels**: colour, rename (rewrites
    every meta and in-file key in one undo step), and delete.
  - The project has 9 default colours (`EditorTags.cpp` values). A label with no entry gets a
    palette colour picked by a stable hash of its lower-case name.
- **R24 Type-ahead.** When the Project listing is focused, no text field is active and no
  modifier is held, A–Z/0–9 build a prefix (750 ms window) and select the first item whose name
  starts with it, scrolled into view. Repeating the same single key inside the window cycles
  through the matches.
- **R25 Ctrl/Cmd+wheel** over the listing changes the icon size in 16 px steps, clamped to
  [`MIN_GRID_ICON_SIZE`, `THUMBNAIL_SIZE`], and saves it through `AppSettings` once the wheel
  has been idle for 500 ms. A plain wheel still scrolls. The gesture is a read-only catalog row.
- **R26 Saved searches.** A saved search is `{name, query, folder?, scope: user|project}`.
  - Favorites at the top of the Project tree lists the built-ins All Materials, All Models,
    All Prefabs, All Scenes and All Scripts (read-only, as in Unity), then the user's saved
    searches, then bookmarked folders and assets.
  - Clicking a saved search runs it. Right-click offers Rename, Edit, Delete and "Share with
    Project".
  - Edit opens a structured editor with rows for Type, Name contains, Regex, Label, Extension
    and Path, plus an Any/All toggle. It serialises to the R20 grammar, so a hand-typed query
    and an edited one evaluate the same way. Queries that the structured editor cannot express
    open as raw text.
  - Saved-search operations are undoable on the project history.
- **R27 Token table.** Grammar keys are registered in a table (`AssetQuery::registerToken`) so S29
  can add `vcs:` without touching the parser.
- **R28 Bookmarks** (Window > General > Bookmarks). A bookmark is one of asset, folder or scene
  (stored as GUID + path hint, S04), or GameObject (scene GUID + saved entity id + `EntityPath`
  hint).
  - Add a bookmark by dragging from the Project window or the Hierarchy, or with the
    "Add to Bookmarks" context item on both.
  - Rows can be dragged to reorder. Double-click navigates: it opens the scene if needed, then
    selects and pings.
  - Dragging a bookmark onto the Scene view or Hierarchy behaves like dragging the asset itself.
  - An unresolved bookmark shows dimmed as "(Missing)" with Locate/Remove.
  - Bookmarks are per user, per project. Add, remove and reorder are undoable on the project
    history.

### Functional — settings
- **R29 Registry.** `editor/settings/SettingsRegistry` replaces the MCP-local `settingsTable()`
  (`ProjectTools.cpp:491-500`, `:817-1554`). Every key keeps its name, type, get and set. Each row gains
  page, section, label, tooltip, keywords and an optional custom `draw`. Pages are registered
  with scope (Project | Preference), name and order. The category arrays at
  `EditorSettingsWindow.cpp:39` and `ProjectSettingsWindow.cpp:134` are deleted, and both
  windows list pages from the registry. Registration rejects a row with no label or no get/draw,
  as OpenEngine's `RegisterCategory` does.
- **R30 Row search.** The search field in Preferences and Project Settings filters pages to those
  with a matching row (Filter mode over label, keywords and key). On a page it hides non-matching
  rows, keeps section headers that have a match, and highlights the matched text. Clearing the
  search restores the page exactly. Rows drawn by custom code call `settingsui::beginRow(key)`,
  which reports visibility and highlight. In debug builds, a drawn row that is not registered
  asserts.

### Platform & backends
- **R31** The editor is desktop-only: Windows, Linux (X11 and Wayland) and macOS. Nothing in S25
  touches the runtime, the exporters or the graphics backends. All file writes go through
  S03 `writeFileAtomic`. Paths are stored with `/` separators.

### Performance (desktop reference machine)
- **R32** With the palette closed, S25 costs ≤ 0.05 ms per frame (dispatcher only) and holds no
  snapshot.
- **R33** Building the palette snapshot for 20k assets, 10k GameObjects, 300 commands and
  400 settings takes ≤ 25 ms on the main thread.
- **R34** Scoring runs on an S06 job with a version counter, and a newer keystroke cancels it.
  20k entries are scored in ≤ 30 ms p95. The UI keeps the previous results until new ones
  arrive and never blocks.
- **R35** A Project search over a 50k-asset index takes ≤ 30 ms. A Hierarchy filter over 10k
  GameObjects takes ≤ 8 ms per query change, and is recomputed only when the query or tree
  changes (as today). Add Component ranking takes ≤ 1 ms.
- **R36** Applying a layout preset finishes in ≤ 2 frames. Saving the working layout does not
  happen more than once per second.

### Editor UX (human)
- **R37** Unity names throughout: Search (Edit > Search All...), Shortcuts (Edit > Shortcuts...),
  Window > Layouts with Unity's item names, Favorites, Search by Type / Search by Label /
  Save Search, Asset Labels.
- **R38** Every new window and modal is reachable from a menu, is in the palette, and has a
  tooltip on each icon-only control. Disabled actions say why in their tooltip.

### Agent UX (MCP, CLI)
- **R39** New tools (§6.7): `search_editor`, `run_command`, `list_shortcuts`, `set_shortcut`,
  `layout_presets`, `asset_labels`, `saved_searches`, `bookmarks`.
  - `list_assets` gains `search` (the R20 grammar). Its existing `query` keeps its
    name-substring meaning.
  - `get_project_settings` gains `search`, which returns `{key, page, label, scope}` per
    match.
  - All mutating tools are undoable where the UI action is, and report `files_written`.
- **R40** `docs/mcp/TOOLS.md`, `docs/mcp/guide/editor-integration.md` and the agent guide
  (`McpInstructions.cpp`) document the tools, the R20 grammar and the command ids. The S31 CLI
  reaches them as `jasperengine-editor call <tool>`.

### Data & compatibility
- **R41** All new keys are optional. Old projects load unchanged and get Default as their working
  layout, seeded per R19. `labels:` in a meta is opaque to S04 and is kept by older S04-aware
  editors (S04 R36/AC15). New top-level keys in `.scene`/`.prefab`/`.material`/`project.yaml`
  need no format-version bump: S03 keeps unknown keys. Each new file starts with its own version
  key (`shortcutsVersion`, `layoutVersion`, `organisationVersion`, `layoutBundleVersion`)
  set to 1.

## 6. Design constraints & decisions

### 6.1 Scorer (`editor/search/SearchScorer.{h,cpp}`, port)
```cpp
namespace jasperengine::editor::search {
std::string normalize(std::string_view);                 // NormalizeForSearch: camelCase/acronym split, lower, alnum
int  damerau(std::string_view a, std::string_view b, int cutoff);   // verbatim
int  subsequence(std::string_view q, std::string_view c);           // verbatim
int  fuzzyScore(std::string_view q, std::string_view c);            // verbatim tiers: 1200 exact, 1000 prefix, 800 substring, per-word
enum class MatchMode { Rank, Filter };
// Filter: true when the normalized query is a substring, or every query word is a word-prefix
// or within the Damerau cutoff of some word. A bare subsequence hit does NOT count.
bool filterMatch(std::string_view normQuery, std::string_view normCandidate);
}
```
Filter mode exists because tree filters show every included row in tree order. Subsequence
hits ("pl" matching "sPotLight") would flood them. The thread-local scratch buffers stay.

### 6.2 Palette (`editor/search/SearchPalette`, `SearchProviders`)
`SearchEntry {kind, key, label, detail, searchText, shortcut, priority, enabled, payload}`.
Each provider fills a vector when the palette opens. `SearchEngine::query(const Snapshot&,
std::string_view, Version)` is pure and is what the unit tests call. Kind priorities follow
OpenEngine's values: Command 70, Panel 60, Setting 50, Component 45, GameObject 40, Asset 35.
Running an entry goes through the same function the menu, Hierarchy or Project action uses
(`executeMenuCommand`, `showEditorWindow`, the Add Component callback). The palette does not
re-implement behaviour.

### 6.3 Catalog (`editor/shortcuts/CommandCatalog.{h,cpp}`, `ShortcutDispatcher`, `ShortcutOverrides`)
```cpp
enum class ShortcutContext : uint8_t { Global, Editor, SceneView, Hierarchy, Project, Console };
struct Chord { ImGuiKeyChord value = 0;                      // ImGuiMod_Ctrl | ImGuiKey_S
               static std::optional<Chord> parse(std::string_view);   // "Ctrl+Shift+S", "F2", "Numpad7"
               std::string format(bool mac) const; };
struct CommandDef {
    std::string_view id, group, name;    // "edit.duplicate", "Edit", "Duplicate"
    ShortcutContext context = ShortcutContext::Editor;
    Chord primary, secondary, macPrimary;
    bool readOnly = false, agentSafe = true, repeat = false;
    std::function<void(const CommandArgs&)> run;
    std::function<bool()> enabled;       // also feeds menu enabled state
    std::optional<uint32_t> menuCommand; // AppMenuCommand
};
class CommandCatalog {
public:
    void add(CommandDef);
    const CommandDef* find(std::string_view id) const;
    std::span<const Chord> bindings(std::string_view id) const;      // effective
    const CommandDef* findConflict(std::string_view id, Chord, bool* shadowed = nullptr) const;
    bool setBindings(std::string_view id, std::vector<Chord>);       // false + reason on readOnly
    void resetBindings(std::string_view id);  void resetAll();
    std::string menuShortcutText(std::string_view id) const;
    std::string markdownReference() const;   // R5, also docs/editor/keyboard-shortcuts.md
};
```
`Global` fires everywhere, including the Code Editor. `Editor` covers any editor window except
the Code Editor, and not while typing. The panel contexts apply when that window is focused.
Initial entries carry today's bindings: `file.save` Ctrl+S, `file.saveAll` Ctrl+Shift+S,
`file.openProject` Ctrl+O, `file.buildSettings` Ctrl+Shift+B, `edit.undo` Ctrl+Z, `edit.redo`
Ctrl+Y + Ctrl+Shift+Z (mac primary Cmd+Shift+Z), `edit.selectAll`/`cut`/`copy`/`paste` Ctrl+A/X/C/V
(Hierarchy and SceneView), `edit.pasteAsChild` Ctrl+Shift+V, `edit.duplicate` Ctrl+D, `edit.rename`
F2, `edit.delete` Delete + Backspace, `edit.frameSelected` F, `edit.searchAll` Ctrl+K (new,
Global), `edit.play` Ctrl+P, `edit.pause` Ctrl+Shift+P, `edit.step` Ctrl+Alt+P, `playmode.playOrResume`
F5, `playmode.pauseOnly` F6, `playmode.stop` F7, `gameobject.createEmpty` Ctrl+Shift+N,
`gameobject.createEmptyChild` Alt+Shift+N, `component.add` Ctrl+Shift+A, `window.scene`…`window.animation`
Ctrl+1…6, `window.console` Ctrl+Shift+C, `help.documentation` F1, `tools.hand/move/rotate/scale/rect`
Q/W/E/R/T, `tools.toggleGlobalLocal` X, `sceneView.frameAll` Home, `sceneView.view{Front,Back,Right,Left,Top,Bottom}`
Numpad1/3/7 with Ctrl for the opposite view (today's `SceneWindow.cpp` mapping, kept as it is), `terrain.brush{Smaller,Larger,Weaker,Stronger}`
[ / ] / Shift+[ / Shift+] (repeat). Read-only rows: Scene view mouse navigation, Ctrl+wheel resize.
Streams S15/S23/S26/S27 register their new commands here instead of adding `IsKeyPressed` calls.

### 6.4 Layouts (`editor/layout/LayoutPresets.{h,cpp}`)
Pure functions are unit-tested without a GPU: `filterDocumentWindows(ini)`, `uniqueName(name,
existing)`, `slug(name)`, preset YAML round-trip, bundle import. The App calls
`requestApply(preset)`, and `App::beginFrame` performs it before `NewFrame` (R17). Layout
operations are not in undo history (Unity), and Delete asks for confirmation.

### 6.5 Organisation (`editor/organisation/{AssetLabels,SavedSearches,Bookmarks}`)
`AssetLabels` writes through the S04 meta API (`AssetMeta::setSection("labels", …)`) or the in-file
line-edit helper, and updates the index used by R21. `AssetQuery` (parser and evaluator) is
pure over an `AssetIndexView` interface, so tests feed it a fake index.

### 6.6 Files
```yaml
# <configDir>/shortcuts.yaml
shortcutsVersion: 1
overrides: {tools.rect: ["T"], edit.redo: ["Ctrl+Y", "Ctrl+Shift+Z"], window.console: []}
# <configDir>/layouts/animation.layout.yaml  and  <project>/.jasperengine/user/layout.yaml
layoutVersion: 1
name: Animation            # working layout adds: activePreset: Animation
uiScale: 1.0
panels: {Hierarchy: true, Inspector: true, Project: true, Console: true, Animation: true, Game: true}
ini: |
  [Window][Inspector] ...
# <project>/.jasperengine/user/organisation.yaml
organisationVersion: 1
bookmarks: [{kind: asset, guid: 4b1e..., path: textures/hero.png, name: hero}, {kind: gameobject, scene: 9c1d..., entity: 42, entityPath: Player/Gun}]
savedSearches: [{name: Big Textures, query: "t:texture l:Hero", folder: textures}]
paletteRecent: ["command:edit.duplicate", "asset:4b1e..."]
# project.yaml (shared through VCS)
assetLabels: [{name: Hero, color: "#FF5252"}]
savedSearches: [{name: Enemies, query: "t:prefab l:Enemy"}]   # scope: project
```

### 6.7 MCP tools (new `editor/mcp/tools/OrganisationTools.cpp`, `registerOrganisationTools`)
- `search_editor {query, scopes?: [command,panel,setting,gameobject,component,asset], limit=20 (1-100)}`
  → `[{kind, key, label, detail, shortcut, enabled, score}]`. Read-only. Uses the same snapshot
  and engine as the palette.
- `run_command {id | key, payload?}` runs a catalog or palette command through the menu path.
  It refuses commands whose `agentSafe=false` (exit, new/open project, clear recent projects,
  empty trash) and disabled commands, and says why.
- `list_shortcuts {group?, context?, conflicts_only=false}` →
  `[{id, group, name, context, chords, default_chords, overridden, read_only, menu_path, conflicts, shadowed_by}]`.
- `set_shortcut {id, chords: [..] | null (reset), force=false}` returns a conflict error
  unless `force` is set. With `force`, it reassigns as the UI does.
- `layout_presets {action: list|apply|save|rename|delete|export|import|reset, name?, new_name?, path?}`
  → `{presets:[{name, builtin, active, dirty}], files_written}`.
- `asset_labels {action: get|set|add|remove|list|rename_label|set_color, assets?: [path|guid], labels?, label?, new_name?, color?}`.
- `saved_searches {action: list|save|delete|run, name?, query?, folder?, scope?}`. `run` returns
  the assets (same shape as `list_assets`).
- `bookmarks {action: list|add|remove|move|resolve, ref?: path|guid|entityRef, index?}`.

## 7. Acceptance criteria

Unit tests are in `editor/tests/{search,shortcuts,layout,organisation,settings}/` (target
`jasperengine-editor-tests`, S02). MCP tests are in `tools/mcp/tests/test_search_organisation.py`.
- **AC1 (R1)** Manual and `test_search_organisation.py::test_help_lists_tools`: Help > Keyboard
  Shortcuts contains "Q", "Hand Tool", "T", "Rect Tool", "X", "Toggle Global/Local", and
  contains no line pairing T with global/local. Before the catalog lands this is checked on the
  static text. After it lands, the check runs against `list_shortcuts`.
- **AC2 (R2, R3)** `ShortcutDispatcher.CharacterizesTodaysBindings`: a fake input state per
  §6.3 entry fires exactly the expected command once. `W` while `WantTextInput`, `Q` while the
  Code Editor is focused, `Ctrl+D` while a modal is open, and `T` while the game has keyboard in
  Play fire nothing.
- **AC3 (R4)** `tools/ci/check-hardcoded-shortcuts.py` passes on the tree and fails on a fixture
  file containing `menuShortcut(..., "Ctrl+S")` or `IsKeyPressed(ImGuiKey_Q`.
- **AC4 (R5)** `CommandCatalog.MarkdownReferenceMatchesDocs`: `markdownReference()` equals
  `docs/editor/keyboard-shortcuts.md` byte for byte.
- **AC5 (R7)** `CommandCatalog.DetectsConflictAndShadow`: binding `edit.rename` to Ctrl+D
  reports `edit.duplicate`. Binding a SceneView entry to Ctrl+S reports shadowing, not a
  conflict. `ShortcutOverrides.ConflictingFileResolvesToDefault`: the default holder wins, and
  exactly one warning is logged.
- **AC6 (R8)** `ShortcutOverrides.RoundTripKeepsUnknown`: overrides save and load. An unknown
  id and a bad chord survive a save, and `[]` unbinds.
  `test_search_organisation.py::test_set_shortcut_updates_menu`: after
  `set_shortcut {id: tools.rect, chords: ["Y"]}`, `list_shortcuts` shows Y, the menu model
  shortcut changes, and a conflicting set without `force` fails.
- **AC7 (R9)** Manual on macOS with the native menu: Cmd+S saves once (one "Saved" log line),
  Cmd+Shift+Z redoes once, and the Edit menu shows ⌘⇧Z. The same check runs on Windows with
  Ctrl.
- **AC8 (R6)** Manual: in Edit > Shortcuts..., capture Ctrl+S on "Duplicate". The conflict modal
  appears, and the scene is not saved while capturing. Cancel leaves both bindings unchanged.
- **AC9 (R12, §6.1)** `SearchScorer.PortedSemantics`: exact > prefix > substring >
  per-word. "brnach" matches "branch". "gmo" fails Filter but passes Rank for "GameObject".
  "rigid body" matches "Rigidbody2D" after normalisation. The tier values equal OpenEngine's
  (1200/1000/800, 180/140/35 penalties).
- **AC10 (R11, R12, R13)** `SearchEngine.ScopesRanksRecents` on a fake snapshot: `>dup` returns
  only commands, with Duplicate first and "Ctrl+D" as its shortcut. `@gun` returns
  `Player/Gun`. An empty query lists recents first. Results cap at 100 and contain no
  duplicates.
- **AC11 (R10, R11, R39)** MCP `test_palette_search`:
  - `search_editor {query: "inspector"}` has a panel result and a command result.
  - `search_editor {query: ":gravity"}` returns the physics gravity row.
  - `search_editor {query: "+rigid"}` returns Rigidbody.
  - `run_command {id: "window.console"}` makes `editor_status` report the Console as focused.
  - `run_command {id: "file.exit"}` is refused.
- **AC12 (R14)** `ComponentAddDialogSearch.RanksTypos`: "rigdbody" lists Rigidbody first.
  `HierarchyFilter.TypeToken`: `t:Camera` keeps only camera GameObjects and their ancestors.
  Match Case still behaves as an exact substring.
- **AC13 (R15, R17, R19)** `LayoutPresets.FilterRoundTripUnique`: the scene and code sections are
  removed, YAML round-trips, and importing "Animation" twice yields "Animation (2)". MCP
  `test_layout_presets`:
  - save "A", change a panel (`focus_window` terrain), then apply Default and apply "A";
    `editor_status` reports the same open panels as at save time, and the working layout file
    exists in `.jasperengine/user/`;
  - a corrupt preset file falls back to Default with the warning in `read_console`.
- **AC14 (R18)** Manual: move a splitter in preset A and pick B. The prompt appears. Don't Save
  switches. Re-picking A restores the saved splitter position.
- **AC15 (R20, R27)** `AssetQuery.Grammar`, table-driven over a fake index: `t:texture t:model`
  is OR, `t:texture l:Hero` is AND, `A OR B` works, quoted phrases work, a bad `re:` matches
  nothing with an error, the Unity aliases resolve, `ref:`/`missing:` call the index, and a
  registered test token works.
- **AC16 (R21)** `AssetSearchSlow.FiftyThousandUnder30ms`: under 30 ms, with a CI threshold of
  60 ms. MCP `test_project_search`: `list_assets {search: "t:texture l:Hero"}` returns exactly
  the two labelled fixtures. The unit test runs on a fake `AssetIndexView`, which proves that
  search does no filesystem walk.
- **AC17 (R22, R23)** `AssetLabels.MetaAndInFile`: labels go into `hero.png.meta` `labels:` and
  into a `.prefab` top-level key. The S04 sections survive. Saving the open scene keeps its
  labels. Undo restores the previous labels in one step. MCP `test_asset_labels`:
  - `add`, then `list_assets {search: "l:Hero"}`, then `rename_label` rewrites every file and
    `files_written` lists them;
  - `undo` restores the old name.
- **AC18 (R24, R25)** Manual: in a folder with `apple`, `avocado` and `banana`, typing "a"
  selects `apple`, "a" again within 750 ms selects `avocado`, and "b" selects `banana`. Ctrl+wheel
  steps the size by 16 between 16 and the maximum, and the size persists after a restart.
- **AC19 (R26)** `SavedSearches.StructuredRoundTrip`: structured rows give a query string, which
  parses back to the same rows. MCP `test_saved_searches`: `save` then `run` returns the same
  set as `list_assets {search}`. A project-scope search lands in `project.yaml`. A user-scope
  search lands in `organisation.yaml`.
- **AC20 (R28)** `Bookmarks.ResolveAfterMove`: after an S04 move, an asset bookmark resolves to
  the new path. A GameObject bookmark resolves by entity id, then by `EntityPath`, else reports
  Missing. MCP `test_bookmarks`: add, move and list keep the order, and `undo` re-adds a removed
  bookmark.
- **AC21 (R29)** `SettingsRegistry.CoversMcpKeys`: every key that `get_project_settings` returned
  before S25 (a fixture list) exists with a label and page, and get/set behave identically
  (the existing `test_project.py` settings tests pass unchanged). An unwired row is rejected.
- **AC22 (R30)** `SettingsSearch.FiltersRows`: "vsync" leaves Quality and General, showing only
  their VSync rows. Manual: the matches are highlighted, and clearing restores the page.
- **AC23 (R32-R36)** `SearchEngineSlow.TwentyThousandEntries` (≤ 30 ms, CI threshold 60 ms) and
  `SearchSnapshotSlow.BuildUnder25ms` (CI threshold 50 ms). Manual (S15 CPU profiler):
  the dispatcher costs ≤ 0.05 ms per frame with the palette closed, and a layout apply completes
  within 2 frames.
- **AC24 (R41)** Load the `tools/fixtures` projects, which have no S25 files. There are no
  warnings, and each opens with the seeded working layout. `tools/smoke-test.sh` stays green.

## 8. Dependencies & conflicts

- **Depends on:**
  - S01 (C++20: `std::span`, `starts_with`), S02 (editor test target, slow label), S03
    (`writeFileAtomic`, the settings lock, unknown-key preservation), S04 (GUIDs, meta
    `labels:` section, line-edit helper, dependency index for `ref:`, missing tracker,
    `AssetOps` change events);
  - S05 (index freshness after external changes), S06 (job for scoring), S24 (undo names on
    the project history), S31 (CLI `call`).
- **Feeds:** S15, S23, S26 and S27 register commands, panels and settings through the catalog
  and registry. S29 adds `vcs:` via R27. S26 camera bookmarks are separate from R28.
- **Merge hotspots:**
  - `editor/App.cpp`: `buildMenuModel`, `executeMenuCommand`, key handling at 1904-2010, ini
    setup at 1595 (also S03, S05, S15, S23, S24, S26);
  - `editor/window/MainToolbar.cpp` (S26);
  - `editor/window/ProjectWindow.{h,cpp}` (S04, S05, S29);
  - `editor/window/HierarchyWindow.cpp` (S24, S26 layer filter);
  - `editor/window/SceneWindow.cpp` (S26);
  - `EditorSettingsWindow.cpp` and `ProjectSettingsWindow.cpp` (S03, S05 R26 Asset Pipeline
    page, S06 R27 Threading page, S14). These pages must register through R29;
  - `editor/mcp/tools/ProjectTools.cpp` `settingsTable` (S06, S14);
  - `editor/Stream.cpp` top-level keys (S03, S04);
  - `AppSettings.*`, `Workspace.*`, `NOTICE.md`.

## 9. Risks & defaults

- **D1 Runtime ini reload with docking.** ImGui documents ini loading as a start-up call.
  Default: the between-frames `ClearIniSettings` + `LoadIniSettingsFromMemory` sequence in R17,
  with the Default fallback. Prove it in a spike first (apply 50 times, no leaked nodes). If it
  fails, switch to a recorded DockBuilder replay of the node tree parsed from the ini; the file
  format stays the same.
- **D2 Double fire on macOS.** Default: NSMenu owns menu-bound chords while the native menu is
  active (R9). AC7 is a release gate.
- **D3 Settings migration size** (2.1k-line Project Settings). Default: rows are registered
  first, existing draw code is kept as custom `draw`, and generic rendering is used only for
  plain bool/int/float/enum/string rows. No visual change is required beyond search.
- **D4 Fuzzy noise in tree filters.** Default: Filter mode (§6.1). Match Case is the escape hatch.
- **D5 Labels in dirty open scenes.** Default: the disk line-edit plus the in-memory copy, with no
  dirty flag. If the file write fails, the label change is rolled back and reported.
- **D6 Shared layouts.** Default: a user-wide library plus file export/import, as in Unity.
  `.jasperengine/` is git-ignored, so a project-shared preset folder would need a new committed
  location. That is deferred until someone asks for it.
- **D7 Keyboard layouts.** Chords bind to the `ImGuiKey` our backends deliver, which is
  layout-translated for letters. On AZERTY the Hand tool is the key labelled Q, which sits
  where A is on QWERTY. The help shows key names, not positions. Document this, and do not fix it.
- **D8 Entity identity for bookmarks and recents.** Saved ids can change for GameObjects that
  were never saved. Default: id first, `EntityPath` hint second, then Missing.
- **D9 Licence.** Copied scorer and palette values keep the OpenEngine MIT line. `NOTICE.md`
  gets the entry in the same change as the port (R1 needs none).

## Deviations (implementation)

Recorded by the implementer; each is the closest correct alternative to the text above.

1. **Tool names and group.** §6.7's `layout_presets`, `asset_labels`, `saved_searches` and
   `bookmarks` are `edit_layout_presets`, `edit_asset_labels`, `edit_saved_searches` and
   `edit_bookmarks`: S31's verb_noun rule (`test_tool_conventions.py`). All eight tools are in
   S31's `editor_ux` group, which the conventions table already gave S25. `edit_bookmarks` also
   has `navigate` (what a click in the window does). `list_shortcuts` adds `menu_shortcut` (the
   chord the main menu's item shows now), `also_in`, `gesture` and `agent_safe`; `editor_status`
   adds `windows {focused, open}` and `ui.search_palette`, which AC11, AC13 and the UI tests read.
2. **AC11's `:gravity`.** No project-wide gravity setting exists (gravity is per scene, in the
   scene's physics settings, not Project Settings), so there is no row to find. The MCP test
   searches `:collision` and finds the Physics page's layer collision matrix row instead.
3. **Typing chords.** Function keys and Escape are not typing chords (§6.3 lists every key that
   types or edits text): F2, F5-F7 and F1 fire while a field has the keyboard, when their context
   allows it (Rename is Editor context, so it still does not).
4. **Scene View context.** A Scene View binding fires while the Scene view is focused *or* under
   the mouse, as the old key block did (Home, the numpad views, the terrain brush keys).
5. **Catalog entries.** `CommandDef` owns its strings and adds `alsoIn` (Cut, Copy, Paste and
   Select All bind in the Hierarchy and the Scene View, where the old code read them),
   `macPrimary` (Redo is Cmd+Shift+Z on macOS, Ctrl+Y elsewhere), `repeat` (Undo, Redo, Delete,
   Frame Selected and the terrain brush keys repeat while held), `keyGate` (the keyboard-only
   conditions of the old code) and `agentSafe`.
6. **Grammar details.** Bare words match the name in Filter mode and AND together (every word);
   a `"quoted phrase"` is an exact, case-insensitive substring. The structured editor's "Name
   contains" row writes a quoted phrase. A key the token table does not have is bare text.
7. **Label UI.** The selection's label strip (chips and the Labels menu) is in the Project
   window's bottom bar, not under an Inspector asset preview (the Inspector has none for most
   assets); label colours, rename and delete are in Project Settings > Editor > Asset Labels.
8. **D1 spike result.** Runtime ini reload between frames (`ClearIniSettings` +
   `LoadIniSettingsFromMemory`, then a DockBuilder check of the dockspace and a post-load frame)
   works with docking: `test_layout_presets` applies presets 51 times and checks the dock node
   count does not grow. The DockBuilder-replay fallback was not needed. A preset or working
   layout whose ini leaves the dockspace without nodes falls back to Default.
9. **Reference doc.** `docs/editor/keyboard-shortcuts.md` is written by the unit test itself
   when run with `JASPER_UPDATE_SHORTCUT_DOCS=1` (`CommandCatalog.MarkdownReferenceMatchesDocs`);
   without it the test compares byte for byte. Help > Keyboard Shortcuts opens the Shortcuts
   window in its read-only reference mode, which shows the same table with the current bindings.
10. **Upstream round 1 (oe-port e1ca0c71a).** The keys upstream added are catalog entries too:
    Physics (G) is `tools.physics`; Paint Dye's [ and ] are the brush commands
    (`terrain.brushSmaller/Larger` act on the armed Paint Dye brush first); the lattice point
    keys are `lattice.selectNone` (Alt+A), `lattice.invert` (Ctrl+I), `lattice.expand` (Ctrl+= /
    Ctrl+NumpadAdd), `lattice.shrink` (Ctrl+- / Ctrl+NumpadSubtract) and `lattice.toggleFalloff`
    (O), in the Scene View context. Hold Space (physics drop), Shift+drag (physics snap) and Esc
    (cancel a gesture, put Paint Dye down, clear the lattice points) stay local keys, listed as
    gesture rows. Texture Synthesis and Physics Placement are Window commands and layout panels;
    Preferences > Lattice, General > UI Scale and Player > iOS/Android rows are registry rows.
