# File watching and hot reload

The editor watches the open project's folder. When another program (an image editor, Blender,
VS Code, git, an agent's own file tools) creates, changes, deletes or renames a file, the editor
finds out within a fraction of a second and reloads what reads it. The scene keeps its state:
GameObjects, component values, the selection, the undo history and Play are never touched by a
reload. (Specification: [specs/S05-file-watcher-hot-reload.md](specs/S05-file-watcher-hot-reload.md).)

## What reloads

| File | What happens |
|---|---|
| Image (`.png`, `.jpg`, `.svg`, ...) | Decoded again on a worker with the texture's own settings (SVG scale, alpha border, filters), then swapped in. The old texture stays on screen until the new one is ready, so there is no flicker. Every copy of the texture (undo history and prefab caches included) shows the new pixels. |
| Model (`.gltf`, `.glb`, `.obj`) | Parsed again on a worker. When its node hierarchy is the one loaded, the meshes, skins, morphs and animations are rebuilt on the GameObjects that are there: node transforms, added components and submesh overrides are kept. A hierarchy that changed is not rebuilt (that would drop your edits on its GameObjects): a warning says to reimport it. |
| Material (`.material`) | Read again on a worker and applied to every submesh linked to the file. |
| Shader (`.vert`, `.frag`, `.glsl`) | Only the forks whose files or `#include`s hold the file are compiled again, on a worker. The old program stays bound until the new one compiled; a compile error keeps it and is reported with its file and line. |
| Audio (`.wav`, `.ogg`, `.mp3`, `.flac`) | A new sample replaces the old one: a sound that is playing finishes, the next play uses the new file. |
| Font (`.ttf`, `.otf`) | Loaded again; texts using it rebuild their glyph atlas. |
| Lua script | Script properties of the GameObjects using it are read again, its syntax is checked, the lua-language-server stubs are written. While a scene plays this waits until Play stops. |
| C++ script | Its properties are read again; the scripts need a build, as before. |
| HTML UI (`.html`, `.css` and what documents read) | The documents reading it reload (`[HTML UI] Reloaded ...` in the Console). |
| Prefab not open | Its instances follow the file (unsaved edits in the editor win). |
| Open scene, open prefab, `project.yaml` | **Never reloaded on its own.** The Scene view shows a bar: *"Level1 changed on disk. [Reload] [Keep Mine]"*. Reload reads the file (asking first when there are unsaved changes; it is not an undo step, and the scene's undo history is cleared, as when it closes). Keep Mine keeps the editor's copy, written over the file on the next save. |
| Code editor buffer | Without unsaved edits it reloads; with unsaved edits the Files Changed prompt asks. |

Files added, deleted or renamed outside the editor update the Project window (only the folder
that changed is listed again, keeping the selection, the scroll and a rename in progress) and
the asset database: an asset renamed or moved by git or the file manager keeps its GUID (with
its `.meta`, or without it when the file is the only candidate for the meta left behind), so
every reference to it, closed scenes included, still finds it. The database looks at the disk
only when the watcher reports a change, or on **Assets > Refresh**.

The editor's own saves are not reported back as changes: saving a scene does not reload it,
saving a material does not reload the submesh it came from (other submeshes linked to the same
file follow it).

## The Console

One line per reload, or per batch of files that changed together:

    [Hot Reload] Reloaded assets/hero.png (texture, 34 ms)
    [Hot Reload] Reloaded 37 files: 12 textures, 3 materials, 1 shader

Clicking a line selects the asset in the Project window. A file that does not load keeps the
previous version and gives an error, `[Hot Reload] assets/hero.png: <reason> (kept the previous
version)`; the editor tries it once more half a second later (a file still being written).
Watcher problems (a fallback to polling, an overflow that made it rescan) are warnings, shown
once.

## Preferences and status bar

**Edit > Preferences > Asset Pipeline**:

- **Auto Refresh** (on): reload when files change. Off, the changes wait; the status bar counts
  them (*Auto Refresh off: 3 changes*) and **Assets > Refresh** (Ctrl+R) applies them.
- **Watcher**: *Auto* uses the system's file notifications (inotify, ReadDirectoryChangesW,
  FSEvents) and checks with a probe file at start that they arrive; where they do not (network
  drives, WSL `/mnt/c`, container mounts) it polls. *Native* never probes, *Polling* always polls.
- **Polling Interval** (Polling only, 0.25-10 s, default 1 s). A file deleted while polling is
  reported one interval later, so a save that deletes and writes the file again is one change.
- **Log Reloads** (on): the Console lines above (failures are always shown).

The status bar at the bottom shows the watcher: *Watching (inotify)*, *Polling 1 s* or
*Auto Refresh off: N changes*; its tooltip has the counts, a click opens the preferences.

On Linux each project folder takes an inotify watch. When the system limit is reached the
editor polls that project and says so once; raise `fs.inotify.max_user_watches` to watch big
projects natively.

The command line choice wins over the preference for one run:
`jasperengine-editor --file-watcher=auto|native|polling|off` (also `JASPER_FILE_WATCHER`; the
flag wins over the variable). *off* watches nothing: Assets > Refresh looks at the whole project.

## Agents (MCP)

Edits made with the agent's own file tools are reloaded before its next call, often already when
it saves. That call's result carries `external_changes`:

    {changed, added?, removed?, source: "watcher" | "scan",
     reloaded: [{path, kind, ms}], pending: [path],
     problems: [{path, line, message}], ...}

`source` says whether the change came from the watcher's journal or from the stat scan used when
the watcher is off or degraded. A file written once a call's result has arrived is reported by the
next call, however soon and whatever its timestamp; a file another program writes while a tool call
runs, stamped during that call, is taken for the tool's own. The call waits up to 10 s for the reloads it started; what still
reloads is in `pending`. A reload that failed is a problem (the previous version is kept). A
change the watcher already reloaded before the call is reported once and reloaded once, with
the problems that reload found; the HTML UI documents it reloaded are in `ui_reloaded` when the
call's flush delivered the change, else counted in `ui_current`.

`sync_files` takes `wait` (default true: return after its reloads, at most 30 s),
`rebuild_models` (rebuild a model whose hierarchy changed) and `auto_refresh` (the preference).
`editor_status` has `file_watcher` {backend, state, auto_refresh, roots, watches, events,
coalesced, echoes_suppressed, overflows, pending, reloading, reloads, last_reload} and
`frame_ms` {median, p99, max, frames} over the frames since the previous `editor_status`.

## For code that writes files

Write project files through `io::writeFileAtomic` / `io::publishFile`
([data-safety.md](data-safety.md)): every publish tells the watcher's self-write ledger, so the
write is delivered as the editor's own (origin Editor) and its echo is dropped. A writer that
cannot use them wraps its write in `ScopedExpectedWrite` (`engine/core/io/watch/FileWatchService.h`):
construct it before touching the file, call `reported()` (or `reportedRemoved` /
`reportedMoved`) once the bytes are on disk. Renames go through `FileUtils::renameWithRetry`,
which reports them.

A cache that depends on a file (thumbnails, glyph atlases, cooked textures) follows its reloads
with a `ReloadInvalidator` (`editor/reload/HotReloadService.h`): Reloaded, Dropped and Content
events by path and kind.

A reload is recorded with what the file was before it was read (a save that lands while it
reads keeps its own change) and the watcher's journal sequence; the watcher's delivery and a
call's pick-up of the same change reload once. A stamp taken within 100 ms of the file's mtime
is racy (two saves in one timestamp tick keep the time and the size): it compares the bytes
before it says "unchanged".

Inside `engine/core/io/watch/`: `FileWatcher` (one backend thread per root), the pure
`FileChangeCoalescer` (quiet window 150 ms, settle by two stats 50 ms apart, removals held
200 ms so a safe-save is one change, temp files never delivered), `ProjectFileIndex`,
`ExpectedWriteLedger`, `FileWatchService` (subscriptions, the journal of 65,536 changes, the
main-thread pump within 1 ms a frame, the `flush()` barrier) and `SharedFileReader` (reads that
let another program replace the file on Windows). In games nothing is watched: Android, iOS and
the web have no backend, and players never start the service.

## Linked folders

A folder linked in from out of the project is watched like one of its folders (S68): each such
folder gets a backend of its own on its real path, whose changes are reported by the path under the
link. A folder two links lead to is watched once. A link inside the project adds nothing to watch: its
target is watched by its own name. A link made or removed while watching starts or stops the watch.
