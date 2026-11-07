# Build toolchain: generated projects and build scripts that work for every project, on every host (design)

Date 2026-11-04. Branch `dc/build-toolchain`. Program: Doriax catch-up, ledger `docs/doriax-catchup/LEDGER.md`
(feature 4). Sources: dossier `dc-work/dossiers/build-toolchain.json` (11 Doriax commits), Doriax read-only clone at
`3fb49fbba`. Every Jasper `path:line` below was re-read on `main` (`b4f7816ea`); "inferred" marks what comes from reading
code, not from a run.

## 1. Goals

1. Command-line and agent-driven editor processes never change a person's editor settings: no recent-project
   record, no "last project", no cleared "last project" (Doriax `fe6a776a5`, widened to the whole class).
2. A window title of any characters in any language builds and shows exactly: `;`, `#`, `"`, `\`, `$`, `%`, `&`,
   `^`, `<`, `>`, `,`, emoji and CJK alike, through both generated CMake files (Doriax `b77dab76c`, widened).
3. Exported source projects can be built from a terminal as documented: `./gradlew` and `./jasperengine.py` are
   executable, whether the editor came from a build tree, an install, or a re-export over an older export
   (Doriax `2b8686da1`).
4. The lines a user adds to the editor's `.gitignore` and `.gitattributes` survive every refresh, including a
   Git LFS / Lockable Scenes toggle and files written by older editors (Doriax `33347cdd5`, without its flaws).
5. C++ scripts can call any public Box2D and Jolt function, on Windows (MSVC and MinGW), Linux and macOS
   (Doriax `9e07bcbf6`, `b51a26bb1`, widened: unreferenced functions fail on every OS today, inferred).
6. Visual Studio generator builds of the engine and the editor compile in parallel within a target, under the
   same job cap that drives `cmake --build --parallel` (Doriax `8fc8da2f9`).
7. The generated project `CMakeLists.txt` always compiles scripts with the ABI macros of the engine library it
   links, whatever `JASPERENGINE_LIB_DIR` says, and refuses a library that cannot match (Doriax `2cb687cb5`).
8. `jasperengine.py --platform web` works on Windows with Emscripten 6 and with Visual Studio's own Ninja, and the
   editor's Web build finds that Ninja too (Doriax `0c53eca9d`, `580f6cd0e`).
9. Android source exports select OpenGL ES 3 only, from one platform-to-backend table shared by the Build Settings
   window and MCP; the engine's Android build refuses other backends with a message (Doriax `2658a9143`).

## 2. Non-goals

- Android Vulkan. No code exists for it (engine/CMakeLists.txt:431-437 defines only `SOKOL_GLES3`; nothing in
  `engine/platform/android` creates a Vulkan surface). The single backend table (6.9) makes adding it later one line.
- Quoting of machine paths written into generated CMake (`LocalPaths.cmake`, `toProjectRootCMakePath` outside the
  project, editor/Generator.cpp:487-501, :1478): a path holding `"` or `$` would still break them (inferred). Left to
  `unicode-text` (paths) and `export-pipeline` (safe paths); recorded here so the lead can assign it.
- The Project Hub's own clearing of the last project when it opens over the editor's temporary project
  (editor/App.cpp:3256-3260 -> editor/Project.cpp:6350) in an interactive session: S27's decision, unchanged.
- MSBuild's MultiToolTask scheduler (`UseMultiToolTask`, `EnforceProcessCountAcrossBuilds`): not taken; `/MP<n>`
  is the documented, testable switch (5.6).
- A source export made on Windows and copied to Linux or macOS: NTFS has no exec bit; docs say to run
  `bash ./gradlew` / `python3 jasperengine.py` there or `chmod +x`.

## 3. Users and stories

- A person runs `jasperengine-editor export --project Game --out /tmp/x` from a terminal or CI while their editor is
  closed. The next editor launch opens the project they were working on, not `Game`, and File > Recent Projects is
  unchanged.
- An agent runs `jasperengine-editor mcp --project <scratch copy> --headless` fifty times a day. The person's hub
  never lists a scratch copy, and their last project survives a failed `--project` launch.
- A person titles their game `Pirates; Revenge #2 "Gold" ゲーム` in Project Settings > Player > Window Title. The
  Compile build succeeds and the window shows exactly that.
- A person unzips the editor release, exports the source project for Android and runs
  `cd workspaces/androidstudio && ./gradlew assembleRelease`; it runs (today: "Permission denied").
- A team adds `secrets.env` and `docs/** linguist-documentation` to the generated files, then turns Git LFS on. Both
  lines are still there, after the editor's block, and the LFS lines appear in the block.
- A C++ script on Windows calls `b2World_Explode(world, &def)` or `JPH::Allocate(64)`. It links in Play and in a
  standalone build.
- A Windows user with only Visual Studio and Emscripten 6 runs `python jasperengine.py --platform web`; it
  configures with Visual Studio's Ninja and builds.
- An agent exports source for `platform: android` and `get_build_info.source.backends` is `["opengles"]`; no Vulkan
  shaders are compiled for nothing.

## 4. Findings

### 4.1 CLI and agent processes write the person's settings (Doriax `fe6a776a5`; confirmed)

- `jasperengine-editor export` loads with `updateLastOpened=false` (editor/cli/CommandLine.cpp:664-665), but the
  export saves scenes (editor/Exporter.cpp:2281 `saveSceneToPath`), which saves the project
  (editor/Project.cpp:7338 -> `saveProjectToPath`), which records it unconditionally for a non-temporary project
  (editor/Project.cpp:6765-6767 `addToRecentProjects` + `setLastProjectPath`). `setLastProjectPath` saves the
  settings file at once (editor/AppSettings.cpp:999-1005), merging into the shared file (:941-990). With the hub
  preference off by default (AppSettings.cpp:161) the next launch reopens the exported project (editor/App.cpp:3243-3245).
- The same chain runs in every CLI subcommand that saves (ShippingCommands.cpp:258-270, CommandLine.cpp:1744-1760,
  :2054-2065, :3021-3023), inferred.
- Widened class (inferred, not in Doriax): `jasperengine-editor mcp --project <dir>` opens through
  `Project::openProjectNow` -> `loadProject(projectDir)` with the default `updateLastOpened=true`
  (editor/Project.cpp:5275, editor/App.cpp:3522), and a stdio MCP editor uses the user's settings file
  (editor/App.cpp:5823 `AppSettings::initialize(stdio)`, AppSettings.cpp:207-210). Every scratch copy an agent opens
  becomes the person's last project. A failed `--project` launch falls back to `createTempProject`
  (editor/App.cpp:3219), which runs `AppSettings::setLastProjectPath({})` (editor/Project.cpp:6350): the person's last
  project is erased. `mcp --create` (App.cpp:3534 -> Project.cpp:5251 `createProjectInDirectory`) does both.
- `call --project` already runs its editor in a scratch working directory with its own settings
  (tools/mcp/tests/test_call_discovery.py:150-182 asserts it): not affected.

### 4.2 `;` and `#` in the window title (Doriax `b77dab76c`; confirmed)

- editor/Exporter.cpp:3551-3565 escapes only `\`, `"` (C layer) and `\`, `"`, `$` (CMake layer), written at :3576 as
  `set(JASPERENGINE_WINDOW_TITLE "...")`; engine/CMakeLists.txt:189 appends `JASPERENGINE_WINDOW_TITLE="..."` to a
  list expanded unquoted at :2352 `target_compile_definitions`, so `;` splits the definition. The second copy,
  editor/Generator.cpp:503-525 (`add_definitions`), has the same two loops.
- CMake's own documentation of `COMPILE_DEFINITIONS` lists values that are broken in some generators: `#` (almost
  everywhere), `;`, `,` (VS IDE), `%` (NMake), `&` `|` (MinGW), `^` `<` `>` `\"` (Make tools on Windows). Doriax
  escapes only `;` and `#`.
- Non-ASCII titles reach MSVC as command-line bytes whose meaning depends on the active code page and `/utf-8`
  (inferred; `unicode-text` changes the code page, but an escape that carries bytes needs no assumption).
- Nothing restricts the title: editor/Project.cpp:1794-1812 replaces control characters only; SettingsRows.cpp:876
  accepts any string. `#` silently gives the title "JasperEngine" (engine/platform/common/PlatformDefaults.h:27-28)
  when CMake drops the definition (Doriax's finding; not run here).

### 4.3 Executable bits lost (Doriax `2b8686da1`; confirmed)

- Git has `engine/jasperengine.py` and `engine/workspaces/androidstudio/gradlew` as `100755`.
- CMakeLists.txt:1707-1709 `install(DIRECTORY ${CMAKE_SOURCE_DIR}/engine ...)` has no `USE_SOURCE_PERMISSIONS`,
  whose documented default drops exec bits: the installed editor's engine tree (`editor-install/bin/engine`) has
  them off (inferred from CMake docs; Jasper's CI confirms the effect: tools/ci/android_size.sh:116-117 "the export
  does not keep gradlew's executable bit: run through bash", :124 `bash ./gradlew`).
- The export copies with `fs::copy_file` (editor/Exporter.cpp:3437 for `jasperengine.py`; the tree copy of
  `workspaces/`, then `copyTreeIfChanged` :3326-3375, now `exportfiles::mirrorTree` in editor/export/ExportFiles.cpp),
  which keeps the source's mode and adds nothing. The tree copy skipped a file whose bytes are equal (:3354-3358), so a
  re-export never repaired the mode of an older export.

### 4.4 Lines added to `.gitignore` / `.gitattributes` are lost (Doriax `33347cdd5`; confirmed)

- editor/Project.cpp:6627-6660 `writeVersionControlMetadata` keeps a file only if its first line lacks the marker
  `# Generated by JasperEngine Editor.` (:6544); otherwise it writes the generated text whole. Called on every
  project save (:6484-6485) and on an LFS / Lockable Scenes change (:2679).
- Doriax's fix (a multiset subtraction of the current text) is wrong here: `.gitattributes` depends on two settings
  (`versionControlAttributes(lfs, lockScenes)`, :6600-6625), so after a toggle the previous variant's lines count
  as user lines and survive beside the new ones; template changes between editor versions do the same; and user
  lines are reordered to the end with blank-line handling that drops intended spacing.

### 4.5 Box2D and Jolt not exported from the editor's engine library (Doriax `9e07bcbf6`, `b51a26bb1`; confirmed, uncertain on Windows)

- engine/CMakeLists.txt:2222-2251 (`JASPERENGINE_SHARED`) defines nothing for Box2D or Jolt. Box2D's
  `BOX2D_EXPORT` is empty unless `box2d_EXPORTS` (engine/libs/box2d/include/box2d/base.h:11-23); Jolt's `JPH_EXPORT`
  is empty unless `JPH_SHARED_LIBRARY` (engine/libs/joltphysics/Jolt/Core/Core.h:210-237), which only the vendored
  CMake sets under `BUILD_SHARED_LIBS` (engine/libs/joltphysics/CMakeLists.txt:502,524). On Windows the DLL exports
  only `dllexport` symbols, so no `b2*` and no `JPH::*` (Doriax's finding; Windows not built here).
- Reachable from scripts: the script include path has both libraries (editor/Generator.cpp:1817), and the public
  API hands out their handles (engine/core/object/physics/Rigidbody2D.h:41 `b2BodyId getBox2DBody()`, Rigidbody.h:59-61
  `JPH::Body& getJoltBody()`, PhysicsSystem.h:758 `b2WorldId getWorld2D()`).
- Widened (inferred, not in Doriax): Box2D and Jolt are static archives linked into the shared engine; a linker
  pulls only the archive members the engine references. A script calling a Box2D or Jolt function the engine never
  calls has no definition in the library on any OS, and the plugin link fails because it is linked with
  `-Wl,-z,defs,--no-undefined` on Linux and ld64's default `-undefined error` on macOS (Generator.cpp:1384-1390).
  Doriax's fix does not cover this.
- MSVC links Jolt's variables (`JPH::Allocate`, Memory.h:21) only through `dllimport`, so scripts and the editor need
  `JPH_SHARED_LIBRARY` when they compile. editor/Generator.cpp:452-485 `joltCompileDefinitions()` forwards it only if
  the editor itself saw it, which never happens today.

### 4.6 No `/MP` for Visual Studio builds (Doriax `8fc8da2f9`; confirmed)

- engine/CMakeLists.txt:917-918 adds `/bigobj` only; the root CMakeLists.txt adds nothing. Only the generated plugin
  gets `/MP${JASPERENGINE_PARALLEL_BUILD_JOBS}` (editor/Generator.cpp:1372-1381). `cmake --build --parallel N`
  (Exporter.cpp:1638-1643) is MSBuild's `/m:N`, which parallelizes projects, not files.
- Generator.cpp:3354-3362 requires one value to drive both `/MP` and `--parallel`; the export's configure
  (Exporter.cpp:1557-1586) passes no `JASPERENGINE_PARALLEL_BUILD_JOBS`.

### 4.7 ABI macros missing with a custom `JASPERENGINE_LIB_DIR` (Doriax `2cb687cb5`; confirmed, worth "could")

- `JASPERENGINE_SHARED` and `JASPERENGINE_EDITOR` are added for plugins (editor/Generator.cpp:1271-1279) and inside
  the default-library-directory branch only (:1335-1341). A standalone build of the generated project with
  `-DJASPERENGINE_LIB_DIR=<dir>` compiles `HybridArray` (engine/core/util/HybridArray.h:9,17) with the non-editor
  layout against the editor's library: memory corruption at run time.

### 4.8 `jasperengine.py` web builds on Windows (Doriax `0c53eca9d`, `580f6cd0e`)

- engine/jasperengine.py:34-45 `find_emcmake` looks only for `emcmake.bat` under `EMSDK`/`EMSCRIPTEN_ROOT`/
  `EMSCRIPTEN` on Windows; Emscripten 6 ships `emcmake.exe` (Doriax's finding). The script then wraps CMake in
  emcmake (:155-160), whose `.bat` form runs through cmd.exe, the reason the editor dropped emcmake
  (Exporter.cpp:1559-1567).
- A Windows web build needs a generator other than Visual Studio. The script picks Ninja only when it is on PATH
  and only for non-Windows platforms (:146-150, `platform != "windows"` is true for web, so web on Windows gets Ninja
  from PATH or nothing, and then emcmake's own choice). Visual Studio installs its own Ninja
  (`Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`) off PATH.
- The editor: `Exporter::emscriptenWindowsGeneratorArgs` (Exporter.cpp:6286-6309) searches the SDK environment's PATH
  for `mingw32-make`, then `ninja`, and otherwise fails; Visual Studio's Ninja is never found.
  `isEmscriptenDir` (:6128-6132) accepts `emcc`, `emcc.bat`, `emcc.py`, not `emcc.exe` (whether Emscripten 6 keeps
  `emcc.py` on Windows was not checked: accept both).
- `580f6cd0e` (skip in the dossier): Jasper never forces `-G` onto a configured directory (jasperengine.py:148-149);
  its rule is kept in 6.8 for the new Windows web branch.

### 4.9 Android preset selects Vulkan (Doriax `2658a9143`)

- editor/window/dialog/ExportWindow.cpp:253, :458, :879 (three copies of one table) and
  editor/mcp/tools/BuildTools.cpp:176 (a fourth) give Android `{GLES3, Vulkan}`; Exporter.cpp:5838-5880 compiles every
  shader for each selected backend. The engine's Android block defines only `SOKOL_GLES3` and accepts any other
  `GRAPHIC_BACKEND` silently (engine/CMakeLists.txt:431-437), so `-PjasperBackend=vulkan`
  (engine/workspaces/androidstudio/app/build.gradle:16-17,67-68; tools/players/android_player.sh:14,26) produces a
  library with no graphics backend (inferred). docs/shipping.md:49 already says Android is gles3 only.

## 5. Product decisions

1. **Only a person's interactive editor writes recent projects and the last project.** CLI subcommands, stdio MCP
   editors (`jasperengine-editor mcp`), headless editors (`--headless`) and benchmarks never add, touch or clear
   them, on load, save, create or the temporary-project fallback. Reason: those processes are tools, often agents
   or CI on scratch copies, and a person's list must show the projects they opened. An interactive
   editor with MCP over HTTP still records (the person sees that project; `open_project` there is like File > Open).
2. **Titles are carried as bytes, not text.** One helper turns the title into a C string literal body in which
   every byte outside `[A-Za-z0-9 _.-]` is a three-digit octal escape (`\073` for `;`); the CMake layer then
   escapes only the backslashes. Reason: removes every character CMake documents as broken in some generator, and
   makes UTF-8 titles independent of the compiler's source and execution charsets. The generated files stay
   readable for plain titles.
3. **Exec bits are fixed at both places they are lost.** The install keeps source permissions; the export makes
   `jasperengine.py` and `gradlew` executable (owner, group, others: `+x` added, never removed) and
   the tree copy (`exportfiles::mirrorTree`) copies a file's permissions when only they differ. Reason: an export from an older install,
   or over an older export, must still be right; the CI workaround is removed so CI checks it.
4. **The generated version-control files get a managed block.** The editor owns the lines from the first-line
   marker down to an end marker; everything after the end marker is the user's, kept byte for byte. A file in the
   old format (marker, no end marker) is migrated once: its lines that match, whitespace-normalised, any line the
   editor ever generated (every `lfs` x `lockScenes` variant, the current and every earlier template) are dropped,
   the rest move below the end marker in their order. To override a generated rule a user adds a later line (Git:
   the last matching line wins, `!pattern` re-includes); to own the whole file, turn off Project Settings > Editor >
   Version Control Files or delete the first line (unchanged). Reason: correct under setting toggles and template
   changes, keeps user order, and matches Git's precedence so user lines can override.
5. **The editor's engine library exports all of Box2D's and Jolt's public API on every desktop OS.** Both archives
   are linked whole into the shared engine, compiled with their export macros, and the editor, its tests and every
   generated project compile with the matching import macros. Reason: scripts are told they can use the libraries
   (include path, handles in the API); "works on Linux if the engine happens to call the same function" is not a
   contract. Shipped games are unaffected (static engine).
6. **`/MP<jobs>` for Visual Studio generators, engine and editor.** Only when `CMAKE_GENERATOR` matches
   `Visual Studio`; `<jobs>` is `JASPERENGINE_PARALLEL_BUILD_JOBS` when set (> 0), else bare `/MP`. Exports pass
   their effective job count at configure, as Play builds already do. Ninja builds are unchanged (Ninja parallelises
   files itself; `/MP` there would oversubscribe).
7. **The generated project always compiles with the editor engine's ABI macros** (`JASPERENGINE_SHARED`,
   `JASPERENGINE_EDITOR`, the Jolt and Box2D import macros), plugin or standalone, default or custom library
   directory. The generated file only ever links an editor engine library; configure fails with a clear message when
   `JASPERENGINE_LIB_DIR` holds a static `jasperengine` library (`.a`, or a `.lib` without `jasperengine.dll` beside
   it on Windows). Reason: a mismatch is silent memory corruption; a configure error is not.
8. **`jasperengine.py` drops emcmake** and configures exactly as the editor does: CMake with Emscripten's toolchain
   file, `CMAKE_CROSSCOMPILING_EMULATOR` set to the SDK's node, the SDK environment. On Windows the generator is the
   configured directory's, else Ninja on PATH, else Visual Studio's Ninja (with `CMAKE_MAKE_PROGRAM`), else
   `mingw32-make`, else an error naming what to install. The editor's Web build gets the same Visual Studio Ninja
   fallback. The script gains `--dry-run` (print the commands, run nothing) for users, agents and tests. Reason: no
   launcher-name chase (`.bat`, `.exe`, `.py`) and one behaviour for both entry points.
9. **Android is OpenGL ES 3 only, in one table.** `editor/export/PlatformBackends.{h,cpp}` holds the source-export
   presets and the host's desktop backends; ExportWindow (three sites) and BuildTools (two functions) use it. The
   engine's Android block, `build.gradle` and `android_player.sh` refuse any other backend with a message. A project
   whose saved source-export backends include Vulkan keeps them (another platform may need them; the user's choice).

Rejected: none of the dossier's items. `580f6cd0e` (worth skip) is folded into decision 8 as the "configured
directory's generator" rule.

## 6. Design

### 6.1 Recent projects (editor/EditorHost, editor/App, editor/Project)

- `EditorHost` (editor/EditorHost.h:54+): add
  `virtual bool recordsRecentProjects() const;` with comment "false for tools: the command line, agents' editors";
  default (editor/EditorHost.cpp) returns `false`. The CLI's `EditorHostOverrideGuard hostOverride(nullptr)`
  (CommandLine.cpp:631, ShippingCommands, `runChecks` :2968) and unit tests get the default.
- `App::recordsRecentProjects()` (editor/App.h/.cpp, next to `shouldSyncEngineApi`):
  `return !mcp::mcpLaunchOptions().stdio && !mcp::mcpLaunchOptions().headless && !benchmarkEnabled;`
- `Project` (editor/Project.cpp):
  - `saveProjectToPath` :6765: `if (!isTempProject() && editor::getEditorHost().recordsRecentProjects())`.
  - `loadProject` :6902: `if (updateLastOpened && !isTempProject() && editor::getEditorHost().recordsRecentProjects())`.
  - `createProjectInDirectory` :6350: clear the last project only when the host records.
- No other writer exists (`addToRecentProjects` callers: Project.cpp:6766,6903, App.cpp:3451 hub creation,
  interactive only). `AppSettings` is unchanged; its merge already leaves untouched keys alone.

### 6.2 Window title (new editor/util/CMakeText.{h,cpp})

```cpp
namespace jasperengine::editor::cmaketext {
    // The body of a C string literal holding `value`'s bytes: [A-Za-z0-9 _.-] as is, every other
    // byte as a three-digit octal escape ("\073"). Safe in every generator's command line.
    std::string cStringLiteralBody(std::string_view value);
    // `value` inside a CMake quoted argument: \ " $ escaped (no ${} or $ENV{} expansion).
    std::string cmakeQuoted(std::string_view value);
    // Both: what set(VAR "...") or add_definitions("-DX=\"...\"") needs for a C string macro.
    std::string cStringDefinitionValue(std::string_view value);   // cmakeQuoted(cStringLiteralBody(v))
}
```

- Exporter.cpp:3551-3576 and Generator.cpp:503-525 use `cStringDefinitionValue(window.title)`; their two loops are
  deleted, and the comments say the title is carried as escaped bytes.
- Three digits always (so a following digit cannot extend the escape). The C layer produces no `"`, so no `\"` ever
  reaches a command line.
- `Project::getWindowSettings` (:1794-1812) keeps replacing control characters (the window title of a running game
  stays one line).

### 6.3 Executable bits

- CMakeLists.txt:1707: `install(DIRECTORY ${CMAKE_SOURCE_DIR}/engine DESTINATION bin USE_SOURCE_PERMISSIONS ...)`.
  `CopyEditorEngine.cmake:109` already keeps modes for the build-tree copy.
- `Exporter::copyEngine` (Exporter.cpp:3436-3445), after `jasperengine.py` and `workspaces/` are copied, on non-Windows
  hosts: `ensureExecutable({targetDir/"jasperengine.py", targetDir/"workspaces/androidstudio/gradlew"})`, a new
  private static `Exporter::ensureExecutable(const std::vector<fs::path>&, std::string& error)` that adds
  `owner_exec | group_exec | others_exec` (`fs::perm_options::add`). A failure fails the export with "Could not make
  <file> executable: <reason>" (an export a terminal cannot build is a failed export; consistent with
  `export-pipeline`'s "failed copies fail the export").
- `exportfiles::mirrorTree` (editor/export/ExportFiles.cpp; `copyTreeIfChanged` :3354-3358 when this was written):
  when sizes and bytes are equal but
  `fs::status(entry).permissions() != fs::status(target).permissions()`, set the target's permissions
  (`fs::perm_options::replace`) instead of copying; no mtime change (the build stays incremental). Windows: the
  comparison is skipped (`#ifndef _WIN32`), NTFS modes are synthetic.
- `tools/raw_writes_allowlist.txt:45` names the `fs::copy_file(sdkRoot / "jasperengine.py"` line: keep the entry
  matching if the line changes.
- tools/ci/android_size.sh:114-124: drop "run through bash" and call `./gradlew`.

### 6.4 Version-control files (editor/Project.cpp, new editor/util/VersionControlFiles.{h,cpp})

The text logic moves out of Project.cpp into a pure, testable unit:

```cpp
namespace jasperengine::editor::vcsfiles {
    extern const char* const startMarker;   // "# Generated by JasperEngine Editor." (unchanged, first line)
    extern const char* const endMarker;     // "# End of the JasperEngine Editor's lines: your own lines go below and are kept."
    std::string ignoreBlock();                                   // today's versionControlIgnore, without the end marker
    std::string attributesBlock(bool lfs, bool lockScenes);      // today's versionControlAttributes
    // Every line any editor version generated for this file (all lfs/lockScenes variants, legacy templates),
    // whitespace-normalised: the subtraction set of an old-format file.
    const std::set<std::string>& knownGeneratedLines(FileKind kind);
    enum class Refresh { Write, LeaveAlone };
    // existing: the file's text, or nullopt when it does not exist
    Refresh refresh(const std::optional<std::string>& existing, const std::string& block, FileKind kind, std::string& out);
}
```

`refresh` rules:

1. No file: `out = block + endMarker + "\n"`.
2. First line (after an optional UTF-8 BOM, `\r` stripped) does not start with `startMarker`: `LeaveAlone`.
3. An `endMarker` line exists: `out = block + endMarker + "\n" + <bytes after the end-marker line, verbatim>`.
4. No end marker (written by an older editor): each later line is normalised (trim, internal runs of blanks to one
   space, `\r` stripped); it is a user line unless its normalised form is in `knownGeneratedLines(kind)` or is a
   comment line that equals a generated comment. Blank lines between user lines are kept; leading and trailing blank
   lines dropped. `out = block + endMarker + "\n" + userLines`.
- `knownGeneratedLines` is built from the current blocks of all four `lfs` x `lockScenes` combinations plus a
  `legacy` table: every line of every earlier template, collected by the implementer from
  `git log -p -- editor/Project.cpp` (strings `versionControlIgnore`/`versionControlAttributes`) back to the fork
  point, including Doriax's constant attributes text at `doriax-base`.
- `Project::writeVersionControlMetadata` (:6627-6660) reads each file (binary), calls `refresh`, and writes with
  `FileUtils::writeIfChanged` under the existing `safewrite::EditorInternal` scope; an unchanged result writes nothing.
- The Project Settings row text (ProjectSettingsWindow.cpp:2743-2745) adds: "Lines you add below the editor's block
  are kept."

### 6.5 Box2D and Jolt from C++ scripts

engine/CMakeLists.txt, after the `JASPERENGINE_SHARED` block (:2222-2251), only when `JASPERENGINE_SHARED`:

```cmake
# C++ scripts link only the engine library: it carries and exports all of Box2D and Jolt
if(JASPERENGINE_PHYSICS_2D)
    target_compile_definitions(box2d PRIVATE box2d_EXPORTS)
    target_compile_definitions(jasperengine INTERFACE BOX2D_DLL)        # dllimport on MSVC; ignored elsewhere
    if(MINGW)
        target_compile_options(box2d PRIVATE "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/platform/win/Box2DExport.h\"")
    endif()
    jasperengine_link_whole_archive(jasperengine box2d)
endif()
if(JASPERENGINE_PHYSICS_3D)
    target_compile_definitions(joltphysics PRIVATE JPH_SHARED_LIBRARY JPH_BUILD_SHARED_LIBRARY)
    target_compile_definitions(jasperengine INTERFACE JPH_SHARED_LIBRARY)
    if(MINGW)
        target_compile_options(joltphysics PRIVATE "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/platform/win/JoltExport.h\"")
    endif()
    jasperengine_link_whole_archive(jasperengine joltphysics)
endif()
```

- `jasperengine_link_whole_archive(target lib)` (new, engine/cmake/JasperLinker.cmake; CMake 3.20 has no
  `LINK_LIBRARY:WHOLE_ARCHIVE`): MSVC `target_link_options(<t> PRIVATE "/WHOLEARCHIVE:$<TARGET_FILE:<lib>>")`;
  Apple `LINKER:-force_load,$<TARGET_FILE:<lib>>`; GNU/lld/MinGW
  `LINKER:--whole-archive $<TARGET_FILE:<lib>> LINKER:--no-whole-archive` placed so the archive is not linked twice
  (keep the normal `target_link_libraries` for ordering; duplicate definitions do not arise because whole-archive
  members satisfy the later references). The implementer checks the link line on Linux (`VERBOSE=1`).
- The engine's own sources do not get the `INTERFACE` macros (they are in the same library); the editor objects,
  `jasperengine-editor-tests` and anything linking `jasperengine` do. Jolt's only other use of
  `JPH_SHARED_LIBRARY` is the export macro (Core.h:210-237; the implementer greps `Jolt/` to confirm no layout
  change).
- New engine/platform/win/Box2DExport.h and JoltExport.h: from Doriax `9e07bcbf6` / `b51a26bb1` (same copyright
  header as ours; the fork's NOTICE.md entry covers Doriax code).
- editor/Generator.cpp: `joltCompileDefinitions()` (:452-485) now sees `JPH_SHARED_LIBRARY` through the editor's
  compile; add `#ifdef BOX2D_DLL definitions += " BOX2D_DLL"` beside it (rename the function
  `physicsCompileDefinitions()`; its comment says both libraries are imported from the engine library). The
  generated line at :1266 is unchanged in shape.
- Cost: the editor's engine library grows by the Box2D and Jolt members it did not use (measure and record in the
  commit; expected a few hundred KB). Shipped games: none (static engine, no `JASPERENGINE_SHARED`).

### 6.6 `/MP` (engine/cmake/JasperEngineToolchain.cmake, CMakeLists.txt, engine/CMakeLists.txt, Exporter)

- New `jasperengine_msvc_parallel_compile()` in JasperEngineToolchain.cmake: returns unless `MSVC` and
  `CMAKE_GENERATOR MATCHES "^Visual Studio"`; guarded by a global property so it applies once per configure;
  `add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:/MP${jobs}>)` where `jobs` is
  `JASPERENGINE_PARALLEL_BUILD_JOBS` when defined and > 0, else empty (bare `/MP`).
- Called in the root CMakeLists.txt before `add_subdirectory(${JASPERENGINE_ROOT})` (:556, so the engine and the
  editor get it) and in engine/CMakeLists.txt next to `/bigobj` (:918, for standalone and exported builds).
  engine/CMakeLists.txt declares `JASPERENGINE_PARALLEL_BUILD_JOBS` as a cache STRING (empty default), as the
  generated project does (Generator.cpp:1244).
- Exporter: a private `effectiveBuildJobs()` (today's computation at Exporter.cpp:1638-1639) used by `runBuild` and by
  the Desktop configure (:1572-1577), which adds `-DJASPERENGINE_PARALLEL_BUILD_JOBS=<jobs>`. It is not part of the
  compile-tree key (Exporter.cpp:1330-1340): a changed job count reconfigures the cache value only.

### 6.7 ABI macros regardless of the library directory (editor/Generator.cpp:1236-1350)

- Move `add_compile_definitions(JASPERENGINE_SHARED JASPERENGINE_EDITOR)` out of both conditional blocks to one
  unconditional line next to the physics definitions (:1266), with the comment "libjasperengine is always an editor
  engine build (shared, editor layouts): match its linkage and data layouts". The plugin block keeps
  `JASPERENGINE_EDITOR_PLUGIN`.
- After `find_library` (:1347-1350), generate:

```cmake
if(JASPERENGINE_LIB MATCHES "\\.a$" OR (WIN32 AND NOT EXISTS "${JASPERENGINE_LIB_DIR}/jasperengine.dll"
                                        AND NOT EXISTS "${JASPERENGINE_LIB_DIR}/libjasperengine.dll"))
    message(FATAL_ERROR "${JASPERENGINE_LIB} is not an editor engine library (shared). JASPERENGINE_LIB_DIR must "
                        "name the folder of a JasperEngine Editor (its engine library).")
endif()
```

  (The implementer confirms the MinGW DLL name from the root CMakeLists output name.)

### 6.8 `jasperengine.py` (engine/jasperengine.py) and the editor's Web generator

- Replace `find_emcmake` with `find_emscripten()` returning `(emscripten_dir, emsdk_root, node, config, python)` with
  the editor's order (Exporter.cpp:6184-6290 `detectEmsdk`/`fillEmsdk`): `EMSDK` (then the older `EMSCRIPTEN_ROOT`,
  `EMSCRIPTEN`), as an emsdk root (`upstream/emscripten`) or an emscripten dir; else `emcc` on PATH, resolved. An
  emscripten dir has `cmake/Modules/Platform/Emscripten.cmake` and one of `emcc`, `emcc.exe`, `emcc.bat`, `emcc.py`.
  Node: `EMSDK_NODE`, else the config's `NODE_JS`, else the newest `<emsdk>/node/*/bin/node[.exe]`, else `node` on PATH.
- Web configure: `cmake -DCMAKE_TOOLCHAIN_FILE=<.../Emscripten.cmake> [-DCMAKE_CROSSCOMPILING_EMULATOR=<node>] ...`
  run with the environment the editor builds (`emscriptenEnvironment`, Exporter.cpp:6369-6390: SDK dirs first on
  PATH, `EMSDK`, `EM_CONFIG`, `EMSDK_NODE`, `EMSDK_PYTHON`). Paths use forward slashes.
- Generator (`choose_generator(platform, build_dir, given)`): `--generator` wins; else Xcode platforms as today; else,
  if `build_dir/CMakeCache.txt` exists, none (CMake keeps its own); else non-Windows: Ninja when on PATH; Windows web:
  Ninja on PATH, else `find_vs_ninja()` (vswhere `-products * -property installationPath`, each install's
  `Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe`) with `-G Ninja -DCMAKE_MAKE_PROGRAM=<path>`, else
  `mingw32-make` on PATH (`-G "MinGW Makefiles"`), else `fail("no CMake generator for Emscripten: install Ninja ...")`.
  Windows desktop: unchanged (CMake's Visual Studio default).
- `--dry-run`: print each command (`> cmake ...`) and the environment variables it sets, run nothing, exit 0.
- Editor: `CommandRunner::findVisualStudioNinja()` (editor/util/CommandRunner.{h,cpp}, Windows; uses the existing
  `findVswherePath`/`vsInstallationPath` pattern, all installs). `Exporter::emscriptenWindowsGeneratorArgs` gains a
  `const fs::path& vsNinja` parameter (empty: none): after PATH's `mingw32-make` and `ninja`, a non-empty `vsNinja`
  gives `{"-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + forward-slashed path}`; the error message adds "or install Visual
  Studio's C++ CMake tools". `emscriptenCMakeArgs` passes `CommandRunner::findVisualStudioNinja()` on Windows.
  `isEmscriptenDir` (:6128-6132) also accepts `emcc.exe`. `vswhere` output encoding is `unicode-text`'s
  (`-utf8`); this feature calls the existing helper.

### 6.9 One backend table (new editor/export/PlatformBackends.{h,cpp})

```cpp
namespace jasperengine::editor::exportbackends {
    // The backends a source export compiles shaders for, per target platform
    std::set<ShaderBackend> sourcePreset(BuildPlatform platform);
    std::optional<std::set<ShaderBackend>> sourcePreset(std::string_view mcpPlatform);   // "android", ...; nullopt: unknown
    // Graphics APIs a Desktop player of this host can use
    std::span<const ShaderBackend> desktopBackends();
}
```

- Table: Windows {GLCore, Vulkan, D3D11}; macOS {MetalMacOS, GLCore}; Linux {GLCore, Vulkan}; Android {GLES3};
  iOS {MetalIOS}; Web {GLES3}. Desktop: as today (ExportWindow.cpp:35-49).
- ExportWindow.cpp:249-254, :453-462 (`platformBackends`), :875-880 and :35-49, and BuildTools.cpp:161-181
  (`desktopBackends`, `platformBackends`) call it; their copies are deleted.
- engine/CMakeLists.txt:434-437 Android: `else() message(FATAL_ERROR "Android supports GRAPHIC_BACKEND gles3 (got
  '${GRAPHIC_BACKEND}')")`. build.gradle:16 comment and the `jasperBackend` use: accept `gles3` only, `throw new
  GradleException(...)` otherwise. android_player.sh:14,26: `gles3` only.
- No change to saved projects (5.9).

### 6.10 MCP, Lua, editor UI, docs

- MCP: no new tool or parameter. `build_project`/`get_build_info` report Android source backends from the table.
  `docs/mcp/TOOLS.md` unchanged (no tool definition changes; run `gen_tool_docs.py --check` to confirm).
- Lua: none (no runtime behaviour).
- Editor UI: Project Settings > Editor > Version Control Files help text (6.4); the Build Settings Custom Graphics
  APIs page follows the table.
- Docs:
  - docs/editor/project-hub.md: which processes record recent projects (5.1).
  - docs/editor/version-control.md:23-26 and docs/mcp/guide/vcs.md: the managed block, own lines below it,
    overriding with later lines.
  - docs/mcp/guide/cpp-scripting.md: "Box2D and Jolt directly": the handles (`getBox2DBody`, `getWorld2D`,
    `getJoltBody`), any `b2*`/`JPH::` call works in Play and builds; `#include "box2d/box2d.h"`,
    `#include <Jolt/Jolt.h>` first.
  - docs/shipping.md: source exports: `./gradlew`, `./jasperengine.py` (`--dry-run`), Windows web builds and Visual
    Studio's Ninja, the Windows-to-Unix exec-bit note, Android gles3 only.
  - docs/testing.md "Tool checks" row: `python3 tools/ci/test_jasperengine_py.py`.
  - The agent guide digest: unchanged (no byte cost).

## 7. Behaviour rules and edge cases

- 6.1: a person's interactive editor opening a project by `--project` (hub launch options) still records; File >
  Open, the hub, `open_project` over HTTP in that editor record. An interactive editor's Save As of the temporary
  project records. The stdio MCP editor whose agent then opens another project through `open_project`: not recorded.
- 6.1: AppSettings still loads and may save other keys in tool processes (an Exporter's `setLastCMakeKit` is a user
  setting the CLI legitimately updates); only the two recent-project effects are gated.
- 6.2: an empty title still falls back to the project name (Project.cpp:1801-1805); a title of only escaped bytes is
  fine; length unchanged in meaning (escapes expand the command line up to 4x; titles are short, no cap added).
- 6.3: a target whose filesystem refuses chmod (FAT, some network mounts) fails the export with the OS reason; the
  user can export elsewhere. Windows hosts: no-op.
- 6.4: a user line equal to a generated line in the new format is kept (it is below the end marker); a user line in
  an old-format file that equals a known generated line is dropped once (it is redundant: the block has it, or it
  was a previous variant). A file with the start marker twice or the end marker twice: the first end marker counts.
  CRLF files: the user part keeps its bytes; the block is written with `\n` (`.gitattributes` sets `eol=lf`).
  Version Control Files off: nothing is read or written (unchanged).
- 6.5: a script using Box2D/Jolt types only through the engine's wrappers is unaffected. A MinGW editor needs the
  forced-include headers; Clang-cl follows the MSVC branch (`_MSC_VER`).
- 6.6: `JASPERENGINE_PARALLEL_BUILD_JOBS=0` or empty: bare `/MP` (cl's CPU count). Ninja/Makefiles/Xcode: no flag.
- 6.7: a custom directory with a matching editor library now builds and runs; a static library fails at configure.
- 6.8: an existing build directory configured with emcmake keeps its cache (no `-G`); the toolchain file argument is
  accepted by CMake again. A build directory configured with Visual Studio for web (impossible for Emscripten) is not
  special-cased.
- 6.9: an MCP `build_project mode: source, platform: android, backends: ["vulkan"]` keeps the explicit list (the
  caller's choice, as today); only the preset changes.

## 8. Performance and cost

- Runtime (games): zero. No engine runtime code changes; shipped games link the static engine without whole-archive.
- Editor: the engine library grows by the unused Box2D/Jolt members (6.5; measured and recorded). Startup and Play:
  unchanged (symbols, not code paths).
- Builds: Visual Studio generator builds of the engine and editor get per-file parallelism (Doriax reports a large
  speedup; measure one clean `windows-vs2022` build in CI when available). Ninja builds unchanged.
- Project saves: `writeVersionControlMetadata` reads two small files it already opened for the marker check; no new
  disk writes when nothing changed (`writeIfChanged`).

## 9. Compatibility

- project.yaml, scenes, settings.yaml: no format change.
- `.gitignore` / `.gitattributes`: old-format files (marker, no end marker) migrate on the next project save or LFS
  toggle (6.4 rule 4); user-owned files (no marker) untouched as before. No version field: the end marker is the
  format.
- Generated `CMakeLists.txt`, `LocalPaths.cmake`: regenerated on every build; existing build trees reconfigure.
- Exports: the next export over an older export repairs its exec bits (6.3).
- Saved source-export backends including Vulkan stay selected (5.9).
- `jasperengine.py`: same options plus `--dry-run`; build directories configured by the old script keep working.

## 10. Tests

Suites per docs/testing.md. "Red" = fails on `main` before the change, verified by the implementer on the build
before the fix lands (state the run in the commit).

gtest `jasperengine-editor-tests`:

1. `RecentProjects.ToolProcessesLeaveSettingsAlone` (new editor/tests/data_safety/RecentProjectsTest.cpp):
   `AppSettings::initializeWithFile(tmp)` with `last_project_path: A`, `recent_projects: [A]`; default host;
   `loadProject(B, true)`, `saveProject()`, `createTempProject(...)`; the file still has last `A`, recent `[A]`, and
   `getLastProjectPath() == A`. **Red** (save records B; the temp project clears A).
2. `RecentProjects.InteractiveHostRecords`: a test host whose `recordsRecentProjects()` is true: load + save B
   records B (guards the positive path).
3. `CMakeText.TitleBytesSurviveBothLayers`: for titles `A; B`, `Game #2`, `"q" \ $HOME`, `100% & a|b ^<>,`,
   `ゲーム 🎮`, `x\0731` (a digit after an escape): `cStringLiteralBody` has only `[A-Za-z0-9 _.\-\\]`; a reference
   decoder (CMake unescape, then C unescape) returns the original bytes; `cStringDefinitionValue("Plain Title")` is
   `Plain Title`.
4. `GeneratorCMake.TitleIsEscapedAsBytes` (extend editor/tests/GeneratorInputTest.cpp or new): the generated
   `CMakeLists.txt` for title `A; B #2` contains `JASPERENGINE_WINDOW_TITLE=\"A\\073 B \\0432\"`. **Red**.
5. `GeneratorCMake.AbiMacrosOutsideTheLibDirBranch`: the generated file has
   `add_compile_definitions(JASPERENGINE_SHARED JASPERENGINE_EDITOR` before and outside
   `if(NOT DEFINED JASPERENGINE_LIB_DIR`, and the static-library guard. **Red**.
6. `GeneratorCMake.PhysicsImportMacros`: the physics definitions line contains `JPH_SHARED_LIBRARY` (and
   `BOX2D_DLL` on MSVC builds). **Red** (the editor never sees the macro today).
7. `VersionControlFiles.*` (new editor/tests/vcs/VersionControlFilesTest.cpp, pure text):
   new file; user-owned file left alone; new-format file keeps its user bytes verbatim (CRLF, blank lines,
   a line equal to a generated one); old-format file: generated lines of another lfs/lockScenes variant and of a
   legacy template removed, whitespace variants (`*.png binary`) removed, user lines kept in order; idempotent
   (refresh of the output is the output); BOM tolerated.
8. `VersionControlMetadata.UserLinesSurviveLfsToggle` (editor/tests/vcs/VersionControlMetadataTest.cpp): create,
   save, append `secrets.env` and `docs/** linguist-documentation`, `setVcsProjectSettings(lfs off, apply)`, save:
   both lines present once after the end marker, no `filter=lfs` anywhere, `git check-ignore secrets.env` succeeds
   and `git check-attr linguist-documentation docs/a.md` is `set`. **Red**.
   `ExistingProjectKeepsBinariesOutOfLfs` (:61) additionally asserts the old `*.png binary` line is gone.
9. `ExportCopy.PermissionsFollowTheSource` (new editor/tests/export/ExportCopyTest.cpp, non-Windows):
   `exportfiles::mirrorTree` over a destination with equal bytes but mode 0644 where the source is 0755: the destination
   becomes 0755 and keeps its mtime. **Red**. `ensureExecutable` adds `+x` to a 0600 file (0700) and reports a missing file.
10. `EmscriptenSdk.WindowsGeneratorFallsBackToVisualStudioNinja` (editor/tests/EmscriptenSdkTest.cpp:154 style):
    PATH without ninja/mingw32-make and a fake `vsNinja` gives `{-G, Ninja, -DCMAKE_MAKE_PROGRAM=<path>}`; PATH's
    tools still win; empty `vsNinja` keeps the error. **Red** (no parameter today: compile-fails, then fails).
    `EmscriptenSdk.AcceptsEmccExe`: a layout with only `emcc.exe`. **Red**.
11. `PlatformBackends.AndroidIsGles3Only`: `sourcePreset(BuildPlatform::Android) == {GLES3}`,
    `sourcePreset("android")` the same; every platform's preset equals the documented table. **Red** (Android).

Tool check (Python, docs/testing.md "Tool checks"; CI step next to ci.yml:83-85):

12. `tools/ci/test_jasperengine_py.py` imports engine/jasperengine.py as a module and, with fake SDK layouts in a
    temp dir and a patched `host_platform`/`shutil.which`/`subprocess.run` for vswhere:
    an Emscripten 6 Windows layout (`emcmake.exe`, `emcc.exe`, no `.bat`) is found; `--dry-run --platform web` prints
    a `cmake` command with `-DCMAKE_TOOLCHAIN_FILE=.../Emscripten.cmake`, no `emcmake`; on "windows" without PATH
    Ninja and with vswhere's Ninja: `-G Ninja -DCMAKE_MAKE_PROGRAM=...`; with `CMakeCache.txt` present: no `-G`;
    nothing found: exit 1 naming Ninja. **Red** (today: "Emscripten not found" for the Emscripten 6 layout).

MCP end-to-end (`tools/mcp/tests/`, Linux, installed editor):

13. `test_build.py::test_source_export`: after the export, `jasperengine.py` and
    `workspaces/androidstudio/gradlew` are executable (`os.access(X_OK)`). **Red** on the installed editor (6.3).
    Also: export with `platform: android` alone gives `get_build_info.source.backends == ["opengles"]`. **Red**.
14. `test_cli_paths.py::test_export_leaves_recent_projects_alone` (new case): the scratch `HOME` holds
    `.config/jasperengine/settings.yaml` with `last_project_path: <other>` and `recent_projects: [<other>]`; `export
    --project proj --out ...` from a working directory without settings; the file's `last_project_path` and
    `recent_projects` are unchanged (parsed, not mtime: other keys may legitimately merge). **Red**.
15. `test_cli_paths.py::test_title_with_every_awkward_character` (new): set `player.window_title` in project.yaml to
    `Pirates; Revenge #2 "Gold" \ $HOME 100% & | ^ < > , ゲーム 🎮`, export, configure the export with
    `-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, take the game target's compile command for its `main.cpp` from
    `compile_commands.json`, run it with `-E -P -x c++` on a probe source holding `JASPERENGINE_WINDOW_TITLE`, decode
    the printed literal: equal to the title bytes. **Red** (the `;` splits the definition).
16. New `test_cpp_physics_libs.py` (CI=True, `create`): a C++ script whose `onStart` calls a Box2D function and a Jolt
    function the engine does not reference (the implementer picks them with `nm -DC libjasperengine.so` before the
    change, e.g. `b2World_Explode` with `b2DefaultExplosionDef`, and a `JPH::` non-inline function of the same kind)
    plus `JPH::Allocate`/`JPH::Free`, and logs their results; `compile_scripts` succeeds, `play` logs the line.
    **Red** on Linux (undefined symbol at the plugin's `-z defs` link). Second case: configure the generated project
    standalone with `-DJASPERENGINE_LIB_DIR=<the editor's bin dir>` and `CMAKE_EXPORT_COMPILE_COMMANDS=ON`: every
    compile command has `-DJASPERENGINE_EDITOR`. **Red**.
17. `test_hub.py` or a new `test_tool_settings.py::test_headless_mcp_leaves_recent_projects_alone`: a headless stdio
    editor (`mcp --project <copy> --headless`) whose settings file holds `last_project_path: <other>`;
    `save_project`, quit; the file's two keys unchanged. Then `mcp --project <missing dir> --headless` (the
    temp-project fallback): `last_project_path` still `<other>`. **Red** (both).

CI scripts:

18. tools/ci/android_size.sh runs `./gradlew` (not `bash ./gradlew`): the compiled-arm64 step fails without 6.3.
19. Windows CI (`.github/workflows/ci.yml` Windows MSVC job, after the install): `instdir/bin/jasperengine-editor-cmd
    check --cpp --project <copy of tools/fixtures/cpp-physics-probe>` (a new fixture: one C++ script calling the
    functions of test 16). The Windows red control: it cannot be run on this host (no Windows); the implementer
    states that the step is unverified until a Windows CI run, keeps it compiling by reading, and records the first
    CI result in the ledger. If `check --cpp` needs a GL context the runner lacks, the fallback is a plain CMake
    probe project in `tools/ci/cpp-physics-probe/` linking `instdir/bin/jasperengine.lib` with the generated
    definitions.

Manual (recorded with output in the commit): `tools/build.sh exec web python3 <export>/jasperengine.py --platform
web --no-build` configures (Linux, Emscripten 6.0.10 image); `tools/build.sh linux` succeeds and the installed
`bin/engine/jasperengine.py` is `-rwxr-xr-x`.

## 11. Acceptance criteria

1. Tests 1-17 pass; each **Red** test was run on the tree without its fix and failed, and the run is named in the
   commit message or the ledger.
2. Every `jasperengine-editor` CLI subcommand (they all run on the default `EditorHost`), stdio MCP editors and headless
   editors leave `last_project_path` and `recent_projects` unchanged (tests 1, 14, 17); an interactive editor records
   as before (test 2).
3. A window title with any of `; # " \ $ % & | ^ < > ,` and non-ASCII text builds from source and decodes to the same
   bytes (tests 3, 4, 15); Exporter and Generator share one helper (no title-escaping loop left in either file).
4. A source export from the installed editor has executable `jasperengine.py` and `gradlew` (test 13); re-exporting
   over an export with lost bits repairs them (test 9); android_size.sh uses `./gradlew` (18).
5. Lines below the end marker of `.gitignore`/`.gitattributes` survive saves and LFS/Lockable Scenes toggles; old-format
   files keep only their user lines (tests 7, 8).
6. A C++ script calling Box2D and Jolt functions the engine does not use compiles, links and runs in Play on Linux
   (test 16); the Windows CI step exists (19) and the Box2DExport/JoltExport headers compile in a MinGW configure by
   reading (stated as unverified).
7. Visual Studio generator configures add `/MP` (with the job value when given) to engine and editor targets: checked
   by reading `CMAKE_CXX_FLAGS`/target options in a `windows-vs2022` configure when CI runs it; exports pass
   `JASPERENGINE_PARALLEL_BUILD_JOBS` (the Desktop configure argv, asserted in a unit test of the argv builder if the
   implementer extracts it, else by reading the build log line).
8. The generated project defines `JASPERENGINE_SHARED JASPERENGINE_EDITOR` for every library directory and refuses a
   static one (tests 5, 16).
9. `jasperengine.py --platform web` needs no emcmake, finds Emscripten 6 layouts and Visual Studio's Ninja (test 12);
   the editor's Web build falls back to Visual Studio's Ninja (test 10).
10. Android presets are `{GLES3}` in the Build Settings window and MCP (tests 11, 13); the engine's Android CMake, Gradle
    and android_player.sh refuse other backends with a message.
11. Docs of 6.10 updated; `tools/mcp/gen_tool_docs.py --check` and `tools/check_agent_docs.py` pass; `tools/check-changed`
    green.

## 12. Files likely touched

- editor/EditorHost.{h,cpp}, editor/App.{h,cpp} (recordsRecentProjects)
- editor/Project.cpp (saveProjectToPath :6765, loadProject :6902, createProjectInDirectory :6350,
  writeVersionControlMetadata :6542-6660 moved out), editor/Project.h (none expected)
- editor/util/CMakeText.{h,cpp} (new), editor/util/VersionControlFiles.{h,cpp} (new)
- editor/Exporter.{h,cpp} (title :3551-3576, copyEngine :3417-3445, the tree copy :3326-3375, now
  editor/export/ExportFiles.cpp `mirrorTree`, configure
  :1557-1586, runBuild :1630-1643, emscriptenWindowsGeneratorArgs :6286-6309, emscriptenCMakeArgs :6311-6330,
  isEmscriptenDir :6128)
- editor/Generator.cpp (title :503-525, physics definitions :452-485 and :1266, ABI block :1268-1350)
- editor/util/CommandRunner.{h,cpp} (findVisualStudioNinja)
- editor/export/PlatformBackends.{h,cpp} (new), editor/window/dialog/ExportWindow.cpp (:35-49, :249-254, :453-462,
  :875-880), editor/mcp/tools/BuildTools.cpp (:161-181)
- editor/window/dialog/ProjectSettingsWindow.cpp (:2743 help text)
- engine/CMakeLists.txt (Android :434-437, /MP :918, physics export block after :2251, jobs cache var),
  engine/cmake/JasperEngineToolchain.cmake, engine/cmake/JasperLinker.cmake, CMakeLists.txt (/MP call before :556,
  install :1707), CMake source lists for the new editor files
- engine/platform/win/Box2DExport.h, JoltExport.h (new)
- engine/jasperengine.py, engine/workspaces/androidstudio/app/build.gradle, tools/players/android_player.sh,
  tools/ci/android_size.sh, tools/raw_writes_allowlist.txt, .github/workflows/ci.yml
- Tests: editor/tests/data_safety/RecentProjectsTest.cpp, editor/tests/vcs/VersionControlFilesTest.cpp,
  editor/tests/vcs/VersionControlMetadataTest.cpp, editor/tests/export/ExportCopyTest.cpp,
  editor/tests/EmscriptenSdkTest.cpp, editor/tests/GeneratorInputTest.cpp (or a new GeneratorCMakeTest.cpp),
  editor/tests/export/PlatformBackendsTest.cpp, tools/ci/test_jasperengine_py.py, tools/mcp/tests/test_build.py,
  tools/mcp/tests/test_cli_paths.py, tools/mcp/tests/test_cpp_physics_libs.py, tools/mcp/tests/test_tool_settings.py,
  tools/fixtures/cpp-physics-probe/
- Docs: docs/editor/project-hub.md, docs/editor/version-control.md, docs/mcp/guide/vcs.md,
  docs/mcp/guide/cpp-scripting.md, docs/shipping.md, docs/testing.md

Hot files shared with other features: editor/Exporter.cpp, editor/Generator.cpp, editor/Project.cpp,
engine/CMakeLists.txt, editor/window/dialog/ExportWindow.cpp, build.gradle, editor/util/CommandRunner.cpp.

## 13. Cross-feature dependencies

- **unicode-text**: owns Windows code pages, `vswhere -utf8` (CommandRunner) and saved-path slashes. Octal title
  escapes (6.2) make titles independent of its code-page change; `findVisualStudioNinja` uses its vswhere helper
  as it lands. Both touch Exporter.cpp/Generator.cpp/CommandRunner.cpp: merge whichever is second by rebasing onto
  the helper changes. The non-goal on path quoting in generated CMake (section 2) is offered to it.
- **export-pipeline**: owns failed copies failing the export and build errors in the Build dialog; 6.3's
  `ensureExecutable` failure follows the same rule and should use its error-reporting helper if it lands first.
  Both edit `copyEngine`, the tree copy and `runBuild`: expect a textual merge in Exporter.cpp.
- **mobile-shipping**: retires AdMob from build.gradle, ExportWindow and Exporter (and Android paths I touch:
  build.gradle `jasperBackend`, ExportWindow presets). Merge after it or resolve the ExportWindow table sites by
  taking its deletions and my table calls.
- **platform-lifecycle**: Android Gradle deprecations in build.gradle; my change there is the `jasperBackend` check
  only.
- **agent-authoring**: its "build-tools check on Windows" may report Ninja availability; it should count Visual
  Studio's Ninja through `CommandRunner::findVisualStudioNinja()` (6.8).
- **scene-integrity**: Save All / save callbacks touch `Project::saveProject*` near :6430-6770; my edits are two
  conditions there. Small textual merge.
- **scripting**: C++ completion hiding Lua-only statics; no overlap beyond docs/mcp/guide/cpp-scripting.md sections.

## Deviations (implementation, 2026-11-04)

1. **One backend table in `ShippingTypes`, not a new `editor/export/PlatformBackends`.** `ShippingTypes.{h,cpp}`
   already held the per-platform player backends (`platformBackends`), shared by the window, MCP and the prebuilt
   exporter; `sourceExportBackends()` and `hostDesktopBackends()` joined it there. That table also listed Android
   Vulkan players (`{"gles3", "vulkan"}`), and `tools/build.sh players` built an Android Vulkan player by default:
   both are gles3 only now, with the refusals of 6.9.
2. **Jolt and Box2D macros on the engine's own sources too.** The spec put `JPH_SHARED_LIBRARY` on the engine as
   `INTERFACE` only. Jolt's profiler declarations differ with that macro (`Jolt/Core/Profiler.h`), so the engine's
   own sources compile with `JPH_BUILD_SHARED_LIBRARY` + `JPH_SHARED_LIBRARY` (PUBLIC) and `box2d_EXPORTS`
   (PRIVATE), as the libraries do; consumers get `JPH_SHARED_LIBRARY` and `BOX2D_DLL`.
   `jasperengine_link_whole_archive` passes GNU's `--whole-archive` as one `SHELL:` item per library (CMake
   de-duplicates repeated link options).
3. **No CI.** `.github/workflows` was removed on `main` while this was implemented, so the planned `ci.yml` steps
   (the Windows link check, the Python test) are gone. `tools/ci/check-script-physics-link.py <editor>` is a local
   script (it runs on Linux too) and `tools/ci/test_jasperengine_py.py` is a tool check (`docs/testing.md`).
   Nothing runs the Windows or MinGW paths: the export macros, `Box2DExport.h`, `JoltExport.h`, `/WHOLEARCHIVE`,
   `/MP` and `findVisualStudioNinja` were written by reading, not compiled.
4. **Test names and places.** Generator tests are `editor/tests/GeneratorCMakeTest.cpp`; the Visual Studio Ninja
   case has its own file (`EmscriptenWindowsNinjaTest.cpp`, a new parameter); the headless MCP cases are
   `tools/mcp/tests/test_tool_settings.py`; the Android preset is checked in `test_build.py::test_source_export`
   through the exported shader headers (an agent's `platform` choice is not remembered by `get_build_info`).
   The physics test calls `JPH::TriangleSplitterMean`, the one public Jolt class the red build's engine library
   lacked (its dynamic symbols against `libjoltphysics.a`'s), with `b2ShapeCast`/`b2MakeProxy`,
   `JPH::ConvexHullBuilder2D`, `JPH::StateRecorderImpl` and `JPH::Allocate`. On Linux every public Box2D function
   was already exported (the engine pulls all of Box2D's objects): Box2D's red control is Windows only, unrun.
5. **`Exporter::ensureExecutable` is a public static** so the unit test reaches it; the tree copy it tests is
   `exportfiles::mirrorTree` (export-pipeline moved `copyTreeIfChanged` into editor/export/ExportFiles).
6. **No eval case.** No tool text or agent-guide text changed; the additions are in area guides
   (`docs/mcp/guide/vcs.md`, `cpp-scripting.md`), which the eval surface does not carry.
7. **Gate.** Per the lead's later rule the branch gate is `WERROR=1 tools/check-changed --base main` plus this
   spec's tests and red controls; the full suites run at landing.
8. **Red tree for the unit tests.** The red unit run used the base's code with this branch's tests; the tests of
   new API (`CMakeText`, `VersionControlFiles`, `ExportCopy`, `PlatformBackends`, the Visual Studio Ninja case)
   were left out of it (they do not compile there), and `RecentProjectsTest` without its two lines that name
   `recordsRecentProjects()`.
9. **`/MP` and MSBuild's project parallelism.** Under a Visual Studio generator `cmake --build --parallel N` is
   MSBuild's `/m:N`, and each project's `cl` takes `/MP<N>`: up to N x N compilers at once while several projects
   build. Capping that needs MSBuild's MultiToolTask scheduler (`UseMultiToolTask`, `EnforceProcessCountAcrossBuilds`),
   which section 2 left out and which cannot be checked here (no Windows); `/MP` stays at the job count.
10. **Review fixes.** The editor's C++ editor scripts (`EditorScripts::writeCppProject`, now a public static so a test
    reads its CMake) and the clangd flags compile with `Generator::physicsCompileDefinitions()` too (they lacked the
    Jolt layout macros as well); `jasperengine.py` leaves the generator to `CMAKE_GENERATOR` when it is set and
    takes node from `EMSDK_NODE`, then the config's `NODE_JS`, then the emsdk's newest, then `PATH`; the editor runs
    vswhere for Visual Studio's Ninja only when PATH has no generator, and reads its output as UTF-8;
    `/WHOLEARCHIVE` is passed as is to MSVC-style drivers and through `LINKER:` for a GNU-style clang++ targeting
    MSVC.

## Follow-ups (2026-11-07)

Workstream platform-build (branch `fu/platform-build`).

1. **Visual Studio builds run at most the job count of compilers** (deviation 9). Under a Visual Studio
   generator the build step now passes `--parallel 1` (`Generator::buildStepParallelJobs`, from the
   tree's `CMAKE_GENERATOR`; the editor's Play build, the export's build and `jasperengine.py`), and
   each project's `cl` keeps `/MP<jobs>` from the configure's `JASPERENGINE_PARALLEL_BUILD_JOBS`
   (`jasperengine.py` passes it now too). MSBuild's `/m:<jobs>` over projects that each ran
   `/MP<jobs>` started up to jobs x jobs compilers while the engine's libraries built side by side.
   Rejected: splitting the budget (`/m:a` x `/MP:b`): the engine library, the longest project, builds
   alone after its dependencies and would compile at a fraction of the cores; MSBuild's MultiToolTask
   scheduler (`UseMultiToolTask`, `EnforceProcessCountAcrossBuilds`) is still opt-in, reported to fail
   with precompiled headers, and cannot be checked without Windows. Cost: small libraries and link
   steps no longer overlap. Ninja and the other generators keep `--parallel <jobs>`. Builds started
   from the Visual Studio IDE follow its "maximum number of parallel project builds" setting, which
   the editor does not control. Not run here: a Visual Studio build.
2. The spec named `copyTreeIfChanged`, which export-pipeline replaced by `exportfiles::mirrorTree`
   (editor/export/ExportFiles.cpp): the references say so now.
