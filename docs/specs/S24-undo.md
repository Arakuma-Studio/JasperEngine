# S24 — Undo & play-mode change review

Wave 1. Depends on S02 (gtest target). Works with S03 (file writes, recovery starts an empty
history), S04 (`AssetOps` go on the project history), S06 (background lane) and S09 (RAII
subscriptions). It feeds S08, S14, S18, S20, S21, S25, S29 and S31: all of them implement or call
the interfaces defined here.

## 1. Goal

Every undo step has a name ("Undo Move Player"). The history is capped by memory, so a long
session no longer grows without bound. Esc (or Ctrl+Z) during a gizmo or field drag puts the
values back, and edits that changed nothing never become steps. Selection is part of history,
as in Unity. Ctrl+Z follows one timeline that interleaves scene and asset edits, so the window
that last had focus no longer decides what gets undone. An Undo History panel lets the user jump
to any step. Agent (MCP) edits carry the agent's name in the history. After Play, the user can
keep chosen play-time edits: they are replayed onto the restored scene as one undoable step.

## 2. Scope

**In scope**
- `Command` naming, type ids, entry ids and timestamps. Every class that derives `Command`
  gets a name (80 in `editor/command/type/` and about 10 elsewhere, §3).
- `CommandHistory`: depth and memory caps, `InteractiveEdit` (Esc-cancel), dropping no-op
  commands, selection steps, origins (user/agent), linked groups, `BackgroundRelease` for big
  frees, a before-mutation hook.
- `UndoTimeline`, the single Ctrl+Z timeline over the project history and the scene histories.
  It defines Edit menu labels and keyboard routing.
- Undo History panel, Editor Settings ▸ Undo, Play Mode Changes window.
- Prefab override-aware undo: an audit and a test matrix over `PrefabEditUndo`.
- MCP: `get_undo_history`, `undo`/`redo` extensions, the play-change tools, attribution. Tests.

**Out of scope**
- Text undo inside the Code editor and ImGui text fields. They keep their own undo.
- The MCP approval gate and permission modes (report 09 §5 item 3). No spec owns these yet,
  and S24 only attributes and groups agent edits.
- Play-change review for runtime copies of child scenes (`ownedRuntime`). They have no history
  (`Project.cpp:5014-5017`).
- A persisted history (saved across editor restarts). Unity has none either.
- Their byte-snapshot component undo (`MultiEntityComponentSnapshot.h`, `EntityComponentsEdit.h`).
  It depends on POD components (do-not-copy: their ECS core). We keep our property-granular
  commands.

## 3. Current state

- `editor/command/Command.h` (29 lines) declares `execute/undo/mergeWith/affectsStructure/setNoMerge`.
  There is **no name**, no id, no size and no no-op query.
- `editor/command/CommandHistory.{h,cpp}` (61+195 lines) is a `vector<Command*>` plus an index.
  - Merging happens on push (`CommandHistory.cpp:52-77`), and the anonymous `CommandGroup`
    (`:12-38`) holds grouped commands.
  - `mark()/groupSince()` (`:139-155`) do the grouping. The play floor is
    `beginPlaySession/endPlaySession` (`:163-195`).
  - There is **no cap**. A trimmed or dropped entry is deleted on the main thread.
- `editor/command/CommandHandle.{h,cpp}` keeps a static map from scene id to `CommandHistory`,
  one per open scene and per prefab stage. `Project::getProjectCommandHistory()`
  (`editor/Project.cpp:2787`) holds the asset file operations: rename, copy, delete,
  create dir, material file, `WriteFilesCmd`.
- Routing by focus is the root of the "wrong thing undone" bug:
  - `App::buildMenuModel` (`editor/App.cpp:428-440`) and the menu handler (`:677-696`) pick
    the project history when `lastFocusedWindow == Resources`, the code editor when it is Code,
    and the selected scene otherwise.
  - The keyboard path (`App.cpp:1972-1981`, `:2011-2019`) again picks by `projectWindow->isFocused()`.
  - So renaming a file and then clicking the Scene view makes Ctrl+Z undo a scene edit.
  - The menu says only "Undo" / "Redo" (`App.cpp:499-504`).
- Drags are streams of merging commands that the caller ends with `setNoMerge()`:
  - gizmos: `editor/render/SceneRender.cpp:1375-2007` (`dragCommand`, `lastCommand`);
  - inspector rows: `editor/window/InspectorWindow.cpp:600-609`, `:1350`, `:1758`, `:2197`;
  - terrain strokes: `editor/window/TerrainEditWindow.cpp:932-1268`.
  - Esc does nothing to a drag. It is used only to leave the camera preview (`SceneWindow.cpp:1002`).
- No-ops become steps. `PropertyCmd::execute` (`editor/command/type/PropertyCmd.h:117-160`) skips
  equal values but still returns true and sets `isModified = true`.
- Selection is not undoable:
  - It is stored in `SceneProject::selectedEntities` (`editor/Project.h:136`).
  - It is set through `replaceSelectedEntities/setSelectedEntity/addSelectedEntity/clearSelectedEntities`
    (`Project.h:1397-1404`), from 33 call sites in 17 files, and directly in `PrefabStage.cpp:990`.
- Prefab override-aware undo is `editor/command/PrefabEditUndo.h`. 13 command types use it:
  Property, ObjectTransform, ComponentValues, Add/RemoveComponent, EntityName, EntityState,
  Link/UnlinkMaterial, MeshChange, ModelLoad/Clear, CreateTrackKey, plus terrain commands.
- `editor/util/BackgroundRelease.h` frees YAML on a worker. Only `DeleteEntityCmd.cpp:163`,
  `DuplicateEntityCmd.cpp:94`, `PasteEntitiesCmd.cpp:90` and the clipboard use it.
- MCP:
  - `undo`/`redo` take `scope: scene|project` and `steps` (`editor/mcp/tools/CoreTools.cpp:218-262`).
  - `batch undo_group` uses `mark/groupSince` per history, and its result has `complete` (`:46-72`).
  - There is no way to list the history. 26 test files in `tools/mcp/tests` exercise undo.
- Play:
  - `Project.cpp:7927` starts the session (`beginPlaySession`).
  - `finalizeStop` (`:4996-5017`) restores the snapshot (`restorePlaySnapshot`, `:4844-4930`)
    and then calls `endPlaySession(restored)`.
  - Every play-time edit is dropped silently.
- CLI `scene-dump --ops` undo/redo uses its own `done`/`undone` vectors
  (`editor/cli/CommandLine.cpp:1036-1050`), not `CommandHistory`.
- There are no C++ tests of `CommandHistory`. S02 adds the base suite (S02 §5 "CommandHistory").

## 4. Reference (OpenEngine, MIT — `NOTICE.md` entry if any code is copied)

| File | Take | Improve |
|---|---|---|
| `Apps/Editor/Include/UndoRedo/IEditorCommand.h` | `GetName()` + `GetTypeName()` split | Our name is captured at first execute (entity names change later) |
| `Apps/Editor/Include/UndoRedo/UndoRedoService.h` (647 lines) | Lockstep ids/timestamps (`GetUndoEntryIdAt`, `:71-98`), `TrimUndo` (`:597-610`), `InteractiveEdit` (`:293-420`: no-op commit dropped, cancel restores), `SetBeforeMutation` (`:64`), `SetOnHistoryChanged` | A memory budget, not just `maxUndo = 1024`. A cancel also restores the redo tail. A history per scene plus a merged timeline instead of one global stack |
| `Apps/Editor/Source/Panels/UndoHistoryPanel.cpp` (257 lines) | List with names and times, click to jump, back/forward | ImGui; filter, selection toggle, memory footer, play-floor divider |
| `Apps/Editor/Source/UndoRedo/DeleteEntitiesSelectionCommand.h` (73 lines) | Undoing a delete re-selects | We generalise it: every entry stores the selection before and after |
| `Apps/Editor/Source/Panels/HierarchyPanel.cpp:1373-1460` | `m_SuppressSelectionUndo` re-entrancy idea | Recording happens centrally in `Project`, not per panel |
| `Apps/Editor/Source/PlayMode/PlayModeManager.cpp:421-505` and `Panels/PlayModeChangeReviewModal.cpp` (169 lines) | Play-time history → pending list → keep chosen as one compound "Apply Play Mode Changes" | They re-execute the play-time command objects, so undo restores play-time "before" values. We build **fresh** commands from the final play values, and they execute against the restored scene, so undo returns to edit-time values and prefab overrides are captured on the restored world. Rows are per property, and the window is non-modal |
| `Tests/Editor/PlayModeChangeReviewTests.cpp` (331 lines) | Test ideas | gtest + MCP |

The code is re-implemented, so we expect to copy nothing verbatim. If an implementer copies a
block anyway, they add the attribution.

## 5. Requirements

### Functional — names, ids, entries

- **R1** `Command` gains these members:
  - `virtual std::string describe() const = 0`: pure, so every subclass must name itself.
  - `virtual const char* typeName() const = 0`: a stable id such as `"PropertyCmd"`.
  - `virtual bool isNoOp() const { return false; }`
  - `virtual size_t memoryBytes() const { return sizeof(*this); }`
  - `virtual bool releaseOffThread() const { return false; }`
  - `virtual void collectKeepCommands(KeepContext&) const {}` (R26)
  - `const std::string& name() const`

  `CommandHistory` sets `name()` from `describe()` after the first successful execute and again
  after every merge. Undo and redo never change it.
- **R2** Names follow Unity verbs, `<Verb> <subject>`, and are at most 80 characters (an
  ellipsis cuts the subject):
  - **Entity subject:** the GameObject name, or "N Objects" when N>1.
  - **Examples:**
    - Move/Rotate/Scale Player
    - Set Intensity (Sun) / Set Intensity (3 Objects); S08 R11 uses the same form
    - Add Component Rigidbody (Player), Remove Component …, Reset Light (Sun),
      Paste Component Values …
    - Create GameObject, Create Prefab Player.prefab, Delete 3 Objects, Duplicate Player,
      Paste 2 Objects, Reorder Player, Rename Player → Hero
    - Rename File a.png → b.png, Move 2 Files, Delete 2 Files, Copy File, Create Folder,
      Create Material, Write 3 Files
    - Link Material …, Unpack Prefab …, Apply Overrides …, Sculpt Terrain, Paint Terrain
    - Select Player / Select 3 Objects / Deselect All
    - Apply Play Mode Changes (N)
  - **Property names** come from the inspector label, through S08 display names once S08
    lands. Until then they come from the Catalog property name, title-cased.
- **R3** Every pushed entry has an `EntryInfo`:
  - `uint64_t id`: global and strictly increasing across all histories. A merge keeps the
    older id.
  - `std::chrono::system_clock::time_point time`: updated on merge.
  - `Origin`: kind User | Agent | PlayReview | System, plus client name, MCP call id and tool.
  - `uint64_t linkId` (0 = none) and `size_t bytes`.
  - `SelectionState before/after` (scene histories only, R14).
- **R4** `CommandGroup` becomes a named public type (`editor/command/CommandGroup.h`):
  - `describe()` is the group label if one was set.
  - Otherwise it is the single child's name, or "<first child name> (+N more)".
  - `isNoOp()` is true only when every child is a no-op.

### Functional — caps and memory

- **R5** The caps:
  - `CommandHistory::maxEntries` applies per history: default 1000, setting range 50-10000.
  - A global `UndoBudget` (default 512 MB; setting range 64-8192 MB, 0 = unlimited) sums
    `bytes` over all histories.
  - After each push, the oldest entries are trimmed until both caps hold. Trimming picks the
    lowest id over all histories, so the oldest edit in any scene goes first.
  - Trimming never removes the entry that was just pushed.
  - Trimming never removes play-time entries above a `playFloor`. When only those remain over
    the budget, it logs one warning per session.
  - Trimming never removes entries inside an open `InteractiveEdit` or `UndoGroupScope`
    (R10, R21). It waits until they close.
  - The redo tail is never trimmed.
- **R6** Positions are absolute:
  - `position()` and `mark()` return `trimmedCount + index`, so a mark survives trimming.
  - `groupSince(mark)` returns false when `mark < trimmedCount`. The existing false cases stay.
  - `playFloor` is stored the same way.
- **R7** `memoryBytes()` must be implemented for every command that holds YAML, a
  `ComponentClip`, file contents, terrain patches or mesh buffers:
  - Delete, Duplicate, Paste, CreatePrefab, ImportPrefab, ReplacePrefab, UnpackPrefab,
    ComponentValues, ModelLoad/Clear, MeshChange, TerrainMapPatch, the terrain commands,
    `WriteFilesCmd`, DeleteFile, CreateMaterialFile, CopyAs, `McpStepsCmd`.
  - A new helper, `yamlFootprint(const YAML::Node&)` in `editor/util/`, estimates a YAML tree
    as 64 bytes per node plus scalar lengths.
  - Each value is computed once at push or merge and cached in `EntryInfo.bytes`.
- **R8** BackgroundRelease:
  - An entry is freed through `BackgroundRelease::release(std::unique_ptr<Command>)` when it is
    trimmed, dropped (redo tail cut, play session rolled back, cancelled, no-op), or cleared
    (scene close, `CommandHandle::remove`), and when it has `releaseOffThread()` and
    `bytes >= 256 KB`.
  - `releaseOffThread()` may return true only when the destructor frees memory and touches no
    editor, engine or GPU state.
  - Every command marked off-thread gets a review checklist line in its header.

### Functional — interactive edits, no-ops

- **R9** No-ops:
  - After `execute()` returns true, `addCommand` checks `isNoOp()`. A no-op command is
    undone, freed and not recorded. `addCommand` still returns true, and the scene's
    `isModified` stays as it was.
  - `PropertyCmd` (all entities' old==new, when T is comparable), `MultiPropertyCmd` (all
    children), `ObjectTransformCmd` (equal TRS), `ComponentValuesCmd` (equal YAML),
    `EntityNameCmd` and `EntityStateCmd` implement `isNoOp`.
  - `PropertyCmd::execute` no longer sets `isModified` for a no-op.
- **R10** `InteractiveEdit`:
  - `CommandHistory::beginInteractive(name)` returns an RAII `InteractiveEdit` with
    `commit()`, `cancel()` and `isOpen()`.
  - From begin onward, the history sets the redo tail aside (as the play floor does) and
    collects every command pushed until the edit closes. Merging works as today.
  - `commit()` folds the collected commands into one entry named `name`, or the single
    command's name.
    - If the entry `isNoOp()`, it is dropped and the redo tail comes back.
    - Otherwise the redo tail is freed (R8).
  - `cancel()` undoes the collected commands newest first, frees them and restores the redo
    tail. The scene values, prefab overrides, `isModified` and selection are byte-identical to
    before `beginInteractive`.
  - The destructor commits and logs a debug warning, so a forgotten close never loses user
    work.
  - At most one edit is open per history. A second begin commits the first.
- **R11** These drags must run through `InteractiveEdit`:
  - gizmo translate/rotate/scale, and every other drag in `SceneRender.cpp`: track points,
    polygon points, UI rect resize, 2D handles;
  - inspector drag/slider/colour widgets (`IsItemActivated` begins, `IsItemDeactivatedAfterEdit`
    commits);
  - terrain sculpt/paint/foliage strokes;
  - the animation window's key drags.

  New tools in S18, S20 and S21 must use it as well.
- **R12** Esc cancels:
  - While an `InteractiveEdit` is open, Esc cancels it. This works in the Scene view, the
    Inspector, the Terrain window and the Animation window.
  - The cancel also calls `ImGui::ClearActiveID()`, so the widget stops dragging.
  - Ctrl+Z while an edit is open cancels it and undoes nothing more.
  - Opening a menu or popup, losing window focus, or starting Play commits the open edit.
  - Text entry in an `InputText` keeps ImGui's own Esc (revert the text).
- **R13** `CommandHistory::beforeMutation` is a static hook. It runs before every push, undo,
  redo, jump and trim, and it:
  - commits any open `InteractiveEdit` in another history;
  - refuses (returns false; the caller shows "Busy: <reason>") while a Play start/stop is in
    progress (`ScenePlayState::LOADING`/stopping) or a modal save is running.

### Functional — selection

- **R14** Each entry in a scene history stores the selection before execute and after execute:
  an ordered entity list plus the active entity. Undo restores `before` and redo restores
  `after`, minus entities that no longer exist. This is what makes undoing a Delete re-select
  the restored objects (the `DeleteEntitiesSelectionCommand` behaviour) and undoing a
  Duplicate re-select the originals.
- **R15** All writes to the selection go through `Project::setSelection(sceneId, SelectionState,
  SelectionCause)`. The existing four setters forward to it, and `PrefabStage.cpp:990` is
  converted.
  - **During a command:** a change made while a command executes, undoes or redoes, or while
    `SelectionCause::Restore` applies, is not recorded on its own. It becomes part of the
    command's `after`.
  - **Standalone change:** any other change records a `SelectionCmd` entry ("Select Player").
    That covers a Hierarchy click, a Scene pick or box select, Select All, and MCP
    `select_entities`.
  - **Same gesture:** changes in one frame, or during one box-select drag, merge into one entry.
  - **Not recorded:**
    - deselection forced by destruction (`deselectDestroyedEntities`) and by scene reload;
    - a standalone change while the history has a redo tail. This deviates from Unity on
      purpose: clicking around after an undo must not destroy the redo.
- **R16** A `SelectionCmd` entry:
  - is non-structural (`affectsStructure() == false`);
  - never sets `isModified`;
  - is skipped by the play review;
  - is hidden in the Undo History panel unless "Show Selection Changes" is on.

  The "Record Selection in Undo" setting (default on) turns standalone selection entries off.
  R14 stays active either way.

### Functional — one timeline (exact semantics)

- **R17** Histories: `P` is the project history (asset/file edits). `S_x` is one history per
  open scene or prefab stage x, as today. The **active timeline** `T(x)` for the selected scene
  x is the union of `S_x` and `P`, ordered by entry id.
- **R18** `UndoTimeline::undo(x)`:
  1. Take the newest applied entry of `S_x` and of `P`, and undo the one with the larger id.
  2. If it has a `linkId`, also undo every other entry with that `linkId` that is the newest
     applied entry of its history (any history, newest id first).
  3. A linked entry that is not on top stays applied. A Console warning names its history:
     "Batch step partly undone: Scene 'Level2' has newer edits".
- **R19** `UndoTimeline::redo(x)` is the mirror image: of the first redo entries of `S_x` and `P`,
  redo the one with the smaller id, with linked partners as in R18.
- **R20** Pushing a new entry into `S_x` frees the redo tails of `S_x` and `P`. Pushing into `P`
  frees the redo tails of `P` and of the selected scene's history. Other scene histories are
  never touched. Switching scene tabs never changes any history.
- **R21** Routing:
  - Ctrl+Z / Ctrl+Y (Cmd+Shift+Z on macOS) and Edit ▸ Undo/Redo go to the active ImGui text
    field (`WantTextInput`), then to the Code editor if it has focus, and otherwise always to
    `UndoTimeline` for the selected scene.
  - `lastFocusedWindow == Resources` no longer selects a history, and the keyboard paths at
    `App.cpp:1972-1981` and `:2011-2019` are merged into one.
  - After any undo or redo that touched `P`, `projectWindow->refreshCurrentDirectory()` runs.
- **R22** Asset edits always go to `P`:
  - rename, move, copy, delete (S04 `AssetOps`, to `.trash`), create folder/material/script;
  - `.material` file edits (`WriteFilesCmd`, `update_material`);
  - import settings and `.meta` edits (a generic `SetAssetMetaCmd(path, key, old, new)` that
    S14 and S04 reuse);
  - VCS revert (S29).

  A material edit that changes both a linked submesh (scene) and the file is one linked group
  across `S_x` and `P`.
- **R23** Closing a scene, or calling `CommandHandle::remove`, frees `S_x`. Entries of `P` linked to
  it lose the link.

### Functional — agent attribution and groups

- **R24** Agent attribution:
  - `McpServer::execute` sets an `UndoOriginScope` for the duration of each tool handler. The
    scope is set again around every job continuation of that call, and it holds
    {Agent, client name from MCP `initialize.clientInfo.name` (sanitised, ≤32 chars, default
    "Agent"), call id, tool}.
  - Commands pushed in that scope carry the origin, and the displayed name becomes
    "<client>: <name>", for example "Claude Code: Set Intensity (Sun)".
  - When the call replies, each history's contiguous top run of entries from that call is
    folded into one entry named after the tool title and subject. Those entries are linked
    across histories.
  - A run broken by a user entry is not folded, and each part keeps its attribution.
  - Read-only and Control tools are not wrapped. `batch` is not wrapped; its calls are.
- **R25** `batch undo_group` keeps today's contract:
  - `mark()` per history at start, `groupSince` per history at end;
  - `complete=false` when any `groupSince` refuses;
  - the same refusals (undone past the mark, a redo tail in the way, a play boundary).

  New:
  - The groups are linked, so one Ctrl+Z undoes the whole batch (R18).
  - They are named by the new optional `undo_name` argument, default "Batch (N calls)".
  - `UndoGroupScope` holds back trimming until the batch ends.

### Functional — play-mode change review

- **R26** Capture at Stop:
  - When `finalizeStop` reaches a scene with a history (not `ownedRuntime`), it calls
    `PlayChangeReview::capture(sceneProject)` **before** `restorePlaySnapshot`, so the
    play-time state is still live.
  - `capture` walks the play-time entries (above `playFloor`, selection entries excluded) and
    calls `collectKeepCommands`. Each command adds rows, each with:
    {entity, component, property path | whole component | entity created/deleted, the play
    value as display text, the source entry's name, and a factory for a **fresh** command
    that sets that final value}.
  - Rows are deduplicated by (entity, component, property): the last write wins.
  - Implementations:
    - `PropertyCmd<T>` makes a new `PropertyCmd<T>` with the current value read through
      `Catalog::findProperty`.
    - `MultiPropertyCmd` makes one per child, and `ObjectTransformCmd` uses the current TRS.
    - `ComponentValuesCmd` and Paste Component Values capture a clip of the current component.
    - Add/RemoveComponent, EntityName and EntityState are supported.
    - Create/Duplicate/Paste encode the created subtree with `Stream::encodeEntity`. Their
      replay pastes it under the same parent, keeping the entity ids.
    - Delete becomes a `DeleteEntityCmd` on the restored entities.
    - Commands that do not implement it produce one disabled row, "<name>: can't be kept".
- **R27** "Keep Values After Play" catches changes made by scripts:
  - During Play, the Inspector component menu has "Keep Component Values After Play" and the
    Hierarchy GameObject menu has "Keep Values After Play (all components)".
  - These add marks, and at capture each mark becomes a row backed by a `ComponentValuesCmd`
    of the current values.
  - The rows list the differing YAML keys. Rows are per component until S08 provides
    per-field paths.
- **R28** After restore, a row is keepable only when its target exists in the restored scene.
  For a create, the parent must exist. Rows that touch runtime-only entities (not in
  `playStateEntities` or `entities`) are disabled with the reason
  "Created by a script during Play".
- **R29** Apply (Keep Selected / Keep All / MCP):
  - The chosen factories are built in capture order into a `CommandGroup` "Apply Play Mode
    Changes (N)" with origin PlayReview, then pushed to `S_x` as one entry.
  - Fresh commands capture old values and `PrefabEditUndo` on the restored world, so the
    edit-time values and override records are what undo restores.
  - A factory whose command fails is skipped and reported. The rest still apply.
  - Discard frees everything (R8).
- **R30** The pending review is dropped when the user applies or discards it, closes the
  window, starts Play again in that scene, closes that scene, or closes the project.
  - When the "Review Play Mode Changes" setting is off (default on), Stop behaves as today.
  - When Stop came from MCP, the window does not open. The Console instead logs
    "N play-mode changes can be kept: Window ▸ Play Mode Changes".

### Prefab override-aware undo

- **R31** Every command that changes a value on a GameObject of a prefab instance restores both
  the value and its override record on undo, through `PrefabEditUndo`. Merges keep the oldest
  state.
  - The audit adds it to: `ComponentToPrefabSharedCmd`, `TerrainMapPatchCmd` (on instance
    terrains), `ScenePropertyCmd` (n/a, documented), `MultiPropertyCmd` (it delegates; test
    only), `InteractiveEdit` cancel (R10), the play-review apply (R29) and every MCP-local
    command (`EntityTools.cpp:587`, `McpStepsCmd`).
  - Structural instance commands (`AddEntityToPrefabCmd`, `RemoveEntityFromPrefabCmd`,
    Unpack, Replace, the PrefabOverride(s) commands) are covered by the same test matrix (AC14).

### Editor UX

- **R32** The Edit menu (`App.cpp:499-504`) shows "Undo <name>" / "Redo <name>" for the
  timeline's next entry, and plain "Undo"/"Redo" (disabled) when there is none or a text field
  or the Code editor is active. `UndoTimeline::onChanged` (an S09 subscription; a
  `std::function` until S09 lands) refreshes the native menu model only when the label changes.
- **R33** The Undo History panel lives in Window ▸ General ▸ Undo History and is dockable. It is
  in the default layout, tabbed with the Console.
  - **Rows:** "(Start)" first, then the entries of `T(selected)` oldest to newest. Each row
    shows an icon (scene / asset / selection / agent / play review), the name and a relative
    time ("12 s"). The tooltip adds the absolute time, the history (scene name or "Project"),
    the origin, the size and the type name.
  - **Current position:** the current entry is highlighted. Redo rows are dimmed.
  - **Play Mode:** during Play, a divider marks the play floor and edit rows are disabled.
  - **Click to jump:** clicking a row jumps there through R18/R19 steps until that entry is the
    newest applied one; clicking "(Start)" undoes everything. A jump stops at the first refused
    step and shows why.
  - **Toolbar:** Back and Forward buttons, a text filter, a "Show Selection Changes" toggle,
    and a "Show other scenes" toggle that lists the other histories read-only.
  - **Footer:** "Memory: 37 MB / 512 MB" and "Clear History…" (with a confirmation; clears
    `T(selected)`).
- **R34** Editor Settings ▸ Undo has these settings, all in `AppSettings`:
  - Max Undo Steps;
  - Undo Memory Limit (MB);
  - Record Selection in Undo;
  - Review Play Mode Changes.

  S25 settings search must index them.
- **R35** The Play Mode Changes window (Window ▸ General ▸ Play Mode Changes) is non-modal.
  - **Opening:** it opens on Stop when there are rows.
  - **Rows:** a tree of GameObject → component → property. Each property row has a checkbox, the
    edit value (read from the restored scene), the play value and the source step name.
    Disabled rows show the reason as a tooltip, and every keepable row starts checked.
  - **Buttons:** Keep Selected, Keep All, Discard. Closing the window discards.
  - **Selection:** clicking a GameObject row selects it in the Hierarchy. That is recorded like
    any selection change.

### Agent UX (MCP, CLI)

- **R36** `get_undo_history` (Read):
  - Arguments: `{scene?, scope: "timeline"|"scene"|"project" = "timeline", limit = 30 (1..500),
    include_selection = false, include_redo = true}`.
  - Result: `{entries:[{id, name, type, history:"project"|scene id, origin:{kind, client?, tool?},
    time, bytes, applied, link_id?}], undo_name, redo_name, can_undo, can_redo,
    memory:{used, limit}, play_floor_id?}`.
  - Entries are listed newest first. The text is one line per entry, budgeted at about 2 KB
    for 30 rows.
- **R37** `undo` / `redo` (extended, backward compatible):
  - `scope` accepts "timeline" as well. The default stays "scene" for one release, and the
    agent guide recommends "timeline". `steps` is unchanged.
  - New `to_id`: step until that entry is the newest applied one (undo) or has been applied
    (redo). It fails if the id is not in the chosen scope.
  - New `expect_top_id`: refuse, without changing anything, unless the next entry is that id.
    This is the safe way for an agent to undo only its own last call.
  - The result lists the `{id, name}` of every step it undid or redid.
- **R38** Every non-read-only MCP tool result includes `undo: {id, name}` in `structured` for
  the entry its call produced, after folding (R24). The field is absent when the call made no
  entry.
- **R39** Play-change tools:
  - `get_play_changes {scene?}` → rows `{id, game_object, component, property, edit_value,
    play_value, source, keepable, reason?}`.
  - `apply_play_changes {scene?, ids?: [..] | all: true}`: one undo step. It returns `undo`
    and the skipped rows. Rows not listed are discarded.
  - `discard_play_changes {scene?}`.
  - `play action=stop` gets `keep_changes: "none"|"all"`. When the argument is absent, the
    review stays pending (R30).
  - `get_play_state` reports `pending_play_changes: N`.
- **R40** `select_entities` records a selection step attributed to the agent (R15).
  `editor_status` gains `undo: {undo_name, redo_name}` for the selected scene.
- **R41** CLI `scene-dump --ops` replaces its `done`/`undone` vectors with a `CommandHistory`, so the
  headless prefab tools (`tools/prefab-*.py`) exercise the real merge, no-op and group code.
  A new op, `history`, prints `id name` lines. Byte-identical prefab re-saves must stay
  identical.
- **R42** There is no Lua API: undo is editor-only. The MCP agent guide
  (`editor/mcp/McpInstructions.cpp`, `docs/mcp/TOOLS.md`) gets an "Undo" section covering the
  timeline, `expect_top_id`, attribution and play changes.

### Performance

- **R43** These budgets are checked by the gtest benchmarks in AC17:
  - Main-thread cost of a push with trimming: ≤ 50 µs for an entry with no YAML.
  - Freeing a 1000-GameObject Delete entry: ≤ 0.5 ms on the main thread (R8).
  - `yamlFootprint` on a 1000-GameObject encode: ≤ 10% of the encode time.
  - The Undo History panel: ≤ 0.3 ms per frame with 1000 entries (it uses `ImGuiListClipper`).
  - Selection recording: zero allocations when the selection does not change.

### Data & compatibility

- **R44** No file format changes, and no scene or asset version bump. Settings are new
  `AppSettings` keys with defaults. The MCP changes only add optional arguments and result
  fields; `scope` keeps its default and `steps` behaves as before.

## 6. Design constraints & decisions

- Keep our command pattern and the per-scene `CommandHistory`. `UndoTimeline`
  (`editor/command/UndoTimeline.{h,cpp}`) is a stateless view over `CommandHandle` and
  `Project::getProjectCommandHistory()`. It has no stack of its own, so per-scene histories,
  the play floor and `mark/groupSince` keep working unchanged.
- The global entry id is the ordering key: a static `std::atomic<uint64_t>` in
  `CommandHistory`. Entries are stored as `struct Entry { std::unique_ptr<Command> cmd;
  EntryInfo info; }` and replace the raw `Command*` vector, so ownership is explicit.
- Names are captured strings, not computed on display: entity names change, and a history
  row must say what was done at the time.
- An `InteractiveEdit` is built on mark/group/undo, not on their snapshot target. It needs no
  per-tool Capture/Apply code.
- The play review replays **fresh** commands built from final values. Their approach of
  re-executing the play command objects is rejected, because those objects carry play-time
  old values.
- Selection is recorded centrally in `Project::setSelection`, not per panel.

```cpp
// editor/command/Command.h (additions)
struct KeepContext;                       // PlayChangeReview.h
class Command {
public:
    virtual std::string describe() const = 0;
    virtual const char* typeName() const = 0;
    virtual bool isNoOp() const { return false; }
    virtual size_t memoryBytes() const { return sizeof(*this); }
    virtual bool releaseOffThread() const { return false; }
    virtual void collectKeepCommands(KeepContext&) const {}
    const std::string& name() const { return label; }
private:
    friend class CommandHistory; std::string label;
};

// editor/command/CommandHistory.h (additions)
class InteractiveEdit { public: void commit(); void cancel(); bool isOpen() const; ~InteractiveEdit(); };
InteractiveEdit beginInteractive(std::string name);
const Entry* top() const;  const Entry* nextRedo() const;
std::vector<const Entry*> entries() const;
bool undoTo(uint64_t id); bool redoTo(uint64_t id);
static std::function<bool(std::string& whyNot)> beforeMutation;

// editor/command/UndoTimeline.h
namespace UndoTimeline {
    bool undo(uint32_t sceneId, std::string* why = nullptr);
    bool redo(uint32_t sceneId, std::string* why = nullptr);
    bool jumpTo(uint32_t sceneId, uint64_t entryId, std::string* why = nullptr); // 0 = start
    std::vector<EntryView> entries(uint32_t sceneId, bool includeSelection);
    std::string undoLabel(uint32_t sceneId); std::string redoLabel(uint32_t sceneId);
}
```

MCP tool names: `get_undo_history`, `undo`, `redo`, `get_play_changes`, `apply_play_changes`,
`discard_play_changes`. They join the existing `CoreTools.cpp` and `PlayTools.cpp` modules.

## 7. Acceptance criteria

`CommandHistoryTests.*` and `UndoTimelineTests.*` (gtest, `editor/tests/command/`, extending
S02's suite) use a recording fake command with a configurable name, size, no-op flag and
merge behaviour.

- **AC1** `CommandHistoryTests.NamesCapturedAtFirstExecute`:
  - the name is set after the push and kept through undo/redo;
  - after a merge the name is the newest one and the id the oldest;
  - a compile-time check that `Command` is abstract in `describe`;
  - an MCP test where `get_undo_history` after `set_transform` returns "Move <name>" and the
    Edit-menu label equals "Undo " + that name (via the `editor_status.undo.undo_name` test hook).
- **AC2** `CommandHistoryTests.TrimByCountAndBytes`:
  - with max 10 and a 1000-byte budget, the oldest entries go first across two histories
    (lowest global id);
  - the newest entry, the play-time entries and an open-scope entry are never trimmed;
  - a `mark()` taken before a trim still groups (absolute positions);
  - `groupSince` returns false for a trimmed mark.
- **AC3** `CommandHistoryTests.NoOpDropped`:
  - a `PropertyCmd` setting the current value creates no entry and leaves `isModified` false;
  - an MCP `set_properties` to the current value leaves `get_undo_history` unchanged.
- **AC4** `CommandHistoryTests.InteractiveCommitCancel`:
  - commit folds 30 merging and non-merging pushes into one entry;
  - cancel restores values, overrides and `isModified`, and keeps the redo tail available;
  - a no-op commit restores the redo tail;
  - a destructor without commit commits.
- **AC5** Esc gizmo manual check:
  - in a new 3D scene, create a Cube (Hierarchy ▸ Create ▸ 3D Object ▸ Cube), select it, drag the move
    gizmo about 2 m, and press Esc before releasing the mouse;
  - the position equals the pre-drag value, Undo History has no new row, and Edit ▸ Undo shows
    the previous step;
  - repeat with an Inspector float drag and a terrain sculpt stroke.
- **AC6** `UndoTimelineTests.Interleave`:
  - sequence A1 (scene A), P2, B3 (scene B), with undo in B;
  - undo gives B3 then P2, and nothing more in B;
  - in A, undo gives A1, and redo gives A1 then P2;
  - a new entry in A frees P's redo tail and leaves B untouched.
- **AC7** `UndoTimelineTests.LinkedBatch`:
  - a linked group across A and P is undone by one call;
  - with a newer entry on P, only A's part is undone and the warning text is produced.
  - MCP: `test_undo.py::test_batch_undo_group_linked` runs `batch undo_group` with a
    `create_entity` and a `create_material`; one `undo scope=timeline` reverts both, and
    `complete` is true.
- **AC8** Regression for the focus bug, in two parts.
  - The routing decision (R21) is a pure function, `UndoTimeline::resolveTarget(textInput,
    codeEditorFocused, lastFocusedWindow)`. `UndoTimelineTests.RoutingIgnoresWindowFocus`
    checks that Resources, Scene, Hierarchy and Inspector focus all give the timeline, and
    that only text input and the Code editor do not.
  - `test_undo.py::test_focus_independent`:
    1. make a scene edit, then rename an asset with `move_asset`;
    2. call `focus_window` for the Scene view;
    3. read `editor_status`.

    `undo.undo_name` names the rename, and the Edit menu label comes from the same function.
    `undo scope=timeline` then reverts the rename and leaves the scene edit applied.
- **AC9** `CommandHistoryTests.SelectionSteps`:
  - two standalone selections give two entries;
  - same-frame changes merge;
  - selection inside a command is not recorded separately;
  - undoing a Delete re-selects the deleted objects;
  - nothing is recorded while a redo tail exists, or when the setting is off.
  - MCP: `select_entities` then `undo scope=scene` restores the previous selection.
- **AC10** `CommandHistoryTests.BackgroundRelease`:
  - entries ≥256 KB with `releaseOffThread` are handed to a fake release sink (injected), and
    the others are deleted inline;
  - the sink receives the cleared entries on `CommandHandle::remove`.
- **AC11** `test_undo.py::test_history_tool`:
  - `get_undo_history` lists ids newest first, `applied` flags, `origin.kind == "agent"` and the
    client name prefix;
  - `undo to_id` stops at the right entry;
  - `undo expect_top_id=<stale>` fails and changes nothing;
  - each edit result carries `structured.undo.id`.
- **AC12** `test_undo.py::test_attribution_folding`: one `set_properties` on three entities
  across two components gives exactly one entry named "<client>: Set Properties (3 Objects)".
- **AC13** `test_play.py::test_play_change_review` (MCP):
  1. start Play;
  2. `set_properties` on an authored light (intensity) and `set_transform` on an authored cube;
  3. `create_entity`, and edit a script-spawned entity;
  4. `play stop`;
  5. `get_play_changes`.

  It checks:
  - Four rows are listed. The script-spawned one has `keepable=false`.
  - `apply_play_changes ids=[light]` sets the light to its play value and keeps the cube at its
    edit value.
  - One `undo` restores the edit value.
  - With `keep_changes:"all"`, the created entity exists after Stop.

  A gtest, `PlayChangeReviewTests.FreshCommandsUndoToEditValues`, covers the same flow on a
  headless `Project`.
- **AC14** The prefab matrix, `PrefabUndoTests.*` (gtest) plus `test_prefab.py::test_undo_overrides`:
  - For each value-changing command type in R31, on a GameObject of a prefab instance:
    - after the edit, the override is recorded;
    - after undo, both the value and `getPrefabEditOverrides` are identical to before;
    - after redo, they match the edit again.
  - It also covers an InteractiveEdit cancel and a play-review apply on an instance (the override
    is recorded on the restored world, and undo removes it).
- **AC15** Manual check of the Undo History panel:
  - after 5 edits, clicking the 2nd row leaves 2 applied rows and the scene matches;
  - "(Start)" reverts everything, and Forward redoes one;
  - the filter and the selection toggle work;
  - the memory footer is non-zero after deleting 100 objects.
- **AC16** Manual check of the Play Mode Changes window:
  - after a Play in which the Inspector changed two properties, Stop opens the window with two
    checked rows;
  - unchecking one and pressing Keep Selected applies only the other, in a single
    "Apply Play Mode Changes (1)" row;
  - closing the window without choosing discards.
- **AC17** Benchmarks, `CommandHistoryBench.*` (S02 benchmark target), meet R43:
  - 10k pushes with trimming average ≤ 50 µs;
  - a 1000-GameObject Delete entry is released in ≤ 0.5 ms on the main thread;
  - `yamlFootprint` costs ≤ 10% of the encode.
- **AC18** The existing undo suites stay green: the 26 MCP test files, `tools/prefab-*.py`
  (including byte-identical re-saves) and `tools/smoke-test.sh`.

## 8. Dependencies & conflicts

- **Depends on:**
  - S02: gtest target and benchmarks.
- **Soft dependencies** (each has an interim fallback):
  - S09: subscriptions (`std::function` until it lands).
  - S06: background lane for `BackgroundRelease` (today's worker until then).
  - S04: `AssetOps` (today's file commands until then).
- **Feeds:**
  - S08 implements `describe`, `isNoOp` and `collectKeepCommands` for `ReflectedValueCmd`.
    That also enables per-field rows in R27.
  - S14 import settings use `SetAssetMetaCmd`, and S29 VCS revert goes on `P`.
  - S18, S20 and S21 drags use `InteractiveEdit`.
  - S25 adds Undo History to layouts and the command palette uses entry names.
  - S31 `jasperengine-editor call undo/get_undo_history`.
- **Interacts:**
  - S07: undo `recreateEntity` keeps ids, so the play-review replay relies on that.
  - S03: a recovery restore starts an empty history (S03 R18).
- **Merge hotspots:**
  - `editor/command/**` (all 80 types get `describe`/`typeName`; S08 also edits
    `PropertyCmd.h` and `ComponentValuesCmd`, S07 the stamping);
  - `editor/App.cpp` menu and keys (S05, S15, S25);
  - `editor/Project.cpp` `finalizeStop`/selection (S03, S04, S29);
  - `editor/render/SceneRender.cpp` drags (S26);
  - `editor/window/InspectorWindow.cpp` (S08);
  - `editor/mcp/McpServer.cpp`, `CoreTools.cpp` and `PlayTools.cpp` (S31).
- **Land order:**
  1. R1 with all names. This is mechanical and should be one PR, to keep the conflict window
     short.
  2. R5-R10.
  3. The timeline.
  4. Selection.
  5. Review.

## 9. Risks & defaults

- **Undoing an asset step from another scene's timeline** can break a reference that the other
  scene's later edits rely on. Default: accept. S04 GUIDs keep rename and move references
  valid, and an undone delete restores the file. The Undo History marks asset rows so the
  user can see them.
- **Selection steps cluttering the history.** Default: they are recorded but hidden in the
  panel, never recorded while a redo tail exists, and merged within a gesture. A setting turns
  them off.
- **Play review accuracy:** a kept property may also carry script-driven changes made to the
  same property after the user's edit. Default: the final value wins, and the window shows the
  play value so the user can judge. Whole-component "Keep Values After Play" rows list every
  differing key.
- **Memory estimates are approximate** (YAML footprint ±50%). Default: the budget is 512 MB, and
  the footer shows the estimate, labelled as such.
- **`describe()` as a pure virtual** breaks out-of-tree commands. Default: none exist. This is a
  deliberate compile-time completeness check.
- **The MCP `undo` default scope stays "scene"** for compatibility. Default: it switches to
  "timeline" in the release after S24, with a guide note now.
- **The InteractiveEdit destructor commits**, while theirs cancels. Default: commit, because
  losing a drag is worse than an extra step. A debug warning finds the call sites that forget
  to close.

## Deviations (implementation record)

Decisions taken while implementing, where the spec was silent, wrong in a detail or in conflict
with the code base:

1. **Entry type name.** `struct Entry` clashed with an `Entry` already in `jasperengine::editor`
   (`EntityClipboard.cpp`). The step type is `UndoEntry`, with `CommandHistory::Entry` as an alias.
2. **Widget drags (R11).** Instead of an `IsItemActivated`/`IsItemDeactivatedAfterEdit` pair at each
   of the Inspector's several hundred widget call sites, `App` installs
   `CommandHistory::beforeRecord`: the first user command pushed while an ImGui item is active opens
   an `InteractiveEdit` for that item, and the edit commits when the item lets go. This covers every
   drag, slider and colour picker of every window (Inspector, Animation window, Scene settings).
   Scene view handles, terrain strokes and Animation timeline drags (which write live, not through
   widgets) open their own edits and put the live values back on cancel.
3. **Dropped commands are freed late.** Widgets keep the pointer of the command they last pushed (to
   end its merge chain), and a no-op or cancelled command is now freed by the history. With
   `CommandHistory::deferReleases` (set by the editor) such commands are freed at
   `flushReleased()`, called when no ImGui item is active. Headless tools free at once.
4. **Selection writes.** The four setters, `deselectDestroyedEntities` (Forced), `PrefabStage.cpp`
   (Forced) and the prefab replace/override bookkeeping (Forced) go through `setSelection`. The raw
   `selectedEntities` resets of scene teardown, scene decoding and Play's runtime-copy carry stay:
   they reset state of a scene whose history is cleared with it, not selection changes.
5. **`TerrainMapPatchCmd` and `PrefabEditUndo` (R31).** Painting writes the map file, an asset shared
   by every terrain using it, and records no override (see the comment in `TerrainMapPatchCmd.cpp`),
   so there is no override record to restore; `TerrainTextureEditCmd` (a map rebound on an instance)
   already restores its overrides. `ComponentToPrefabSharedCmd` got `PrefabEditUndo`.
6. **Closing a scene frees its history (R23).** A closed (not removed) scene kept its history before;
   `closeScene` now calls `CommandHandle::remove`, which also drops a pending Play Mode Changes review.
7. **Play-time MCP edits (R39, AC13).** `set_properties`, `set_transform`, `create_entity` and
   `delete_entities` are allowed while playing (they were refused): their steps are what the review
   offers after Stop.
8. **Attribution of continuations (R24).** The origin scope is set around the handler and around the
   call's `waitUntil` / `afterFrames` continuations (`McpServer::tick`). A detached call completing
   from another main-thread task is not attributed; no edit tool does that today.
9. **Origins and merging.** Steps of different sources (user, an agent call, another call) never merge
   into each other, so an agent's step can't absorb the user's drag and vice versa.
10. **Group naming in folds.** The subject of a folded agent call comes from the GameObjects its
    commands report through a small `EntityEdit` interface (property, transform, component values,
    state commands); a call that edits none is named by the tool title alone.
11. **`UndoTimeline::onChanged`** is a `std::function` (S09 has not landed), plus `onProjectChanged`,
    which refreshes the Project window after an undo/redo of an asset step (R21). The native menu model
    is rebuilt every frame and diffed by the backend, so `onChanged` only requests a redraw.
12. **Busy refusals (R13).** `beforeMutation` refuses while a scene is `SAVING`, `LOADING` or
    `CANCELLING`; Stop is synchronous (`finalizeStop`), so there is no separate stopping state.
13. **Tests.** S02 landed while this stream ran: the suites are in `jasperengine-editor-tests`
    (`editor/tests/command/`), named as the spec names them (`CommandHistoryTests`,
    `UndoTimelineTests`, `PrefabUndoTests`, `PlayChangeReviewTests`, `CommandHistoryBench`) in files
    following S02's `*Test.cpp` rule. There is no editor benchmark target, so the AC17 budgets are a
    gtest suite (budgets not asserted under sanitizers). The Project-level suites load
    `tools/fixtures/prefab-runtime` headless, as `scene-dump` does, and emulate Play's snapshot and
    Stop as `scene-dump --ops playStart/playStop` do. S02's characterization tests needed only the
    fake command's names (the history still frees in list order).
14. **Manual checks (AC5, AC15, AC16)** need a person at the editor; their behaviour is covered by the
    gtest suites (interactive cancel/commit, jumps, the review) and the MCP tests, and is left for the
    integrator's manual pass.
