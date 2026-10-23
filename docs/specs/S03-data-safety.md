# S03 — Data safety

Wave 1. Owner area: `engine/core/io`, `editor/Stream*.cpp`, `editor/Project.cpp`, `editor/project/`,
`editor/AppSettings.cpp`, `editor/Workspace.cpp`, `editor/mcp/tools/SceneTools.cpp`.

## 1. Goal

The editor must never destroy a user's file. Today a crash or a full disk mid-save truncates a
scene, two editor instances overwrite each other's settings, a crash loses every unsaved edit, and
opening a scene written by a newer editor (or one with a typo'd component) silently drops data that
the next Ctrl+S makes permanent. This spec adds one atomic-write primitive that every persistent
write goes through, cross-process locks, scene autosave with crash recovery, a scene format version
with migrations, verbatim preservation of data the editor does not understand, and a save gate that
refuses to drop data unless the user (or agent) explicitly says Save Anyway.

## 2. Scope

In scope:
- `writeFileAtomic` / `publishFile` / `renameWithRetry` / `ScopedFileLock` / orphan-temp sweep in
  `engine/core/io`, used by the editor *and* the runtime (`UserSettings.xml`).
- Routing every write path listed in §3.2 through it, plus a CI check that stops new raw writes.
- Project lock (one editor per project) and a locked read-merge-write for the global editor settings.
- Scene and prefab-stage autosave to `.jasperengine/backup/`, crash-recovery prompt, headless policy.
- `formatVersion` for scenes, `project.yaml` and `.material`; an ordered migration table; the
  existing prefab `version` (3) joins the same framework.
- Load report (per issue: entity, component, field, file:line, did-you-mean, preserved/dropped).
- Preservation of unknown components, unknown fields, unknown enumerators and unparsable values.
- Lossy-save gate with Save Anyway in the editor UI, MCP and CLI.

Out of scope:
- Export directory staging and swap: S30 owns that. Here, exporter text writes become atomic only
  because `FileUtils::writeFile` does (§3.2 group C).
- File-watcher echo suppression (S05). S05 consumes the publish notification defined here.
- Reflection-driven schemas (S08). S08 replaces the hand-kept known-key tables from R24.
- VCS lock acquisition (S29). This spec only refuses to write over read-only files.
- Changing the YAML format or adopting their INI-style scene format, their `SceneIO.cpp` or their
  POD ECS (do-not-copy list). We take the design and the small std-only files named in §4.

## 3. Current state

### 3.1 What exists
- `editor/htmlui/ImportManifestWriter.cpp:99` `writeFileAtomic`: a temp sibling
  (`<name>.tmp-<threadhash>-<n>`), `std::ofstream`, then `fs::rename`. It has no fsync, no retry,
  no pid in the temp name, no mode preservation and no read-only check. Only the HTML UI importers
  use it (`RemoteImporter.cpp:197`, `HtmlUiImporters.cpp:200`).
- Ad-hoc temp+rename copies: `editor/cli/CommandLine.cpp:1874-1887` (hook state),
  `engine/core/shader/RuntimeShaderCompiler.cpp:143-160` (shader cache),
  `editor/Exporter.cpp:1201-1262` (resource pack). None of them retries.
- No cross-process lock anywhere in `editor/` or `engine/`.
- Autosave exists only for Prefab Mode, which writes the asset itself ~350 ms after an edit
  (`editor/PrefabStage.cpp:36,951-958`). Scenes have no autosave and no recovery.
- Prefab files carry `version: 3` (`editor/util/Prefab.h:108`). Older files set `needsUpgrade`
  (`editor/util/Prefab.cpp:184`). A file *newer* than 3 is loaded silently. Scenes, `project.yaml`
  and `.material` files have no version key (`editor/Stream.cpp:2955-3063`). Legacy values are
  handled by ad-hoc branches (`Stream.cpp:1165`, `:1512`).
- `Stream::decodeComponents` (`editor/Stream.cpp:4474`) looks up each known component name. Any
  other key under `components` is ignored. `decodeEntity` (`:3939`) returns nothing for an entity
  `type` other than `Entity`. Unknown field keys inside a component are ignored. All 52
  `stringToX` enum decoders fall back to a default without saying so (e.g. `stringToLightType`,
  `:1312`). A failed scalar conversion throws and fails the whole scene load (`:3980-3985`).
- Headless hosts: the base `EditorHost` (used by the CLI and `jasperengine-editor mcp --headless`,
  `editor/mcp/McpLaunch.cpp:227`) answers `registerConfirmAlert` / `registerThreeButtonAlert` by
  running **onYes** (`editor/EditorHost.cpp:78-88`). A naive "Restore?" or "Save anyway?" prompt
  would therefore auto-accept in automation. This spec must not route its decisions through those
  defaults.

### 3.2 Write paths that must go through the primitive

Durability: **D** = Document (fsync before publish), **C** = Cache (atomic, no fsync). **Gate**
marks paths that the lossy-save gate (R31) applies to.

| # | Path (file:line) | What | Dur. | Gate |
|---|---|---|---|---|
| A1 | `editor/Project.cpp:5697` `saveSceneFile` | scene save (sync and `saveSceneToPathAsync` `:5880`) | D | yes |
| A2 | `editor/project/ProjectPrefabPaths.cpp:460` `savePrefabToDisk` | prefab asset (also reached by Prefab Mode save and autosave, `PrefabStage.cpp`) | D | yes |
| A3 | `editor/Project.cpp:5193` `saveProjectFile` | `project.yaml` | D | yes |
| A4 | `editor/Project.cpp:2234` | bulk material rewrite (visitor) | D | yes |
| A5 | `editor/window/InspectorWindow.cpp:413` | save linked material from Inspector | D | yes |
| A6 | `editor/command/type/CreateMaterialFileCmd.cpp:72` | create/overwrite material file | D | – |
| A7 | `editor/mcp/tools/ScriptTools.cpp:71` `w4::writeFile` (`WriteFilesCmd` execute+undo; material, write_file and PNG tools in `AssetTools.cpp`) | agent file writes | D | material: yes |
| A8 | `editor/window/CodeEditor.cpp:1398` `save`, `:1031` (script property insert), `:1331` (header edit) | user scripts and shaders | D | – |
| A9 | `editor/util/TerrainMapFileWriter.cpp:85-89`, `editor/util/PngWriter.cpp:143`, `editor/mcp/TerrainMaps.cpp:222` | terrain height/splat maps | D | – |
| A10 | `editor/util/AssetDatabase.cpp:35` `writeText` (GUID line inserted into asset files, `:199`) | rewrites scenes, prefabs, materials | D | – |
| A11 | `editor/mcp/tools/ProjectTools.cpp:1935` | `ProjectBuild.cmake` | D | – |
| A12 | `editor/window/ProjectWindow.cpp:3587`; `editor/Project.cpp:5324` (VCS metadata via `writeIfChanged`) | script template refresh, `.gitignore`/`.gitattributes` | D | – |
| B1 | `editor/AppSettings.cpp:405` | global editor settings (+ lock, R11) | D | – |
| B2 | `editor/AppSettings.cpp:723` | `.jasperengine/user/build.yaml` | D | – |
| B3 | `editor/Workspace.cpp:303` | `.jasperengine/user/workspace.yaml` | D | – |
| B4 | `engine/core/util/XMLUtils.cpp:98,124` | runtime `UserSettings.xml` (games on every platform) | D | – |
| C1 | `editor/util/FileUtils.h:126` `writeFile`/`writeIfChanged` | ~45 Generator/Exporter/ShaderHeaderBuilder/Factory/HtmlUiExamples call sites, engine API sync `Project.cpp:4716` | C | – |
| C2 | `editor/util/AssetDatabase.cpp:353` | `.jasperengine/assetdb.yaml` cache | C | – |
| C3 | `editor/shader/ShaderBuilder.cpp:392`; `RuntimeShaderCompiler.cpp:143` | shader signature and cache | C | – |
| C4 | `editor/window/ProjectWindow.cpp:144,678`; `editor/util/GraphicUtils.cpp:361` | thumbnails and stamps | C | – |
| C5 | `editor/cli/CommandLine.cpp:1874`; `editor/htmlui/ImportManifestWriter.cpp:99` | hook state, import manifest (replace the local copies) | C | – |
| C6 | `editor/Exporter.cpp:1201-1262` (resource pack), `:3093,3268-3284` (icons), `:920`, `editor/Generator.cpp:381` (kit markers) | export outputs | C | – |
| C7 | `editor/mcp/tools/ViewTools.cpp:282`; `editor/cli/CommandLine.cpp:841,1057` | screenshots, scene-dump `--out`, bake output | C | – |
| N1 | `editor/window/dialog/ScriptCreateDialog.cpp:125-235`, `ProjectWindow.cpp:3609-3621`, `editor/util/HtmlUITemplates.cpp:77,85`, `editor/util/ProjectUtils.cpp:334`, `editor/htmlui/VideoImporter.cpp:303` | new files (no previous target) | D | – |
| M1 | `editor/command/type/{DeleteFileCmd.cpp:68,101, RenameFileCmd.cpp:34,42, CopyFileCmd.cpp:37}`, `editor/Project.cpp:2323,2361`, `editor/Out.cpp:110` | moves and renames: use `renameWithRetry` only | – | – |

Excluded, and listed in the CI allow-list with a reason: append-only logs (`Out.cpp:27`,
`BuildTools.cpp:422,1458`, `CommandLine.cpp:1930`), private temp files (`HttpClient.cpp:355`), the
write probe (`AppSettings.cpp:40`), benchmark/doc output to a user-named path (`App.cpp:3169`,
`CommandLine.cpp:665`), and the `Exporter.cpp` `copy_file` sites (S30 staging).

## 4. Reference (OpenEngine, MIT, read-only)

Root: `/var/lib/orquester/workspaces/jaspersito/JasperEngine-2/external/OpenEngine`.
- `Engine/Modules/FileSystem/Source/RenameWithRetry.cpp` (33 lines): 5 attempts, 200 ms apart, retry
  only on `permission_denied` / `device_or_resource_busy`. **Take** the policy.
- `Engine/Modules/FileSystem/Source/FileSystemCore_Native.cpp:45` `PublishFile`: never copies over the
  target, so a failed replace keeps the old file. **Take.** They do not fsync; **we add** fsync,
  mode preservation and the read-only refusal.
- `Engine/Modules/FileSystem/Source/FileSystem.cpp:12` `MakeTemporarySiblingPath` (pid-thread-counter)
  and `Source/OrphanedTemporaryFiles.cpp` (sweep only temps whose pid is dead). **Take** the
  pid-in-name idea. **Improve:** quarantine orphans instead of deleting them, because a crash
  between write and rename leaves the newest complete copy in the temp.
- `Engine/Modules/FileSystem/Source/ScopedFileLock_Native.cpp`: `flock` / `LockFileEx` on a sidecar.
  **Take**, but lock a byte at offset 2^30 on Windows so other processes can still read the owner
  info in the lock file.
- `Apps/Editor/Source/SettingsStore.cpp:131-185`: temp file, then `renamex_np(RENAME_SWAP)` on
  macOS, else `PublishFile`. `:226-310`: `ScopedFileLock` held over read-merge-write, merging only
  the top-level keys this process changed; an unreadable file is set aside as `.corrupt` rather
  than overwritten. **Take** both rules.
- `Apps/Editor/Source/Scene/SceneBackupRecovery.{h,cpp}` (133 lines): scan, resolve, discard. A
  backup counts only if it is newer than the scene. **Take** the structure (pure filesystem
  functions, unit-testable). **Improve:** decide "newer" by a recorded base-content hash, not
  mtime (git checkout and clock skew break mtime). Drop the terrain `_Zones` payload parts.
- `Apps/Editor/Source/Scene/SceneEditorController.cpp:1351-1405` `TickAutoSave` (dirty only, never
  mid-build, interval clamped) and `:700-745` (headless `OpenRestorePolicy`). **Take** the rules.
- `Engine/Include/Scene/SceneIO.h:40-100` (`SceneLoadSkip`, `SceneLoadDegradation::DroppedCount`,
  `SkipIsOutstanding`) and `Apps/Editor/Include/Scene/SceneLoadFailure.h` (`SceneLoadDegraded`,
  `DegradedSavePolicy{Refuse, SaveAnyway}`, Refuse by default). **Take** the data model and naming.
- `Engine/Modules/ECS/Include/ECS/UnresolvedComponentStore.h`: preserved component / preserved field
  side table; a preserved field is retired once the live value is written ("FallbackText" string
  tier). **Take** the string-tier rule only. Their byte-offset tier relies on POD components.
- `Engine/Modules/Types/Include/Types/NearestName.h` (81 lines, std-only): **copy verbatim** to
  `editor/util/NearestName.h`. Keep their header, change only the namespace, and add a `NOTICE.md`
  entry.

## 5. Requirements

### Functional — atomic writes and locks
- **R1** `engine/core/io/AtomicFile.{h,cpp}` provides `writeFileAtomic(target, bytes, options)`.
  It creates parent directories, writes to a temp sibling with native calls (POSIX `open/write/
  fsync`, Win32 `CreateFileW/WriteFile/FlushFileBuffers`), closes it, then publishes. When the call
  returns success, `target` holds exactly the new bytes. Whatever fails, `target` holds exactly
  its previous bytes, or is still absent, and the temp is removed.
- **R2** The temp name is `.<filename>.<pid>-<counter>.je-tmp` in the target's directory (the
  counter is a process-wide atomic). Dot-prefixed names are hidden from the Project window
  (`ProjectWindow.cpp:1896`) and skipped by the AssetDatabase (`AssetDatabase.cpp:64`).
  `isTemporarySiblingName()` recognises the pattern, for S05.
- **R3** Publish = `renameWithRetry`. Retry when the error is `permission_denied`,
  `device_or_resource_busy`, Win32 `ERROR_SHARING_VIOLATION` or `ERROR_LOCK_VIOLATION`. Defaults:
  5 attempts, 200 ms apart. Configurable, and `shouldCancel` stops it. Windows uses
  `MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH)`. macOS/iOS use `renamex_np(RENAME_SWAP)` when the
  target exists, then unlink the swapped-out temp, and fall back to `rename` on `ENOTSUP`/`EINVAL`
  (non-APFS volumes). Linux/Android fsync the parent directory after the rename for Document
  writes. Emscripten does a plain `rename`; the runtime then calls `System::syncFileSystem()`.
- **R4** Durability: `Document` fsyncs the temp (`F_FULLFSYNC` on Apple) before publishing.
  `Cache` skips every sync.
- **R5** Before publishing, an existing target's POSIX mode bits are copied to the temp. A symlinked
  target is resolved, and the temp is written beside the real file, so the link survives.
- **R6** If the target exists and is read-only (no owner write bit, or Windows
  `FILE_ATTRIBUTE_READONLY`), the write fails with `WriteError::ReadOnlyTarget` and nothing is
  touched. Reason: Git LFS / Perforce lockable files signal "not locked" by being read-only.
- **R7** `skipIfUnchanged` keeps today's `FileUtils::writeFile` semantics: compare first, and return
  `Unchanged` without touching the file or its mtime.
- **R8** `publishFile(temp, target, options)` exists for writers that stream to a path
  (`ShaderDataSerializer::writeToFile`, the resource pack). PNG writers switch to
  `stbi_write_png_to_func` (memory) and then `writeFileAtomic`. `PngWriter` builds its buffer
  already.
- **R9** Every path in §3.2 groups A, B, C and N goes through R1/R8, and group M through
  `renameWithRetry`. `ImportManifestWriter::writeFileAtomic` and `FileUtils::writeFile` become thin
  forwarders, so their callers do not change. A failed Document write leaves the document modified
  and reports `Couldn't save "<path>": <reason>. Your changes are still in the editor.` (an alert in
  the GUI, an error in the console and MCP).
- **R10** Project lock: `loadProject` takes an exclusive, non-blocking `ScopedFileLock` on
  `.jasperengine/user/editor.lock` and holds it until the project closes. The lock file holds JSON
  `{pid, host, started, editor_version}`. If another live process holds the lock, the GUI shows the
  §5 Editor UX dialog. Headless writers (`scene-dump --save`, `mcp --headless`) fail with
  `project_locked` (CLI exit code 3) unless `--ignore-lock` is passed. If locking is unsupported
  (`ENOLCK`, read-only volume), the editor warns once and continues unlocked. Because the OS
  releases the lock on crash, a stale lock never blocks.
- **R11** Global editor settings (B1) are saved under `ScopedFileLock(<settings>.lock)` as
  read-merge-write. Only top-level keys whose value differs from this process's load-time snapshot
  are applied. `recent_projects` merges as an ordered union, ours first. If the file cannot be
  parsed, it is renamed to `<file>.corrupt-<YYYYmmdd-HHMMSS>` before the first save, with a
  warning. B2 and B3 are protected by the project lock.
- **R12** Orphan sweep: when a project opens, the existing project file walk (no extra walk) and a
  scan of `.jasperengine/` collect `*.je-tmp` files whose pid is not running. Each one that is
  non-empty is moved to `.jasperengine/backup/orphans/<relative path>.<timestamp>`; empty ones are
  deleted. One warning lists them. Quarantined orphans older than 7 days are deleted.

### Functional — autosave and crash recovery
- **R13** Autosave writes backups, never the user's file. Location:
  `.jasperengine/backup/scenes/<project-relative path>`. Untitled scenes go to
  `.jasperengine/backup/untitled/<sceneId>.scene`, and Prefab Mode stages with Auto Save off go to
  `.jasperengine/backup/prefabs/<path>`. Each backup has a `<backup>.json` sidecar:
  `{source, base_sha1, saved_at, format_version, editor_version, pid}`. `base_sha1` is the SHA-1
  (`editor/util/SHA1.h`) of the source file's bytes as last loaded or saved by this session; it is
  empty for untitled scenes.
- **R14** Tick rules. A backup is written for a scene only when all of these hold:
  - the scene `isModified` is set;
  - `playState == STOPPED` (so not playing and not `SAVING`);
  - no project load or scene open is in progress;
  - no ImGui item or gizmo drag is active (otherwise the write waits for the next frame);
  - the scene has no outstanding *dropped* load issues (R27); preserved-only issues are fine;
  - the encoded bytes differ from the last backup written.
  Interval: the `editor.autosave.interval_seconds` setting, default 60, clamped to 30-300.
  `editor.autosave.enabled` defaults to true.
- **R15** The backup is encoded on the main thread with `Stream::encodeSceneProject`, without the
  mutating pre-save steps of `saveSceneFile` (`Project.cpp:5674-5692`). It is then written as a
  Document atomic write on a background thread. The scene never enters `SAVING`, so
  editing continues.
- **R16** Lifecycle:
  - A successful explicit save deletes that scene's backup.
  - Closing a scene with Discard deletes it.
  - A clean project close or editor quit deletes every backup whose sidecar `pid` is this process.
  - A crash leaves backups in place.
- **R17** Recovery check: on every scene open, including the scenes restored at project load,
  `scanSceneBackup(path)` returns `Usable`, `Stale` or `None`.
  - Usable means: the backup exists, its `base_sha1` equals the SHA-1 of the current file, its
    bytes differ from the file, and its `pid` is not a running editor.
  - Stale (the file changed on disk since, e.g. `git pull`): the backup moves to
    `.jasperengine/backup/stale/` and a warning names it. A stale backup is never offered and
    never deleted automatically.
  - Untitled backups are offered once, when the project opens.
- **R18** Resolution:
  - **Restore** loads the backup's content, keeps the real document path, marks the scene modified,
    starts with an empty undo history, and keeps the backup until the next save or discard.
  - **Discard** deletes the backup and opens the saved file.
  - **Keep** (GUI "Decide Later", and the headless default) opens the saved file and leaves the
    backup untouched, so it is offered again next time.
- **R19** Policy: the GUI asks through the dedicated recovery modal, never through
  `registerThreeButtonAlert`. `EditorHost` gets a new
  `virtual RecoveryChoice resolveSceneRecovery(const SceneBackupInfo&)`. The base (headless) host
  returns `Keep`. MCP `open_scene` passes its `recovery` argument (R36), so an agent never meets a
  modal.

### Functional — versioning and migrations
- **R20** The scene root, `project.yaml` root and `.material` root get `formatVersion: <int>`.
  Current values are scene 1, project 1 and material 1; an absent key means 0. Prefabs keep their
  key `version` (current 3). Encoders write the key first in the map.
- **R21** `editor/StreamMigrations.{h,cpp}` holds one ordered table per document kind:
  `{from, to, name, void(*)(YAML::Node& root, MigrationLog&)}`. Migrations run on the raw YAML
  before any decode, so decoders only ever see the current shape. A migrated document is *not*
  marked modified (like `needsUpgrade`). It is written in the current format on its next save, and
  an info line says so. Scene 0→1 is a no-op that only stamps the version. The ad-hoc legacy
  branches stay where they are in this spec; new legacy handling must be a migration.
- **R22** Bump rule (applies to every spec): adding an optional component or field does **not**
  bump the version, because preservation and defaults cover it. Renaming, moving or re-typing a
  key, or changing a value's meaning, bumps the version and needs a migration with a fixture.
- **R23** A file whose version is newer than this editor knows still loads best-effort, with an
  issue `NewerFormat` that counts as one dropped item. Its saves are therefore gated (R31), with
  the message `Written by a newer JasperEngine (scene format 2; this editor writes 1).`

### Functional — load report and preservation
- **R24** `editor/StreamSchema.{h,cpp}` provides:
  - `knownFieldKeys(ComponentType)`, the top-level keys each component's decoder reads;
  - `knownRootKeys(kind)` for scene, entity node, project, material and prefab header;
  - name tables for every enum decoded from YAML. The 52 `stringToX` functions become table-driven
    `decodeEnum(node, key, table, fallback)`.
  S08 later replaces these tables with reflection.
- **R25** `editor/LoadReport.h`:
  `LoadIssue{kind, fate, sceneId, entity, entityPath, component, field, message, suggestion, file, line, column}`.
  - `kind` is one of `UnknownComponent`, `UnknownField`, `UnknownEnumValue`, `InvalidValue`,
    `UnknownEntityType`, `UnknownRootKey`, `NewerFormat`, `Migrated`.
  - `fate` is one of `Preserved`, `Dropped`, `Info`.
  - `LoadReport{documentPath, versionRead, versionCurrent, issues}` has `dropped()`, `preserved()`
    and `outstanding(store)`.
  - Lines come from `YAML::Node::Mark()` (1-based in the report).
  - `suggestion` is `NearestNameSuffix` over the relevant candidates: component names from
    `Catalog`, field keys from R24, or enum names.
  The decoder reports through a thread-local `ScopedLoadReport` installed by the loader. With no
  report installed (clipboard, duplicate, undo), nothing is recorded but preservation still
  applies.
- **R26** Preservation (`editor/StreamPreserved.{h,cpp}`). The side table is
  `PreservedStore{entity → {rootKeys, components: name→YAML, fields: component→key→PreservedField}}`
  and is owned by the `EntityRegistry` through an opaque `std::shared_ptr<void> editorData`
  member. That ties its lifetime to the registry; entries are pruned when the entity is destroyed.
  What is preserved:
  - (a) an unknown key under `components`: the whole node, verbatim;
  - (b) an unknown top-level key of a known component;
  - (c) an unknown key at entity-node root or document root (scene, project, material,
    prefab header);
  - (d) a known key whose value fails to decode (`InvalidValue`, `UnknownEnumValue`): the field gets
    its default and the authored node is kept;
  - (e) a component whose decoder throws: it is kept verbatim as in (a) and not instantiated,
    except Transform, which gets a default value and a preserved shadow so the hierarchy stays
    intact.
  Not preserved in v1, and reported as `Dropped`: unknown entity `type`s, unknown keys nested below
  a component's top level (e.g. inside `submeshes[i]`), and unknown component names inside prefab
  instance override records.
- **R27** Re-emission: `encodeEntity`, `encodeComponents`, the document encoders and
  `encodePrefabNode` append preserved nodes after the known keys, in their authored order. A (d)/(e)
  substitution holds only while the live field re-encodes to the same text it re-encoded to right
  after load. Once the user writes that field (or component), the live value wins and the
  preserved entry retires. Removing the component or deleting the entity also retires its entries.
  A preserved issue is *outstanding* while its entry still exists. Because duplicate, clipboard,
  undo snapshots and prefab apply all go through `encodeEntity`/`decodeEntity`, preserved data
  travels with them.
- **R28** File-level overlay for whole-file documents (`project.yaml`, `.material` A3-A7): at save,
  the result is the encoded node plus every authored key of the current on-disk file that is not in
  `knownRootKeys`. This covers files that were never loaded through a report, such as the bulk
  material rewrite at `Project.cpp:2205`.
- **R29** A YAML parse error still fails the open, as today, and the file is never written. The
  message now names `file:line:col` from `YAML::ParserException::mark`.
- **R30** Exported and played builds are generated from the in-memory scene, so preserved unknown
  components are not in them. Play and Export log one warning per affected scene:
  `Level1.scene: 2 components this editor does not know are kept in the file but not in the build.`

### Functional — lossy-save gate
- **R31** `saveSceneFile`, `savePrefabToDisk`, Prefab Mode save and autosave, `saveProjectFile`
  and the material saves (A4, A5, A7) take a `DegradedSavePolicy`, default `Refuse`. If the target
  is the document's own source file and `report.dropped() > 0`, a `Refuse` save writes nothing,
  returns `SaveResult::RefusedDegraded`, and the document stays modified. `SaveAnyway` writes, then
  clears the dropped issues (the file no longer has them) and logs a warning listing what was lost.
- **R32** Save As to a different path is not gated, because the original keeps the data. After it,
  the document points at the new file and its dropped issues are cleared, with a warning:
  `The original Level1.scene still holds 3 values this save did not keep.`
- **R33** Prefab Mode autosave never auto-saves a degraded stage. It turns into "Auto Save paused:
  this prefab did not load completely" in the stage bar.

### Platform & backends
- **R34** `AtomicFile`, `ScopedFileLock` and the sweep compile and pass their tests on Windows,
  Linux and macOS (editor), and on Android, iOS and Emscripten (runtime `UserSettings.xml`). On
  Emscripten, `ScopedFileLock` is a no-op that reports `isLocked() == true` (single process), and
  the pid check treats every orphan as dead except its own. Rendering backends are not affected.

### Performance
- **R35** Budgets, measured on the CI Linux runner:
  - a `Cache` atomic write of 64 KB is ≤ 1.5× a plain `ofstream` write;
  - a `Document` write of a 1 MB scene adds ≤ 25 ms (fsync included);
  - autosave main-thread encode is ≤ 10 ms for `tools/fixtures` medium scene (≈2k entities). If one
    encode exceeds 50 ms, the scene's interval doubles (capped at 300 s) and one info line is
    logged;
  - with no modified scene, autosave costs one float accumulate per frame;
  - decoding with an installed report is ≤ 5% slower than without on the 5k-entity fixture;
  - the orphan sweep adds no directory walk;
  - a `renameWithRetry` that keeps failing blocks ≤ 0.8 s. Autosave never retries on the main
    thread.

### Editor UX (human)
- **Recovery modal** "Recover Unsaved Changes":
  - Text: `"Level1.scene" has changes from a session that did not close normally (auto-saved
    14:32:05).`
  - Buttons: **Restore** (default), **Discard**, **Decide Later**.
  - Discard asks for confirmation ("The auto-saved changes will be deleted.").
- **Lossy-save modal** "Saving Would Lose Data":
  - Text: `"Level1.scene" did not load completely. Saving it now permanently removes 3 values this
    editor could not read:`.
  - Below it, a list of up to 20 issues in the form `Player › Light.type = "area": unknown value
    (did you mean "spot"?) — Level1.scene:142`, then `Show All in Console`.
  - Buttons: **Cancel** (default focus), **Save As…**, **Save Anyway** (red).
  - Save All shows the modal once per degraded document, after saving the clean ones.
- **Degraded state:**
  - The scene tab and window title get the suffix ` (Partial Load)`.
  - A Hierarchy banner reads `This scene did not load completely: 3 values dropped, 5 kept.
    [Details]`. Details opens the Console filtered to that file.
- **Console:**
  - One line per issue, format `<file>:<line>: <Component>.<field>: <message><suggestion> —
    kept|dropped`, so S23 click-to-source can parse it.
  - One summary line per load, e.g. `Loaded Level1.scene with 8 issues (5 kept, 3 dropped).`
  - Migrations are logged at info level.
- **Second-instance dialog:**
  - Text: `"MyGame" is already open in another JasperEngine editor (PID 4242 on host "ws1", since
    09:12). Two editors on one project overwrite each other's changes.`
  - Buttons: **Cancel** (default), **Open Anyway**.
- **Editor Settings > General:**
  - `Auto Save Scenes` (checkbox) and `Auto Save Interval (s)` (30-300);
  - `Open Backup Folder` button.

### Agent UX (MCP, CLI, Lua)
- **R36** `open_scene` gets `recovery: keep|restore|discard` (default `keep`). Its result carries
  `recovery: {available, autosaved_at, status: usable|stale|none, applied}` and
  `load_report: {skipped, preserved, dropped, newer_format}`.
- **R37** `save_scene` (single and `all=true`), `save_project`, and every material/prefab write tool
  get `allow_data_loss` (default false). A refused save fails with `code: "degraded_document"`,
  `data.dropped: [issue…]` (max 50) and the hint `Pass allow_data_loss=true to write anyway, or
  path=<new file> to Save As.` Every save result includes `degraded: {preserved, dropped}`. With
  `all=true`, clean scenes are saved, refused ones are listed, and the call fails only if one was
  refused.
- **R38** New tools:
  - `get_load_report {scene}`: full issue list, including outstanding state;
  - `list_scene_backups {}`: `[{source, backup, status, autosaved_at}]`;
  - `discard_scene_backup {scene}`.
  The MCP agent guide (`editor/mcp/McpInstructions.cpp`) and `docs/` gain a "Data safety" section.
- **R39** CLI: `scene-dump` prints the load report to stderr, and `scene-dump --save` honours the
  gate. The new flag `--allow-data-loss` overrides the gate, and a refusal exits with code 4.
  `--ignore-lock` is as in R10.
- **R40** No Lua API change. `UserSettings` becomes crash-safe transparently (B4).

### Data & compatibility
- **R41** Files written by today's editor load with zero issues, and a load-then-save round trip
  differs from the input only by the added `formatVersion` line. Repeated round trips are
  byte-identical.
- **R42** Preserved nodes are re-emitted structurally equal to what was authored: same keys, same
  scalar text and same sequence order. YAML comments and flow/block style are not guaranteed;
  today's saves lose them too.

## 6. Design constraints & decisions

- **One primitive, in the engine.** The runtime (B4) needs it as well as the editor, and
  `engine/core` already uses `std::filesystem` (`RuntimeShaderCompiler.cpp`). Namespace
  `jasperengine::io`. Sketch:
  ```cpp
  enum class Durability { Cache, Document };
  enum class WriteError { None, Unchanged, CreateDirectory, OpenTemp, WriteTemp, SyncTemp,
                          ReadOnlyTarget, Replace, Cancelled };
  struct AtomicWriteOptions { Durability durability = Durability::Document; bool skipIfUnchanged = false;
      int attempts = 5; std::chrono::milliseconds retryDelay{200}; std::function<bool()> shouldCancel; };
  struct WriteResult { WriteError error = WriteError::None; std::error_code code; std::string message;
      int attempts = 0; bool ok() const { return error == WriteError::None || error == WriteError::Unchanged; } };
  WriteResult writeFileAtomic(const std::filesystem::path&, std::string_view bytes, const AtomicWriteOptions& = {});
  WriteResult publishFile(const std::filesystem::path& temp, const std::filesystem::path& target, const AtomicWriteOptions& = {});
  std::filesystem::path makeTemporarySiblingPath(const std::filesystem::path& target);
  bool isTemporarySiblingName(std::string_view filename);
  bool renameWithRetry(const std::filesystem::path&, const std::filesystem::path&, const AtomicWriteOptions&, std::error_code&);
  class ScopedFileLock { public: enum class Mode { Exclusive, Shared };
      ScopedFileLock(const std::filesystem::path&, Mode = Mode::Exclusive, bool wait = true);
      bool isLocked() const; const std::string& failureReason() const; };
  struct OrphanSweep { std::vector<std::filesystem::path> quarantined, removed; };
  OrphanSweep quarantineOrphanedTemporaryFiles(std::span<const std::filesystem::path> candidates,
                                               const std::filesystem::path& quarantineRoot);
  // Test seam (no-op unless set): fail the Nth write/sync/rename with a given error.
  void setAtomicFileFaultInjector(std::function<std::error_code(std::string_view step, const std::filesystem::path&)>);
  ```
- **Publish hook for S05:** `setPublishListener(std::function<void(const path& target, uintmax_t size,
  file_time_type mtime)>)` is called after every successful publish. S05's self-write ledger
  subscribes to it; nothing else may.
- **Enforcement:** `tools/check_raw_writes.py` fails CI (S02) on `std::ofstream`, `std::fstream` in
  write mode, `fopen(…"w`/`"a`, file-path `stbi_write_*`, and `copy_file(… overwrite_existing`
  under `editor/` and `engine/core/`, except for the allow-list file
  `tools/raw_writes_allowlist.txt`, where each entry has a reason.
- **Backups live in `.jasperengine/backup/`, not beside the scene.** That keeps the AssetDatabase,
  the Project window and VCS clean: `.jasperengine/` is already skipped and git-ignored. The
  decision is the base-content hash, not mtime (R17).
- **Degraded state lives on the document:** `SceneProject::loadReport` and `Prefab::loadReport`, plus
  `Project::projectLoadReport`. The gate checks `outstanding()`, never a cached count. The
  load-time SHA-1 is stored as `SceneProject::diskSha1` and updated after each successful save
  (R13).
- **No decision through the default `EditorHost` alert callbacks.** Recovery uses
  `resolveSceneRecovery`. The lossy-save gate uses `EditorHost::confirmDegradedSave(report) ->
  DegradedSavePolicy`; the base implementation returns `Refuse`.
- **Known-key tables are hand-kept until S08.** Tests keep them honest (AC12).
- Migrations are pure YAML→YAML functions with no editor state, so the CLI and tests can run them.
- `NearestName.h` is the only file copied verbatim. Everything else is our code written against our
  YAML.

## 7. Acceptance criteria

gtest suites (S02 framework) under `editor/tests/data_safety/` unless noted. MCP tests are under
`tools/mcp/tests/`.

- **AC1** `AtomicFile.FailureKeepsTarget`: the fault injector fails each step in turn (open, write
  at 50%, sync, rename ×5). Each time, the target's bytes equal the old bytes and no `*.je-tmp`
  remains. Covers R1.
- **AC2** `AtomicFile.RetryThenSucceeds`: rename fails with `device_or_resource_busy` twice, the
  write succeeds with `attempts == 3`, and elapsed time is ≥ 400 ms. On Windows CI, holding the
  target open without `FILE_SHARE_DELETE` for 300 ms also succeeds. Covers R3.
- **AC3** `AtomicFile.ReadOnlyRefused`, `.ModePreserved` (0755 kept, POSIX), `.SymlinkPreserved`,
  `.UnchangedKeepsMtime`. Covers R5-R7.
- **AC4** `AtomicFile.ConcurrentWriters`: 8 threads × 200 writes to one target. Every read
  observes one complete payload, never a mix or a truncation. Covers R1 and R2.
- **AC5** `ScopedFileLock.CrossProcess`: a child process holds the lock, and a non-blocking acquire
  in the parent fails with the owner JSON readable. Once the child is killed, the acquire succeeds.
  Covers R10.
- **AC6** `test_data_safety.py::test_project_lock`: a second `jasperengine-editor mcp --headless -p`
  on the same project fails with `project_locked`. With `--ignore-lock` it starts. Covers R10.
- **AC7** `AppSettings.MergeConcurrent`: processes A and B load, A changes the window width, B adds
  a recent project, both save. Both changes are present. A corrupt file is set aside as
  `.corrupt-*`. Covers R11.
- **AC8** `OrphanSweep.QuarantinesDeadPid`: a temp with a dead pid is moved under
  `backup/orphans/`, a temp with a live pid is kept, and an empty one is deleted. Covers R12.
- **AC9** `tools/check_raw_writes.py` exits 0 on the tree and 1 when a fixture with a raw `ofstream`
  is added. Every §3.2 row is fixed or allow-listed. Covers R9.
- **AC10** `SceneBackup.ScanUsableStaleNone`: the matrix of base hash, backup bytes and pid alive
  gives Usable, Stale (moved to `stale/`) or None. `ResolveRestoreDiscardKeep` gives the content
  path, document path and modified flag of R18. Covers R17 and R18.
- **AC11** `test_data_safety.py::test_crash_recovery`:
  1. edit a scene over MCP;
  2. wait for the autosave (interval set to 30 s through an MCP settings tool or the
     `JE_AUTOSAVE_INTERVAL_TEST=1` env override);
  3. `kill -9` the editor;
  4. relaunch, then `open_scene` returns `recovery.available=true` and `applied=keep`;
  5. `open_scene recovery=restore` shows the edit and the scene is modified;
  6. `save_scene` deletes the backup.

  A second pass with `recovery=discard` shows the saved state and no backup. Covers R13-R19.
- **AC12** `StreamSchema.EncoderKeysAreKnown`: a maximal non-default fixture of every
  `ComponentType` encodes only keys listed in `knownFieldKeys`. `tools/check_stream_keys.py` checks
  that every `node["literal"]` inside each `decodeX` body in `Stream*.cpp` is in the table.
  Covers R24.
- **AC13** `Preservation.RoundTrip`: the fixture `tools/fixtures/data-safety/future.scene` has:
  - an unknown component `Jumpable{height: 2}`;
  - an unknown field `Light.areaSize`;
  - `Light.type: area`;
  - `Transform.position: [a, b]`;
  - an unknown root key `lighting2`;
  - an unknown entity root key.

  Loading it creates no exception and the report's issues match an expected list exactly
  (kind/component/field/line/fate; `area` → suggestion `spot`). Saving with `SaveAnyway` = false
  succeeds, because nothing is dropped. The output equals the input structurally, apart from
  `formatVersion`. Covers R25-R27 and R42.
- **AC14** `Preservation.Retire`: after setting `Light.type` to `point`, the save writes `point`.
  After removing Jumpable, the save omits it. Duplicating the entity duplicates Jumpable, and
  undoing a delete restores it. Covers R27.
- **AC15** `SaveGate.RefuseAndSaveAnyway`: the fixture `dropped.scene` has an unknown entity type
  and an unknown key under `submeshes[0]`. `saveSceneFile(Refuse)` returns `RefusedDegraded` and
  the file's bytes are unchanged. `SaveAnyway` writes and clears the issues. Save As to a new path
  writes without refusing. The degraded scene never gets an autosave backup. Covers R31, R32 and
  R14.
- **AC16** `test_data_safety.py::test_mcp_degraded_save`:
  - `save_scene` on `dropped.scene` fails with `degraded_document` and lists 2 issues;
  - `allow_data_loss=true` succeeds;
  - `save_scene all=true` saves the clean scenes and reports the refused one;
  - `get_load_report` matches AC15;
  - in headless mode no modal blocks.

  Covers R36-R38.
- **AC17** `Migrations.SceneV0ToV1`, `.NewerFormatGated`: a v0 fixture loads clean, is not
  modified, and saves with `formatVersion: 1`. A `formatVersion: 99` file loads and is gated with
  the R23 message. A prefab `version: 4` is gated. Covers R20-R23.
- **AC18** `Compat.CorpusRoundTrip`: every `.scene`/`.prefab`/`.material`/`project.yaml` under
  `tools/fixtures` and the example projects loads with zero issues. A round trip changes at most
  the `formatVersion` line, and a second round trip is byte-identical. Covers R41.
- **AC19** Benchmarks `AtomicFileBench`, `AutosaveEncodeBench` and `LoadReportOverheadBench` meet
  the R35 thresholds in the CI job, which fails above threshold.
- **AC20** Manual check, recorded with screenshots in the PR:
  - open `future.scene` in the GUI: the tab shows no "(Partial Load)" (nothing dropped) and the
    Console shows 6 issues;
  - open `dropped.scene`: the tab shows "(Partial Load)", the banner appears, Ctrl+S shows the
    lossy-save modal with Cancel focused, and Esc cancels;
  - kill the editor after an autosave and relaunch: the recovery modal appears and Decide Later
    offers it again on the next open;
  - open a second instance on the project: the second-instance dialog appears.
- **AC21** Runtime: `engine/tests` `UserSettings.AtomicOnCrash`: the fault injector fails the rename,
  and the previous `UserSettings.xml` stays readable. The test is run in the Android and web CI
  jobs (S02). Covers R34 and B4.
- **AC22** `tools/smoke-test.sh` and the existing MCP suite stay green. `test_cli_paths.py` and
  `test_project.py` are unchanged.

## 8. Dependencies & conflicts

- **Depends on:** S01 (C++20 `std::span`), S02 (gtest target, CI job for the check scripts and
  benchmarks, Android/web test jobs).
- **Blocks / feeds:**
  - S05 uses `isTemporarySiblingName` and the publish listener;
  - S08 replaces `StreamSchema` with reflection and must keep AC12/AC13 passing;
  - S24 relies on undo snapshots carrying preserved data (R27);
  - S29 relies on the read-only refusal and `formatVersion` in the semantic diff;
  - S30 builds export staging on `publishFile`/`renameWithRetry`;
  - S23 relies on the console `file:line` format;
  - S04 relies on GUID writes into asset files going through A10, and its missing-asset tracker
    should reuse `LoadIssue`;
  - S28 must never trigger autosave from a crash handler.
- **Every spec that adds serialized fields** follows R22 and adds its keys to `StreamSchema`
  (until S08).
- **Merge hotspots:**
  - `editor/Stream.cpp` (S07, S08 and every component spec): land R24's table-driven enums early,
    as one mechanical commit;
  - `editor/Project.cpp` (save/open paths: S04, S24, S29);
  - `editor/project/ProjectPrefabPaths.cpp`, `editor/PrefabStage.cpp` (S24);
  - `editor/AppSettings.cpp` and `EditorSettingsWindow.cpp` (S25 settings search);
  - `editor/util/AssetDatabase.cpp` (S04);
  - `editor/mcp/tools/SceneTools.cpp` (S26, S31);
  - `editor/EditorHost.h` (several specs add virtuals).

## 9. Risks & defaults

- **Main-thread encode stalls on huge scenes.** Default: the adaptive interval (R35). If the
  medium-fixture budget fails, encode into a detached `EntityRegistry` snapshot on the main thread
  and serialize it off-thread. Not before.
- **False "unknown field" reports because a known-key table is stale.** This would cause gate
  refusals on valid files. Mitigation: AC12 plus the corpus test AC18. Default: an unknown *field*
  of a known component is always `Preserved`, never `Dropped`, so a stale table can only cause
  noise, never a refusal.
- **`RENAME_SWAP` unsupported** (exFAT, SMB): fall back to `rename`. Tested by injecting `ENOTSUP`.
- **Network drives without locks:** warn and continue (R10). Opening one project twice from two
  machines remains possible there.
- **Windows AV holding files longer than 0.8 s:** the save fails cleanly and the document stays
  modified. The user retries. No longer default retry is configured.
- **Restore after a crash during Play:** autosave never runs during Play, so the backup predates
  Play. That is the accepted default.
- **A degraded scene has no crash protection** (R14), by decision. Its edits could otherwise be
  restored into a file that silently lacks the dropped data. The banner says
  `Auto Save is off for this scene.`
- **Hard links are broken by rename-publish.** Accepted and documented.
- **`editorData` slot on `EntityRegistry` touches the engine.** It is one opaque member, with no
  YAML dependency in engine code.

## Deviations

Decisions taken while implementing, each the closest correct alternative to the text above:

- **Test targets.** Until S02 lands, the gtest suites build as `jasperengine-core-tests`
  (`engine/tests/core`, `-DJASPERENGINE_TESTS=ON` on the engine tree) and
  `jasperengine-editor-tests` (`editor/tests`, root tree), run by `tools/build.sh unit-tests`,
  with googletest from the yaml-cpp tree. The engine primitive's suites (AC1-AC5, AC8, AC19's
  `AtomicFileBench`, AC21) live in `engine/tests/core/io/` (S02's mirror of `engine/core/io`,
  tested against the static engine) rather than `editor/tests/data_safety/`.
- **`EntityRegistry::editorData`** is a `std::shared_ptr<EntityRegistryEditorData>`: a three-method
  interface (`entityDestroyed`, `componentRemoved`, `registryCleared`) instead of `void`, because
  pruning entries when an entity or component goes needs the registry to tell the side table.
- **API details.** `quarantineOrphanedTemporaryFiles` takes `const std::vector<path>&` (C++17
  until S01) and a third `relativeTo` argument for the quarantine path, with `pruneQuarantine`
  next to it. `ScopedFileLock` adds `getStatus()` (Busy / Unsupported / Failed),
  `writeOwnerInfo`, `readOwnerInfo` and `release`. `WriteResult` adds `written()`;
  `describeWriteError`, `temporarySiblingWriter` / `Target`, `currentProcessId` and
  `isProcessRunning` are public.
- **Host decisions are asynchronous in the GUI.** `EditorHost::confirmDegradedSave` takes a
  `DegradedSaveRequest` (document, dropped issues, `saveAnyway` / `saveAs` continuations): the
  editor window queues "Saving Would Lose Data" and answers Refuse, and its buttons run the
  continuation. `resolveSceneRecovery` answers Keep and queues "Recover Unsaved Changes", whose
  Restore reopens the scene from the backup (`Project::restoreSceneFromBackup`).
  `confirmOpenLockedProject` was added for the second-instance dialog the same way.
- **`mcp --headless` runs the editor App** (under Xvfb), not the base host. An editor serving MCP
  on stdio answers the three questions like the base host (Refuse, Keep, no), so no modal ever
  waits for an agent; `--ignore-lock` was added to `mcp`, and a locked project exits with code 3.
  `check`, `export` and `benchmark` take no lock (they never write documents); `scene-dump` takes
  it only with `--save` or `--ops`.
- **Did-you-mean for enum values** offers the nearest name of the (small, closed) set whatever the
  distance: AC13 expects `area` → `spot`, four edits apart, beyond `NearestName`'s cap of 2. Unknown
  component and field names keep the cap.
- **Which nested structures are checked.** Unknown keys below a component's top level are
  reported (dropped) inside the structures with decoders of their own and a key table: submesh,
  material, material pass, stencil and texture. Others are not checked yet (S08's reflection
  covers them).
- **Unknown enum values outside a component's top level** (inside a submesh's material, in
  `project.yaml` or a `.material` file, whose file overlay keeps unknown keys only) are dropped,
  so they gate saves. The scene settings block (`scene:`) is preserved like a component (its own
  pseudo component `#scene` in the side table).
- **Material file saves (A4, A5, A7)** gate on a report of the file as it is on disk at save time
  (materials are read in many places, so no report lives on a document); a new material file gets
  `formatVersion` and the file overlay.
- **Prefab file header order.** Prefabs keep `type` before `version`, as today: moving `version`
  first would change every prefab file on its next save, against R41.
- **Play and Export save modified scenes first.** A degraded scene therefore stops them with a
  message (Save Anyway or Save As first) instead of being saved without its data; R30's warning is
  logged for the scenes they build.
- **MCP prefab tools.** `add_to_prefab`, `remove_from_prefab` and `prefab_overrides` change the
  editor's model before the asset is saved; when the gate refuses that save they fail with
  `degraded_document` after the change (the asset file is untouched, the prefab stays modified).
  `prefab_mode action=save` refuses before writing. `load_report.skipped` counts the preserved
  plus dropped issues.
- **Verification without a GPU.** `Project` loads scenes with a scene render, so AC10's
  Restore/Discard/Keep and AC15's gate (refusal, Save As, no autosave) are verified in a running
  editor (`tools/mcp/tests/test_data_safety.py`); the unit suites cover the report, the side table
  and the backup files. AC20's manual check (screenshots) is left to the PR review.
- **Autosave encode budget (R35).** Building the YAML tree of a 2k-GameObject scene costs about
  250 ms in a Release build (yaml-cpp allocates a node per value; Transforms hold three
  matrices), far over 10 ms. The main thread only builds the tree; its text, hash, comparison
  and write run on a worker, and the scene's interval doubles after an encode over 50 ms (the
  spec's default for this risk). `AutosaveEncodeBench` guards 0.25 ms per GameObject against
  regressions. The snapshot-then-encode-off-thread design of §9 is the follow-up if the
  interval adaptation is not enough.
- **`JE_AUTOSAVE_INTERVAL_TEST=1`** makes the interval 2 s (the test waits for a backup); R35's
  decode-overhead budget allows 1 ms of absolute slack over the 5% on top of timer noise.
- **R41 and `shaderKeys`.** A scene's `shaderKeys` is a set ordered by keys that hold custom
  shader ids, which each session assigns in its own order, so its order (never its content) can
  change on the first save in another session, as before this spec. `Compat.CorpusRoundTrip`
  compares it as a set and the rest of the file byte for byte.
- The window title suffix reads ` - Level1.scene (Partial Load)` after the project name.
- **The agent guide's "Data safety" section (R38)** is two lines pointing at a new agent
  reference, `docs/mcp/guide/data-safety.md` (`jasper://docs/data-safety`): the guide was 20
  bytes under its 16 KB budget (`test_autosync.py`), so the detail lives in the served doc and
  a few intro words were tightened to make room.
- **Projects below the temp directory.** `isTempProject()` is true for any project under the
  system temp directory (MCP test scratch projects included); Auto Save and the project lock skip
  only the editor's own temporary project (`<temp>/<name>`, remade at each start). A project made
  with `--create`/File > New, or the temporary project saved elsewhere, takes the lock at once.
- `mcp --project` on a locked project exits with code 3 after one frame on the empty temporary
  project (closing before the first frame leaves the render queue waiting on shutdown).
