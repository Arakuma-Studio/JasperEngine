# Project Hub

The **Project Hub** is the editor's start screen (docs/specs/S27-editor-tools.md R15-R23): your
projects, new projects from templates, opening a folder, and importing a zip or a Git
repository.

## When it shows

- At start-up when there is no last project, or it is missing or fails to load.
- When Shift is held as the editor starts, or with `jasperengine-editor --hub`.
- Always, with Preferences > General > **Show Project Hub at Startup** = Always (default: When No
  Project).
- **File > Project Hub...** (Projects), **File > New Project...** (New Project) and **File > Open
  Project...** (Open), after the usual unsaved-changes prompt. Esc closes it and returns to the
  open project.

`jasperengine-editor --project <dir>`, `benchmark`, `record` and `mcp` never show it.

## Projects

Recent projects with their thumbnail (written from the Scene view when a project closes), name,
path and when they were last opened. Search filters; sort by Last Opened or Name; **Pin** keeps
a project at the top. Missing projects are greyed with **Locate...** and **Remove from List**.
Right-click: Show in Explorer/Finder, Remove from List, **Open Without Last Scene**. The list
keeps 30 projects (pinned ones are never dropped); File > Recent Projects shows the first 10.

## New Project

Pick a template, a **Project Name** (default "New Project") and a **Location** (the last one
used, else `~/JasperEngine Projects`). **Create** stays disabled while `<location>/<name>` exists
and is not empty, with the reason. The project is created off the main thread and opens on the
template's start scene. It is built next to the target (`<name>.importing`) and moved there whole:
a failure or a crash midway leaves `<location>/<name>` as it was, and a failure removes the rest
(a crash: see [Leftovers of a crash](#leftovers-of-a-crash)).

Built-in templates:

- **Empty**: one 3D scene `Main` with only a camera.
- **3D**: a lit scene: a Directional Light with shadows, the Daylight sky, a 20 x 20 m ground with
  the `materials/Ground` material, and a cube turned by `scripts/Rotator.lua`.
- **2D**: a sprite (`sprites/player.png`) moved by the arrow keys or WASD
  (`scripts/PlayerMove.lua`), on a 1280 x 720 Letterbox canvas.
- **UI**: an HTML/CSS main menu (`ui/main_menu.html` and `.css`) with Play and Quit buttons wired
  in Lua, on a 1920 x 1080 canvas.

**My Templates** are yours: **File > Save Project as Template...** saves the open project (without
`.jasperengine/`, `build/`, `Recordings/`, `.trash/` and `.git/`) with a name, a description and
a thumbnail of the Scene view into `<settings dir>/templates/<name>/`.

New projects get Git LFS settings for version control (version-control.md).

## Open

**Open Folder...** picks a project folder (one with a `project.yaml`).

## Import

- **From Zip...**: the archive is checked first (no absolute paths, `..`, links or devices; at
  most 8 GiB and 200,000 entries, and the free space), extracted next to the target and the
  project folder (the shallowest with a `project.yaml`, at most two levels down) moved into place.
  An archive without a project leaves nothing behind.
- **From Git URL...**: `https://`, `ssh://` or `git@host:path` URLs, cloned with your `git`
  (`--depth 1`); with Git LFS in the repository and `git lfs` installed, the LFS files are
  pulled. Credentials are your credential manager's or SSH keys' (no password prompt).

Both show the progress and the last lines of the log, and **Cancel** removes what was written.

## Leftovers of a crash

An import, a new project and **Save Project as Template...** build their copy in a staging folder
(`<name>.importing` beside the target, `<settings dir>/templates/.staging/` for a template) with an
owner marker beside it, `<name>.importing.lock`, whose file lock the editor holds while it copies.
The system frees the lock when the editor dies, however it dies. So:

- **An editor that was killed or crashed leaves nothing for you to find.** When an editor starts, and
  before each import or template copy, the staging folders whose owner died are removed, and the
  next import of the same project gets the plain name. Removing takes place off the main thread;
  one that fails (a file still open on Windows) is written to the Console and retried next time.
- **A second editor at work is never touched**: its lock is held. It may even import beside yours.
- **Folders with such a name but no marker are never removed by themselves**: an editor from before
  this feature made them, or they are your own, so they may hold your files. **Leftover Folders**
  on the Projects and Import pages lists them with **Show in Folder** and **Remove** (which asks
  first). So does a folder whose owner cannot be told, on a network share without file locks: an
  import there warns in the Console that its folder will not clean up by itself. A recent project
  with such a name is not listed.

The Hub only looks in the folders it knows: the parents of your recent projects, the last location
and the folders of copies still running (`<settings dir>/hub-staging/` notes those for as long as they
run), one level deep. An older leftover elsewhere shows when you import into its folder.

Agents: `import_project` and `create_project {template}` list leftovers in their result;
`clear_project_leftovers` (group `editor_tools`) sweeps, lists and removes them.
