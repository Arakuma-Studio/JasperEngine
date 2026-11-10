# Data safety

The editor never destroys a file you made. This page says how, what you see when something
goes wrong, and the rules code that writes files or adds serialized data follows.
(Specification: [specs/S03-data-safety.md](specs/S03-data-safety.md).)

## What the editor guarantees

- **Saves are atomic.** A scene, prefab, material, `project.yaml`, script, setting or generated
  file is written to a hidden temporary file next to it (`.Level1.scene.4242-7.je-tmp`), flushed
  to the disk, then renamed over the original. A crash, a power cut or a full disk leaves the old
  file or the new one, never half of one. If the save fails, the scene stays modified and the
  editor says `Couldn't save "<path>": <reason>. Your changes are still in the editor.`
- **A save never leaves a GameObject out.** The encode records every GameObject it writes; one
  of the scene (or of a Prefab Mode stage) it would leave out refuses the save before anything is
  written: `Couldn't save "<path>": N GameObject(s) of scene 'X' would be left out ('Name' id,
  ...) ... (an editor fault: please report it); nothing was written. Your changes are still in
  the editor.` Every writer refuses the same way: Save, Save All, the background save of Ctrl+S,
  Auto Save (the backup is skipped), Play's snapshot (Play does not start), an export (it stops),
  and `scene-dump`'s `save` op and `--save` (exit 1). A complete file whose Transform order splits
  a branch is written with a console warning. Members of a prefab instance whose root is written
  are in the prefab file, not the scene's.
- **Read-only files are not written over.** Git LFS and Perforce lock files by making them
  read-only; the save fails with "the file is read-only (is it locked by version control?)".
- **One editor per project.** Opening a project another editor has open shows
  *"MyGame" is already open in another JasperEngine editor (PID 4242 on host "ws1", since 09:12).*
  with **Cancel** and **Open Anyway**. The lock is `.jasperengine/user/editor.lock`; the
  operating system releases it when that editor exits or crashes, so a stale lock never blocks.
  On a network share without file locks the editor warns once and opens the project unlocked.
- **Editor settings are shared safely.** Two editors saving the global `settings.yaml` merge
  their changes (each writes only the settings it changed; recent projects are merged). A
  settings file that cannot be read is set aside as `settings.yaml.corrupt-<date>` instead of
  being overwritten.
- **Interrupted saves are kept.** When a project opens, temporary files left by a session that
  crashed between writing and renaming are moved to `.jasperengine/backup/orphans/` (they can
  hold the newest copy of a document) and deleted after 7 days.

## Scene files and identity

A scene is one `.scene` file, and the file says which scene it is: its `id` (what child scenes,
the build list and the generated code name it by) and its `name` (unique in a project, it names
the generated code and `SceneManager.loadScene("Level1")`).

- **One file per scene.** No save writes a scene over the file of another scene of the project:
  the Save Scene dialog shows *"Level1.scene is the file of scene "Level1": choose another name"*
  and disables Save, and every other route (Save All, MCP `save_scene`, the background save)
  refuses with the same reason. Two spellings of one file count as one on a volume that ignores
  case (Windows, macOS by default). `project.yaml` lists each scene file once; a second entry for
  a file (an old project, a hand edit) is ignored at load with a warning and dropped at the next
  save.
- **Scenes stay inside the project.** The Save Scene dialog takes a name, not a path: `/`, `\`,
  `..`, a drive and names Windows refuses (`CON`, `a?b`, a trailing dot) are refused there, so a
  project moves between systems. A scene file from outside the project is not opened into it:
  copy it in first (drag it into the Project window). A scene an older project lists outside its
  folder still loads (with a warning) and saves where it is.
- **A copy is a new scene.** Duplicating or copying a `.scene` (the Project window, MCP
  `move_asset copy`, a copied folder) gives the copy its own scene id, and the scene name of its
  file (`Level1 1.scene` is scene "Level1 1"; a name given by hand gets a number). Undo and redo of
  the copy give it the same id again. References inside copied scenes are not redirected to the
  other copies: a copied parent scene keeps its original child scenes, as every other reference
  in a copied folder keeps its original asset. Copying over an existing scene file keeps that
  file's id and name: it stays the scene it was.
- **Two files claiming one id.** A copy made outside the editor, or a merge, can still bring two
  files with one id. As Unity regenerates a duplicate GUID, the scene listed first in
  `project.yaml` (or open first) keeps it, and the other gets a fresh id written into its file at
  once; the Console names the file and both ids. A read-only file (a version control lock) keeps
  the fresh id in the editor until the next save writes it.
- **Save All saves every scene.** File > Save All saves the scenes with changes, then asks where
  to save each scene that was never saved (Ctrl+S on an untitled scene asks too, changed or not).
  Cancel stops there. The save before quitting or switching projects leaves out an untitled scene
  with no changes (there is nothing in it to keep). The first scene saved becomes the project's
  start scene when it has none.
- **Every failed save says why**, in an alert and in the Console, whatever the route.

## Auto Save and crash recovery

Auto Save never writes your scene file. Every minute (Editor Settings > General > Auto Save
Interval, 30 to 300 s) each modified scene is copied to `.jasperengine/backup/scenes/<path>`
(untitled scenes to `backup/untitled/`, Prefab Mode with its own Auto Save off to
`backup/prefabs/`), with a JSON note of the file it was made from. It waits while you drag or
type, never runs in Play, and skips scenes that did not load completely (below).

- Saving the scene, or closing it without saving, deletes its backup. Quitting normally deletes
  every backup of the session. A crash leaves them.
- When you open a scene that has a backup from a session that did not close normally,
  **Recover Unsaved Changes** offers **Restore** (the scene takes the backup's content and stays
  modified; save to keep it), **Discard** (asks first, then deletes the backup) or **Decide
  Later** (the saved file opens; the backup is offered again next time).
- A backup counts only for the exact file it was made from (its SHA-1 is recorded, not its
  time). If the file changed since (a `git pull`, another editor), the backup is moved to
  `backup/stale/` with a warning and never offered or deleted automatically.
- Editor Settings > General has **Auto Save Scenes**, **Auto Save Interval (s)** and **Open
  Backup Folder**.
- The temporary project the editor makes for itself when no project is open (in the system temp
  directory, remade at each start) has neither Auto Save nor a project lock; save it as a
  project of its own to get both.

## Files this editor cannot fully read

A scene written by a newer editor, a component from a plugin you do not have, or a typo in a
hand-edited file used to vanish silently on the next save. Now each load has a **load report**:

```
Level1.scene:142: light.type: unknown value "area" (did you mean "spot"?) (in Player) — kept
Loaded Level1.scene with 3 issues (2 kept, 1 dropped).
```

Each issue is **kept** (preserved) or **dropped**:

- **Kept, written back on save:** an unknown component (the whole block), an unknown field of a
  known component, an unknown key on a GameObject or at the top of the file, and a known field
  whose value does not read (an unknown enum name, text where a number goes): the field shows
  its default and the original text is written back for as long as you do not change that
  field. A component that cannot be read at all is kept but not loaded (a Transform gets a
  default so the hierarchy stays). Duplicate, copy/paste, undo and prefab Apply carry kept data
  along.
- **Dropped, lost by the next save:** a GameObject `type` other than `Entity`, an unknown key
  nested inside a component (`submeshes[0].glint`), an unknown component in a prefab override,
  and a file written in a newer format (*Written by a newer JasperEngine (scene format 2; this
  editor writes 1).*).

A document with dropped issues shows **(Partial Load)** on its tab and in the window title, and
the Hierarchy says *This scene did not load completely: 1 value dropped, 2 kept.* with
**Details** (the Console, filtered to the file). Saving it asks first: **Saving Would Lose
Data** lists what would go, with **Cancel** (default), **Save As…** (the original keeps the
data) and **Save Anyway**. Save All saves the clean documents and asks about each degraded one.
Prefab Mode's Auto Save pauses for such a prefab. Play and Export save modified scenes first,
so they stop and say why.

Play and exported builds are made from the scene in memory, so kept unknown components are not
in them: *Level1.scene: 2 components this editor does not know are kept in the file but not in
the build.*

A YAML syntax error still stops the load (`Level1.scene:12:5: ...`) and the file is never
written.

## Agents and the command line

- MCP: `open_scene` takes `recovery: keep|restore|discard` (default keep) and returns
  `recovery {available, autosaved_at, status, applied}` and `load_report {skipped, preserved,
  dropped, newer_format}`. `save_scene` (also `all=true`), `save_project`, `create_material`,
  `update_material`, `apply_material_preset`, `add_to_prefab`, `remove_from_prefab`,
  `prefab_overrides` and `prefab_mode` take `allow_data_loss` (default false); a refused save
  fails with `code: "degraded_document"`, the dropped issues and the hint *Pass
  allow_data_loss=true to write anyway, or path=<new file> to Save As.* `get_load_report`,
  `list_scene_backups` and `discard_scene_backup` complete the set. No modal ever waits for an
  agent: an editor serving MCP on stdio answers them the safe way (refuse, keep).
- `jasperengine-editor mcp --project <dir>` on a project another editor has open exits with
  code 3 (`project_locked`) unless `--ignore-lock`.
- `jasperengine-editor scene-dump` prints every scene's load report to stderr; `--save` refuses
  degraded scenes (exit code 4) unless `--allow-data-loss`, and takes the project lock (exit 3,
  `--ignore-lock`). `check` and `export` only read the project and take no lock.
- Games: `UserSettings` (`UserSettings.xml`) is written atomically on every platform; nothing
  changes in the Lua API.

## Rules for code (contributors)

**Writing files.** Every persistent write goes through `engine/core/io/AtomicFile.h`:

```cpp
#include "io/AtomicFile.h"
io::WriteResult result = io::writeFileAtomic(path, bytes);            // Document: fsync, for user data
io::AtomicWriteOptions cache;
cache.durability = io::Durability::Cache;                             // no fsync: caches, thumbnails, exports
cache.skipIfUnchanged = true;                                         // leaves an identical file (and its mtime) alone
io::writeFileAtomic(path, bytes, cache);
io::AtomicWriteOptions existing;
existing.createDirectories = false;                                   // a missing folder fails (MissingDirectory) instead of being created
io::writeFileAtomic(folderTheUserPicked / name, bytes, existing);
io::publishFile(io::makeTemporarySiblingPath(path), path, options);   // for writers that stream to a file
io::AtomicFileWriter writer;                                          // output too large to hold in memory: open, write in pieces, commit
io::renameWithRetry(from, to, ec);                                    // moves and renames
if (!result.ok()) Out::error(DocumentFiles::saveFailureMessage(path, result));
```

Editor helpers on top: `DocumentFiles::write` (Document writes of documents, with the failure
message), `FileUtils::writeFile` / `writeIfChanged` (Cache, skip if unchanged),
`FileUtils::copyFileAtomic`, `util/PngWriter.h` `writePng` (PNG encoded in memory),
`util/AtomicFileStream.h` (an `ostringstream` written atomically when it goes out of scope).
`tools/check_raw_writes.py` fails on a new `std::ofstream`, `fopen("w")`, `stbi_write_*` to a
path or `copy_file(..., overwrite_existing)` under `editor/` and `engine/core/`; the few raw
writes left (append-only logs, private temp files, export and import staging) are listed with a reason in
`tools/raw_writes_allowlist.txt`. A file watcher ignores `io::isTemporarySiblingName()` names
and subscribes to `io::setPublishListener` for its own writes (S05; nothing else may).
`io::ScopedFileLock` is the cross-process lock (shared or exclusive, blocking or not).

**Changing the format.** Scenes, `project.yaml` and `.material` files carry `formatVersion`
(current: scenes 3, the others 1) as their first key; prefabs keep `version` (current 5). Loaders run
`StreamMigrations::migrate` (`editor/StreamMigrations.h`) on the raw YAML before decoding, so
decoders only see the current shape; a migrated file is not marked modified and is written in
the current format by its next save.

- Adding an optional component or field: **no version bump** (defaults and preservation cover
  older and newer editors). A component field goes into the component's reflection table
  (`editor/reflect/components/`, `OmitIfDefault` from S08 on; docs/specs/S08-reflection.md R31):
  its key, aliases and enum names are known from it. A component with a codec of its own lists
  the keys its decoder reads in its table's `extraKeys`; other keys go into
  `editor/StreamSchema.cpp` (`knownNestedKeys` or `knownRootKeys`). `tools/check_stream_keys.py`
  and `StreamSchemaTest` fail otherwise.
- Renaming, moving or re-typing a key, or changing what a value means: **bump the kind's
  version** and add a migration (a pure `void(YAML::Node& root, MigrationLog&)` in
  `StreamMigrations.cpp`, appended to its table), with a fixture under
  `editor/tests/fixtures/migrations/` and a test.
- Enums read from YAML use `decodeEnum(node, key, StreamSchema::<table>())`, never a bare
  string compare: an unknown name is then reported and kept.

**Loading and saving documents.** Read with `DocumentFiles::read(path, kind)` (parse errors
with file:line:col, migrations, SHA-1) under a `ScopedLoadReport`; keep the report on the
document (`SceneProject::loadReport`, `Prefab::loadReport`, `Project::getProjectLoadReport()`).
Saves take a `DegradedSavePolicy` (default `Refuse`) and return `SaveResult::RefusedDegraded`
without writing when the report has dropped issues. Ask the user through
`EditorHost::confirmDegradedSave`, `resolveSceneRecovery` and `confirmOpenLockedProject`, never
through the generic alerts: the headless host answers them safely (refuse, keep, no).
Preservation (`editor/StreamPreserved.h`) works inside `Stream::encodeComponents` /
`decodeComponents` / `encodeEntity` / `decodeEntity`, so code that goes through them carries
unknown data for free; `Stream::encodeComponentsLive` encodes only the live values.

## Tests

`tools/build.sh unit-tests` runs `jasperengine-core-tests` (`engine/tests/core/io`: AtomicFile,
ScopedFileLock, OrphanSweep, UserSettings, AtomicFileBench) and `jasperengine-editor-tests`
(`editor/tests/data_safety`: Preservation, SaveGate, Migrations, StreamSchema, Compat,
SceneBackup, AppSettings, benchmarks). `tools/mcp/tests/test_data_safety.py` covers the lock,
crash recovery and degraded saves in a running editor; `test_save_completeness.py` the saves
that would leave a GameObject out (and the prefab refreshes, moves and imports that once did). `python3 tools/check_raw_writes.py`
(and `--self-test`) and `python3 tools/check_stream_keys.py` are the static checks.
