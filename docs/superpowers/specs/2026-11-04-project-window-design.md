# Project window: Unity-grade asset file operations (design)

Date 2026-11-04. Branch `dc/project-window`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`.
Dossier: `dc-work/dossiers/project-window.json` (12 Doriax commits). EXTRA-FINDINGS.md has no section for this
feature, so no owner decision binds it beyond the program rules.

Claims below are labelled **(read)** when taken from reading Jasper's code at `b4f7816ea`, **(inferred)** when they
follow from reading but were not traced end to end, and **(assumed)** otherwise. Nothing here was built or run.

## 1. Goals

1. Copy, Cut, Paste and drag in the Project window land exactly where the user aimed, move only what the user
   picked, and never read a listing that was rebuilt under them. The class of bug is fixed, not one call site: no
   action inside the listing loop may rebuild the listing.
2. Duplicate (Ctrl+D, the item menu, Edit > Duplicate, Assets > Duplicate) as in Unity: copies next to the
   originals with Unity's numbered names, one undo step, the copies selected.
3. One Create menu, the same everywhere (the toolbar "+", right-click on empty space, on a file, on a folder, on a
   folder of the tree, and Assets > Create), creating in the folder that was right-clicked. Every Create item is one
   undo step. Lua Script, C++ Script, C++ Source File, C++ Header File and HTML Document ask for their name first
   (the name is the class or module), with one set of templates shared with the Inspector and `create_script`.
4. Name clashes never overwrite and never fail: copies and moves land under the free Unity name (`hero 1.png`).
   A folder is never pasted, dropped or duplicated into itself. Cut follows Explorer's clipboard rules.
5. Changing the assets or Lua directory either completes or changes nothing: a preflight refuses before any file
   moves, the result says what moved, what was rewritten and what failed, folders the moves emptied are removed,
   and agents can ask for the plan first (`dry_run`).
6. The small items of the dossier: texture slots show file names; an empty texture path has no id; objects without
   a name show as `GameObject <id>`; Maximize On Play gets a key (F11), a menu item and a hint; Save Project As
   copies the project and restores everything when it fails; each editor gets its own temporary project.

## 2. Non-goals

- In-place (inline) rename in the grid. The rename stays the existing modal; only its uses change.
- Copying files between the OS file manager and the Project window through the OS clipboard (dropping files in is
  `importExternalPaths`, unchanged).
- A default name for objects a script creates without one (runtime behaviour, `runtime-core`/`scripting`); MCP JSON
  keeps the real (empty) name.
- Doriax's F11 full screen over the menu bar (rejected, decision D14).
- Create > Scene as a file in a folder and Create > Material: Jasper's new scene is an unsaved open scene and the
  Material has no default document today; both stay as they are (main menu Scene submenu kept).
- Undo of an asset-root change. Project Settings are not undoable (`set_project_settings` says so); the change is
  confirmed first and the reverse change moves files back.
- A Lua API: everything here is editor behaviour (CLAUDE.md asks for Lua only for runtime behaviour). The one engine
  change (texture id) has no API surface.

## 3. Users and stories

- Human: right-clicks `hero.png` > Paste with a copied `enemy.png`: `enemy.png` lands beside `hero.png` (today: the
  paste fails, "Couldn't copy"). Right-clicks the `Sprites` folder > Paste: it lands inside `Sprites`.
- Human: cuts four images, right-clicks a sibling folder > Paste: they move, the listing updates, nothing crashes.
  Cutting a folder and pasting it into its own subfolder is refused with a message and the cut stays on the
  clipboard, dimmed, ready for another target.
- Human from Unity: selects three prefabs, Ctrl+D: `Tree 1.prefab`, `Rock 1.prefab`, `Bush 1.prefab` appear selected;
  holding Ctrl+D makes one set, not thirty; Ctrl+Z removes all three.
- Human: presses on a file in the same instant the pointer arrives and drags it onto a folder: only that file moves
  (today neighbours along the drag can be selected and moved with it).
- Human: right-clicks `scripts/enemies` > Create > Lua Script, types `Goblin`, Enter: `scripts/enemies/Goblin.lua`
  with module and class `Goblin`; Ctrl+Z removes it.
- Human: Project Settings > Editor > Assets Directory = `assets`, OK: a confirmation lists "34 referenced files move
  into assets/", the scenes and materials rewritten, "Move and Apply". When `assets/hero.png` already exists with
  other content the dialog refuses, names the file and nothing moves.
- Agent: `move_asset {path: "sprites/hero.png", copy: true}` (no `to`) duplicates it as `sprites/hero 1.png` and
  returns the path. `set_project_settings {values: {"editor.assets_dir": "assets"}, dry_run: true}` returns the move
  plan; without `dry_run` the result lists moves, rewritten files, emptied folders removed and failed saves.
- Agent: `run_command project.duplicate` duplicates what the Project window has selected; `editor_status` reports
  the Project window clipboard; `configure_game_view` reports whether the Game view is maximized now.

## 4. Findings

F1 Paste on a file targets a path under the file **(read)**. `editor/window/ProjectWindow.cpp:2206-2208`
`pasteFiles(currentPath / lastSelectedFile)`; the right click sets `lastSelectedFile` to any item (:2016-2024);
`AssetOps::copy` then cannot create a parent under a file (`editor/asset/AssetOps.cpp:189-211`). Doriax 65c4c88d3.

F2 The listing is rebuilt while it is iterated **(read)**. `pasteFiles` (:3061-3070) calls `scanDirectory`, which
runs `files.clear()` (:2570), from inside the range-for over `files` (:1530) with no `break` after it, unlike the
deferred drop (:2260-2265). The same holds for Create > Prefab Variant (`scanDirectory` at :2049) and Select Base
(`revealAsset` :2059-2060, which scans at :4288). After a Cut + Paste onto a sibling folder, the loop keeps its old
end and reads destroyed entries (their heap strings, `thumbnailPath` included, are freed) **(inferred: undefined
behaviour; an ASan report needs entries past the new end with heap-allocated strings, see test M2)**. Doriax 65c4c88d3
fixes the paste call only.

F3 A failed cut paste empties the clipboard **(read)**: `pasteFiles` ignores `addCommand`'s result and clears a cut
clipboard (:3062-3067). Doriax 37d7cdc41.

F4 The marquee can start on a file **(read)**. The marquee start (:1479-1488) runs before the items, so
`IsAnyItemHovered()` sees only last frame's hover; a press that arrives with the pointer move starts a marquee on a
file and clears the selection, then the drag selects neighbours (:1556-1581) and `handleInternalDragAndDrop`
(:2773-2779) moves `selectedFiles`, not the payload it dragged. Doriax 9050bbaca (also needs `dragEnd = dragStart`).

F5 No Duplicate for assets **(read)**: the item menu has Copy, Cut, Paste with no shortcut text (:2204-2208);
`project.*` has no duplicate (`editor/shortcuts/DefaultCommands.cpp:380-386`); `edit.duplicate` is off when the
Project window is focused (`editor/AppCommands.cpp:76, :149`); `pasteFiles` does not select its results. Key repeat
and the typing gate already exist (`editor/shortcuts/CommandCatalog.h:63` `repeat=false`, `AppCommands.cpp:321`).
Doriax 37d7cdc41.

F6 Moves onto a taken name fail; copies into themselves are not refused **(read)**. Copies use Unity's free name
(`editor/command/type/CopyFileCmd.cpp:104-135` `freeCopyName`), moves use the plain name (:168, :255) and
`AssetOps::move` refuses an existing target (`AssetOps.cpp:143-146`). Neither `CopyFileCmd` nor `AssetOps::copy`
refuses a folder into itself; `FileUtils::copyPath` is a recursive `fs::copy` (`editor/util/FileUtils.h:207-217`).
Only `move_asset` checks (`editor/mcp/tools/AssetTools.cpp:1741-1743`, and it says "move" for a copy). A folder named
`my.folder` gets the copy name `my 1.folder` (stem/extension split, :128-133). Doriax 691d3d90c.

F7 Create is split and partly not undoable **(read)**. The Project window's own menu (`renderCreateMenuItems`
:1060-1112, toolbar :1126-1131, empty-space menu :2283-2300) has no script, HTML or C++ item; Assets > Create
(`editor/App.cpp:596-622`) has no Physics Material. Folder rows have no Create (their Create is prefab-only,
:2034-2052) and the folder tree has no context menu at all (`renderDirectoryTree` :2455-2549). `assetsCreateScript`
(:4350-4419) and `HtmlUITemplates::createDocument` (`editor/util/HtmlUITemplates.cpp:75-99`) write files directly, not
undoable, while folders, clips, controllers, masks, montages, poses, mixers, physics materials and shake presets are
undoable. Renaming a new Lua script rewrites it with `DocumentFiles::write` outside the rename's undo step
(:4333-4348, :2966-2971): undoing the rename leaves `NewLuaScript.lua` declaring `Player`. The Project window and
`ScriptCreateDialog::writeTemplateFiles` (`editor/window/dialog/ScriptCreateDialog.h:29`, used by the Inspector and
`create_script`) carry two different template sets. New-asset names use three schemes: `_1`
(`uniqueRelativePath` :3508-3524), ` 1` (physics materials, poses) and `1` (scripts, HTML). Doriax 1e5c78706.

F8 Texture slots show pool ids **(read)**. `editor/window/InspectorWindow.cpp:4644` starts from `getId()`; only the
svgScale marker is cut (:4653-4656), so `path|alphaborder` (`engine/core/texture/Texture.cpp:418-424`) fails
`fs::exists` and shows whole; the tooltip shows the id (:4687-4688). AUTO alpha border is on for sprites, UI,
tilemaps, polygons, points. The slot also runs `fs::exists` every frame. Doriax 2cb188fbc.

F9 An empty texture path with the alpha border fix gets the id `|alphaborder` **(read)** (`Texture.cpp:418-424`,
reached by `setPath` :202 and `reloadAlphaBorder` :920); `!id.empty()` checks then treat it as named (:605, :631).
User-visible effect not traced **(inferred)**. Doriax 454d43ae5.

F10 Asset-root relocation reports success when it fails and has no preflight **(read)**. `Project::changeAssetRoots`
is `void` (`editor/Project.cpp:2314`); `SettingsRows.cpp:2133-2140` returns true after it; `set_project_settings`
then reports "Set 1 setting(s)" echoing the old value (`editor/mcp/tools/ProjectTools.cpp:994-1022`). A differing
file at the destination is kept with a warning (:2384-2389); a directory link is walked (:2369); scene, prefab,
material and project saves are unchecked (:2580, :2597, :2601, :2663); closed scenes whose save failed are unloaded
(:2658); a failure raises a modal alert (:2626) that a later MCP edit reports as dismissed; the Project Settings
window applies other settings and closes either way (`editor/window/dialog/ProjectSettingsWindow.cpp:4380`). A
referenced model moved alone leaves its `.humanoid` sidecar and a `.vat` leaves its `.vat.glb` behind (moveEntry moves
file + `.meta` only, :2412-2423; `AssetOps`' `withMeta` knows the sidecar, `AssetOps.cpp:28-43`) **(inferred)**.
Text references (Lua `require`, HTML/CSS) broken by the moves are not reported, unlike `move_asset` **(read)**.
Doriax 35c8e31da.

F11 Folders emptied by a relocation stay **(read)**: only a folder moved whole is removed (:2380). Doriax c3aed7ee9.

F12 Unnamed objects show as an icon with no text **(read)**: `HierarchyWindow.cpp:1636` label is the name; drag text
(:1772, :1801) and search (`editor/search/HierarchyFilter.h:33`, `markTree` reads `node.name`) use it too. Lua
`createEntity` takes no name **(read, `engine/core/script/binding/CoreClassesLua.cpp:3101`)**. Doriax 96f44ae05.

F13 Maximize On Play has no key and no menu item **(read)**: `GameWindow.cpp:537` (`maximize = settings.maximizeOnPlay
&& sessionActive`), :558-573 (WorkPos/WorkSize), toolbar toggle :808-811; persisted (`AppSettings.h:136`) and in
`configure_game_view` (`ViewTools.cpp:1640, :1870`). F11 is unbound (F10 is the highest, `DefaultCommands.cpp:223`).
Doriax 11879305d.

F14 Save Project As **(read)**. `saveProjectToPath` (`Project.cpp:6688`) points `projectPath` at the new folder
(:6704) before copying; a failed copy returns without restoring it (:6739-6742), leaving the editor on a partial
copy. Files are copied only from the temporary project (:6710): Save Project As of a saved project (`App.cpp:874`,
the dialog demands an empty folder, `ProjectSaveDialog.cpp:75-79`) re-points the editor at the empty folder and
copies nothing **(inferred: the assets then read as missing; not run)**. The lock on the destination is taken after
the copy, its result ignored (:6757-6759). Doriax 41a2ed0d5.

F15 Every editor shares one temporary project **(read)**: `<temp>/MyJasperEngineProject` (`Project.cpp:6321-6322`),
never locked for the editor's own temp project (:6376), so two editors without a project work in one folder and the
first Save As deletes it (`remove_all(oldPath)`, :6734) under the second **(inferred)**. Doriax 41a2ed0d5.

Dossier coverage: 65c4c88d3 -> 6.1, 6.2; 9050bbaca -> 6.5; 37d7cdc41 -> 6.2, 6.4; 691d3d90c -> 6.3; 1e5c78706 -> 6.6;
2cb188fbc -> 6.10; 454d43ae5 -> 6.10; 35c8e31da -> 6.9; c3aed7ee9 -> 6.9; 41a2ed0d5 -> 6.13; 96f44ae05 -> 6.11
(Doriax's Theme colour move rejected: Jasper's tree colours are local constants, HierarchyWindow.cpp:1607-1631, and the
disabled colour is the theme's); 11879305d -> 6.12 (the full-screen part rejected, D14). No dossier item is refuted
or skip.

## 5. Product decisions

D1 Paste targets (Explorer and Unity): right-click on a folder pastes into it; on a file, into the file's folder;
on empty space, Ctrl+V and Edit > Paste, into the folder the listing shows. During a search the listing is no
folder: a result's own folder (a folder result: into it), else the folder the search started from
(`searchReturnPath`, else the project root). Reason: what users of both expect; Ctrl+V never guesses from selection.

D2 Moves follow copies on name clashes: a cut-paste or drag into a folder holding the same name lands under the
free Unity name (`hero 1.png`), and the console line and the undo step name it ("Move File hero.png as hero 1.png").
Unity's drag-move does the same (unique asset path). `move_asset` stays exact (an agent names the destination).

D3 A folder never goes into itself or below itself (copy, move, duplicate onto a selected folder, drop): refused as a
whole before any file changes, with "Can't paste 'Sprites' into itself." A drop target that is one of the dragged
items or below one does not accept the drop (no highlight). Explorer refuses the same way.

D4 Clipboard (Explorer): Cut dims the cut items (50% alpha) until pasted; a successful cut-paste empties the
clipboard; a refused or failed paste keeps it; a cut pasted into its own folder changes nothing and keeps it; items
gone since the copy are skipped with a console note ("2 of 5 copied files are gone"), and when all are gone the
clipboard empties with "Nothing to paste: the copied files were moved or deleted". The clipboard empties when the
project changes. A Copy only reads, so Copy and Paste work during Play as today.

D5 Duplicate = Unity's Edit > Duplicate on assets: each selected item next to itself, Unity names, one undo step
"Duplicate File hero.png" / "Duplicate 3 Files", the copies selected, the Inspector on the last, the last pinged.
Ctrl+D in the Project window (`project.duplicate`, no key repeat); "Duplicate" in the item menu and the tree menu;
Assets > Duplicate; Edit > Duplicate when the Project window was the last focused panel. Edit > Cut / Copy / Paste /
Delete / Rename route to the Project window the same way (Unity's Edit menu acts on the focused window).

D6 One Create list (`AssetCreateMenu`), Unity order, the same labels everywhere; the toolbar "+" shows the items
directly, every right-click menu starts with a "Create" submenu (Unity). Main-menu-only extras stay main-menu only:
Scene (an unsaved open scene, not a file) and Native Plugin... (its own location). The Project window gains Lua
Script, C++ Script, C++ Source File, C++ Header File and HTML Document; Assets > Create gains Physics Material and
Physics Material 2D.

D7 Scripts and HTML are named first: a small modal ("New Lua Script", name prefilled with a free default and
selected) creates the file with its final name and class in one undo step; Esc creates nothing. Other assets keep
create-then-rename (Unity). Reason: the name is the class/module, so a rename-after-create rewrite (today's
`renameNewLuaScript`) cannot be one consistent undo step. Lua Script is enabled only inside the Lua directory
(tooltip says where it is), matching `create_script`'s refusal; C++ Script/Source outside the Script Directories
show a dim note "compiled only when attached as a script" (Doriax 1e5c78706).

D8 One template set: `ScriptTemplates` (moved out of `ScriptCreateDialog::writeTemplateFiles`, which agents and the
Inspector already use) for the Project window too. The Project window's own templates are removed.

D9 One naming rule for new assets: Unity's `<name> 1` with a space (`New Animation 1.anim`); identifiers (scripts,
HTML document names used as module names) take a bare number (`NewLuaScript1`). `uniqueRelativePath` changes from `_1`.

D10 The project's assets and Lua directories (and folders holding them) are not moved, renamed or deleted from the
Project window: refused with "'assets' is the project's assets directory: change it in Project Settings > Editor >
Assets Directory, which moves the referenced files". Copy and Duplicate of them stay allowed. `move_asset` and
`delete_asset` already refuse (`McpRefs::trustedRootTouched`).

D11 Asset-root change: preflight before anything changes (new root under a file; a destination that exists with
other content; a source or destination through a link; an unwritable project.yaml, scene, prefab or material that
must be rewritten; for agents, unsaved Code Editor buffers of files that would move). A destination with identical
content is used and its source stays (as today, now reported). Files move with their companions (`.meta`,
`.humanoid`, the other file of a Vertex Animation pair). After the rewrite, folders the moves emptied are removed up to
(not including) the project root and the four roots, never through a link. A scene or prefab whose save failed stays
loaded and modified (its new paths survive in memory) and is listed. The human path confirms first; the agent path
can ask `dry_run`. Not undoable (non-goal). It stays one synchronous main-thread step (it loads, rewrites and saves
scenes); if the fixture measures above 1 s, the MCP path becomes a job (`McpJobs`, conventions "more than about 1 s")
with the same result, decided by that measurement at implementation and recorded in the implementation notes.

D12 Unnamed objects read `GameObject <id>` in the disabled text colour, in the Hierarchy, its drag text, its
search (`42` and `GameObject` find it), the Inspector name field's hint and the Inspector's object-link labels. The
name itself stays empty. Unity names every GameObject; this is the nearest honest label (Doriax shows `(id)`).

D13 Texture slots show the file name of `getPath(0)` (a cubemap: face 0's name + " (Cubemap)"); the tooltip shows the
project-relative path(s); a texture with no path (generated, pooled) shows its id. No per-frame `fs::exists`.

D14 Maximize On Play: command `playmode.maximizeOnPlay` "Maximize On Play", F11 (Global, rebindable), a checked item
in the Edit menu under Stop, the Game toolbar toggle as today. The key toggles the persisted setting and acts at
once during Play. When the Game view maximizes, a hint "Maximize On Play: F11 shows the editor" (the user's binding)
fades out over 3 s in its top-right corner. The menu bar and main toolbar stay visible (Unity keeps them; Stop and
Pause stay reachable). Doriax's full screen over the menu bar is rejected for that reason. On macOS F11 is Show
Desktop by default: the key works with fn or when rebound; the docs say so.

D15 Save Project As copies: from the temporary project it moves (copy, then delete the temp folder) as today; from a
saved project it copies the whole project to the empty folder and continues in the copy, the original staying as last
saved. Before anything is copied: the destination is empty or new, not inside the project (a copy into itself), and
its lock is taken. Any failure removes what the copy created, restores the project path, roots, asset database and
lock, and says why.

D16 Each editor's temporary project is its own: `MyJasperEngineProject`, else `MyJasperEngineProject 2`, ... the first
whose lock is free (Doriax picks numbered folders too). The lock is taken quietly (no owner dialog).

## 6. Design

### 6.1 Listing iteration safety (the class fix for F2)

`ProjectWindow` (`editor/window/ProjectWindow.{h,cpp}`):

```cpp
bool listingIterating = false;                 // renderFileListing's loop over `files` is running
std::optional<std::filesystem::path> deferredScan;   // a scanDirectory asked for meanwhile
```

- `renderFileListing` sets `listingIterating = true` right before `for (auto& file : files)` and false after
  `EndTable`. Then, before the empty-space menu: `if (deferredScan) scanDirectory(*std::exchange(deferredScan, {}));`.
- `scanDirectory(path)`: when `listingIterating`, record `deferredScan = path` and return (no other state changes).
  Every caller inside the loop (paste, duplicate, Create from a row menu, Create Prefab Variant, Select Base, the VCS
  and editor-script menu items, double-click navigation) is safe without its own `break`. The existing
  `deferredDirectoryChange` / `dropTarget` breaks stay (they also clear the selection).
- `rebuildFileIndex()` and `sortWithSortSpecs()` assert `!listingIterating` (debug builds).
- The tree walk (`renderDirectoryTree`) gets the same treatment for its new menu (6.7): actions run after the walk
  through `std::function<void()> pendingTreeAction`.

### 6.2 Paste targets and the asset clipboard

New `editor/asset/AssetClipboard.{h,cpp}` (pure logic, no ImGui, unit tested):

```cpp
class AssetClipboard {
public:
    void set(std::vector<std::filesystem::path> absolutePaths, bool cut);
    void clear();
    bool empty() const;  bool isCut() const;
    const std::vector<std::filesystem::path>& paths() const;
    bool isCutItem(const std::filesystem::path& absolute) const;   // hashed: the listing dims it
    // the paths still on disk, and how many are gone
    std::vector<std::filesystem::path> existing(size_t* gone = nullptr) const;
    // after a paste: success empties a cut clipboard; refusal or failure keeps it
    void pasted(bool succeeded);
};
// D1, one rule for every menu and key
std::filesystem::path pasteTarget(const std::filesystem::path& listingFolder,   // search: searchReturnPath or root
                                  const std::filesystem::path* clicked, bool clickedIsDirectory);
```

`ProjectWindow` replaces `clipboardFiles`/`clipboardCut` with an `AssetClipboard`, cleared in
`notifyProjectPathChange`. `copySelectedFiles(cut)` and the tree's Copy/Cut call `set`. The row menu computes
`fs::path pasteInto = pasteTarget(...)` and calls `pasteFiles(pasteInto)` (safe through 6.1). `pasteFiles(target)`:

1. `existing(&gone)`; nothing left: clear, console "Nothing to paste: ...", return.
2. Root refusal for a cut (D10) and `AssetOps::moveRefusal` / `copyRefusal` (6.3) for each source: the first refusal
   shows an alert ("Paste", the reason) and returns with the clipboard kept.
3. `CopyFileCmd` with a `Report` sink (6.3) on the project history; `pasted(ok)`.
4. On success: when `target` is the listing folder, select the landed names (relative to the listing; search: as the
   listing names them), Inspector on the last, ping it; else select the target folder's row when it is listed.

Menus show the catalog's key text (`CommandCatalog::editor().menuShortcutText("project.copy")`, as Rename does) on
Copy, Cut, Paste, Duplicate, Delete. `ProjectWindow::UiState` gains `clipboard` (project-relative paths, `cut`), which
`editor_status` reports under `ui.project_window.clipboard` (`editor/mcp/tools/CoreTools.cpp:674`).

### 6.3 `CopyFileCmd` and `AssetOps`

`editor/command/type/CopyFileCmd.{h,cpp}`:

- Destinations (D2): when `target` exists and is not the source itself, `freeCopyName(target)` for moves as for copies
  (project and file-only paths). A move whose target is its source (same folder) is skipped. The pair logic of
  `freeCopyName` (Vertex Animation) applies to moves too. Directories take the whole-name suffix (`my.folder 1`).
- Refusals (D3), checked for every entry before the first file operation, the command then fails changing nothing:
  `AssetOps::copyRefusal(project, from, to)` / `moveRefusal(project, from, to)` (new, `editor/asset/AssetOps.{h,cpp}`):
  a folder into itself or below itself (compared lexically normal and resolved, `safewrite::resolvedPath`); a copy of
  a folder holding a `project.yaml` (`safewrite::isProjectFileName`, as `move_asset` does). `AssetOps::copy` and
  `AssetOps::move` call them too (defence for every caller). `move_asset` uses them, which fixes its "move" wording for
  copies.
- `struct Report { std::vector<std::filesystem::path> landed; size_t gone = 0; std::string error; };`
  `void reportTo(std::shared_ptr<Report>)`, filled by every execute (the history deletes a failed command, the sink
  outlives it, as `WriteFilesCmd::reportFailureTo`). `failure()` returns the last error.
- `isNoOp()`: true when no entry went (all gone, all same-folder moves): no undo step (`CommandHistory::addCommand`).
- `void setDuplicate(bool)`: `describe()` says "Duplicate File hero.png" / "Duplicate 3 Files"; a move that took a
  free name says "Move File hero.png as hero 1.png".
- New constructor for entries with their own target folders (Duplicate across search results):
  `CopyFileCmd(Project*, std::vector<FileCopyData> entries, bool copy)`.
- The console line of a renamed landing: `Out::info("Moved hero.png as hero 1.png (the name was taken)")`.

### 6.4 Duplicate

`ProjectWindow::duplicateSelection()`: entries = the selection (absolute), each with `targetDirectory =
parent_path()`; refusals as 6.3; one `CopyFileCmd(entries, true)` with `setDuplicate(true)` and a `Report`; then the
selection is the landed items (6.2 step 4). `canDuplicate()` = selection non-empty and no rename/modal open.
Catalog (`DefaultCommands.cpp`, group Project): `project.duplicate` "Duplicate Assets", Project context, Ctrl+D.
`AppCommands.cpp`: bind it like `project.copy` (enabled `canDuplicate`, keyGate `projectKeys`). Menus: row menu and
tree menu "Duplicate" after Paste; Assets menu "Duplicate" (the catalog item `project.duplicate`) between Rename and
Delete. Edit menu routing (D5): in `App::executeMenuCommand` the Duplicate, Copy, Cut, Paste, Delete and Rename cases
call the Project window's functions when `lastFocusedWindow == LastFocusedWindow::Resources`; the `enabled` lambdas of
`edit.duplicate/copy/cut/paste/delete/rename` (`AppCommands.cpp:144-162`) OR in the Project window's state then.
Their keyGates (`notAssetPanel`, `sceneKeys`) stay, so keys keep going to the `project.*` commands.

### 6.5 Drag and marquee (F4)

- The marquee start moves after the item loop and joins the empty-click block (:2302): `if (IsWindowHovered() &&
  IsMouseClicked(0) && !clickedInFile && !IsAnyItemHovered() && !GetDragDropPayload())` starts it with `dragEnd =
  dragStart`; the selection is cleared there only without Ctrl or Shift (Ctrl+marquee adds, as the overlap code
  already does).
- While a drag-and-drop payload is active (`ImGui::GetDragDropPayload()`), the marquee stops (`isDragging = false`).
- `handleInternalDragAndDrop(target, sources)` takes the payload's paths (parsed from the `resource_files` payload:
  `\0`-separated absolute paths), not `selectedFiles`; the three drop targets (listing folder rows, breadcrumb,
  tree nodes) pass them. A drop target peeks the payload (`AcceptBeforeDelivery | AcceptNoDrawDefaultRect`) and does
  not accept when the target is one of the sources or below one (D3), or when a source is a protected root (D10).
- Drops onto a folder holding the same name use D2.

### 6.6 Create menu

`editor/window/AssetCreateMenu.{h,cpp}`:

```cpp
enum class AssetCreateKind : uint8_t {
    Folder, LuaScript, CppScript, CppSource, CppHeader, HtmlDocument,
    AnimationClip, AnimatorController, AvatarMask, AnimationMontage, PoseAnimation,
    AudioMixer, PhysicsMaterial, PhysicsMaterial2D, ShakePreset };
struct AssetCreateItem {
    AssetCreateKind kind; const char* label; const char* icon;
    const char* commandId;          // "assets.createLuaScript": the main menu and the palette
    bool separatorBefore;
};
std::span<const AssetCreateItem> assetCreateItems();   // the one list, in Unity's order
```

Order and separators: Folder | Lua Script, C++ Script, C++ Source File, C++ Header File | HTML Document | Animation
Clip (when `animationClipAppHooks().createClipAsset` is set), Animator Controller, Avatar Mask, Animation Montage, Pose
Animation | Audio Mixer | Physics Material, Physics Material 2D | Shake Preset.

- New catalog ids (`DefaultCommands.cpp`, `AppMenuCommand.h`): `assets.createCppSource`, `assets.createCppHeader`,
  `assets.createPhysicsMaterial`, `assets.createPhysicsMaterial2D`. `App.cpp:596-622` builds Assets > Create from the
  list, then Scene ▸ and Native Plugin... as today.
- `ProjectWindow::renderCreateItems(const fs::path& directory)` draws the list as ImGui items (each pushes its rect to
  `uiBuilding.contextMenu` for tests). The toolbar "+" shows them directly, then Import New Asset..., Import Folder....
  Each right-click menu (empty space, file row, folder row, tree node) starts with `BeginMenu("Create")` drawing them
  for its folder: empty space = listing folder (D1 search rule), file = its folder, folder = the folder.
- `ProjectWindow::createAsset(AssetCreateKind, const fs::path& directory)` dispatches; the existing
  `assetsCreate*()` take the directory (`assetsCreatePoseAnimation(dir)`, ...). The main-menu commands call
  `createAsset(kind, createTargetDirectory())`, where `createTargetDirectory()` is the listing folder, during a search
  `searchReturnPath` or the project root. A directory that no longer exists: alert, nothing created.
- Name-first kinds (D7) open `NewItemDialog` (a modal of the Project window replacing `handleNewDirectory`):

  ```cpp
  struct PendingNewItem { AssetCreateKind kind; fs::path directory; char name[256]; std::string error; bool open; };
  ```

  Title "New Folder" / "New Lua Script" / "New C++ Script" / "New C++ Source File" / "New C++ Header File" / "New HTML
  Document"; the name is prefilled with a free default (`New Folder`, `NewLuaScript`, `NewScript`, `NewSource`,
  `NewHeader`, `NewDocument`) and selected; scripts use `ScriptCreateDialog::classNameCharFilter`; the error shows
  inline in red (empty; taken, for C++ Script either file; a Lua keyword; a folder name with `/ \ : * ? " < > |`,
  `.`/`..` or leading/trailing spaces); Enter or Create creates, Esc or Cancel creates nothing. A typed extension is
  accepted (`Goblin.lua` = `Goblin`).
- Writes: one `ProjectFileWrites::writeUndoable` step with a name. `writeUndoable` gains a trailing
  `const std::string& stepName = {}` passed to `WriteFilesCmd::setName`: "Create Lua Script Goblin.lua", "Create C++
  Script Goblin", "Create File Foo.cpp", "Create HTML Document Menu". Lua scripts regenerate their stubs
  (`Generator::writeLuaScriptStubs`) in `afterChange`, so undo and redo do too. Folders keep `CreateDirCmd` (into the
  directory). HTML Document: `HtmlUITemplates` gains `documentFiles(directory, name)` returning the two items, and
  `createDocument` becomes that plus `writeUndoable` (one step for both files).
- Templates (D8): `editor/util/ScriptTemplates.{h,cpp}` returns strings: `lua(name)`, `cppScript(name)` (header +
  source), `cppSubclass(scene, entity, name)`, `cppSource(name, bool withHeader)` (`#include "<name>.h"` when a
  same-named header exists in the folder, else a comment line), `cppHeader(name)` (`#pragma once`).
  `ScriptCreateDialog::writeTemplateFiles` becomes a wrapper; `createScriptTool` (`ScriptTools.cpp:752-880`) takes the
  strings directly (no scratch directory). `renameNewLuaScript`, `newLuaScriptFile`, `newLuaScriptText` and
  `newLuaScriptTemplate` are deleted.
- After a create: select the new item, Inspector on it (files), ping it. An undo that removes a file whose Code
  Editor tab is open and unmodified closes the tab (the `afterChange` callback, `CodeEditor::closeFile`).
- `uniqueRelativePath` (D9) uses `" " + n`.

### 6.7 Folder tree context menu

Right-click on a tree node opens `FolderTreeContextMenu` for that folder: Create ▸ (6.6, into it), Copy, Cut, Paste
(into it), Duplicate, separator, Show in File Manager. Copy/Cut/Duplicate are disabled on the project root; Cut also
on protected roots (D10). Actions run after the tree walk (`pendingTreeAction`, 6.1). Each item pushes its rect to
`uiBuilding.contextMenu`.

### 6.8 Protected roots in the Project window (D10)

`ProjectWindow::rootRefusal(const fs::path&)` wraps `mcp::McpRefs::trustedRootTouched(project, path, false)` with the
Project window wording. Checked by Cut, Paste of a cut, drop, Rename (`handleRename`) and Delete (the confirmation
modal lists the refused items and deletes nothing when any is refused).

### 6.9 Asset-root relocation (F10, F11)

`editor/Project.h`:

```cpp
struct AssetRootChange {
    struct Move { std::filesystem::path from, to; bool folder = false; };
    std::vector<Move> moves;                              // planned, then done (companions included)
    std::vector<std::filesystem::path> alreadyThere;      // identical file at the destination: used, source kept
    std::vector<std::filesystem::path> keptOutside;       // outside both old roots: references kept as they are
    std::vector<std::filesystem::path> rewritten;         // scenes, prefabs, materials, project.yaml (absolute)
    std::vector<std::filesystem::path> savedScenesWithEdits;   // open scenes that had unsaved changes
    std::vector<std::filesystem::path> unsavedBuffers;    // Code Editor buffers of moving files
    std::vector<std::filesystem::path> failedSaves;       // rewritten in memory, not on disk
    std::vector<std::filesystem::path> removedFolders;    // emptied by the moves
    std::vector<AssetOps::TextReference> textReferencesBroken;
    std::string blocker;    // preflight refusal: nothing changed
    std::string error;      // failed while moving: everything moved was put back, roots unchanged
    bool changed = false;   // the roots changed (also with failedSaves)
};
// What a change would do; changes nothing (closed scenes are loaded and unloaded to read references)
AssetRootChange planAssetRootChange(const std::filesystem::path& newAssetsDir, const std::filesystem::path& newLuaDir,
                                    bool refuseUnsavedBuffers);
AssetRootChange changeAssetRoots(const std::filesystem::path& newAssetsDir, const std::filesystem::path& newLuaDir,
                                 bool refuseUnsavedBuffers);
```

Implementation (`editor/Project.cpp`, restructuring `changeAssetRoots` :2314-2665, keeping its meta moves, scoped file
operations, copy fallback across volumes and `undoMoves`):

1. Plan (shared by both): collect references (today's `visitReferences(assetCollect, luaCollect)`), plan each
   relocation (today's `relocate` split into `planRelocation` + execute, Doriax 35c8e31da's shape), add companions
   (`AssetOps::companionsOf(path)`: the `withMeta` set plus `vat::existingPairOf`; exposed from `AssetOps.cpp:28-43`),
   and the `terrain_maps` move.
2. Preflight (the blocker, in this order): a new root is a file or under one (`canHoldDirectory`); a planned source
   or destination resolves through a link (`fs::symlink_status`, `safewrite::resolvedPath` against the new root:
   "would leave assets/ through a link"); a destination file exists with other content (byte compare, 64 KiB chunks;
   "assets/hero.png already exists and differs from hero.png the project references; move or rename one of them
   first"); a file the change rewrites is not writable (project.yaml, every scene file, the prefab and material files
   holding references) through `AssetRootChange::Probe` (default: open for append, plus creating and removing a
   uniquely named probe directory beside it; a test replaces it); `refuseUnsavedBuffers` and a moving file has an
   unsaved Code Editor buffer ("save or revert Player.lua first").
3. `changeAssetRoots`: plan; a blocker returns it (temporary scenes unloaded, nothing else touched). The GUI path
   (`refuseUnsavedBuffers = false`) saves Code Editor buffers as today. Moves via `moveEntry` (which no longer walks
   linked directories), companions with their files. A move failure: `undoMoves`, `error`, return. Roots switch,
   references are rewritten; each `saveSceneToPath` / `savePrefabToDisk` / material write / `saveProjectFile` result is
   checked; a failed scene or prefab stays loaded and modified (not unloaded) and goes to `failedSaves`. Then folders
   emptied by the moves are removed (c3aed7ee9, with its link guard; stop at the project root, the old and new assets
   and Lua roots, a non-empty folder or a link). Text references broken by the moves are collected
   (`AssetOps::textReferencesInto` for each source before it moves). No `registerAlert` inside: callers report.
4. `SettingsRows.cpp` (`editor.assets_dir`, `scripting.lua_dir`, :2097-2141): validation (`apply == false`) also runs
   `planAssetRootChange(..., true)` when the value differs and returns its blocker as the error ("Nothing was
   changed"); apply runs `changeAssetRoots(..., true)` and returns false with `error` or `blocker`. The result goes to a
   new `SettingsContext::report` (`mcp::Json*`, null outside MCP) as `relocation` (6.14).
5. `ProjectSettingsWindow::applySettings` (:4314): when the roots changed, the asset roots are applied first. OK opens
   a "Change Asset Directories" modal from `planAssetRootChange(..., false)`: a summary ("34 referenced files move
   into assets/ (list)", "12 files rewritten", "files nothing references stay where they are", saved scenes with
   edits, buffers that will be saved, "This cannot be undone; changing it back moves the files back") with Move and
   Apply / Cancel; a blocker shows it with Cancel only. After Move and Apply: on `error` or `blocker` the window stays
   open showing the reason and nothing else is applied; `failedSaves` show in an alert listing the files ("they keep
   the new paths in the editor; save them again"); otherwise the remaining settings apply and the window closes.
6. MCP `set_project_settings` (6.14).

### 6.10 Texture slot label and texture id (F8, F9)

- Engine: `Texture::buildPathTextureId` (`engine/core/texture/Texture.cpp:418-424`) appends `|alphaborder` only when
  the path is not empty (Doriax 454d43ae5).
- Editor: `editor/util/TextureLabels.{h,cpp}`:
  `struct TextureSlotLabel { std::string text, tooltip; }; TextureSlotLabel textureSlotLabel(const Texture&, const Project*)`:
  a texture loaded from a path: `filename()` of `getPath(0)` (UTF-8 through `FileUtils`), cubemaps
  (`getNumFaces() == 6`) face 0's name + " (Cubemap)"; tooltip: the project-relative path(s), one line per face; no
  path: the id; empty: `< Not set >`. `InspectorWindow.cpp:4644-4659` uses it for the plain branch (camera, fluid,
  video and Missing branches unchanged) and :4687-4688 for the tooltip. No `fs::exists`.

### 6.11 Unnamed objects (F12)

- `editor/util/EntityLabels.h`: `std::string entityDisplayName(const std::string& name, Entity id)` (name, else
  `"GameObject " + id`), `bool isUnnamed(const std::string&)`.
- `HierarchyWindow`: `TreeNode` gains `displayName`, set where `name` is set (:2721, :2748, :2873, :2963); the label (:1636), drag
  texts (:1772, :1801) use it; unnamed rows use `ImGuiCol_TextDisabled` unless a colour rule above it applies
  (search match, previewing camera, play-created). `HierarchyFilter::markTree` gains a name accessor parameter
  (default `node.name`); the Hierarchy passes `displayName`.
- Inspector: the name field (`InspectorWindow.cpp:1886`) becomes `InputTextWithHint` with the display name as hint
  when the single selection is unnamed; object-link labels (camera :4764, video :4785, fluid) use `entityDisplayName`.

### 6.12 Maximize On Play (F13)

- Catalog: `playmode.maximizeOnPlay` "Maximize On Play", Global, F11, menu command
  `AppMenuCommand::ToggleMaximizeOnPlay`; Edit menu item under Stop, checked by the setting
  (`catalogItem(..., label, checked)` as Play/Pause).
- `GameWindow`: `toggleMaximizeOnPlay()` (flip, `saveSettings()`), `isMaximizedNow()` (`maximize && windowOpen` of the
  last frame). On a false-to-true change of `maximize` it sets `maximizeHintUntil = ImGui::GetTime() + 3.0` and draws
  the hint text in the maximized window's top-right corner, alpha falling over the last second, only while
  `GetTime() < maximizeHintUntil`. The hint names the current binding (`menuShortcutText`), or "the Game view toolbar"
  when unbound.
- `configure_game_view` read result gains `maximized` (bool, now).

### 6.13 Save Project As and the temporary project (F14, F15)

- `Project::saveProjectToPath(path)` (`Project.cpp:6688`): preflight first: `path` not inside the project and the
  project not inside `path` (`Util::isInsidePath` on resolved paths); `path` new or empty; create it; take its lock
  (`takeProjectLock`, refused: "already open in another editor") unless it is the editor's own temporary project. Then
  copy (temporary project: everything as today, then delete it; saved project: the project directory without
  `.jasperengine/build`, `.jasperengine/externalbuild`, `.trash` and the lock file), recording what the copy created.
  `projectPath` changes only after the copy succeeded. On failure: remove the created entries, keep `projectPath`, roots
  and asset database, release the new lock, alert with the reason. After success from a saved project, the old
  project's lock is released (the session continues in the copy) and the copy is added to the recent projects.
- `Project::chooseTempProjectDir(tempRoot, name, isBusy)` (static, pure): `name`, then `name 2`, `name 3`, ... the
  first not busy; `createTempProject` uses it with `isBusy` = "its lock is held elsewhere" and takes that lock quietly.
  `isEditorTempProject()` accepts the numbered names.

### 6.14 MCP, guide and docs

- `move_asset` (`AssetTools.cpp:3881-3897`, handler :1661): `to` becomes optional; required unless `copy: true`.
  Without `to`, a copy lands beside the source under Unity's free name. The result gains `to` always (the landed
  path). Refusals through `AssetOps::copyRefusal` / `moveRefusal` (the "copy" wording fixed). Description:
  "...; or copy it (copy: true; omit to for a Duplicate beside it, named like hero 1.png)...".
- `set_project_settings` (`ProjectTools.cpp:954-1022`, :1497-1511): new `dry_run` (boolean, default false):
  validates every value and returns `{settings: <values as they would be>, relocation: <plan>}` changing nothing. On
  apply, a root change adds `relocation: {moved: [{from, to}], already_there, kept_outside, rewritten,
  saved_scenes_with_edits, removed_folders, failed_saves, text_references_broken}` (project-relative) and a text
  line ("Moved 34 referenced file(s) into assets/; rewrote 12 file(s); removed 3 emptied folder(s)."). A blocker
  fails the call with it and "Nothing was changed." `failed_saves` keeps the call successful with a warning line
  naming them and how to retry (`save_scene`). Agents refuse on unsaved Code Editor buffers of moving files
  (`refuseUnsavedBuffers = true`).
- `configure_game_view`: read result `maximized`. `editor_status`: `ui.project_window.clipboard`.
- `run_command` reaches `project.duplicate` and `playmode.maximizeOnPlay` (both `agentSafe`).
- `tools/mcp/tool_budget.py --check tools/mcp/budget.json`, `tools/mcp/gen_tool_docs.py` (TOOLS.md).
- Guide `docs/mcp/guide/assets.md`: Duplicate (`move_asset` copy without `to`), the numbered names, the asset-root
  section (`dry_run` first; set `editor.assets_dir` / `scripting.lua_dir` right after creating a project, before
  importing, because a later change moves files; what the result lists). `McpInstructions.cpp`: unchanged (its
  `move_asset` line holds).
- Docs: new `docs/editor/project-window.md` (file operations, paste targets, clipboard rules, Duplicate, the Create
  menu and its targets, protected roots, shortcuts); `docs/editor/keyboard-shortcuts.md` regenerated
  (`JASPER_UPDATE_SHORTCUT_DOCS=1`, `CommandCatalog.MarkdownReferenceMatchesDocs`); `docs/unity-glossary.md` rows:
  Duplicate on assets (Ctrl+D), Maximize On Play (F11), Create menu in the Project window.

### 6.15 Platforms

Editor-only work: the Linux, Windows and macOS editors; no exported game changes (the texture id fix runs everywhere,
no backend involved). Names go through `FileUtils::pathToUtf8` / `pathFromUtf8` (no `.string()` on names, per
`unicode-text`). Free-name checks use `fs::exists`, correct on case-insensitive volumes (Windows, macOS); a cut
pasted into its folder under another case is the same file and a no-op. Moves use the existing `assetio::renameFile` /
`io::renameWithRetry` (Windows sharing retries). F11 on macOS: D14. Windows and macOS are not built here; the
implementer keeps them compiling by following the existing patterns (no platform-specific code is planned).

## 7. Behaviour rules and edge cases

- Paste into the folder that holds the copied file: a copy becomes `hero 1.png` (Duplicate behaviour); a cut is a
  no-op, no step, clipboard kept.
- Cut then drag the same item elsewhere: the clipboard item is gone; a later paste skips it (D4).
- Paste of several items where one is refused (into itself): the whole paste is refused, nothing moves.
- A copied folder pasted into a folder that holds a same-named folder: `Sprites 1` (no merge; Unity does the same).
- Vertex Animation pairs: duplicated, pasted and moved as pairs with paired free names (`walk 1.vat` + `walk
  1.vat.glb`), as copies are today.
- Duplicate of a scene: the file on disk (unsaved edits are not in it); its new scene id is `scene-integrity`'s rule
  for every copy (13). Duplicate of a prefab or clip: new GUIDs (existing `AssetOps::copy`).
- Duplicate or paste during a search: copies land in their sources' folders; selected when the refreshed results list
  them.
- Ctrl+D or Ctrl+V held: one operation per press (catalog `repeat = false`). Typing in the search field or the name
  dialog: the keys go to the text (`projectKeys`).
- Create while the target folder was deleted meanwhile: alert, nothing created. Create Lua Script with a Lua root of
  `.`: enabled everywhere in the project.
- Name dialog with a name taken by a file of another case on a case-insensitive volume: reported as taken.
- Drag of items onto one of themselves or into their own subfolder: no drop highlight, nothing happens.
- Asset-root change to the same roots: no-op, success. To a root inside the current root or vice versa: files
  relocate as today (relative layout); the old root is never removed. Two references to one file: one move.
- An asset-root change during Play or a build: refused as today.
- Unnamed GameObject rename: F2 starts with an empty field; an empty result is refused as today.
- Maximize On Play toggled off during Play: the Game view docks back at once; on again: maximized at once with the hint.
- Save Project As into a folder inside the project, or onto the project itself: refused before anything is copied.

## 8. Performance and cost

- Nothing new runs while unused. Per listing frame: one hash lookup per visible entry while a cut is on the clipboard
  (dimming), none otherwise; the marquee and drop checks move, they are not added.
- The texture slot loses a per-frame `fs::exists` (a filesystem call per drawn slot per frame).
- Hierarchy: `displayName` is built with the tree (no per-frame formatting); search cost unchanged.
- Maximize hint: one time comparison per frame while maximized.
- Asset-root preflight: on demand only (OK in Project Settings, a settings call): one pass over the referenced files
  plus a byte compare of existing destinations and one probe per rewritten file. Measured on the fixture project in
  the MCP test (logged); D11 sets the job rule.
- Duplicate/paste: unchanged cost per file (`AssetOps::copy`); refusal checks are lexical plus one resolve per source.

## 9. Compatibility

No file format changes: no scene, prefab, meta or `project.yaml` field is added. New catalog ids get default bindings;
`shortcuts.yaml` overrides are keyed by id, so existing overrides stay. The `_1` to ` 1` naming change affects only
names chosen from now on. `move_asset` with `to` behaves as before; `set_project_settings` without `dry_run` keeps its
result fields and adds `relocation`. The texture id change only affects textures with no path (no pool entry or file
is renamed). The temporary project folder may now be numbered; old `MyJasperEngineProject` folders keep working as
the first choice. No migration is needed.

## 10. Tests

Red controls are marked **red**: they fail on `main` (`b4f7816ea`) and pass with the change; each implementation
commit records the red run (command and failing output) in the spec's implementation notes.

Unit tests, `jasperengine-editor-tests` (`tools/build.sh unit-tests`):

1. `editor/tests/command/CopyFileCmdTest.cpp` (TempDir project, the `UndoProjectTest` harness):
   - `MoveOntoATakenNameLandsUnderTheNumberedName` **red** (today the move fails "already exists").
   - `CopyOfAFolderIntoItselfIsRefusedAndChangesNothing` **red** (today a nested copy is attempted).
   - `MoveIntoItsOwnFolderAddsNoStep` **red** (today `addCommand` returns false).
   - `FolderWithADotGetsTheWholeNameSuffix` (`my.folder` -> `my.folder 1`) **red**.
   - `ReportListsLandedPathsAndSurvivesAFailedCommand`; `DuplicateNamesItsStep` ("Duplicate 3 Files");
     `VertexAnimationPairMovesUnderPairedFreeNames`; `UndoAndRedoKeepTheNumberedDestination`.
2. `editor/tests/asset/AssetClipboardTest.cpp`: `PasteTargetOfAFileIsItsFolder`, `PasteTargetOfAFolderIsTheFolder`,
   `PasteTargetDuringASearchIsTheStartFolder`, `FailedPasteKeepsACut`, `SuccessfulPasteEmptiesACut`,
   `GoneItemsAreSkippedAndCounted`.
3. `editor/tests/asset/AssetOpsRefusalTest.cpp`: `CopyAndMoveRefuseIntoThemselvesAfterResolvingLinks`,
   `CopyRefusesAFolderHoldingAProjectFile`.
4. `editor/tests/project/AssetRootChangeTest.cpp` (TempDir project with a scene, a prefab, a material and textures;
   the `AnimatorWindowTest` project fixture pattern):
   - `DifferingDestinationRefusesAndChangesNothing` **red** (today the change proceeds and the roots change).
   - `UnwritableSceneRefusesBeforeAnyMove` (Probe replaced) **red** (today files move, the save fails silently).
   - `FailedSaveKeepsTheSceneLoadedAndListsIt` (Probe passes, the save hook fails) **red** (today it is unloaded).
   - `EmptiedFoldersAreRemovedButNotRootsOrLinks` **red** (today the folders stay).
   - `HumanoidSidecarAndVatPairMoveWithTheirFile` (model in a folder that also holds a `.lua`) **red**.
   - `LinkedSourceDirectoryIsNotWalked`; `IdenticalDestinationIsUsedAndReported`; `DryRunChangesNothing` (plan equals the
     later result's moves).
5. `editor/tests/ProjectSaveAsTest.cpp`:
   - `SaveAsOfASavedProjectCopiesIt` **red** (today the new folder gets no assets).
   - `FailedSaveAsKeepsTheProjectWhereItWas` (destination with a file that makes the copy fail) **red** (today
     `getProjectPath()` is the new folder).
   - `SaveAsIntoTheProjectIsRefused` **red**.
   - `TempProjectDirSkipsBusyFolders` (`chooseTempProjectDir` with a fake `isBusy`) **red** (today always the base).
6. `editor/tests/util/TextureLabelsTest.cpp`: `AlphaBorderIdShowsTheFileName`, `SvgScaleShowsTheFileName`,
   `CubemapShowsFaceZeroAndAllPathsInTheTooltip`, `GeneratedTextureShowsItsId`.
7. `editor/tests/search/HierarchyFilterTest.cpp`: `UnnamedNodeMatchesItsDisplayName` (search `42` and `GameObject`).
   `editor/tests/util/EntityLabelsTest.cpp`: `EmptyNameReadsGameObjectAndId`.
8. `editor/tests/shortcuts/CommandCatalogTest.cpp`: `MarkdownReferenceMatchesDocs` (existing) after regeneration;
   new `ProjectDuplicateAndMaximizeOnPlayAreBound` (Ctrl+D in Project context, F11 Global, no repeat).
9. `editor/tests/util/ScriptTemplatesTest.cpp`: `LuaTemplateDeclaresTheModuleClass`, `CppScriptPairNamesTheClass`,
   `WriteTemplateFilesWritesTheTemplateStrings` (the wrapper and `create_script` produce identical bytes).

Unit test, `jasperengine-core-tests`: `engine/tests/core/texture/TextureIdTest.cpp`
`EmptyPathWithAlphaBorderHasNoId` **red** (today the id is `|alphaborder`), `PathWithAlphaBorderKeepsItsSuffix`.

MCP tests (`tools/build.sh linux`, then `tools/mcp/run-tests.sh -k ...`; `CI = True`):

- M1 `tools/mcp/tests/test_project_window_ops.py` (real input with `ui_input`, as `test_asset_identity_ui.py`; the
  fixture is a scratch copy with folders `A`, `C` and images `d1.png`..`d4.png`):
  - `test_paste_on_a_file_lands_in_its_folder` **red** (today: no copy, console "Couldn't copy").
  - `test_paste_on_a_folder_goes_into_it`, `test_create_menu_on_a_folder_creates_inside_it`.
  - `test_cut_paste_on_a_sibling_folder_moves_and_redraws` (M2 below is its red run).
  - `test_refused_cut_paste_keeps_the_clipboard` (cut `A`, paste into `A/sub`): refused, alert text,
    `ui.project_window.clipboard` still holds `A` **red** (today the clipboard empties).
  - `test_ctrl_d_duplicates_once_and_selects_the_copies` (held key; `undo scope=project` removes all copies).
  - `test_press_with_the_pointer_move_drags_only_that_file`: `xdotool mousemove X Y mousedown 1` in one invocation
    onto `d2.png` from empty space, drag onto `C`: only `d2.png` moves **red** (today neighbours move or the
    selection is the marquee's).
  - `test_create_lua_script_names_first_and_undoes` (right-click folder > Create > Lua Script, type, Enter; file with
    module `Goblin`; `undo scope=project` removes it) **red** for the undo (today Assets > Create > Lua Script cannot
    be undone).
  - `test_create_html_document_is_one_undo_step` **red** (today not undoable).
  - `test_root_folder_cannot_be_cut_or_dragged` (assets dir set to `assets`).
  - `test_texture_slot_shows_the_file_name` (a sprite's texture slot via `find_ui_item("ship.png", "Inspector")`)
    **red** (today the slot shows the path with `|alphaborder`).
  - `test_unnamed_object_reads_gameobject_and_id` (`eval_lua` creates an unnamed object during Play, `find_ui_item`
    in the Hierarchy) **red**.
  - `test_f11_toggles_maximize_on_play` (Play, key F11, `configure_game_view` read `maximized` true, F11 again false).
- M2 The use-after-free control: the cut-paste test above run against an ASan editor
  (`SANITIZE=address,undefined tools/build.sh linux`) on `main` with four images listed after `C`, two of them cut:
  **red** = an ASan heap-use-after-free report in the editor's stderr (kept scratch); green = no report. If ASan does
  not report on `main` (the destroyed entries' strings happened to fit SSO), the record says so and F2 stays
  "inferred"; the fix stands on the reading.
- M3 `tools/mcp/tests/test_assets.py`: `test_move_asset_copy_without_to_duplicates_beside` (Unity name, `to`
  returned, new GUID); `test_copy_into_itself_says_copy`.
- M4 `tools/mcp/tests/test_project.py`: `test_assets_dir_with_a_differing_destination_is_refused` **red** (today "Set
  1 setting(s)"); `test_assets_dir_dry_run_changes_nothing`; `test_assets_dir_result_lists_moves_and_removed_folders`
  **red** for `removed_folders` (and the emptied folder gone on disk); `test_unsaved_buffer_of_a_moving_file_refuses`.
- M5 `test_tool_conventions.py`, `test_call_cli.py`, the budget check and `gen_tool_docs.py --check` pass.

Per commit: `tools/check-changed` (AGENTS.md); the full suite at the batch landing.

## 11. Acceptance criteria

1. Paste from a file's menu lands in that file's folder; from a folder's menu inside the folder; Ctrl+V and the
   empty-space menu in the listing folder; during a search per D1 (M1, test 2).
2. No action inside the listing loop rebuilds `files`; `scanDirectory` during the loop is deferred (code review of
   6.1 plus the debug assertions; M1 and M2 pass, M2 recorded).
3. A refused or failed paste keeps a cut clipboard; a successful one empties it; cut items are dimmed; gone items are
   skipped with a note (tests 2, M1).
4. Duplicate works from Ctrl+D, the item menu, the tree menu, Assets > Duplicate and Edit > Duplicate (Project window
   last focused), one undo step, copies selected, one copy per key press (M1, test 1).
5. Moves and copies onto taken names land under Unity's free name; directories take the whole-name suffix; nothing
   is overwritten (test 1).
6. Copying, moving, duplicating or dropping a folder into itself is refused before any change, in the Project
   window, `CopyFileCmd`, `AssetOps` and `move_asset` (tests 1, 3, M1, M3).
7. A press arriving with the pointer move onto a file drags only that file; drops move the payload's files (M1).
8. The Create list is identical in the toolbar "+", every right-click menu (Create submenu) and Assets > Create (plus
   its Scene and Native Plugin extras); Create from a folder row or tree node creates inside that folder (M1, review).
9. Every Create item is one undo step; scripts and HTML documents are named before they are written; the Project
   window, the Inspector and `create_script` write identical templates (tests 9, M1).
10. Lua Script is disabled outside the Lua directory with a tooltip; C++ outside the Script Directories shows the note.
11. The assets and Lua roots cannot be moved, renamed or deleted from the Project window (M1).
12. An asset-root change with a blocker changes nothing and says why, in Project Settings and `set_project_settings`;
    a successful one reports moves, rewritten files, removed folders and broken text references; failed saves keep
    their scenes loaded and are reported; `dry_run` changes nothing (tests 4, M4).
13. Texture slots show file names and the tooltip the path; an empty path has an empty id (test 6, core test, M1).
14. Unnamed objects read `GameObject <id>` in the Hierarchy, its drag text, its search, the Inspector hint and object
    links (tests 7, M1).
15. F11 and Edit > Maximize On Play toggle the setting, effective at once during Play, with the 3 s hint; the menu
    bar stays visible; `configure_game_view` reports `maximized` (M1, test 8).
16. Save Project As of a saved project copies it; any Save As failure leaves the project where it was; Save As into
    the project is refused (test 5).
17. Two editors without a project use two temporary folders (test 5; review of `createTempProject`).
18. TOOLS.md regenerated, budget check passes, `keyboard-shortcuts.md` regenerated, `docs/editor/project-window.md`,
    the assets guide and the glossary updated.
19. Every **red** test in section 10 was run red on `main` and green on the branch, recorded in the implementation
    notes with commands and outputs.

## 12. Files likely touched

Hot (likely shared with other branches): `editor/window/ProjectWindow.{h,cpp}`, `editor/Project.{h,cpp}`
(`changeAssetRoots`, `saveProjectToPath`, `createProjectInDirectory`), `editor/window/InspectorWindow.cpp`,
`editor/window/HierarchyWindow.cpp`, `editor/App.cpp`, `editor/AppCommands.cpp`, `editor/shortcuts/DefaultCommands.cpp`,
`editor/mcp/tools/AssetTools.cpp`, `editor/mcp/tools/ProjectTools.cpp`, `editor/mcp/tools/ScriptTools.cpp`,
`editor/window/dialog/ScriptCreateDialog.{h,cpp}`, `editor/window/dialog/ProjectSettingsWindow.cpp`,
`engine/core/texture/Texture.cpp`, `docs/mcp/TOOLS.md`, `tools/mcp/budget.json`, `docs/editor/keyboard-shortcuts.md`,
`docs/unity-glossary.md`.

Less contended: `editor/command/type/CopyFileCmd.{h,cpp}`, `editor/asset/AssetOps.{h,cpp}`,
`editor/asset/AssetClipboard.{h,cpp}` (new), `editor/window/AssetCreateMenu.{h,cpp}` (new),
`editor/util/ScriptTemplates.{h,cpp}` (new), `editor/util/TextureLabels.{h,cpp}` (new), `editor/util/EntityLabels.h`
(new), `editor/util/ProjectFileWrites.{h,cpp}`, `editor/util/HtmlUITemplates.{h,cpp}`, `editor/settings/SettingsRows.cpp`,
`editor/settings/SettingsRegistry.h`, `editor/search/HierarchyFilter.h`, `editor/window/GameWindow.{h,cpp}`,
`editor/AppMenuCommand.h`, `editor/mcp/tools/CoreTools.cpp`, `editor/mcp/tools/ViewTools.cpp`,
`docs/mcp/guide/assets.md`, `docs/editor/project-window.md` (new), the tests of section 10.

## 13. Cross-feature dependencies

- `scene-integrity`: "copied scenes get new ids" must live in the copy path (`AssetOps::copy` or `CopyFileCmd`) so
  Duplicate and Paste of a `.scene` inherit it; this spec does not implement it and its tests duplicate a scene only
  once that lands (or assert the id rule there). Both edit `editor/Project.cpp` (Save All and save callbacks there,
  Save As and the asset roots here) and `HierarchyWindow.cpp` (drop targets there, labels here): merge order free,
  conflicts expected to be textual.
- `unicode-text`: path and name handling in `ProjectWindow.cpp` (`scanDirectory` uses `filename().string()[0]`) and
  forward slashes in saved paths. New code here uses `FileUtils::pathToUtf8`; whoever merges second resolves the
  `scanDirectory` lines.
- `scripting`: "Inspector Add Script accepts Lua" touches `ScriptCreateDialog`; this spec moves its templates to
  `ScriptTemplates`. Merge either first; the second rebases onto the other's file layout.
- `ui-components`: edits `InspectorWindow.cpp` around Button textures and MCP refusal; the texture-slot block
  (:4625-4690) is ours. `McpValues.cpp` is theirs only.
- `agent-authoring`: edits `ProjectTools.cpp` (held edits during a human save) and the agent guide; our
  `set_project_settings` change is in its handler and schema. `McpInstructions.cpp` is not touched here.
- `export-pipeline`, `build-toolchain`: `AppSettings` recent projects (Save As adds the copy, as today's path does);
  no shared functions otherwise.
- `sprite-batching`: if it keys batches on texture ids, an empty-path texture's id becomes empty (6.10); batching must
  treat an empty id as "no texture", as `Texture::empty()` does.
- `code-editor-git`: the Code Editor tab handling after a moved or undone file (`handleFileRename`, `closeFile`) is used
  as is.

## Deviations

Recorded while implementing on `dc/project-window`; each keeps the spec's intent.

1. **Cut items** are dimmed with the alpha the listing already uses for files outside the build (0.4), not 50%: one
   dimming look in the window.
2. **HTML Document**: `HtmlUITemplates` gains no `documentFiles`; the Project window builds the two write items from
   `documentText` / `styleSheetText` and writes them as one undo step. `createDocument` stays for the HTML UI tests
   (it is the no-project writer).
3. **Two catalog commands for Duplicate**, as Rename has (`assets.rename` / `project.rename`): `assets.duplicate`
   (Assets > Duplicate, no key) and `project.duplicate` (Ctrl+D, Project context). The palette lists both, like Rename.
4. **Save Project As** does not save the open scenes on its own: the copy is made from disk, the editor continues in
   it, and unsaved scene edits are written into the copy by the next Save (as a saved scene's edits always are).
5. **Protected roots** (D10): `ProjectWindow::rootRefusal` has its own check (lexical and resolved, as
   `McpRefs::trustedRootTouched`) to word the Project Settings path the user needs; it also refuses the project folder.
6. **Edit menu routing** goes through `main`'s panel claims (S61, `PanelClaims`), which landed after the spec: the
   Project window's claim gains Duplicate (`project.duplicate`) beside its Delete, Copy, Cut, Paste, Select All and
   Rename, so Edit > Duplicate and Ctrl+D act on the assets while the Project window has focus. No `App` routing of
   its own.
7. **Tests not written as named**: the Vertex Animation pair half of `HumanoidSidecarAndVatPairMoveWithTheirFile` (the
   pair move reuses `vat::existingPairOf` and the planned-move path the sidecar test covers; a valid baked pair fixture
   is VatEditorTest's), and the MCP `test_unsaved_buffer_of_a_moving_file_refuses` (no MCP tool makes a Code Editor
   buffer dirty; the refusal is the plan's `unsavedBuffers` check).
8. **MCP text** follows the lean rules merged after the spec (`fe3e90b97`): the agent guide is untouched; the new
   `move_asset` `to` wording and `set_project_settings` `dry_run` parameter each have an eval case
   (`guard_duplicate_asset_beside`, `guard_asset_root_dry_run`).
9. **The texture slot's MCP check** (`test_texture_slot_shows_the_file_name`) is a unit test only
   (`TextureLabelsTest`): `find_ui_item` sees items with an id, and the slot's name is plain text. Its red control is
   the helper run with the old id-based text.
10. **The shortcuts doc** is regenerated as AGENTS.md says: the unit test with `JASPER_UPDATE_SHORTCUT_DOCS=1` run
    inside the image (`tools/build.sh exec linux`), since `build.sh` does not forward the variable.
11. **Linked folders (S68, `docs/specs/S68-linked-folders.md`)**: a linked folder is a folder of the project. In the
    Project window a copy, paste-copy or duplicate of one copies what it holds (a real copy, new GUIDs); a cut-paste,
    drag or rename moves the link entry itself, never its target; the into-itself guard compares both as named and
    resolved, so a link leading back into a copied folder is refused too. The asset-root change treats containment by
    path (a link leading out of the project is inside it, R7) and **moves the link, not its target**: a referenced file
    reached through a linked folder relocates by moving the topmost link on its path as one entry, so the target (a
    library shared with other projects, say) is never emptied, and every file reached through the link keeps its layout
    under the link's new place. Test: `AssetRootChangeTest.ALinkedFolderMovesAsTheLinkAndItsTargetStays`.
12. **Modals (S69, landed after the spec)**: the Create name dialog, the delete refusal and Change Asset Directories go
    through `EditorModal`, so agents see and answer them (`respond_dialog`, field `name`). The folder prompt's three
    nested error boxes became the one name dialog with its error inline (`EditorModal::error`), so
    `test_editor_modals.py` drives "New Folder" instead of "Create New Directory" and counts 68 modal sites instead of 71;
    its error-box test uses the rename prompt's Invalid Name box instead.
