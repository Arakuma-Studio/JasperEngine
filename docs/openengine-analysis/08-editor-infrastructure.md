# 08 — Editor infrastructure: JasperEngine (OURS) vs OpenEngine (THEIRS)

Scope: undo/redo, selection, prefabs, play mode, editor asset/thumbnail pipeline, missing assets, VCS,
debug server and automation, editor tests, crash/hang/device-loss handling, Unity import.
Paths: OURS relative to the repo root; THEIRS relative to `external/OpenEngine/`.
I read the code for every claim below; file names alone were not taken as evidence.

---

## 1. What THEIRS has

### Undo/redo: one global stack, command pattern plus byte snapshots
- `Apps/Editor/Include/UndoRedo/UndoRedoService.h` (header-only, about 560 lines). A single stack for the whole editor:
  - `IEditorCommand` (`Include/UndoRedo/IEditorCommand.h`) has `GetName()`, `GetTypeName()`, `Do/Undo/Redo`, and `CanMergeWith/MergeWith`.
  - `Execute()` runs a command. `CommitAlreadyApplied()` pushes one whose effect is already live.
  - `BeginCompound/EndCompound` nest. A nested compound folds into its parent. `CommitGenericEdit` closes the compound with a scope guard even if the edit throws (`Source/UndoRedo/GenericEditUndo.cpp`).
  - **InteractiveEdit**: `BeginInteractiveEdit(name, SnapshotTarget{Capture, Apply, Notify})` returns an RAII handle with `Preview(fn)`, `Commit()` and `Cancel()`. Commit compares the before and after byte snapshots and pushes nothing if they are equal. Destroying the handle without a commit cancels and restores the "before" state. `HasOpenInteractiveEdit()` is counted through a shared_ptr, so it stays safe if the service dies first.
  - Each entry carries a **stable id** (`GetUndoEntryIdAt`) and a **timestamp**. `maxUndo = 1024` trims the oldest entries.
  - `SetOnHistoryChanged` updates menu labels. `SetBeforeMutation` drains an in-flight deferred scene build before any capture or undo.
  - `DetachHistory/AttachHistory` is used to isolate play mode.
- Snapshot helpers:
  - `Include/UndoRedo/MultiEntityComponentSnapshot.h`: one component across N entities with a length-checked wire format. Apply refuses a snapshot whose entity count does not match.
  - `Include/UndoRedo/EntityComponentsEdit.h` (`CommitEntityComponentsEdit`): diffs every component's bytes before and after an edit that does not know what it touches.
  - `World::CaptureComponentBytes/ApplyComponentBytesImmediate` back both helpers.
- Asset-level commands share the same stack: `RenameAssetFileCommand.cpp` (refuses to overwrite an existing destination and preflights shader+material pairs), `DeleteAssetPathsCommand.h` (stages a copy for undo and moves the original to the **OS trash**), `SetAssetMetaValueCommand.cpp`, `CreateTextFileCommand.h`, `VcsRevertCommand.h` (an undoable VCS revert that keeps an in-memory backup), and Tag/Todo/Bookmark commands.
- Entity commands: `Source/UndoRedo/DeleteEntitiesCommand.h` (515 lines; revives the same handles), `DuplicateEntitiesCommand.h`, `ReparentReorderEntitiesCommand.h`, `CreateEmptyParentCommand.h`, `MaterialDropOnMeshCommand.h`, `TextureDropOnMeshCommand.h`, `SceneViewDropUndoCommand.h`, and spline edits.
- `Source/Panels/UndoHistoryPanel.cpp` (257 lines): history list with names and timestamps, click to jump, back and forward buttons. The debug server exposes `get_undo_stack`, `undo` and `redo`.

### Selection model
- `UI::Interaction::SelectionModel` holds ids plus an anchor for range selection.
- **Selection changes are undoable**: `HierarchySelectionCommand` in `Source/Panels/HierarchyPanel.cpp:1373-1460` uses a `m_SuppressSelectionUndo` re-entrancy flag.
- `Source/UndoRedo/DeleteEntitiesSelectionCommand.h`: one Ctrl+Z revives the deleted entities and re-selects them.
- The scene file stores hierarchy UI state (expanded nodes, ordered selection) and the editor camera (`Engine/Source/Scene/SceneIO.cpp` near line 4230, `[editor_camera]`).

### Prefabs: no editor-level prefab system
- `grep -ril prefab` finds only comments: `Apps/Editor/Source/Picking/PickRootResolver.cpp:26` says "planned with the prefab/blueprint system".
- The engine has a "prefab-lite" called **blueprints**:
  - A `[blueprint id source parent]` section in a scene, implemented in `Engine/Source/Scene/SceneIO.cpp:1226-1268` and `2678-2860`.
  - Overrides apply to the **instance root only**: `+Component`, `-Component`, `Comp.prop = value`.
  - Instance children are tagged `id.` and excluded from save (`SceneIO.cpp:3686-3700`). Parenting anything under an instanced child is refused at save (`SceneIO.cpp` near line 3960).
  - It has no nesting semantics in the editor, no variants, and no apply, revert, Prefab Mode, or override UI. The root is marked with `Engine/Include/Components/SceneBlueprintInstance.h`.
  - Also present: `[subscene]` (additive, with offset) and `[include]` (resource merge only).

### Play mode: same-world snapshot, undo isolation, change review
- `Source/PlayMode/PlayModeManager.cpp` (587 lines), entry:
  - Refuses while a scene build is in progress.
  - `PlayModeEntryGate` (`Include/PlayMode/PlayModeEntryGate.h`) holds entry while C#/native scripts build and refuses after a failed build.
  - **Detaches the edit undo history**.
  - Snapshots the terrain zone payloads and the markup notes, which live outside the ECS.
  - `CaptureSnapshot()` is the binary `World::SerializeWorld` ("V1 world isolation: same world snapshot/restore").
  - Records the pre-play entity ids so the Hierarchy can flag runtime entities (`IsRuntimeEntity`).
  - Defers native hot reload during play, then calls `StartUserSystems` and the managed `OnEnter` through CoreCLR.
- Exit:
  - Managed `OnExit`, terrain restore, `StopUserSystems`, and a hard reset of the physics world.
  - Releases the terrain collider slots, stops all voices, applies the snapshot, and resets the GPU-cache components.
  - Flushes the deferred hot reload.
- **ChangeReview state**: the play-time undo stack becomes `m_PendingChanges`. `PlayModeChangeReviewModal.cpp` lets the user pick which changes to keep, and `ApplyPendingChanges(keep)` re-executes them as one compound, "Apply Play Mode Changes". `Tests/Editor/PlayModeChangeReviewTests.cpp` covers it.
- Saving is blocked while playing: `IsPlayModeBlockingSceneWrites` gates both manual save and the autosave tick (`Source/EditorApplication.cpp:1459`, `6677`; `Tests/Editor/SaveBlockedDuringPlayTests.cpp`).

### Scene document robustness
- **Autosave plus crash recovery**:
  - `Source/Scene/SceneEditorController.cpp:1351-1405`: interval of 30 to 300 s (default 120), preferences re-polled, held while a build pumps or play is active.
  - `SceneDocumentManager::SaveBackupCopy` (`Source/Scene/SceneDocumentManager.cpp:1023-1090`) writes `<scene>.backup.<ext>` only when the scene is dirty, and never when the load was lossy.
  - `Source/Scene/SceneBackupRecovery.cpp` (133 lines): scan, promote, discard. A backup counts only if it is newer than the scene.
  - Opening a scene with a usable backup prompts Restore or Discard (`SceneEditorController.cpp:708-740`, `860-880`). A headless policy exists for automation. Staged backups are cleaned on a clean close.
- **Atomic writes**:
  - `SaveSceneToFile` ends in `WriteTextFileAtomic` (`Engine/Source/Scene/SceneIO.cpp:4266`).
  - `FileSystem::PublishFile` (`Engine/Modules/FileSystem/Source/FileSystemCore_Native.cpp:45`) renames through `RenameWithRetry` (5 tries 200 ms apart on permission or busy errors) and never truncates the target.
  - `SettingsStore.cpp:131-185` writes a same-directory temp file, then uses macOS `renamex_np(RENAME_SWAP)` or `PublishFile`, under `ScopedFileLock` (`.lock` sibling) for multiple editor instances.
- **Degraded load**:
  - `Engine/Include/Scene/SceneIO.h:60-100`: `SceneLoadDegradation` lists the skipped lines. A preserved skip is re-emitted verbatim; `DroppedCount()` counts what a save would really lose.
  - `Apps/Editor/Include/Scene/SceneLoadFailure.h`: `SceneLoadDegraded` and `DegradedSavePolicy{Refuse, SaveAnyway}`, Refuse by default. The title shows "(PARTIAL LOAD)" (`SceneDocumentManager.cpp:257`).
- `MissingAssetTracker` (`Include/MissingAssetTracker.h`, `Source/MissingAssetTracker.cpp`, 324 lines):
  - Scans the world for GUID references unknown to the AssetRegistry and records GUID, type, authored path, last-known path, entity tag, component and property.
  - Uses a mutex snapshot with version and listeners.
  - `ClearAllReferencesTo(guid)` goes through each schema's `ApplyProperty("0")`.
  - Feeds the Missing Assets panel and the ghost thumbnails in the asset browser.

### Crash, hang and device loss
- `Apps/Editor/Source/main.cpp:53-330`:
  - Windows `MiniDumpWriteDump` (full memory or indirect memory) into the user cache `GameEngine/CrashDumps`, from the SEH filter, terminate and signal paths.
  - Symbolized backtrace through `Logger::CaptureBacktrace`, plus a `[CRASH]` mirror on stderr.
  - Log flush bounded by 2 s (`FlushForCrash`).
  - The signal handler says itself that it is "not strictly async-signal-safe".
- `Source/Diagnostics/MainThreadHangWatchdog.{h,cpp}` (90 + 489 lines):
  - A background thread samples the main-loop heartbeat. On a stall over 2 s it logs the main thread's stack (Windows, dbghelp) and the job-system thread occupancy.
  - A separate shutdown watch, `HangWatchdogPhase` annotations, and `GE_HANG_WATCHDOG*` environment switches.
  - Tested in `Tests/Editor/MainThreadHangWatchdogTests.cpp`.
- `Source/DeviceLossSurfacer.cpp` and `Include/DeviceLossSurfacer.h`:
  - Native OS dialogs, because the GPU UI cannot draw. A title-bar toast after 250 ms of Hung, with a 400 ms debounce.
  - The save-and-restart prompt runs on a detached thread so unattended runs never deadlock. Tested in `Apps/Editor/Tests/DeviceLossSurfacerTests.cpp`.

### Version control
- `Engine/Modules/VCSIntegration` (about 930 lines):
  - `IVCSIntegration`: status, add, remove, move, commit, update, revert, `GetLockInfo`, `Acquire/Release/RequestLock`, `IsIgnored`, `RefreshStatus(path)`, `GetLog`, branch.
  - `VCSFileStatus` has 11 states.
  - `VCSStatusPoller`: one thread, 10 s interval, `Wake()` with cooldown so a slider-drag burst collapses to at most 2 polls.
  - `VCSCommandExecutor`: no shell. CreateProcessW with per-argument quoting on Windows; fork/exec with everything prebuilt before fork on POSIX.
  - `VCSPathKey`.
- Provider packages:
  - `Packages/git-vcs`: `git status --porcelain`, a separate `--ignored` cadence, Git LFS locks through `lfs locks --json` and `lfs lock/unlock`, and `GetBaseContent` through `git show`.
  - `Packages/svn-vcs`, `Packages/lore-vcs` (JSON event stream), and `Packages/diversion-vcs` (REST with `TokenEncryption`).
  - Each package has Settings, a Provider descriptor and ModuleInit. The registry is `Source/Editor/Vcs/EditorVcsProviderRegistry.cpp`.
- Editor UI: `Source/VersionControl/Ui/{HierarchyVcsController (1204 lines: status and lock markers, lock entity/scene acquire/release/request), InspectorVcsController (513 lines: per-property diff markers with a baseline tooltip, "Revert Property to Baseline", copy the baseline value), VcsCommitDialog, VcsLogViewer}.cpp`, and `Source/Editor/Vcs/VcsStatusUi.cpp`.
- **Semantic scene diff**:
  - `Source/VersionControl/SceneDiff.cpp` aligns sections and properties by stable id, not by line, and detects reparenting separately.
  - `SceneDiffVcs.cpp` builds the diff against the provider's base revision, memoized on size, mtime and VCS status.
  - `SceneDiffLiveOverlay.cpp` draws the markers. Tested in `Tests/Editor/SceneDiffTests.cpp`.

### Unity import
- `Packages/unity-import`:
  - `UnityImportModal.cpp` (1147 lines): pick a `.unitypackage` or a folder, list its scenes, convert into a temporary stage outside the file watcher, check overwrite collisions, move into Assets in one pass, register with the AssetRegistry, then report dropped items ("honest-drop").
  - It drives the **prebuilt .NET `Tools/UnityConverter.dll`** (295 KB binary, MIT, built from the external repo Game-Crafters-Guild/OpenEngine-Unity-Scene-Converter at a pinned commit, with sha256 in `UnityConverter.dll.source`), hosted on CoreCLR (`Source/Editor/Assets/UnityImportHostedConverter.cpp`).
  - The output is OpenEngine's `.scene` format.

### Automation and debug server
- `Source/DebugServer/EditorDebugServer.{h,cpp}`:
  - TCP ndjson on port 9999 with multiple clients. A network thread queues requests; handlers run on the main thread in `FlushPendingRequests`.
  - Deferred responses (GPU readback) and render-phase callbacks.
  - A **request-gate registry** (`Source/Editor/Registries/DebugRequestGateRegistry.cpp`) can refuse or park requests, keeping per-connection order.
- About 133 handlers (DebugHandlers.cpp is 10661 lines), covering undo/redo/`get_undo_stack`, `set_play_mode`, `save_scene`, `open_scene`, `set_component`, `execute_command`, `invoke_menu_item`, `respond_modal`, input injection, `take_screenshot`, `debug_crash`, render-graph and profiler introspection, and more.
- `Source/Automation/UiReplayRunner.{h,cpp}` (3.5k lines): scripted per-frame UI input replay with JSONL telemetry, virtualization and paint-offset assertions, a pixel-hash probe, and an exit code. Scenarios live in `Tests/UIReplay/*.json`.
- `EditorSmokeHooks` is environment-driven (Polyhaven download smoke).

### Thumbnails (editor side)
- `Source/Thumbnails/*` (about 9.8k lines): `ThumbnailService` dispatches to per-type handlers (texture, model, video, cube LUT, shader-graph material).
- Persistent PNG cache in `<Project>/.Editor/Thumbnails` (`ModelThumbnailPngPersistence.cpp`), folder bake with a report, a model bake queue, render lanes, and live material previews.
- Covered by `Apps/Editor/Tests/Thumbnail*Tests.cpp`.

### Editor tests
- `Tests/Editor/`: **278 gtest files**. 33 of them use `UndoRedoService`; examples are PlayModeChangeReview, SceneBackupRecovery, SaveBlockedDuringPlay, SceneDiff, EntityComponentsEdit, AssetRename, AssetMetaValueCommand, DebugServerRequestGates and DebugServerReadPurity.
- `Apps/Editor/Tests/` has 10 more.
- `Tests/EditorHarness/*.mjs` drives a live editor over the debug server (play-review-dismissal, project-startup, terrain-sculpt-roundtrip).
- Also `Tests/UIReplay`, `Tests/Mcp`.

---

## 2. What OURS has

### Undo/redo: per-scene command histories, merge-by-command
- `editor/command/Command.h`: `execute()/undo()`, `mergeWith(prev)`, `setNoMerge()`, `affectsStructure()` (a non-structural edit skips the Hierarchy rebuild). There is **no name**.
- `editor/command/CommandHistory.{h,cpp}` (195 lines): a vector plus an index. Merging happens on push when both commands are mergeable; a drag is a stream of merging commands and the caller sets `setNoMerge()` on release.
  - `mark()/groupSince()` collapses a run into a `CommandGroup`; MCP batch tools use it (`editor/mcp/tools/CoreTools.cpp:54-71`, `SceneTools.cpp:457`, `PrefabTools.cpp:867`, `WorldTools.cpp:282-293`).
  - **Play floor**: `beginPlaySession()` sets aside the edit redo tail. `endPlaySession(rolledBack)` drops the play-time commands and brings the tail back when the scene was restored from its snapshot.
  - **No size cap**, no timestamps, no ids.
- `editor/command/CommandHandle.{h,cpp}`: a static map from scene id to `CommandHistory`, so **one history per open scene**. `Project::getProjectCommandHistory()` (`editor/Project.cpp:2787`) holds the file operations. The App routes Ctrl+Z by last-focused window: Resources, then Code (text editor undo), then the scene (`editor/App.cpp:427-440`, `679-684`).
- 80 command types in `editor/command/type/`:
  - Templated `PropertyCmd<T>`, multi-entity, with a no-op check through `has_equality_operator_v`, a submesh override mask, and `isNonStructuralProperty`.
  - `MultiPropertyCmd`, `ObjectTransformCmd`, Create, Delete, Duplicate and Paste entities, component add/remove, child scenes, model load/clear, terrain patch, track keys.
  - Prefab commands: Create, Import, Replace, Unpack, PrefabOverride(s), Add/RemoveEntityTo/FromPrefab, ComponentToPrefabShared, CreatePrefabVariantAsset.
  - File commands: Copy, Delete (to the project `.trash`), Rename (moves the thumbnail too), CreateDir, CreateMaterialFile, ForkShader.
- `editor/command/PrefabEditUndo.h`: every property command snapshots the touched GameObjects' prefab overrides before its first run, restores them on undo, and keeps the oldest state across merges, so undo puts back both the value and its override record.
- `editor/util/BackgroundRelease.{h,cpp}` frees large YAML trees held by undo steps or the clipboard on a worker thread.

### Selection
- `SceneProject::selectedEntities` (`editor/Project.h:136`), set through `replaceSelectedEntities/setSelectedEntity`.
- Not undoable. `DeleteEntityCmd` undo does not reselect; Paste and Duplicate select their results (`PasteEntitiesCmd.cpp:402-437`, `DuplicateEntityCmd.cpp:123`).
- Editor camera and window state persist in the workspace (`editor/Workspace.cpp:2919-2934`), not in the scene file.

### Prefabs: full Unity model (contract in `editor/project/PREFABS.md`, 1367 lines)
- File format v3 with a GUID, `nextEntity`, `root`, and members. Registry ids equal file ids and are never reused (sections 1-2).
- `editor/util/AssetDatabase.{h,cpp}`: Unity-style GUIDs in the file. Detects moves, copies, deletes and edits made outside the editor. Duplicate GUIDs are resolved. Legacy GUIDs are derived from the path for deterministic upgrades. **Only `.prefab` is tracked** for now.
- Missing Prefab keeps the instance data and re-links when the file returns (`ProjectPrefabMissing.cpp`, sections 4-5).
- Override model v2 (`ProjectPrefabOverrideModel.cpp`, `ProjectPrefabOverrides.cpp`, `util/Prefab.h`):
  - Overrides per Catalog property (indexed elements, `"*"` for the whole component) plus name, active, tag and layer bits.
  - Added and removed components, added and removed GameObjects.
  - Ownership rules for nested levels.
  - Asset-level `nestedInstances` data at any depth.
- Nested prefabs with **stripped objects** (references into nested prefabs) and a baked export (section 17).
- Variants with base chains, Apply to any level (Apply to the base clears intermediate overrides), Revert, Unpack and Unpack Completely, and cycle detection (`ProjectPrefabVariant.cpp`, section 19).
- Replace Prefab Asset (`ProjectPrefabReplace.cpp`, 1349 lines).
- Instance editing that records overrides (`ProjectPrefabInstanceEdit.cpp`), and an Inspector prefab bar with an override UI (`window/InspectorPrefabBar.cpp`).
- **Prefab Mode** (`editor/PrefabStage.{h,cpp}`, 1125 lines): stages stack, In Context opens with a gray veil (`PrefabContextOverlay`), and Unity's Auto Save toggle (350 ms debounce, `PrefabStage.cpp:36`, `951-958`).
- Runtime `Instantiate`/`Destroy`, prefab script properties, play templates, and the generated `build_prefab_*` sources (`ProjectPrefabRuntime.cpp`, section 18).
- Prefab copy and paste across scenes and editor instances (`editor/util/EntityClipboard.{h,cpp}`, 986 lines, snapshot-based).

### Play mode
- `editor/Project.cpp:7352-7620` (`runPlayStartup`):
  - An asynchronous, cancellable startup session that saves, regenerates the standalone sources, and builds the C++ scripts through CMake (`Generator::checkBuildTools`, `generator.build`), then connects the library (`Conector`).
  - Snapshot: `Stream::encodeSceneProject` gives a YAML snapshot, plus `capturePlayPrefabState`, plus the `playStateEntities` set alive before play.
  - Final initialization runs on the main thread, guarded against `addChildScene` re-entry.
  - Child scenes run as owned runtime copies.
- Stop (`Project.cpp:4940-5030`, `8013-8140`): restores the selection, forgets pending `GameObject.destroy` calls, stops audio, merges shader keys discovered during play into the editor scene, `restorePlaySnapshot`, `endPlaySession`, and resets the layer matrices.
- Saving is refused during play (`Project.cpp:4136`, `5582`, `5887`).
- `editor/window/PlayInput.{h,cpp}`: input routing for the Game view. `Backend::setMouseControlSuspended` manages cursor capture while paused or loading.

### Robustness
- `editor/util/CrashHandler.{h,cpp}` (301 lines):
  - `set_terminate`, signal handlers, `SetUnhandledExceptionFilter`.
  - Paths are prepared at install time, writes are raw (no stdio lock), re-entry is bounded, and the backtrace comes from `backtrace_symbols_fd` or `SymFromAddr`. Output goes to the log file and stderr.
  - **No minidump.**
- **No scene autosave or crash recovery.** Only Prefab Mode autosaves.
- **Scene and prefab saves are not atomic**: `std::ofstream` truncates the target in place (`Project.cpp:5697`, `project/ProjectPrefabPaths.cpp:460`, `project.yaml` at `Project.cpp:5193`, `AppSettings.cpp:405,723`). The repo already has an atomic helper, `htmlui/ImportManifestWriter::writeFileAtomic` (`editor/htmlui/ImportManifestWriter.cpp:99`), that these saves do not use.
- Unknown component keys in a scene are silently dropped on load (`Stream::decodeComponents`, `editor/Stream.cpp:4474`, which only looks up known names), so a re-save loses them.
- No main-thread hang watchdog. No device-loss surfacing (OpenGL is the default backend).

### VCS, Unity import, automation, tests
- VCS: only writes `.gitignore` and `.gitattributes` (`Project.cpp:5304`; setting `editor.version_control_files`). No status, locks, diff or commit.
- Unity: name and glossary migration only (`tools/unity_rename.py`, `docs/unity-glossary.md`). No `.unitypackage` import.
- Automation:
  - An in-process MCP server (`editor/mcp`, HTTP and stdio transports; `PlayTools.cpp`, `PrefabTools.cpp`, undo and redo tools, batch grouping).
  - A CLI (`editor/cli/CommandLine.cpp`): `export`, `check`, `benchmark`, `scene-dump --ops`.
- Tests:
  - `tools/prefab-{fixtures,nested-test,variant-test,instance-edit-test,dump-compare}.py`: headless prefab regression through `scene-dump`, with byte-identical re-saves, undo/redo of prefab operations, missing/rename/move cases, and the generated sources.
  - `tools/smoke-test.sh`: export, build, run the game under Xvfb, screenshot the game and the editor, and fail on logged errors. Backends: opengl, vulkan (lavapipe), gles3.
  - `tools/mcp/tests`: 64 Python files, 26 of which exercise undo, plus `test_play.py`, `test_playctl.py`, `test_prefab.py`.
  - C++ tests exist only for htmlui (`editor/htmlui/tests`, `engine/tests/htmlui`). **There are no C++ unit tests for CommandHistory, AssetDatabase or the override model.**
- Thumbnails: `editor/window/ProjectWindow.cpp:194-600` runs a worker thread with a queue, a bounded GPU texture cache, and a disk cache (`Project::getThumbnailPath`, `Project.cpp:6889`). Material, model and prefab renders come from `editor/render/preview/{MaterialRender,ModelRender,PrefabRender,MeshPreviewRender}`. Prefab thumbnails are stamp-checked.

---

## 3. Gaps: THEIRS has, OURS lacks

| Feature | Their location | Value | Effort |
|---|---|---|---|
| Atomic scene/prefab/settings writes (temp, then rename with retry) | `Engine/Source/Scene/SceneIO.cpp:4266`, `Engine/Modules/FileSystem/Source/{FileSystemCore_Native,RenameWithRetry}.cpp`, `Apps/Editor/Source/SettingsStore.cpp:131-185` | H | S (we already have `writeFileAtomic`) |
| Scene autosave to a sibling backup plus a crash-recovery prompt on open | `Source/Scene/SceneBackupRecovery.cpp`, `SceneEditorController.cpp:1351-1405, 708-880`, `SceneDocumentManager.cpp:1023` | H | M |
| Degraded-load census: preserve unknown data verbatim, refuse lossy saves | `Engine/Include/Scene/SceneIO.h:60-100`, `Apps/Editor/Include/Scene/SceneLoadFailure.h` | M-H | M |
| Named undo entries, "Undo \<name\>" menu labels, Undo History panel (jump to step), entry ids and timestamps | `Include/UndoRedo/{IEditorCommand,UndoRedoService}.h`, `Source/Panels/UndoHistoryPanel.cpp` | M | S-M (we need to add names to 80 commands) |
| Undo depth cap (`maxUndo` 1024) | `UndoRedoService.h` `TrimUndo` | M (our commands hold YAML snapshots, so memory grows without bound) | S |
| InteractiveEdit Begin/Preview/Commit/**Cancel** (Esc reverts a drag, no-op commits are dropped) | `UndoRedoService.h` `InteractiveEdit` | M | M |
| Selection undo, and delete-undo that reselects | `Panels/HierarchyPanel.cpp:1373-1460`, `UndoRedo/DeleteEntitiesSelectionCommand.h` | M | S |
| Play-mode change review: keep the chosen play-time edits | `PlayMode/PlayModeManager.cpp` (ExitPlayMode, ApplyPendingChanges), `Panels/PlayModeChangeReviewModal.cpp` | M | M (replay against the restored world needs stable entity ids; ours are file ids, which helps) |
| Missing-asset tracker and panel for every asset type, plus "clear all references" | `Include/MissingAssetTracker.h`, `Source/MissingAssetTracker.cpp` | M | M-L (our GUIDs cover only prefabs; depends on the asset DB) |
| Main-thread hang watchdog | `Source/Diagnostics/MainThreadHangWatchdog.{h,cpp}` | M | S-M |
| Windows minidumps on crash | `Apps/Editor/Source/main.cpp:53-175` | M | S |
| VCS provider interface, status poller, process executor; Git (+LFS locks) and SVN | `Engine/Modules/VCSIntegration/*`, `Packages/git-vcs/Editor/*`, `Packages/svn-vcs/Editor/*` | M | L (the UI must be rebuilt in ImGui) |
| VCS overlays: Hierarchy and Inspector markers, lock UI, commit dialog, log viewer, undoable revert | `Source/VersionControl/Ui/*`, `Include/UndoRedo/VcsRevertCommand.h` | M | L |
| Semantic scene diff vs HEAD with per-property "Revert to Baseline" | `Source/VersionControl/SceneDiff{,Vcs,LiveOverlay}.cpp` | M | M (diffing YAML by entity id is simpler than their INI parser) |
| `.unitypackage` scene import | `Packages/unity-import/*` | M | XL (the converter targets their format and needs .NET) |
| Undoable asset metadata edits and delete-to-OS-trash | `UndoRedo/SetAssetMetaValueCommand.cpp`, `DeleteAssetPathsCommand.h` | L | S |
| A guard run before every undo capture (drains async scene building) | `UndoRedoService::SetBeforeMutation` | L-M | S |
| Device-loss native UI | `Source/DeviceLossSurfacer.cpp` | L (GL default; matters only for Vulkan/D3D11) | S-M |
| Scripted UI replay with pixel hashes | `Source/Automation/UiReplayRunner.*`, `Tests/UIReplay` | L-M | L |
| C++ unit-test suite for editor logic (gtest) | `Tests/Editor/*` (278 files) | M | M (framework plus first tests for CommandHistory, AssetDatabase, override model) |
| Debug-request gates (refuse or park while a modal or play is up; read-purity tests) | `Source/Editor/Registries/DebugRequestGateRegistry.cpp`, `Tests/Editor/DebugServer*Tests.cpp` | L (the MCP analyst covers this) | M |
| Locked settings file for multiple editor instances | `SettingsStore.cpp:226,260` (`ScopedFileLock`) | L | S |

---

## 4. Things THEIRS does better, and what OURS does better

### THEIRS better (evidence-based)
1. **Data-safety discipline on save.**
   - Their scene, settings and staged payloads all publish atomically, with retries on permission or busy errors (antivirus or indexers on Windows).
   - Our `saveSceneFile` truncates the `.scene` in place (`Project.cpp:5697`), so a crash or a full disk mid-write corrupts the user's scene. Same for prefabs and `project.yaml`.
2. **Crash recovery.**
   - They back up only dirty documents and never a lossy load, require the backup to be newer than the scene, prompt on open, and offer a headless policy for automation.
   - We have nothing for scenes; a crash loses everything since the last Ctrl+S.
3. **Unknown-data preservation.**
   - Their loader keeps unparseable lines and re-emits them, and a save that would drop data is refused unless the caller passes `DegradedSavePolicy::SaveAnyway`.
   - Our `Stream::decodeComponents` ignores unknown keys, so opening a newer project in an older editor and saving loses components silently.
4. **Undo UX.**
   - Their entries have names, so the menu, the history panel and the debug-server dump can show them. They have a depth cap and Esc-cancel on interactive edits, and selection is part of history (Unity behaviour).
   - Our `Command` has no name at all, so "Undo Move" labels or a history panel are impossible without touching all 80 commands.
5. **One undo stack for scene and asset edits.** Rename, delete, meta and VCS revert interleave with scene edits in a single timeline. Ours routes by focused window (`App.cpp:427-440`), so a Ctrl+Z after renaming a file in the Project window and clicking the Scene undoes a scene edit, not the rename.
6. **Play-mode change review** is a genuine UX feature (Unity lacks it; there you copy component values instead).
7. **Diagnostics for freezes and crashes.** The hang watchdog with stacks and job occupancy, plus minidumps, turns user reports into actionable data. Ours stops at a backtrace in the log.
8. **VCS.** A provider-neutral interface with Git LFS locking, status overlays and a semantic scene diff, plus a careful no-shell process executor (prebuilt argv before fork; `VCSCommandExecutor.cpp:366-400`).
9. **Test depth on editor logic.** 278 gtest files, including ones for play-review, backup recovery, scene diff and debug-server gates. Our C++ editor logic (CommandHistory play floor, groupSince, AssetDatabase GUID conflicts) has no unit tests; it is covered only end to end through Python.

### OURS better (be fair)
1. **Prefabs: ours wins by a wide margin.**
   - We have nesting at any depth with stripped objects, variants with base chains, property-granular override model v2, Apply/Revert to any level, Unpack and Unpack Completely, Replace Prefab Asset, Prefab Mode (stacked, In Context, autosave), Missing Prefab that keeps its data, GUIDs that track moves made outside the editor, runtime Instantiate, a baked export, and override-aware undo (`PrefabEditUndo`).
   - Theirs is a root-only `[blueprint]` include: children cannot be edited or saved (`SceneIO.cpp:3686-3700`) and there is no editor workflow. **Nothing to take from them here.**
2. **Entity copy/paste.** Ours (`EntityClipboard`) works across scenes and editor instances and keeps references into prefab instances. They have only Duplicate; `Paste` appears only for graph nodes.
3. **Undo through play.** Our `CommandHistory` play floor keeps the edit-time redo tail across a play session and restores it when the scene rolls back. Theirs detaches the whole history (equivalent), but ours also supports **per-scene histories for multi-scene editing**, and `mark/groupSince` gives MCP batches a single undo step with safety checks (it refuses after undo past the mark or across a play boundary).
4. **Crash-handler correctness.** Ours preallocates paths, writes raw (no stdio or CRT locks), bounds re-entry, and uses `backtrace_symbols_fd`. Theirs calls the Logger and `fprintf` inside signal handlers and admits it is "not strictly async-signal-safe" (`main.cpp` signal handler).
5. **Play startup.** Ours is a cancellable asynchronous session that builds C++ scripts against a checked toolchain, runs child scenes as runtime copies and captures prefab state. Theirs depends on a CoreCLR managed driver and a separate entry gate.
6. **Headless end-to-end testing of the real pipeline.**
   - `scene-dump --ops` covers prefab operations headless and checks byte-identical re-saves.
   - `smoke-test.sh` exports, builds and runs the game on three backends and compares screenshots.
   - Theirs is stronger on unit tests; ours is stronger on "the shipped game matches the editor".
7. **`affectsStructure()`** lets non-structural edits (transform drags, sizes) skip the Hierarchy rebuild. That is a cheap, explicit performance contract.
8. **BackgroundRelease** keeps big undo and clipboard frees off the main thread.

---

## 5. Copy/port candidates (ranked by value/effort)

License: OpenEngine is MIT ("Copyright (c) 2026 Gamecrafters Guild Limited and contributors"). Copied code needs the notice kept, for example in `NOTICE.md` plus a header comment. Nothing below comes from `ThirdParty/`.

1. **Atomic saves.** Value H, effort S.
   - Source: `Engine/Modules/FileSystem/Source/RenameWithRetry.cpp` (33 lines), `FileSystemCore_Native.cpp:45-60` (`PublishFile`), and the macOS `RENAME_SWAP` branch in `Apps/Editor/Source/SettingsStore.cpp:165-185`.
   - Adaptation: extend our own `ImportManifestWriter::writeFileAtomic` (move it to `editor/util/FileUtils.h`) with the retry loop and the RENAME_SWAP path. Route `Project::saveSceneFile` (`Project.cpp:5697`), `ProjectPrefabPaths.cpp:460`, the `project.yaml` write (`Project.cpp:5193`), `AppSettings.cpp:405,723` and the workspace through it.
   - Dependencies: none (std::filesystem). Background saves stay safe because the temp names are unique per thread (ours already are).
2. **Scene autosave plus crash recovery.** Value H, effort M.
   - Source: `Apps/Editor/Source/Scene/SceneBackupRecovery.{cpp,h}` (133 lines, std::filesystem only, plus `PublishFile` and Logger). Port it nearly verbatim and drop the terrain `_Zones` payload parts.
   - Policy from `SceneEditorController::TickAutoSave` (`:1365-1405`) and `SceneDocumentManager::SaveBackupCopy` (`:1023-1090`): write only when dirty, never during play or a build, never after a lossy load, interval clamped to 30-300 s.
   - In ours, encode with `Stream::encodeSceneProject` in a background save (we already save off-thread). Write to `.jasperengine/backup/<scene>.scene` rather than next to the scene, to keep the GUID AssetDatabase and VCS clean. On `openScene`, prompt with `registerThreeButtonAlert`. Cover open prefab stages too.
3. **Undo naming, cap and history panel.** Value M, effort S-M.
   - Source: concepts and code from `Apps/Editor/Include/UndoRedo/UndoRedoService.h`: `GetName/GetTypeName`, the lockstep id and timestamp vectors, `TrimUndo`, `SetOnHistoryChanged`, `DetachHistory`; and `Source/Panels/UndoHistoryPanel.cpp` for the jump-to-step UX (re-implement in ImGui).
   - Adaptation: add `virtual const char* name() const` to `editor/command/Command.h` and a cap in `CommandHistory::addCommand` that never trims below `playFloor`. Show "Undo \<name\>" in `App.cpp:499`, and expose `get_undo_stack` through MCP.
   - Header-only, no dependencies.
4. **InteractiveEdit and selection undo.** Value M, effort M.
   - Source: the `UndoRedoService::InteractiveEdit` class (RAII handle, cancel on destruction, no-op commit dropped), `HierarchySelectionCommand` (`HierarchyPanel.cpp:1381-1460`) and `DeleteEntitiesSelectionCommand.h`.
   - Adaptation: an `InteractiveEdit` over our YAML entity encode (`Stream::encodeEntity`) for gizmo, slider and terrain drags, plus Esc to cancel in `SceneWindow`. Add a selection command for `replaceSelectedEntities`; it must be non-structural and should merge consecutive clicks.
5. **Hang watchdog plus minidumps.** Value M, effort S-M.
   - Source: `Apps/Editor/Source/Diagnostics/MainThreadHangWatchdog.{h,cpp}` (579 lines; depends on their `JobSystem::WorkStealingThreadPool` occupancy, which is optional and can be removed, and on `Logger/Backtrace`, which maps to our CrashHandler's `SymFromAddr` code). Also `WriteEditorCrashDump` from `Apps/Editor/Source/main.cpp:58-175` (dbghelp, which we already link on Windows).
   - Adaptation: heartbeat from the `App` main loop, log through `Out`. Keep our async-signal-safe handler and add the minidump only in the SEH path.
6. **Degraded-load census.** Value M-H, effort M (concept port).
   - Source: `Engine/Include/Scene/SceneIO.h:60-100` and `Apps/Editor/Include/Scene/SceneLoadFailure.h` (data model and `DegradedSavePolicy`).
   - In ours, YAML makes this easy: in `Stream::decodeComponents` collect component keys no decoder consumed into a per-entity `YAML::Node` side map, re-emit them in `encodeEntity`, and surface a "(partial load)" title plus a refuse-to-save confirmation for whatever cannot be preserved.
7. **VCS core.** Value M, effort L.
   - Source: `Engine/Modules/VCSIntegration/{Include,Source}/*` (about 930 lines: `IVCSIntegration`, `VCSStatusPoller`, `VCSCommandExecutor`) and `Packages/git-vcs/Editor/GitIntegration.cpp` (1184 lines: porcelain parsing, LFS locks, `git show` base content).
   - Dependencies: their Logger and `GameEngine` namespaces, and the provider registry. Compare their `VCSCommandExecutor` with our `editor/util/CommandRunner.cpp` and keep whichever has the safer argv handling.
   - The UI (`Source/VersionControl/Ui/*`) is retained-mode HTML/CSS. Write Project-window and Hierarchy status icons and a commit dialog in ImGui instead.
8. **Semantic scene diff** (concept from `Source/VersionControl/SceneDiff.cpp` and the `SceneDiffVcs.cpp` memo on size, mtime and status). Value M, effort M after item 7. Re-implement over our YAML: align by `entity` id, compare components key by key, detect reparenting. Our stable file ids make this cleaner than their tag parsing.
9. **Play-mode change review** (`PlayModeManager::ExitPlayMode/ApplyPendingChanges`, `PlayModeChangeReviewModal.cpp`). Value M, effort M.
   - In ours, `endPlaySession(rolledBack)` would hand the play-time commands to a review list instead of deleting them, then re-execute the selected ones as one `CommandGroup`.
   - Caveats: skip commands that touch runtime-only entities (anything not in `playStateEntities`), and re-run `PrefabEditUndo` capture on the restored world.

---

## 6. Not worth copying (and why)

- **Blueprint/prefab code.** Theirs is a root-only include with no editor workflow; ours is far ahead. Do not regress toward it.
- **The play-mode world snapshot.** They serialize the whole world into the same world, which is the same approach as ours (YAML snapshot plus restore). The CoreCLR `OnEnter/OnExit/OnTick` binding, `PlayModeEntryGate` (our play start already builds C++ in a cancellable session) and the native hot-reload deferral are specific to their scripting stack.
- **The Unity import package.**
  - The core is a **prebuilt .NET DLL** (an untrusted binary blob; it needs CoreCLR, which we do not host) that emits OpenEngine `.scene` files. The modal is their retained UI.
  - If we want Unity import, write a Python or C++ converter to our YAML from scratch. The MIT converter repo (Game-Crafters-Guild/OpenEngine-Unity-Scene-Converter) can serve as a reference for Unity YAML and GUID resolution, with attribution, but do not vendor the DLL.
- **UiReplayRunner.** It is tied to their retained `UIManager`, `ListView`/`GridView` virtualization asserts and RenderGraph readback tickets. For ImGui, the natural equivalent is Dear ImGui Test Engine, which is **not MIT** (it has its own license with commercial-use terms), or a pixel-capture script on our MCP screenshot tools.
- **The DebugServer TCP ndjson.** We already have an in-process MCP server with HTTP and stdio transports. Only the request-gate idea is interesting; the MCP report covers it.
- **DeviceLossSurfacer.** It depends on their `Rendering::DeviceHealth` state machine; sokol exposes no equivalent and our default backend is GL. Revisit only if Vulkan or D3D11 become the editor default.
- **The Diversion and Lore VCS packages.** These are niche providers, and Diversion brings token-encryption code. Git (plus LFS) covers our users, and SVN maybe.
- **The thumbnail service.** It is built around their RenderGraph and texture binding. Ours already covers images, materials, models and prefabs with a disk cache. The only ideas worth noting are typed handler dispatch and "bake folder thumbnails".
- **Polyhaven smoke hooks and EditorSmokeHooks.** They are product-specific.
- **Their snapshot-byte undo for components** (`MultiEntityComponentSnapshot.h`). It relies on POD-like `CaptureComponentBytes`. Our components hold strings and vectors, and `PropertyCmd` is already property-granular and prefab-override-aware, so keep our command pattern and adopt only the InteractiveEdit control flow.
